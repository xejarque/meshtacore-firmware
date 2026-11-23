// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE for the full license text.
#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "MeshCoreProtocol.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "PingReport.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh-pb-constants.h"
#include "MeshCoreCompanionWire.h"
#include "meshUtils.h"
#include <AES.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <SHA512.h>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static MeshCoreCompanion *activeMeshCoreCompanion = &meshCoreCompanion;

static bool meshCoreAuthPredicate(const meshtastic_MeshPacket &p)
{
    MeshCoreCompanion *c = activeMeshCoreCompanion ? activeMeshCoreCompanion : &meshCoreCompanion;
    return c->injectedDirectText(p);
}

MeshCoreCompanion::MeshCoreCompanion()
{
    activeMeshCoreCompanion = this;
    setDecodedAuthPredicate(meshCoreAuthPredicate);
}

MeshCoreCompanion::~MeshCoreCompanion()
{
    if (activeMeshCoreCompanion == this)
        activeMeshCoreCompanion = &meshCoreCompanion;
}

MeshCoreCompanion meshCoreCompanion;

uint32_t MeshCoreCompanion::nextTxTimestamp(uint32_t now)
{
    lastTxTimestamp = now > lastTxTimestamp ? now : lastTxTimestamp + 1;
    return lastTxTimestamp;
}

bool MeshCoreCompanion::cachedChannelHash(ChannelIndex ch, const uint8_t *key, size_t keyLen, uint8_t *out)
{
    if (ch >= MAX_NUM_CHANNELS)
        return channelHash(key, keyLen, out);

    HashCacheEntry &entry = hashCache[ch];
    if (entry.keyLen != keyLen || memcmp(entry.key, key, keyLen) != 0) {
        if (!channelHash(key, keyLen, &entry.hash)) {
            entry.keyLen = 0;
            return false;
        }
        memcpy(entry.key, key, keyLen);
        entry.keyLen = keyLen;
    }
    *out = entry.hash;
    return true;
}

void MeshCoreCompanion::handleFrame(const uint8_t *raw, size_t len, float snr, int32_t rssi, float freqErr)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    if (!raw || !router)
        return;

    MeshCoreRepeater::FrameInfo info;
    if (!MeshCoreRepeater::parse(raw, len, &info))
        return;

#if MESHTASTIC_MESHCORE_PATH_COMMAND
    const size_t pathBytes = (size_t)info.path_hash_count * info.path_hash_size;
    if (pathBytes > 0 && pathBytes <= info.header_len && info.header_len <= len) {
        const size_t pathStart = info.header_len - pathBytes;
        for (size_t h = 0; h < info.path_hash_count; h++) {
            if (isDropped(&raw[pathStart + h * info.path_hash_size], info.path_hash_size))
                return;
        }
    }
    if (info.payload_type == MC_PAYLOAD_TYPE_ADVERT && len >= info.header_len + 4) {
        if (isDropped(&raw[info.header_len], 4))
            return;
    }
    if (info.payload_type == MC_PAYLOAD_TYPE_TXT_MSG && len >= info.header_len + 2) {
        if (isDropped(&raw[info.header_len + 1], 1))
            return;
    }
#endif

    switch (info.payload_type) {
    case MC_PAYLOAD_TYPE_GRP_TXT:
        handleGroupText(raw, len, info, snr, rssi, freqErr);
        break;
    case MC_PAYLOAD_TYPE_ADVERT:
        handleAdvert(raw, len, info, snr, rssi);
        break;
    case MC_PAYLOAD_TYPE_TXT_MSG:
        handleDirectText(raw, len, info, snr, rssi, freqErr);
        break;
    case MC_PAYLOAD_TYPE_ACK:
        handleAck(raw, len, info);
        break;
    case MC_PAYLOAD_TYPE_PATH: // how MeshCore answers a flood, with the ACK inside
        handlePathAck(raw, len, info);
        break;
    case MC_PAYLOAD_TYPE_TRACE:
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        handleTraceFrame(raw, len, info, snr, rssi);
#endif
        break;
    default:
        break;
    }
}

MeshCoreRx MeshCoreCompanion::storedRoute(const MeshCoreNode &n)
{
    if (n.hops() != MeshCoreNode::HOPS_UNKNOWN)
        return MeshCoreRx(n.hops(), n.snr, n.rssi);
    if (n.hasOutPath())
        return MeshCoreRx::unmeasured(n.outPathLen);
    return MeshCoreRx::unmeasured(MeshCoreRx().pathHashCount);
}

void MeshCoreCompanion::handleGroupText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                        int32_t rssi, float freqErr)
{
    rxSeen++;
    if (info.header_len >= len || info.payload_len < 1 + 2 + 16) {
        rxIgnored++;
        return;
    }
    const uint32_t hash = MeshCoreRepeater::hashFrame(raw, len, info);
    // A repeater re-flooding a text of ours (it carries at least its own hash in the path) is the proof it left the room.
    if (info.path_hash_count > 0)
        settleEcho(hash);
    if (wasRecentlySeen(hash)) {
        rxDup++;
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        // Another copy of the same text: record its route (or refresh it) for ".path".
        noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type, snr, rssi);
#endif
        return;
    }

    for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
        uint8_t key[32];
        const size_t keyLen = channels.getMeshCoreKey(i, key);
        if (keyLen == 0)
            continue;

        uint8_t hash8;
        if (!cachedChannelHash(i, key, keyLen, &hash8) || raw[info.header_len] != hash8)
            continue; // not this channel: skip the HMAC and AES work

        char text[MAX_TEXT_LEN + 1];
        uint32_t timestamp = 0;
        if (!decodeGroupText(key, keyLen, raw, len, info, text, sizeof(text), &timestamp))
            continue;

        // Texts read "name: body": the sender gets its own node and the client shows the body alone. Anything
        // without a prefix falls back to the generic "MeshCore" node.
        char sender[sizeof(((meshtastic_User *)nullptr)->long_name)];
        const char *body = text;
        uint32_t fromNum = PSEUDO_NODE_NUM;
        // How the injected text says it arrived: this frame's route, unless it is pinned to a contact below.
        MeshCoreRx textRx(info.path_hash_count, snr, rssi);
        if (splitSender(text, sender, sizeof(sender), &body)) {
            // Someone we can message shows up as that one node in channels too, instead of a second, unmessagable one.
            // The sender is announced before the text, and both wait in the router queue in that order: the client has the
            // name when the text arrives, with or without an advert from them.
            const MeshCoreRx rx(info.path_hash_count, snr, rssi);
            const MeshCoreNode *contact = nodes.findUniqueByName(sender, MeshCoreNode::CHAT);
            if (contact) {
                // The "name: " prefix is not signed: anyone holding the channel key can write it. The text still lands on
                // the contact's node (one node per person), but it proves nothing about the contact: the store keeps the
                // route, signal and recency the signed advert or the MAC-checked direct messages gave it, and the injected
                // packet carries that stored route instead of this frame's.
                fromNum = contact->nodeNum;
                textRx = storedRoute(*contact);
                ensureSenderNode(fromNum, contact->name, true, contact->pub, meshtastic_Config_DeviceConfig_Role_CLIENT,
                                 textRx);
            } else {
                fromNum = senderNodeNum(sender);
                // Two contacts sharing this name leave it ambiguous: the number may belong to one of them, who stays as is.
                if (!nodes.findByNodeNum(fromNum))
                    ensureSenderNode(fromNum, sender, false, nullptr, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
            }
        } else {
            ensurePseudoNode(); // a node-DB reset or eviction may have removed it since boot
        }

        // Looked up before this text is noted, so a text can never quote itself.
        const uint32_t replyId = replyTarget(body, nullptr);
        const uint32_t packetId =
            injectText(fromNum, NODENUM_BROADCAST, i, body, replyId, false, nullptr, textRx.pathHashCount,
                       textRx.hasSignal ? textRx.snr : 0.0f, textRx.hasSignal ? textRx.rssi : 0, freqErr);
        if (!packetId)
            return;

        rememberFrame(hash);
        rxDecoded++;
        if (fromNum != PSEUDO_NODE_NUM)
            noteRecentText(packetId, sender);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        noteHeardRoute(packetId, hash, fromNum, i, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type,
                       snr, rssi);
#endif
        LOG_INFO("MeshCore group text ch=%u ts=%u len=%u", (unsigned)i, timestamp, (unsigned)strlen(body));
        return;
    }

    rxIgnored++;
}

void MeshCoreCompanion::handleAdvert(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                     int32_t rssi)
{
    // ADVERT payload: public key (32), timestamp (4), signature (64), then app data: a flags byte (node type in the
    // low nibble, 0x10 position, 0x20/0x40 reserved features, 0x80 name) followed by those fields in that order.
    constexpr size_t appDataAt = 32 + 4 + 64;
    if (info.header_len >= len || info.payload_len <= appDataAt)
        return;

    const uint8_t *payload = &raw[info.header_len];
    if ((identityReady && memcmp(payload, identityPub, 32) == 0) || (aliasReady && memcmp(payload, aliasPub, 32) == 0))
        return; // our own advert coming back, either identity

    const uint8_t flags = payload[appDataAt];
    const uint8_t type = flags & 0x0F;
    const bool repeater = type == MeshCoreRepeater::ADV_TYPE_REPEATER;
    if (type != MeshCoreRepeater::ADV_TYPE_CHAT && !repeater)
        return; // only named chat nodes (to message) and repeaters (to list): rooms and sensors are not shown

    size_t at = appDataAt + 1;
    int32_t latMicro = 0, lonMicro = 0;
    if (flags & 0x10) {
        if (at + 8 > info.payload_len)
            return;
        latMicro = (int32_t)readLe32(&payload[at]);
        lonMicro = (int32_t)readLe32(&payload[at + 4]);
        at += 8;
        if (!isValidPositionMicro(latMicro, lonMicro))
            latMicro = lonMicro = 0; // a bogus position is dropped: x10 into Meshtastic units would overflow int32
    }
    if (flags & 0x20)
        at += 2;
    if (flags & 0x40)
        at += 2;

    char name[MeshCoreNode::NAME_LEN] = {0};
    if (flags & 0x80) {
        if (at < info.payload_len) {
            size_t nameLen = info.payload_len - at;
            if (nameLen > sizeof(name) - 1)
                nameLen = sizeof(name) - 1;
            memcpy(name, &payload[at], nameLen);
            name[nameLen] = 0;
            sanitizeUtf8(name, sizeof(name));
        }
    }
    if (name[0] == 0) {
        if (repeater) {
            snprintf(name, sizeof(name), "RPT-%02X%02X", payload[0], payload[1]);
        } else {
            return;
        }
    }

    const MeshCoreRx rx(info.path_hash_count, snr, rssi);
    const uint32_t advertTime = readLe32(&payload[32]);
    // The same advert flood reaches us once per route. A copy that is no closer than the one already taken adds nothing, and
    // would overwrite the better hops/SNR, re-announce the node and repeat the signature check.
    auto isWorseCopy = [&](const MeshCoreNode *heard) {
        return heard && advertTime != 0 && heard->lastAdvert == advertTime && info.path_hash_count >= heard->hops();
    };
    if (repeater) {
        // A repeater is not someone to message: numbered from its key, announced unmessagable, and remembered only as far as
        // a path needs. A key prefix and a short name prove nothing about who sent the advert, so every one is verified.
        const uint32_t frameHash = MeshCoreRepeater::hashFrame(raw, len, info);
        const MeshCoreRepeaterName *heard = nullptr;
        const bool known = repeaters.findByPrefix(payload, MeshCoreRepeaterName::PREFIX_LEN, &heard, 1) > 0;
        // The same advert flood reaches us once per route. A copy no closer than the one taken adds nothing, and would
        // overwrite the better hops/SNR, re-announce the node and repeat the signature check.
        if (known && wasRecentlySeen(frameHash) && info.path_hash_count >= heard->hops())
            return;
        const uint32_t num = MeshCoreNodeStore<1>::nodeNumForKey(payload);
        if (known) {
            // Only a prefix of the key is kept, so a key that merely starts the same (cheap to grind for 3 bytes) must not take
            // the entry over: the first key to hold a prefix keeps it, and the other is not remembered as a repeater.
            if (heard->nodeNum != 0 && heard->nodeNum != num)
                return;
            // MeshCore drops an advert older than the last one it took from that key as a possible replay.
            if (heard->lastAdvert != 0 && advertTime < heard->lastAdvert)
                return;
        }
        if (!advertProven(raw, len, info, snr, rssi))
            return;
        rememberFrame(frameHash);
        learnRepeater(payload, name, snr, info.path_hash_count, advertTime);
        const MeshCoreNode *owner = nodes.findByNodeNum(num);
        if (num == PSEUDO_NODE_NUM || (owner && memcmp(owner->pub, payload, 32) != 0))
            return; // a contact already has this number: the repeater is remembered, but not announced over it
        ensureSenderNode(num, name, false, payload, meshtastic_Config_DeviceConfig_Role_REPEATER, rx);
        updatePosition(num, latMicro, lonMicro, rx);
        return;
    }

    MeshCoreNode *known = contactByPub(payload);
    if (isWorseCopy(known))
        return;
    // MeshCore drops an advert older than the last one it took from that key as a possible replay.
    if (known && advertTime < known->lastAdvert)
        return;
    // The key and name are public, so even a known contact's advert must be signed: otherwise anyone could move it on
    // the map or fake its route. Later copies of the same advert are answered from the verified cache.
    if (!advertProven(raw, len, info, snr, rssi))
        return;
    if (known && strncmp(known->name, name, MeshCoreNode::NAME_LEN - 1) == 0) {
        nodes.touch(known);
        noteAdvert(known, advertTime, latMicro, lonMicro, snr, rssi, info.path_hash_count);
        // Brings a node made before keys were set up along.
        ensureSenderNode(known->nodeNum, known->name, true, known->pub, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
        updatePosition(known->nodeNum, latMicro, lonMicro, rx);
        return;
    }

    MeshCoreNode *c = learnContact(payload, name);
    noteAdvert(c, advertTime, latMicro, lonMicro, snr, rssi, info.path_hash_count);
    if (!known)
        contactsLearned++;
    ensureSenderNode(c->nodeNum, c->name, true, c->pub, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
    retireGroupNode(c->name);
    retireKeyedNode(c->pub, c->nodeNum);
    updatePosition(c->nodeNum, latMicro, lonMicro, rx);
}

bool MeshCoreCompanion::advertProven(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                     int32_t rssi)
{
    switch (MeshCoreRepeater::cachedAdvertVerdict(&raw[info.header_len], info.payload_len)) {
    case MeshCoreRepeater::AdvertVerdict::GENUINE:
        return true;
    case MeshCoreRepeater::AdvertVerdict::FORGED:
        return false;
    case MeshCoreRepeater::AdvertVerdict::UNKNOWN:
        break;
    }
    deferredAdverts.hold(raw, len, info, snr, rssi);
    return false;
}

void MeshCoreCompanion::verifyDeferredAdvert()
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    MeshCoreDeferredAdverts::Entry advert;
    if (!deferredAdverts.takeOldest(advert))
        return;
    MeshCoreRepeater::FrameInfo info;
    if (!MeshCoreRepeater::parse(advert.frame, advert.len, &info))
        return;
    // Fills the signature cache, so handleAdvert() now finds its verdict there and runs no second check.
    if (MeshCoreRepeater::verifyAdvert(&advert.frame[info.header_len], info.payload_len))
        handleAdvert(advert.frame, advert.len, info, advert.snr, advert.rssi);
}

uint32_t MeshCoreCompanion::injectText(uint32_t fromNum, uint32_t to, uint8_t channel, const char *text, uint32_t replyId,
                                       bool direct, const MeshCoreNode *contact, uint8_t pathHashCount, float snr, int32_t rssi,
                                       float freqErr)
{
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return 0;

    p->from = fromNum;
    p->to = to;
    p->id = generatePacketId();
    p->channel = channel;
    if (direct && contact) {
        // A direct message is written from the contact's PKI chat: the client opens that one for a node that carries a
        // public key, and every MeshCore contact is announced with the key its ADVERT signed. Mark the reply the same
        // way, or it lands in a second chat with the same name and the user cannot answer the one they are reading.
        p->pki_encrypted = true;
        p->public_key.size = 32;
        memcpy(p->public_key.bytes, contact->pub, 32);
        InjectedDirect &d = injectedDirect[injectedDirectNext];
        injectedDirectNext = (injectedDirectNext + 1) % INJECTED_DIRECTS;
        d.from = fromNum;
        d.id = p->id;
    }
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    // A quoted reply carries the answered packet id, and keeps its "@[name] " tag in the body the way MeshCore shows it.
    if (replyId)
        p->decoded.reply_id = replyId;
    const size_t textLen = strlen(text);
    p->decoded.payload.size = textLen <= sizeof(p->decoded.payload.bytes) ? textLen : sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, p->decoded.payload.size);
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    // Hops are the repeaters the frame crossed (its path). A hop_limit below hop_start also keeps this
    // synthetic packet's relay gate closed, and a next hop that is not us closes it for direct frames.
    hopFields(pathHashCount, &p->hop_start, &p->hop_limit);
    if (!direct && nodeDB) {
        const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
        p->next_hop = (uint8_t)(ourRelay + 1);
        if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
            p->next_hop = 1;
    }
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    // The frequency error the radio measured for this frame, so PPING's Δf is the real one. Set for every frame, direct
    // or relayed: the metrics line reports it either way.
    p->rx_frequency_error = freqErr;
    // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
    if (pathHashCount == 0) {
        p->rx_snr = snr;
        p->has_rx_rssi = true;
        p->rx_rssi = rssi;
    }
    const uint32_t id = p->id;
    router->enqueueReceivedMessage(p);
    return id;
}

uint32_t MeshCoreCompanion::injectDirectText(const MeshCoreNode &c, const char *text, const MeshCoreRepeater::FrameInfo &info,
                                             float snr, int32_t rssi, float freqErr)
{
    // A node-DB reset or eviction may have removed it.
    ensureSenderNode(c.nodeNum, c.name, true, c.pub, meshtastic_Config_DeviceConfig_Role_CLIENT,
                     MeshCoreRx(info.path_hash_count, snr, rssi));
    return injectText(c.nodeNum, nodeDB->getNodeNum(), 0, text, 0, true, &c, info.path_hash_count, snr, rssi, freqErr);
}

void MeshCoreCompanion::handleDirectText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                         int32_t rssi, float freqErr)
{
    // TXT_MSG payload: dest hash (1), source hash (1), MAC (2), ciphertext.
    if (!identityReady || info.header_len >= len || info.payload_len < 1 + 1 + 2 + 16)
        return;
    const uint32_t hash = MeshCoreRepeater::hashFrame(raw, len, info);
    // A repeater re-flooding our own direct message: the last hop on the path is the repeater that carried it.
    if (info.path_hash_count > 0)
        settleRelay(hash, raw[info.header_len - info.path_hash_size]);
    const uint8_t *payload = &raw[info.header_len];
    if (payload[0] != identityPub[0]) {
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        // A re-flood of a direct message we sent is still a route to it, even though it is not for us.
        if (info.path_hash_count > 0)
            noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type, snr,
                           rssi);
#endif
        return; // not for us: the repeater relays it, we cannot read it anyway
    }

    if (wasRecentlySeen(hash)) {
        rxDup++;
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type, snr, rssi);
#endif
        return;
    }

    // The source hash is one byte, so several contacts can share it: the MAC tells which one wrote it.
    MeshCoreNode *candidates[HASH_CANDIDATES_MAX];
    const size_t n = nodes.findByPrefix(&payload[1], 1, candidates, HASH_CANDIDATES_MAX);
    for (size_t i = 0; i < n && i < HASH_CANDIDATES_MAX; i++) {
        MeshCoreNode *c = candidates[i];
        char text[MAX_TEXT_LEN + 1];
        uint32_t timestamp = 0;
        uint8_t flags = 0;
        const uint8_t *secret = c->type == MeshCoreNode::CHAT ? secretFor(*c) : nullptr;
        if (!secret || !decodeDirectText(secret, raw, len, info, text, sizeof(text), &timestamp, &flags))
            continue;

        rememberFrame(hash);
        c->setHops(info.path_hash_count);
        c->snr = snr;
        c->rssi = rssi;
        c->lastHeard = getTime(false);
        nodes.touch(c);
        nodes.noteDirectMessage(c);
        dmRx++;
        LOG_INFO("MeshCore direct text from %s ts=%u len=%u", c->name, timestamp, (unsigned)strlen(text));
        const uint32_t packetId = injectDirectText(*c, text, info, snr, rssi, freqErr);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        if (packetId)
            noteHeardRoute(packetId, hash, c->nodeNum, HeardPath::DIRECT_CHANNEL, pathOf(raw, info), info.path_hash_size,
                           info.path_hash_count, info.route_type, snr, rssi);
#endif
        queueAck(ackHash(timestamp, flags, text, c->pub));
        return;
    }
}

void MeshCoreCompanion::service()
{
    concurrency::RecursiveLockGuard guard(lock);
    verifyDeferredAdvert();
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    checkTraceTimeout();
#endif
    for (auto &e : pending) {
        if (e.used && !Throttle::isWithinTimespanMs(e.sentMs, ACK_TIMEOUT_MS)) {
            e.used = false;
            (e.group ? groupTimedOut : dmTimedOut)++;
            // A relayed message stops here without a failure, as a Meshtastic one does once its relay has ACKed it.
            if (!e.relayed)
                reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_MAX_RETRANSMIT);
        }
    }
}

void MeshCoreCompanion::noteTransmitted(const uint8_t *raw, size_t len)
{
    concurrency::RecursiveLockGuard guard(lock);
    MeshCoreRepeater::FrameInfo info;
    if (raw && MeshCoreRepeater::parse(raw, len, &info))
        rememberFrame(MeshCoreRepeater::hashFrame(raw, len, info));
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
