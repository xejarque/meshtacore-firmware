// MeshCore side of RadioLibInterface: raw frame TX, the relay and ADVERT schedule, the repeater identity and the
// companion bridge. Kept out of RadioLibInterface.cpp so that file stays close to the jentastic upstream and a merge of
// origin/main does not collide with this code. The hooks that call into it remain in RadioLibInterface.cpp.
#include "ContentionStats.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PowerMon.h"
#include "RadioLibInterface.h"
#include "RadioTxHook.h"
#include "SPILock.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "error.h"
#include "main.h"
#include "mesh-pb-constants.h"
#include "modules/NexusModule.h"
#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER
#include "FSCommon.h"
#include "HardwareRNG.h"
#include "MeshCoreRepeater.h"
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
#include "MeshCoreCompanion.h"
#endif
#include "NodeDB.h"
#include "Router.h"
#include "SafeFile.h"
#include "gps/RTC.h"
#endif
#include <pb_decode.h>
#include <pb_encode.h>

bool RadioLibInterface::sendRawFrame(const uint8_t *bytes, size_t len)
{
    // Must not collide with the normal packet TX path or an in-flight raw frame. Listen-before-talk
    // is the caller's job (see maybeSendMeshCoreFrame), which leaves the radio in standby.
    if (len == 0 || len > sizeof(rawTxBuf) || isSending()) {
        LOG_WARN("sendRawFrame rejected (len=%u sending=%d)", (unsigned)len, isSending());
        return false;
    }
    if (disabled || !config.lora.tx_enabled) {
        LOG_WARN("sendRawFrame - tx disabled");
        return false;
    }

    memcpy(rawTxBuf, bytes, len);
    rawTxLen = len;

    // Drive the radio into TX. startTransmit() arms the TX-done IRQ which
    // handleTransmitInterrupt() will route to completeRawSend(). rawSending is set before
    // configHardwareForSend() so drivers can pick the MeshCore sync word for this frame.
    rawSending = true;
    enableInterrupt(isrTxLevel0);
    configHardwareForSend();
    int res = iface->startTransmit(rawTxBuf, rawTxLen);
    if (res != RADIOLIB_ERR_NONE) {
        LOG_ERROR("sendRawFrame startTransmit failed, error=%d", res);
        rawSending = false;
        rawTxLen = 0;
        startReceive();
        return false;
    }
    lastTxStart = millis();
    return true;
}

bool RadioLibInterface::meshCoreTxAllowed()
{
    if (disabled || !config.lora.tx_enabled)
        return false;
    // MeshCore is LoRa with its own sync word: not on the alternate modem (SETFSK/SETFLRC), and only on chips whose driver
    // sets that sync word for a raw frame.
    if (altModemActive() || !meshCoreTxSupported())
        return false;
    // Same duty-cycle guard as Router::send: a foreign-protocol frame must not push us over the regional limit.
    const float effectiveDutyCycle = getEffectiveDutyCycle();
    return config.lora.override_duty_cycle || effectiveDutyCycle >= 100 || airTime->utilizationTXPercent() <= effectiveDutyCycle;
}

bool RadioLibInterface::queueMeshCoreFrame(const uint8_t *bytes, size_t len)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    if (!bytes || len == 0 || len > MeshCoreRepeater::MAX_FRAME_LEN)
        return false;

    if (!meshCoreTxAllowed()) {
        LOG_WARN("MeshCore TX refused: tx disabled or duty cycle exceeded");
        return false;
    }

    // Our own text outranks every queued flood (it evicts the worst one when the queue is full) but never evicts
    // another frame of ours (advert, discover reply) or routed traffic.
    if (!meshCoreRepeater.queueOwnFrame(bytes, len)) {
        LOG_WARN("MeshCore TX refused: the relay queue holds only own and routed frames");
        return false;
    }

    meshCoreCompanion.noteTransmitted(bytes, len);
    meshCoreCompanion.txSent++;
    wakeMeshCoreRelay();
    return true;
#else
    (void)bytes;
    (void)len;
    return false;
#endif
}

void RadioLibInterface::completeRawSend()
{
    rawSending = false;
    uint32_t xmitMsec = getPacketTime(rawTxLen, false);
    airTime->logAirtime(TX_LOG, xmitMsec);
#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER
    meshCoreRepeater.txAirtimeMs += xmitMsec;
#endif
    // txGood feeds the Meshtastic num_packets_tx telemetry: MeshCore frames are counted by meshCoreRepeater/Companion.
    rawTxLen = 0;
}

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER
static constexpr size_t MESHCORE_SEED_LEN = 32;
static constexpr uint32_t MESHCORE_IDENTITY_LOAD_MS = 5 * 1000; // identity ready early: discovery needs it
// Flood adverts only, which reach the direct neighbours too: the first a few minutes after boot so the node shows up
// without waiting, then every advert period (12 hours unless Nexus changed it). A flood is re-sent by every node, so it
// must stay rare.
static constexpr uint32_t MESHCORE_FLOOD_ADVERT_FIRST_MS = 3 * 60 * 1000;
static constexpr uint32_t MESHCORE_MS_PER_HOUR = 60UL * 60 * 1000;
// A flood asked for by Nexus goes out at once; a request closer than this to the last one is refused, so a loop of
// requests cannot flood the mesh.
static constexpr uint32_t MESHCORE_ADVERT_NOW_MIN_MS = 10UL * 60 * 1000;
// A node that relays while announcing itself as a chat node also floods a repeater-typed ADVERT signed with its relay
// alias: MeshCore applications only name a path hop from a contact held as a repeater, so without it the hop we append
// shows as an unknown repeater. It rides the same slot as the main flood, one own frame at a time, so the cost is one
// extra flood per interval.
static const char MESHCORE_ALIAS_SUFFIX[] = " (relay)";
// The alias flood bypasses the 12 h deadline while it is pending, so a queue that keeps refusing it must give up.
static constexpr uint8_t MESHCORE_ALIAS_ADVERT_MAX_ATTEMPTS = 3;
static constexpr uint32_t MESHCORE_ADVERT_RETRY_MS = 60 * 1000;           // no identity / no valid time
static constexpr uint32_t MESHCORE_ADVERT_NUDGE_MS = 1000;                // radio busy: ask again
static constexpr uint32_t MESHCORE_SERVICE_MS = 5 * 1000;                 // longest wait between housekeeping steps
static constexpr uint32_t MESHCORE_CONTACTS_SAVE_MIN_MS = 10 * 60 * 1000; // flash wear: at most one save per 10 minutes
static constexpr uint32_t MESHCORE_TX_POLL_MS = 100;                      // a raw frame is on air: look again for a lost TX-done

/** Runs RadioLibInterface::meshCoreAdvertTick() on its own schedule. */
class MeshCoreAdvertThread : public concurrency::OSThread
{
  public:
    explicit MeshCoreAdvertThread(RadioLibInterface *radio) : OSThread("MeshCoreAdvert", MESHCORE_IDENTITY_LOAD_MS), radio(radio)
    {
    }

  protected:
    int32_t runOnce() override { return radio->meshCoreServiceTick(); }

  private:
    RadioLibInterface *radio;
};

// The settings file is the whole MeshCoreConfig image. A missing or unreadable file reads as false and changes nothing.
static const char *meshCoreConfigPath = "/prefs/meshcore_config.bin";

static bool loadMeshCoreConfigImage(MeshCoreConfig *out)
{
#ifdef FSCom
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    size_t n = 0;
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(meshCoreConfigPath, FILE_O_READ);
        if (!f)
            return false;
        n = f.size() <= sizeof(image) ? f.read(image, f.size()) : 0;
        f.close();
    }
    return MeshCoreConfig::decode(image, n, out);
#else
    (void)out;
    return false;
#endif
}

static bool saveMeshCoreConfigImage(const MeshCoreConfig &settings)
{
#ifdef FSCom
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    if (MeshCoreConfig::encode(settings, image, sizeof(image)) != sizeof(image))
        return false;
    // SafeFile: atomic replace under spiLock. A bare FILE_O_WRITE appends on nRF52 LittleFS.
    SafeFile out(meshCoreConfigPath, true);
    out.write(image, sizeof(image));
    return out.close();
#else
    (void)settings;
    return false;
#endif
}

/**
 * The repeater's MeshCore identity seed, kept in flash so the node keeps the same MeshCore key across reboots.
 * Created from the hardware RNG on first use; never created from a weak source.
 */
static bool loadOrCreateMeshCoreSeed(uint8_t seed[MESHCORE_SEED_LEN], bool forceNew)
{
#ifdef FSCom
    static const char *path = "/prefs/meshcore_identity.bin";
    if (!forceNew) {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(path, FILE_O_READ);
        if (f) {
            bool ok = f.size() == MESHCORE_SEED_LEN && f.read(seed, MESHCORE_SEED_LEN) == MESHCORE_SEED_LEN;
            f.close();
            if (ok)
                return true;
            LOG_WARN("MeshCore identity: %s is corrupt, creating a new key", path);
        }
    }
#endif
    if (!HardwareRNG::fill(seed, MESHCORE_SEED_LEN, true)) {
        LOG_ERROR("MeshCore identity: no hardware entropy, not creating a key");
        return false;
    }
#ifdef FSCom
    // SafeFile: atomic replace under spiLock. A bare FILE_O_WRITE appends on nRF52 LittleFS.
    SafeFile out(path, true);
    out.write(seed, MESHCORE_SEED_LEN);
    if (!out.close())
        LOG_ERROR("MeshCore identity: could not save %s, the key will change on reboot", path);
#endif
    LOG_INFO("MeshCore identity created");
    return true;
}

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
static const char *meshCoreContactsPath = "/prefs/meshcore_contacts.bin";

static void loadMeshCoreContacts()
{
#ifdef FSCom
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    size_t n = 0;
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(meshCoreContactsPath, FILE_O_READ);
        if (!f)
            return;
        n = f.size() <= sizeof(image) ? f.read(image, f.size()) : 0;
        f.close();
    }
    const size_t loaded = meshCoreCompanion.importContacts(image, n);
    LOG_INFO("MeshCore nodes: %u restored from flash", (unsigned)loaded);
#endif
}

static void saveMeshCoreContacts()
{
#ifdef FSCom
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t n = meshCoreCompanion.exportContacts(image, sizeof(image));
    if (n == 0)
        return;
    SafeFile out(meshCoreContactsPath, true); // atomic replace
    out.write(image, n);
    if (!out.close())
        LOG_WARN("MeshCore contacts: could not save %s", meshCoreContactsPath);
#endif
}
#endif

size_t RadioLibInterface::formatContentionStats(char *buf, size_t len)
{
    ContentionStats s;
    s.mtRxGood = rxGood;
    s.mtRxBad = rxBad;
    s.mtTxGood = txGood;
    s.mtCadBusy = meshtasticCadBusy;
    s.mtTxAirtimeMs = meshtasticTxAirtimeMs;
    s.mtRxAirtimeMs = meshtasticRxAirtimeMs;
    s.mcSeen = meshCoreRepeater.rxSeen;
    s.mcDup = meshCoreRepeater.rxDup;
    s.mcRelayed = meshCoreRepeater.txRelayed;
    s.mcDropped = meshCoreRepeater.txDropped;
    s.mcDeferChannel = meshCoreRepeater.deferChannel;
    s.mcDeferRadio = meshCoreRepeater.deferRadio;
    s.mcTxAirtimeMs = meshCoreRepeater.txAirtimeMs;
    s.mcRxAirtimeMs = meshCoreRepeater.rxAirtimeMs;
    s.channelUtilPercent = airTime->channelUtilizationPercent();
    s.txUtilPercent = airTime->utilizationTXPercent();
    return ::formatContentionStats(buf, len, s);
}

uint32_t RadioLibInterface::meshCoreServiceTick()
{
    ensureMeshCoreConfigLoaded();
    uint32_t next = meshCoreAdvertTick();
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    if (meshCoreRepeater.hasIdentity() && !meshCoreCompanion.hasIdentity()) {
        meshCoreCompanion.setIdentity(meshCoreRepeater.getIdentitySeed(), meshCoreRepeater.getIdentityPub());
    }
    if (meshCoreRepeater.hasAliasIdentity())
        meshCoreCompanion.setRelayAliasPub(meshCoreRepeater.getAliasPub());
    if (meshCoreCompanion.hasIdentity() && !meshCoreContactsLoaded) {
        meshCoreContactsLoaded = true;
        loadMeshCoreContacts();
        meshCoreContactsFingerprint = meshCoreCompanion.contactsFingerprint();
        lastMeshCoreContactsSaveMs = millis();
    }

    meshCoreCompanion.service();

    if (meshCoreContactsLoaded && Throttle::hasElapsed(lastMeshCoreContactsSaveMs, MESHCORE_CONTACTS_SAVE_MIN_MS)) {
        const uint32_t fingerprint = meshCoreCompanion.contactsFingerprint();
        if (fingerprint != meshCoreContactsFingerprint) {
            saveMeshCoreContacts();
            meshCoreContactsFingerprint = fingerprint;
            lastMeshCoreContactsSaveMs = millis();
        }
    }
#endif
    return next < MESHCORE_SERVICE_MS ? next : MESHCORE_SERVICE_MS;
}

// The announced MeshCore type follows the Meshtastic role: a router is a repeater, anything else a companion to
// message. A node that relays under a chat identity also needs the alias, so the hop it appends belongs to a repeater
// ADVERT; a repeater-typed identity needs none, and a mute node relays nothing.
void RadioLibInterface::updateMeshCoreRole()
{
    const uint8_t mainType = MeshCoreRepeater::nodeTypeForRole(config.device.role);
    meshCoreRepeater.setNodeType(mainType);
    const bool aliasWasOn = meshCoreRepeater.usesAlias();
    meshCoreRepeater.setAliasEnabled(mainType != MeshCoreRepeater::ADV_TYPE_REPEATER && meshCoreRepeater.canRelay());
    if (!meshCoreRepeater.usesAlias()) {
        meshCoreAliasFloodPending = false;
        meshCoreAliasFloodAttempts = 0;
    } else if (!aliasWasOn && meshCoreFloodAdvertSent) {
        // The alias came on after the flood ADVERT went out and relayed paths already carry its hop: announce it now.
        meshCoreAliasFloodPending = true;
        meshCoreAliasFloodAttempts = 0;
    }
}

void RadioLibInterface::ensureMeshCoreConfigLoaded()
{
    if (meshCoreConfigLoaded)
        return;
    meshCoreConfigLoaded = true;
    reloadMeshCoreConfig();
}

void RadioLibInterface::reloadMeshCoreConfig()
{
    MeshCoreConfig loaded;
    if (!loadMeshCoreConfigImage(&loaded)) {
        LOG_INFO("MeshCore config: no valid %s, settings unchanged", meshCoreConfigPath);
        return;
    }
    meshCoreConfig = loaded;
    meshCoreRepeater.setRelayMode(meshCoreConfig.relay);
    LOG_INFO("MeshCore config: relay %u, advert every %uh", (unsigned)meshCoreConfig.relay,
             (unsigned)meshCoreConfig.effectiveAdvertHours());
}

bool RadioLibInterface::saveMeshCoreConfig()
{
    if (saveMeshCoreConfigImage(meshCoreConfig))
        return true;
    LOG_ERROR("MeshCore config: could not save %s", meshCoreConfigPath);
    return false;
}

void RadioLibInterface::setMeshCoreRelayMode(MeshCoreConfig::RelayMode mode)
{
    ensureMeshCoreConfigLoaded();
    meshCoreConfig.relay = mode;
    meshCoreRepeater.setRelayMode(mode);
}

void RadioLibInterface::setMeshCoreAdvertHours(uint8_t hours)
{
    ensureMeshCoreConfigLoaded();
    meshCoreConfig.advertHours = hours;
}

bool RadioLibInterface::requestMeshCoreAdvertNow()
{
    const uint32_t nowMs = millis();
    if (meshCoreAdvertNowSent && Throttle::isWithinTimespanMs(lastMeshCoreAdvertNowMs, MESHCORE_ADVERT_NOW_MIN_MS))
        return false;
    meshCoreAdvertNowSent = true;
    lastMeshCoreAdvertNowMs = nowMs;
    // The advert thread picks this up on its next step, at most MESHCORE_SERVICE_MS later.
    nextMeshCoreFloodAdvertMs = nowMs;
    return true;
}

uint32_t RadioLibInterface::meshCoreAdvertDueInMs() const
{
    const uint32_t nowMs = millis();
    return Throttle::deadlinePassedAt(nowMs, nextMeshCoreFloodAdvertMs) ? 0 : nextMeshCoreFloodAdvertMs - nowMs;
}

uint32_t RadioLibInterface::meshCoreAdvertTick()
{
    if (!meshCoreRepeater.hasIdentity()) {
        uint8_t seed[MESHCORE_SEED_LEN];
        if (!loadOrCreateMeshCoreSeed(seed, false))
            return MESHCORE_ADVERT_RETRY_MS;
        // A key MeshCore rejects (public key starting 0x00/0xFF, ~1 in 128) is replaced by a fresh one.
        for (int attempt = 0; !meshCoreRepeater.setIdentitySeed(seed); attempt++) {
            if (attempt >= 8 || !loadOrCreateMeshCoreSeed(seed, true))
                return MESHCORE_ADVERT_RETRY_MS;
        }
        const uint8_t *pub = meshCoreRepeater.getIdentityPub();
        LOG_INFO("MeshCore identity ready, pub=%02x%02x%02x%02x...", pub[0], pub[1], pub[2], pub[3]);
        if (meshCoreRepeater.hasAliasIdentity()) {
            // The hash clients show for this node in a path is this prefix, the relay alias.
            const uint8_t *alias = meshCoreRepeater.getAliasPub();
            LOG_INFO("MeshCore relay alias ready, pub=%02x%02x%02x%02x...", alias[0], alias[1], alias[2], alias[3]);
        } else {
            LOG_WARN("MeshCore relay alias could not be derived; relayed paths keep the main key");
        }
    }
    updateMeshCoreRole();

    // One of our frames is queued but not on air yet: nudge the relay thread until it goes. A nudge that finds
    // the relay already scheduled changes nothing, so the next tick can repeat it.
    if (meshCoreRepeater.hasPendingOwnFrame()) {
        wakeMeshCoreRelay();
        return MESHCORE_ADVERT_NUDGE_MS;
    }

    // The alias flood is queued on the tick after the main one, once that frame is on air.
    const bool aliasAdvert = meshCoreRepeater.usesAlias() && meshCoreAliasFloodPending;
    if (!aliasAdvert && !Throttle::deadlinePassed(nextMeshCoreFloodAdvertMs)) {
        const uint32_t nowMs = millis();
        return nextMeshCoreFloodAdvertMs - nowMs;
    }

    // MeshCore clients order adverts by timestamp, so it must be a real time - but any source will do.
    const uint32_t now = getValidTime(RTCQualityDevice);
    if (now == 0) {
        LOG_INFO("MeshCore ADVERT postponed: no valid time yet");
        return MESHCORE_ADVERT_RETRY_MS;
    }

    // MeshCore clients ignore an advert without a name (BaseChatMesh::onAdvertRecv).
    char fallbackName[20];
    const char *name = owner.long_name;
    if (name[0] == 0) {
        snprintf(fallbackName, sizeof(fallbackName), "Meshtastic-%04x", (unsigned)(nodeDB->getNodeNum() & 0xFFFF));
        name = fallbackName;
    }

    // The alias advert keeps the same name with a suffix, so the hop it names reads as this node's relay persona.
    const char *advertName = name;
    char aliasName[MeshCoreRepeater::ADVERT_NAME_MAX + 1];
    if (aliasAdvert) {
        constexpr size_t suffixLen = sizeof(MESHCORE_ALIAS_SUFFIX) - 1;
        size_t keep = strlen(name);
        if (keep > MeshCoreRepeater::ADVERT_NAME_MAX - suffixLen) {
            keep = MeshCoreRepeater::ADVERT_NAME_MAX - suffixLen;
            while (keep > 0 && ((uint8_t)name[keep] & 0xC0) == 0x80)
                keep--; // never split a multi-byte character
        }
        snprintf(aliasName, sizeof(aliasName), "%.*s%s", (int)keep, name, MESHCORE_ALIAS_SUFFIX);
        advertName = aliasName;
    }

    // Flood only: it reaches the direct neighbours too, and every node re-sends it, so it is the only kind worth
    // spending airtime on.
    uint8_t raw[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len = meshCoreRepeater.buildAdvert(raw, sizeof(raw), advertName, now, true, aliasAdvert);
    if (len == 0 || !meshCoreRepeater.queueOwnFrame(raw, len)) {
        LOG_WARN("MeshCore ADVERT could not be built or queued");
        // The alias flood has no deadline of its own: stop retrying it every minute until the next 12 h slot.
        if (aliasAdvert && ++meshCoreAliasFloodAttempts >= MESHCORE_ALIAS_ADVERT_MAX_ATTEMPTS) {
            LOG_WARN("MeshCore relay-alias ADVERT abandoned after %u attempts", (unsigned)meshCoreAliasFloodAttempts);
            meshCoreAliasFloodPending = false;
            meshCoreAliasFloodAttempts = 0;
        }
        return MESHCORE_ADVERT_RETRY_MS;
    }
    LOG_INFO("MeshCore %sflood ADVERT queued (%u bytes, name=%s, ts=%u)", aliasAdvert ? "relay-alias " : "", (unsigned)len,
             advertName, (unsigned)now);
    meshCoreAliasFloodAttempts = 0;
    if (aliasAdvert) {
        meshCoreAliasFloodPending = false;
    } else {
        nextMeshCoreFloodAdvertMs = millis() + (uint32_t)meshCoreConfig.effectiveAdvertHours() * MESHCORE_MS_PER_HOUR;
        meshCoreFloodAdvertSent = true;
        // The alias flood follows as soon as this one is on air.
        meshCoreAliasFloodPending = meshCoreRepeater.usesAlias();
    }
    wakeMeshCoreRelay();
    return MESHCORE_ADVERT_NUDGE_MS;
}

bool RadioLibInterface::isMeshtasticOnOurChannels(size_t length)
{
    // Meshtastic first: a frame that parses as MeshCore may still be a Meshtastic packet between two
    // other nodes. Only the trial decrypt can tell, so it runs on these ambiguous frames alone.
    if (length <= sizeof(PacketHeader))
        return false;
    const PacketHeader &h = radioBuffer.header;
    return decodesOnOurChannels(h.from, h.id, h.channel, radioBuffer.payload, length - sizeof(PacketHeader));
}

MeshCoreRelayThread::MeshCoreRelayThread(RadioLibInterface *radio) : OSThread("MeshCoreRelay"), radio(radio)
{
    disable();
}

void MeshCoreRelayThread::wakeIn(uint32_t ms)
{
    enabled = true;
    setIntervalFromNow(ms);
}

int32_t MeshCoreRelayThread::runOnce()
{
    return radio->meshCoreRelayTick();
}

void RadioLibInterface::wakeMeshCoreRelay()
{
    if (!meshCoreRelayThread || !meshCoreRepeater.hasPendingFrame())
        return;
    const uint32_t dueIn = meshCoreRepeater.relayDueInMs();
    // A frame that is already due is being retried on its own backoff: pulling that forward would defeat it.
    if (dueIn == 0 && meshCoreRelayThread->isArmed())
        return;
    meshCoreRelayThread->wakeIn(dueIn + 1);
}

uint32_t RadioLibInterface::meshCoreRelayTick()
{
    maybeSendMeshCoreFrame();

    if (!meshCoreRepeater.hasPendingFrame())
        return meshCoreRelayThread->disable();
    // On air now: the TX-done event wakes us again; this only covers a TX-done that never comes.
    if (isSending())
        return MESHCORE_TX_POLL_MS;
    // Due but deferred (radio or channel busy): come back after a random backoff. Otherwise at its deadline.
    return meshCoreRepeater.isRelayDue() ? meshCoreRepeater.busyBackoffMs(slotTimeMsec) : meshCoreRepeater.relayDueInMs() + 1;
}

void RadioLibInterface::maybeSendMeshCoreFrame()
{
    if (isSending() || !meshCoreRepeater.hasPendingFrame() || !meshCoreRepeater.isRelayDue()) {
        return;
    }
    // A deferred frame is retried on a backoff; log the reason at most once a second.
    static uint32_t lastDeferLogMs = 0;
    const bool logDefer = !Throttle::isWithinTimespanMs(lastDeferLogMs, 1000);
    // MeshBeacon moves the radio to another preset for its packet: a MeshCore frame sent now would go out on it.
    if (txQueue.getFront() && RadioTxHooks::holdsRadio(txQueue.getFront())) {
        if (logDefer) {
            LOG_DEBUG("MeshCore TX deferred: radio held for a beacon");
            lastDeferLogMs = millis();
        }
        return;
    }
    // A permanent refusal would otherwise retry the same frame forever.
    if (!meshCoreTxAllowed()) {
        meshCoreRepeater.clearPendingFrame();
        meshCoreRepeater.txDropped++;
        return;
    }
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = meshCoreRepeater.takePendingFrame(frame, sizeof(frame)); // checks an ADVERT's signature, radio listening
    if (len == 0) {
        return;
    }
    // Same listen-before-talk as the Meshtastic TX path. A busy radio or channel leaves the frame
    // queued (takePendingFrame copies, not clears) for the next meshCoreRelayTick().
    const bool own = meshCoreRepeater.pendingIsOwn();
    if (!isReceiving || !canSendImmediately()) {
        meshCoreRepeater.deferRadio++;
        if (logDefer) {
            LOG_DEBUG("MeshCore TX deferred: radio busy");
            lastDeferLogMs = millis();
        }
        return;
    }
    if (isChannelActive()) {
        meshCoreRepeater.deferChannel++;
        if (logDefer) {
            LOG_DEBUG("MeshCore TX deferred: channel busy");
            lastDeferLogMs = millis();
        }
        startReceive();
        return;
    }
    // The scan can dispatch a pending radio event (setStandby) that starts a Meshtastic TX: leave that one alone.
    if (isSending())
        return;
    if (!sendRawFrame(frame, len)) {
        startReceive(); // the channel scan left the radio in standby
        return;
    }
    LOG_INFO("MeshCore TX %s: len=%u header=0x%02x path_len=0x%02x", own ? "own" : "relay", (unsigned)len, frame[0], frame[1]);
    if (!own)
        meshCoreRepeater.markRelayed();
    meshCoreRepeater.clearPendingFrame();
}
#else
void RadioLibInterface::maybeSendMeshCoreFrame() {}
size_t RadioLibInterface::formatContentionStats(char *, size_t)
{
    return 0;
}
#endif

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER
void RadioLibInterface::startMeshCoreThreads()
{
    if (meshCoreAdvertThread)
        return;
    // The first flood advert goes out a few minutes after boot, once the identity and the clock are ready.
    nextMeshCoreFloodAdvertMs = millis() + MESHCORE_FLOOD_ADVERT_FIRST_MS;
    meshCoreAdvertThread = new MeshCoreAdvertThread(this);
    meshCoreRelayThread = new MeshCoreRelayThread(this);
}
#endif
