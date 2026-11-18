// SPDX-License-Identifier: AGPL-3.0-only
//
// Delivery of what we send: direct messages waiting for their ACK, channel texts waiting for a repeater to echo
// them, and how each outcome is reported back to the client.

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
#include "MeshCoreCompanionWire.h"

void MeshCoreCompanion::queueAck(uint32_t hash)
{
    uint8_t frame[8];
    const size_t n = encodeAck(hash, frame, sizeof(frame));
    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (n == 0 || !iface || !iface->queueMeshCoreFrame(frame, n))
        LOG_WARN("MeshCore ACK could not be queued");
}

void MeshCoreCompanion::reportDelivery(uint32_t fromNum, uint32_t packetId, meshtastic_Routing_Error err, int relayNode)
{
    meshtastic_Routing r = meshtastic_Routing_init_default;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = err;

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;
    p->from = fromNum;
    p->to = nodeDB->getNodeNum();
    p->id = generatePacketId();
    p->channel = 0;
    p->priority = meshtastic_MeshPacket_Priority_ACK;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.request_id = packetId;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Routing_msg, &r);
    // The router drops a packet whose hop_start is missing, so the delivery report carries real hop fields (direct).
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    hopFields(0, &p->hop_start, &p->hop_limit);
    if (relayNode >= 0)
        p->relay_node = (uint8_t)relayNode;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    router->enqueueReceivedMessage(p);
}

MeshCoreCompanion::PendingAck *MeshCoreCompanion::addPending(uint32_t ackHash, uint32_t packetId, uint32_t contactNum, bool group)
{
    PendingAck *slot = nullptr;
    for (auto &e : pending) {
        if (!e.used) {
            slot = &e;
            break;
        }
        if (!slot || (uint32_t)(millis() - e.sentMs) > (uint32_t)(millis() - slot->sentMs))
            slot = &e; // the oldest one gives way
    }
    if (slot->used && !slot->relayed)
        reportDelivery(slot->contactNum, slot->packetId, meshtastic_Routing_Error_MAX_RETRANSMIT);

    slot->ackHash = ackHash;
    slot->frameHash = 0;
    slot->packetId = packetId;
    slot->contactNum = contactNum;
    slot->sentMs = millis();
    slot->group = group;
    slot->relayed = false;
    slot->used = true;
    return slot;
}

bool MeshCoreCompanion::trackGroupText(const uint8_t *frame, size_t len, uint32_t packetId, uint8_t channel)
{
    concurrency::RecursiveLockGuard guard(lock);
    MeshCoreRepeater::FrameInfo info;
    if (!frame || !MeshCoreRepeater::parse(frame, len, &info))
        return false;
    const uint32_t hash = MeshCoreRepeater::hashFrame(frame, len, info);
    addPending(hash, packetId, PSEUDO_NODE_NUM, true);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    noteSentText(packetId, hash, nodeDB->getNodeNum(), channel);
#endif
    return true;
}

void MeshCoreCompanion::settleEcho(uint32_t hash)
{
    for (auto &e : pending) {
        if (e.used && e.group && e.ackHash == hash) {
            e.used = false;
            groupEchoed++;
            reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_NONE);
            return;
        }
    }
}

bool MeshCoreCompanion::sendDirectText(uint32_t nodeNum, uint32_t packetId, const char *text, uint32_t now, RadioInterface *iface)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    MeshCoreNode *c = contactByNodeNum(nodeNum);
    const uint8_t *secret = c && iface && now != 0 && text ? secretFor(*c) : nullptr;
    if (!secret)
        return false;

    // The ACK hashes the text as the peer will read it, so cut it the way encodeDirectText does.
    char cut[MAX_TEXT_LEN + 1];
    const size_t cutLen = utf8PrefixLen(text, MAX_TEXT_LEN);
    memcpy(cut, text, cutLen);
    cut[cutLen] = 0;

    const uint32_t timestamp = nextTxTimestamp(now);
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t frameLen =
        encodeDirectText(secret, c->pub[0], identityPub[0], timestamp, TXT_FLAGS_PLAIN, cut, frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    if (frameLen == 0 || !MeshCoreRepeater::parse(frame, frameLen, &info) || !iface->queueMeshCoreFrame(frame, frameLen))
        return false;

    PendingAck *slot = addPending(ackHash(timestamp, TXT_FLAGS_PLAIN, cut, identityPub), packetId, nodeNum);
    slot->frameHash = MeshCoreRepeater::hashFrame(frame, frameLen, info);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    noteSentText(packetId, slot->frameHash, nodeDB->getNodeNum(), HeardPath::DIRECT_CHANNEL);
#endif
    nodes.touch(c);
    nodes.noteDirectMessage(c);
    dmTx++;
    return true;
}

void MeshCoreCompanion::settleAck(uint32_t hash)
{
    for (auto &e : pending) {
        if (e.used && !e.group && e.ackHash == hash) {
            e.used = false;
            dmAcked++;
            reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_NONE);
            return;
        }
    }
}

void MeshCoreCompanion::settleRelay(uint32_t hash, uint8_t relayHop)
{
    for (auto &e : pending) {
        if (e.used && !e.group && !e.relayed && e.frameHash == hash) {
            e.relayed = true;
            reportDelivery(nodeDB->getNodeNum(), e.packetId, meshtastic_Routing_Error_NONE, relayHop);
            return;
        }
    }
}

void MeshCoreCompanion::handlePathAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    // PATH payload: dest hash (1), source hash (1), MAC (2), ciphertext. Only ours, from a contact we hold a secret for.
    if (!identityReady || info.header_len >= len || info.payload_len < 1 + 1 + 2 + 16)
        return;
    const uint8_t *payload = &raw[info.header_len];
    if (payload[0] != identityPub[0])
        return;

    MeshCoreNode *candidates[HASH_CANDIDATES_MAX];
    const size_t n = nodes.findByPrefix(&payload[1], 1, candidates, HASH_CANDIDATES_MAX);
    for (size_t i = 0; i < n && i < HASH_CANDIDATES_MAX; i++) {
        uint32_t hash = 0;
        const uint8_t *secret = candidates[i]->type == MeshCoreNode::CHAT ? secretFor(*candidates[i]) : nullptr;
        if (secret && decodePathAck(secret, raw, len, info, &hash)) {
            settleAck(hash);
            return;
        }
    }
}

void MeshCoreCompanion::handleAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    if (info.payload_len != 4 || info.header_len + 4 > len)
        return;
    settleAck(readLe32(&raw[info.header_len]));
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
