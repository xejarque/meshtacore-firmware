#include "MeshCoreRepeater.h"

#include "Arduino.h"
#include "NodeDB.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "meshUtils.h"
#include <string.h>

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER

// MeshCore Dispatcher::tryParsePacket payload types (see MeshCore src/Packet.h)
#define MC_PAYLOAD_TYPE_REQ 0x00
#define MC_PAYLOAD_TYPE_RESPONSE 0x01
#define MC_PAYLOAD_TYPE_TXT_MSG 0x02
#define MC_PAYLOAD_TYPE_ACK 0x03
#define MC_PAYLOAD_TYPE_ADVERT 0x04
#define MC_PAYLOAD_TYPE_GRP_TXT 0x05
#define MC_PAYLOAD_TYPE_GRP_DATA 0x06
#define MC_PAYLOAD_TYPE_ANON_REQ 0x07
#define MC_PAYLOAD_TYPE_PATH 0x08
#define MC_PAYLOAD_TYPE_TRACE 0x09
#define MC_PAYLOAD_TYPE_MULTIPART 0x0A
#define MC_PAYLOAD_TYPE_CONTROL 0x0B
#define MC_PAYLOAD_TYPE_RAW_CUSTOM 0x0F

// MeshCore route types (Packet.h PH_ROUTE_MASK)
#define MC_ROUTE_TRANSPORT_FLOOD 0x00
#define MC_ROUTE_FLOOD 0x01
#define MC_ROUTE_DIRECT 0x02
#define MC_ROUTE_TRANSPORT_DIRECT 0x03

// MeshCore path_len byte (Packet.h): upper 2 bits = hash_size-1, lower 6 = count. Mode 3 reserved.
#define MC_PATH_MODE_MASK 0xC0
#define MC_PATH_MODE_RESERVED 0xC0
#define MC_PATH_COUNT_MASK 0x3F
#define MC_PATH_SIZE_SHIFT 6

// MeshCore header byte 0: bits 6-7 are the payload version (must be PAYLOAD_VER_1 = 0).
#define MC_PAYLOAD_VER_SHIFT 6

// A full Meshtastic on-air header (to/from/id/flags/channel/next_hop/relay_node) must fit for a
// frame to be Meshtastic at all; spelled out to keep this file free of RadioInterface.h.
#define MC_MESHTASTIC_HEADER_LEN 16

static bool isKnownPayloadType(uint8_t type)
{
    return type == MC_PAYLOAD_TYPE_REQ || type == MC_PAYLOAD_TYPE_RESPONSE || type == MC_PAYLOAD_TYPE_TXT_MSG ||
           type == MC_PAYLOAD_TYPE_ACK || type == MC_PAYLOAD_TYPE_ADVERT || type == MC_PAYLOAD_TYPE_GRP_TXT ||
           type == MC_PAYLOAD_TYPE_GRP_DATA || type == MC_PAYLOAD_TYPE_ANON_REQ || type == MC_PAYLOAD_TYPE_PATH ||
           type == MC_PAYLOAD_TYPE_TRACE || type == MC_PAYLOAD_TYPE_MULTIPART || type == MC_PAYLOAD_TYPE_CONTROL ||
           type == MC_PAYLOAD_TYPE_RAW_CUSTOM;
}

bool MeshCoreRepeater::looksLikeMeshCoreFrame(const uint8_t *raw, size_t len, uint32_t ourNodeNum, FrameInfo *out)
{
    if (!raw || len < 2 || len > MAX_FRAME_LEN)
        return false;

    FrameInfo info;
    if (!parse(raw, len, &info))
        return false;

    // Meshtastic-header veto: bytes 0..3 and 4..7 of a real Meshtastic frame are its to/from
    // NodeNums. A frame addressed to or from us is Meshtastic with near certainty (our NodeNum is
    // crc32 of our public key; a MeshCore header colliding with it is a 2^-32 accident), and it
    // must never be swallowed - or worse, flood-relayed - as foreign traffic. Broadcasts
    // (to == 0xffffffff) never reach here: their first byte fails the payload version check.
    if (ourNodeNum != 0 && len >= MC_MESHTASTIC_HEADER_LEN) {
        uint32_t to = raw[0] | (raw[1] << 8) | (raw[2] << 16) | ((uint32_t)raw[3] << 24);
        uint32_t from = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((uint32_t)raw[7] << 24);
        if (to == ourNodeNum || from == ourNodeNum)
            return false;
    }

    if (out)
        *out = info;
    return true;
}

bool MeshCoreRepeater::parse(const uint8_t *raw, size_t len, FrameInfo *out)
{
    if (!raw || len < 2)
        return false;

    const uint8_t header = raw[0];
    FrameInfo info;
    info.route_type = header & 0x03;
    info.payload_type = (header >> 2) & 0x0F;
    info.payload_ver = (header >> MC_PAYLOAD_VER_SHIFT) & 0x03;

    if (info.payload_ver != 0) // PAYLOAD_VER_1 only
        return false;
    if (!isKnownPayloadType(info.payload_type))
        return false;

    size_t i = 1;
    if (info.route_type == MC_ROUTE_TRANSPORT_FLOOD || info.route_type == MC_ROUTE_TRANSPORT_DIRECT) {
        i += 4; // two uint16 transport codes
    }

    if (i >= len)
        return false;

    const uint8_t path_len_byte = raw[i++];
    if ((path_len_byte & MC_PATH_MODE_MASK) == MC_PATH_MODE_RESERVED) // path mode 3 reserved
        return false;

    info.path_hash_size = (path_len_byte >> MC_PATH_SIZE_SHIFT) + 1;
    info.path_hash_count = path_len_byte & MC_PATH_COUNT_MASK;
    if (info.path_hash_size < 1 || info.path_hash_size > MAX_PATH_HASH_SIZE)
        return false;
    if (info.path_hash_count > MAX_PATH_HASH_COUNT)
        return false;

    size_t pathBytes = (size_t)info.path_hash_count * info.path_hash_size;
    if (pathBytes > MAX_FRAME_LEN || i + pathBytes > len) // path can't be larger than the frame
        return false;
    i += pathBytes;

    info.header_len = (uint8_t)i;
    info.payload_len = (uint8_t)(len - i);

    if (out)
        *out = info;
    return true;
}

void MeshCoreRepeater::handleReceivedFrame(const uint8_t *raw, size_t len, int8_t snr, uint32_t airtimeMs)
{
    (void)snr;       // logged by the caller together with RSSI
    (void)airtimeMs; // used by the relay half below
    FrameInfo info;
    if (!parse(raw, len, &info)) {
        rxDropped++;
        return;
    }

    rxSeen++;

#if MESHTASTIC_ENABLE_MESHCORE_RELAY
    // Only flood-routed frames are relayed (like MeshCore's own repeater). Direct frames carry a
    // path meant for a specific node and would be echoed pointlessly by a transparent repeater.
    if (info.route_type != MC_ROUTE_TRANSPORT_FLOOD && info.route_type != MC_ROUTE_FLOOD)
        return;
    rxFlood++;

    // Safety valve: if the pending relay never got its turn (radio events starved), a flood older
    // than the dupe window is worthless - free the slot for the frame just heard.
    if (hasPendingFrame() && !Throttle::isWithinTimespanMs(pendingRelay.firstSeenMs, DUPE_WINDOW_MS)) {
        clearPendingFrame();
        txDropped++;
    }

    // MeshCore's flood_max_advert default (NodePrefs.h): ADVERT floods are not forwarded past 8
    // hops. The general flood_max (64) is implied by the 6-bit hop count plus the path bound below.
    if (info.payload_type == MC_PAYLOAD_TYPE_ADVERT && info.path_hash_count >= MAX_FLOOD_HOPS_ADVERT) {
        LOG_DEBUG("MeshCore: advert flood past %u hops, drop", info.path_hash_count);
        rxDropped++;
        return;
    }

    // MeshCore appends its own hash to the path on each hop (Mesh::routeRecvPacket) and refuses
    // to forward when the path would overflow MAX_PATH_SIZE (64 bytes). Our relay is transparent
    // and adds no hop, so bound the frame by that same budget: past it, MeshCore nodes could no
    // longer extend the path and the flood would die at the next real hop anyway.
    size_t nextPathBytes = ((size_t)info.path_hash_count + 1) * info.path_hash_size;
    if (nextPathBytes > MAX_PATH_SIZE) {
        LOG_DEBUG("MeshCore: flood path full (%u hops), drop", info.path_hash_count);
        rxDropped++;
        return;
    }

    uint32_t hash = hashFrame(raw, len, info);
    if (wasRecentlySeen(hash)) {
        rxDup++;
        onDupeHeard(hash);
        return;
    }

    if (!canRelay())
        return;

    queueRelay(raw, len, hash, airtimeMs);
#endif
}

uint32_t MeshCoreRepeater::hashFrame(const uint8_t *raw, size_t len, const FrameInfo &info)
{
    // FNV-1a over payload_type + payload ONLY. MeshCore appends a hop hash to the path at every
    // forward (Mesh.cpp::routeRecvPacket), so a full-frame hash would make every path variant of
    // the same packet look new and the relay would re-broadcast each of them. Keying on the
    // payload dedupes variants, like MeshCore's own Packet::calculatePacketHash.
    uint32_t hash = 2166136261u;
    hash = (hash ^ info.payload_type) * 16777619u;
    for (size_t i = info.header_len; i < len; i++) {
        hash = (hash ^ raw[i]) * 16777619u;
    }
    return hash;
}

bool MeshCoreRepeater::wasRecentlySeen(uint32_t hash)
{
    const uint32_t now = Time::getMillis();
    for (size_t i = 0; i < DEDUP_ENTRIES; i++) {
        if (dedup[i].hash == hash && Throttle::isWithinTimespanMs(dedup[i].seenAtMs, DEDUP_WINDOW_MS)) {
            return true;
        }
    }
    dedup[dedupNext].hash = hash;
    dedup[dedupNext].seenAtMs = now;
    dedupNext = (dedupNext + 1) % DEDUP_ENTRIES;
    return false;
}

void MeshCoreRepeater::onDupeHeard(uint32_t hash)
{
    // Someone else is retransmitting the frame we have queued: contention, measured on the air.
    // Postpone our copy (ZephCore's extendPendingRetransmit) and keep the dupe as the contention
    // sample this relay will fold into the estimate.
    if (!hasPendingFrame() || pendingRelay.hash32 != hash)
        return;
    if (!Throttle::isWithinTimespanMs(pendingRelay.firstSeenMs, DUPE_WINDOW_MS))
        return;

    if (pendingRelay.dupeCount < 255)
        pendingRelay.dupeCount++;

    uint32_t perDupe = (uint32_t)(REACTIVE_BACKOFF_AIRTIME * (float)pendingRelay.airtimeMs);
    uint32_t cap = (uint32_t)(REACTIVE_AIRTIME_CAP * (float)pendingRelay.airtimeMs);
    if (cap > REACTIVE_HARD_CAP_MS)
        cap = REACTIVE_HARD_CAP_MS;
    if (pendingRelay.reactiveAddedMs >= cap)
        return;
    uint32_t remaining = cap - pendingRelay.reactiveAddedMs;
    if (perDupe > remaining)
        perDupe = remaining;
    if (perDupe == 0)
        return;

    pendingRelay.reactiveAddedMs += perDupe;
    pendingDeadlineMs += perDupe;
}

bool MeshCoreRepeater::canRelay() const
{
    // Same rebroadcast policy as Meshtastic's own relay (FloodingRouter::isRebroadcaster): a mute
    // node or a node with rebroadcast disabled relays nothing, MeshCore floods included.
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE ||
        config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE)
        return false;

    return !hasPendingFrame(); // one relay at a time; a dropped flood is redundant in a flood mesh
}

void MeshCoreRepeater::queueRelay(const uint8_t *raw, size_t len, uint32_t hash32, uint32_t airtimeMs)
{
    if (len > sizeof(pending))
        return;
    memcpy(pending, raw, len);
    pendingLen = len;

    const uint32_t now = Time::getMillis();
    pendingRelay.hash32 = hash32;
    pendingRelay.firstSeenMs = now;
    pendingRelay.airtimeMs = airtimeMs;
    pendingRelay.reactiveAddedMs = 0;
    pendingRelay.dupeCount = 0;
    pendingDeadlineMs = now + adaptiveDelayMs(airtimeMs);
    lastRelayMs = now;
    txRelayed++;
}

bool MeshCoreRepeater::isRelayDue() const
{
    return hasPendingFrame() && Throttle::deadlinePassed(pendingDeadlineMs);
}

uint32_t MeshCoreRepeater::relayDueInMs() const
{
    if (!hasPendingFrame() || Throttle::deadlinePassed(pendingDeadlineMs))
        return 0;
    return pendingDeadlineMs - Time::getMillis();
}

size_t MeshCoreRepeater::takePendingFrame(uint8_t *buf, size_t bufLen)
{
    if (pendingLen == 0 || bufLen < pendingLen)
        return 0;
    memcpy(buf, pending, pendingLen);
    return pendingLen;
}

void MeshCoreRepeater::clearPendingFrame()
{
    foldContentionSample();
    pendingLen = 0;
}

void MeshCoreRepeater::foldContentionSample()
{
    if (pendingLen == 0)
        return;

    // Same fold as ZephCore ContentionTracker::finalizeEntry: the dupe count of this relay is
    // one contention sample, folded into an EMA (alpha 1/8; fast convergence during warmup).
    uint32_t sampleX256 = (uint32_t)pendingRelay.dupeCount << 8;
    int32_t diff = (int32_t)sampleX256 - (int32_t)emaX256;
    if (finalizedSamples < WARMUP_SAMPLES) {
        if (finalizedSamples == 0)
            emaX256 = sampleX256;
        else
            emaX256 = (uint32_t)((int32_t)emaX256 + (diff >> 1));
    } else {
        emaX256 = (uint32_t)((int32_t)emaX256 + (diff >> EMA_SHIFT));
    }
    finalizedSamples++;
}

void MeshCoreRepeater::applyLazyDecay()
{
    // ZephCore decays its estimate when the mesh goes quiet (STALE_MS), paced by a maintenance
    // tick. Here the same steps are computed lazily from the timestamps: no timers at all.
    const uint32_t now = Time::getMillis();
    if (lastRelayMs == 0 || emaX256 == 0)
        return;
    if (!Throttle::hasElapsed(lastRelayMs, STALE_MS)) {
        lastDecayMs = 0;
        return;
    }
    if (lastDecayMs == 0)
        lastDecayMs = now - DECAY_PERIOD_MS; // arm so the first step lands on this call

    for (uint32_t n = 0; n < MAX_DECAY_CATCHUP && emaX256 != 0; n++) {
        if (!Throttle::hasElapsed(lastDecayMs, DECAY_PERIOD_MS))
            break;
        emaX256 -= emaX256 >> EMA_SHIFT;
        lastDecayMs += DECAY_PERIOD_MS;
    }
}

float MeshCoreRepeater::getFloodDelayFactor()
{
    applyLazyDecay();

    if (finalizedSamples < WARMUP_SAMPLES)
        return 0.5f; // conservative default before the estimate is trusted

    float est = (float)emaX256 / 256.0f;
    if (est <= 0.0f)
        return MIN_FLOOD_FACTOR;

    // Factor rises smoothly from MIN toward MAX as observed contention grows (0.60 at est = 4).
    float factor = MIN_FLOOD_FACTOR + (MAX_FLOOD_FACTOR - MIN_FLOOD_FACTOR) * (est / (est + FLOOD_EST_HALFPOINT));
    if (factor > MAX_FLOOD_FACTOR)
        factor = MAX_FLOOD_FACTOR;
    return factor;
}

uint32_t MeshCoreRepeater::adaptiveDelayMs(uint32_t airtimeMs)
{
    // ZephCore Mesh::computeAdaptiveFloodDelay: 20ms floor (neighbours are back in RX) plus a
    // random jitter of up to 5 x airtime x factor, capped at 6 x airtime and 2000ms.
    float factor = getFloodDelayFactor();
    uint32_t jitter = (uint32_t)(JITTER_AIRTIME_MULT * (float)airtimeMs * factor);
    uint32_t airtimeCap = (uint32_t)(JITTER_AIRTIME_CAP * (float)airtimeMs);
    if (jitter > airtimeCap)
        jitter = airtimeCap;
    if (jitter > JITTER_CAP_MS)
        jitter = JITTER_CAP_MS;
    return DELAY_FLOOR_MS + (uint32_t)random(0, (long)jitter + 1);
}

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
