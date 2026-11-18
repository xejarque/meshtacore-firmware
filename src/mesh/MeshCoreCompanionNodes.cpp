// SPDX-License-Identifier: AGPL-3.0-only
//
// The node side of the companion: our identity, the contacts and repeaters it learns, how each is announced to the
// client through the node DB, and how the store is kept in step with that node DB and saved to flash.

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

/// A packet that tells the client about a MeshCore node, as if heard from it now. Null when the pool is empty.
static meshtastic_MeshPacket *localAnnouncement(uint32_t nodeNum, meshtastic_PortNum port, const pb_msgdesc_t *fields,
                                                const void *message, const MeshCoreRx &rx)
{
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return nullptr;
    p->from = nodeNum;
    p->to = NODENUM_BROADCAST;
    p->id = generatePacketId();
    p->channel = 0;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = port;
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), fields, message);
    // Local news for the client, never to be repeated over Meshtastic: a next hop that is not us closes the relay gate. The
    // router drops a packet whose hop_start is missing, so it carries the hop fields of the frame it announces.
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    MeshCoreCompanion::hopFields(rx.pathHashCount, &p->hop_start, &p->hop_limit);
    const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    p->next_hop = (uint8_t)(ourRelay + 1);
    if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
        p->next_hop = 1;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
    if (rx.pathHashCount == 0 && rx.hasSignal) {
        p->rx_snr = rx.snr;
        p->has_rx_rssi = true;
        p->rx_rssi = rx.rssi;
    }
    return p;
}

void MeshCoreCompanion::announceUser(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx)
{
    if (!router || !nodeDB)
        return;
    if (meshtastic_MeshPacket *p = localAnnouncement(nodeNum, meshtastic_PortNum_NODEINFO_APP, &meshtastic_User_msg, &user, rx))
        router->enqueueReceivedMessage(p);
}

void MeshCoreCompanion::announceIfNeeded(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx)
{
    if (nodeDB && needsAnnounce(nodeDB->getMeshNode(nodeNum), user, getValidTime(RTCQualityFromNet), &rx))
        announceUser(nodeNum, user, rx);
}

void MeshCoreCompanion::announcePosition(uint32_t nodeNum, int32_t latitudeI, int32_t longitudeI, const MeshCoreRx &rx)
{
    if (!router || !nodeDB)
        return;
    meshtastic_Position pos = meshtastic_Position_init_default;
    pos.has_latitude_i = true;
    pos.latitude_i = latitudeI;
    pos.has_longitude_i = true;
    pos.longitude_i = longitudeI;
    pos.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    pos.time = getValidTime(RTCQualityFromNet);
    if (meshtastic_MeshPacket *p = localAnnouncement(nodeNum, meshtastic_PortNum_POSITION_APP, &meshtastic_Position_msg, &pos, rx))
        router->enqueueReceivedMessage(p);
}

void MeshCoreCompanion::updatePosition(uint32_t nodeNum, int32_t latMicro, int32_t lonMicro, const MeshCoreRx &rx)
{
    if (!nodeDB || (latMicro == 0 && lonMicro == 0))
        return;
    // MeshCore sends degrees x 1e6, Meshtastic keeps 1e-7.
    const int32_t latI = latMicro * 10, lonI = lonMicro * 10;
    meshtastic_PositionLite known;
    if (nodeDB->copyNodePosition(nodeNum, known) && known.latitude_i == latI && known.longitude_i == lonI)
        return; // adverts repeat every few minutes: only a move is news
    announcePosition(nodeNum, latI, lonI, rx);
}

void MeshCoreCompanion::ensurePseudoNode()
{
    meshtastic_User user = {};
    strncpy(user.long_name, SYSTEM_BOT_LONG_NAME, sizeof(user.long_name) - 1);
    strncpy(user.short_name, SYSTEM_BOT_SHORT_NAME, sizeof(user.short_name) - 1);
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    user.has_is_unmessagable = true;
    user.is_unmessagable = true;
    announceIfNeeded(PSEUDO_NODE_NUM, user, MeshCoreRx());
}

bool MeshCoreCompanion::splitSender(const char *text, char *nameOut, size_t nameCap, const char **body)
{
    if (!text || !nameOut || nameCap == 0 || !body)
        return false;
    const char *sep = strstr(text, ": ");
    if (!sep || sep == text)
        return false;

    const size_t nameLen = sep - text;
    char name[MAX_TEXT_LEN + 1];
    memcpy(name, text, nameLen);
    name[nameLen] = 0;
    const size_t keep = utf8PrefixLen(name, nameCap - 1);
    memcpy(nameOut, name, keep);
    nameOut[keep] = 0;
    *body = sep + 2;
    return true;
}

uint32_t MeshCoreCompanion::senderNodeNum(const char *name)
{
    const uint32_t num = MeshCoreNodeStore<1>::nodeNumForName(name);
    return num == PSEUDO_NODE_NUM ? num + 1 : num;
}

meshtastic_User MeshCoreCompanion::senderUser(const char *name, bool messagable, const uint8_t *publicKey,
                                              meshtastic_Config_DeviceConfig_Role role)
{
    // A Meshtacore node shows up twice, as a Meshtastic node and as a MeshCore one with the same name: the suffix tells them
    // apart.
    constexpr char suffix[] = " (MC)";
    char display[sizeof(((meshtastic_User *)nullptr)->long_name)];
    const size_t nameLen =
        utf8PrefixLen(name, sizeof(((meshtastic_NodeInfoLite *)nullptr)->long_name) - 1 - (sizeof(suffix) - 1));
    memcpy(display, name, nameLen);
    memcpy(&display[nameLen], suffix, sizeof(suffix));

    meshtastic_User user = {};
    strncpy(user.long_name, display, sizeof(user.long_name) - 1);
    memcpy(user.short_name, name, utf8PrefixLen(name, 4));
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = role;
    user.has_is_unmessagable = true;
    user.is_unmessagable = !messagable;
    if (publicKey) {
        user.public_key.size = 32;
        memcpy(user.public_key.bytes, publicKey, 32);
    }
    return user;
}

bool MeshCoreCompanion::needsAnnounce(const meshtastic_NodeInfoLite *lite, const meshtastic_User &user, uint32_t now,
                                      const MeshCoreRx *rx)
{
    // Only a packet makes a node heard, so one that is missing or was never heard over the radio gets that packet.
    if (!lite || !nodeInfoLiteHasUser(lite) || !nodeInfoLiteHasRfHear(lite))
        return true;
    if (strncmp(lite->long_name, user.long_name, sizeof(lite->long_name) - 1) != 0 ||
        nodeInfoLiteIsUnmessagable(lite) != user.is_unmessagable || lite->role != user.role ||
        lite->public_key.size != user.public_key.size ||
        memcmp(lite->public_key.bytes, user.public_key.bytes, user.public_key.size) != 0)
        return true;
    if (rx) {
        // The NodeDB keeps hops_away as hop_start - hop_limit (3-bit Meshtastic fields), so it tops out at HOP_START while a
        // MeshCore path can be much longer. Compare what hopFields stores, or every frame past HOP_START hops re-announces.
        const uint8_t storedHops = rx->pathHashCount < HOP_START ? rx->pathHashCount : HOP_START;
        if (!lite->has_hops_away || lite->hops_away != storedHops)
            return true;
        // A missing SNR is news; a changed one only past the jitter band, else a fraction of a dB would re-announce every frame.
        if (rx->pathHashCount == 0 && rx->hasSignal &&
            (!nodeInfoLiteHasSnr(lite) || fabsf(lite->snr - rx->snr) >= SNR_ANNOUNCE_DELTA_DB))
            return true;
    }
    // Like any packet from a Meshtastic node, a frame moves its last-heard on; spaced out so repeated adverts do not flood the
    // client.
    return now > lite->last_heard && lite->last_heard != 0 && now - lite->last_heard >= HEARD_REFRESH_SECS;
}

void MeshCoreCompanion::ensureSenderNode(uint32_t nodeNum, const char *name, bool messagable, const uint8_t *publicKey,
                                         meshtastic_Config_DeviceConfig_Role role, const MeshCoreRx &rx)
{
    if (!nodeDB)
        return;
    meshtastic_NodeInfoLite *lite = nodeDB->getMeshNode(nodeNum);
    // A real Meshtastic node that happens to share this number reports a hardware model: never rename it. An ignored one is
    // dropped by the router, so announcing it would never get it heard.
    if (lite && ((nodeInfoLiteHasUser(lite) && lite->hw_model != meshtastic_HardwareModel_UNSET) || nodeInfoLiteIsIgnored(lite)))
        return;
    // A keyed peer lives in the node store too: its node DB copy may go first when the DB is full. The node is born from the
    // announce below, so a new one gets the mark on its next frame, or on the next save sweep.
    if (lite)
        nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_IS_MESHCORE_MASK, true);

    announceIfNeeded(nodeNum, senderUser(name, messagable, publicKey, role), rx);
}

void MeshCoreCompanion::hopFields(uint8_t pathHashCount, uint8_t *hopStart, uint8_t *hopLimit)
{
    const uint8_t hops = pathHashCount < HOP_START ? pathHashCount : HOP_START;
    *hopStart = HOP_START; // nonzero: a zero hop_start makes clients show '?' instead of the hop count
    *hopLimit = HOP_START - hops;
}

void MeshCoreCompanion::setIdentity(const uint8_t seed[32], const uint8_t pub[32])
{
    concurrency::RecursiveLockGuard guard(lock);
    memcpy(identitySeed, seed, sizeof(identitySeed));
    memcpy(identityPub, pub, sizeof(identityPub));
    identityReady = true;
    aliasReady = false; // a new seed means a new alias; the radio installs the derived one on its next service tick
    secrets.clear();    // they were derived for another identity
}

void MeshCoreCompanion::setRelayAliasPub(const uint8_t pub[32])
{
    concurrency::RecursiveLockGuard guard(lock);
    memcpy(aliasPub, pub, sizeof(aliasPub));
    aliasReady = true;
}

const uint8_t *MeshCoreCompanion::secretFor(const MeshCoreNode &n)
{
    if (!identityReady)
        return nullptr;
    if (const uint8_t *cached = secrets.find(n.pub))
        return cached;
    uint8_t secret[32];
    return sharedSecret(identitySeed, n.pub, secret) ? secrets.put(n.pub, secret) : nullptr;
}

MeshCoreNode *MeshCoreCompanion::contactByPub(const uint8_t pub[32])
{
    MeshCoreNode *n = nodes.find(pub);
    return n && n->type == MeshCoreNode::CHAT ? n : nullptr;
}

MeshCoreNode *MeshCoreCompanion::contactByNodeNum(uint32_t nodeNum)
{
    MeshCoreNode *n = nodes.findByNodeNum(nodeNum);
    return n && n->type == MeshCoreNode::CHAT ? n : nullptr;
}

bool MeshCoreCompanion::isMeshCoreContact(uint32_t nodeNum, const uint8_t pubKey[32]) const
{
    concurrency::RecursiveLockGuard guard(lock);
    const MeshCoreNode *n = nodes.findByNodeNum(nodeNum);
    if (!n || n->type != MeshCoreNode::CHAT)
        return false;
    if (pubKey && memcmp(n->pub, pubKey, 32) != 0)
        return false;
    return true;
}

bool MeshCoreCompanion::injectedDirectText(const meshtastic_MeshPacket &p) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!p.pki_encrypted || p.public_key.size != 32 || p.id == 0)
        return false;
    for (const InjectedDirect &d : injectedDirect)
        if (d.id == p.id && d.from == p.from)
            return isMeshCoreContact(p.from, p.public_key.bytes);
    return false;
}

MeshCoreNode *MeshCoreCompanion::learnContact(const uint8_t pub[32], const char *name)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    MeshCoreNode *n = nodes.upsert(pub);
    if (n->type != MeshCoreNode::CHAT)
        n->nodeNum = 0; // a repeater turned chat takes the number contacts get, from its name
    n->type = MeshCoreNode::CHAT;
    if (name)
        n->setName(name);
    if (n->nodeNum == 0) // a contact keeps its number across renames, as the client keeps its conversation
        nodes.assignNodeNum(n, MeshCoreNodeStore<1>::nodeNumForName(n->name), PSEUDO_NODE_NUM);
    return n;
}

bool MeshCoreCompanion::nextReplayNode(size_t &cursor, MeshCoreNode &out) const
{
    concurrency::RecursiveLockGuard guard(lock);
    for (; cursor < nodes.capacity(); cursor++) {
        const MeshCoreNode *n = nodes.at(cursor);
        if (!n || n->nodeNum == 0 || n->type != MeshCoreNode::CHAT)
            continue; // repeaters live in their own table, which keeps no key to announce them with
        if (nodeDB && nodeDB->getMeshNode(n->nodeNum))
            continue; // the sync already sent it
        // Copied under the store lock: an advert landing meanwhile (radio thread) cannot tear the key the client is told.
        out = *n;
        cursor++;
        return true;
    }
    return false;
}

const MeshCoreRepeaterName *MeshCoreCompanion::learnRepeater(const uint8_t pub[32], const char *name, float snr, uint8_t hops,
                                                             uint32_t advertTime)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    MeshCoreHeard how;
    how.snr = snr;
    how.hops = hops;
    how.advertTime = advertTime;
    how.uptimeSecs = Time::getUptimeSecs();
    return repeaters.learn(pub, name, how);
}

void MeshCoreCompanion::noteAdvert(MeshCoreNode *n, uint32_t advertTime, int32_t latMicro, int32_t lonMicro, float snr,
                                   int32_t rssi, uint8_t hops)
{
    n->lastAdvert = advertTime;
    n->snr = snr;
    n->rssi = rssi;
    n->setHops(hops);
    uint32_t now = getTime(false);
    n->lastHeard = now ? now : 1;
    nodes.touch(n);
    if (latMicro != 0 || lonMicro != 0) { // 0,0 is "no position", not the Gulf of Guinea
        n->latitudeMicro = latMicro;
        n->longitudeMicro = lonMicro;
    }
}

void MeshCoreCompanion::retireGroupNode(const char *name)
{
    // The node made for this name before it was a contact: ours (no hardware model, unmessagable), not a favourite.
    const uint32_t num = senderNodeNum(name);
    if (nodes.findByNodeNum(num))
        return; // the contact took over this very node: nothing left to retire
    const meshtastic_NodeInfoLite *lite = nodeDB ? nodeDB->getMeshNode(num) : nullptr;
    if (lite && nodeInfoLiteHasUser(lite) && lite->hw_model == meshtastic_HardwareModel_UNSET &&
        nodeInfoLiteIsUnmessagable(lite) && !nodeInfoLiteIsFavorite(lite))
        nodeDB->removeNodeByNum(num);
}

void MeshCoreCompanion::retireKeyedNode(const uint8_t pub[32], uint32_t keepNum)
{
    // Earlier builds numbered a contact's node from its public key; that node carries the same key and no hardware model.
    const uint32_t num = MeshCoreNodeStore<1>::nodeNumForKey(pub);
    if (num == keepNum || !nodeDB)
        return;
    const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNode(num);
    if (lite && nodeInfoLiteHasUser(lite) && lite->hw_model == meshtastic_HardwareModel_UNSET && !nodeInfoLiteIsFavorite(lite) &&
        lite->public_key.size == 32 && memcmp(lite->public_key.bytes, pub, 32) == 0)
        nodeDB->removeNodeByNum(num);
}

void MeshCoreCompanion::syncWithNodeDB()
{
    concurrency::RecursiveLockGuard guard(lock);
    const size_t indexed = indexStoreByNodeNum();
    uint32_t starsToPush[STARS_PUSHED_PER_SYNC];
    size_t pushes = 0;
    const size_t dbNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    for (size_t i = 0; i < dbNodes; i++) {
        meshtastic_NodeInfoLite *lite = nodeDB->getMeshNodeByIndex(i);
        SyncSlot *slot = inMeshCoreNodeNumRange(lite->num) ? findIndexed(lite->num, indexed) : nullptr;
        if (!slot || lite->hw_model != meshtastic_HardwareModel_UNSET)
            continue; // not ours, or a Meshtastic node on our number: ours is then not in the node DB
        MeshCoreNode &n = *slot->node;
        nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_IS_MESHCORE_MASK, true);
        if (n.type == MeshCoreNode::CHAT && syncFavorite(n, lite) == FavoriteSync::PUSH_TO_NODE_DB) {
            if (pushes == STARS_PUSHED_PER_SYNC)
                continue; // left unmarked, so the next sync still sees it for the first time and pushes the star
            starsToPush[pushes++] = n.nodeNum;
        }
        slot->inNodeDb = true;
    }
    // Evicted nodes are no longer in the node DB; the store keeps what it had of them.
    for (size_t i = 0; i < indexed; i++) {
        if (syncIndex[i].inNodeDb)
            syncIndex[i].node->flags |= MeshCoreNode::IN_NODE_DB;
        else
            syncIndex[i].node->flags &= ~MeshCoreNode::IN_NODE_DB;
    }
    // Setting a favourite may re-sort the node DB, so the stars go in once the walk over it is over.
    for (size_t i = 0; i < pushes; i++)
        nodeDB->set_favorite(true, starsToPush[i]);
}

size_t MeshCoreCompanion::indexStoreByNodeNum()
{
    size_t count = 0;
    nodes.forEach([&](MeshCoreNode &n) { syncIndex[count++] = {&n, false}; });
    std::sort(syncIndex, syncIndex + count, [](const SyncSlot &a, const SyncSlot &b) { return a.node->nodeNum < b.node->nodeNum; });
    return count;
}

MeshCoreCompanion::SyncSlot *MeshCoreCompanion::findIndexed(uint32_t nodeNum, size_t indexed)
{
    SyncSlot *end = syncIndex + indexed;
    SyncSlot *at = std::lower_bound(syncIndex, end, nodeNum,
                                    [](const SyncSlot &slot, uint32_t num) { return slot.node->nodeNum < num; });
    return at != end && at->node->nodeNum == nodeNum ? at : nullptr;
}

MeshCoreCompanion::FavoriteSync MeshCoreCompanion::syncFavorite(MeshCoreNode &n, const meshtastic_NodeInfoLite *lite)
{
    // A node DB copy seen for the first time (a reboot that did not save it, or an eviction) has no star of its own yet: the
    // store's is the one the user set. From then on the client stars the node in the node DB, and the store follows.
    const bool firstSight = !(n.flags & MeshCoreNode::IN_NODE_DB);
    if (firstSight && n.isFavorite() && !nodeInfoLiteIsFavorite(lite))
        return FavoriteSync::PUSH_TO_NODE_DB;
    nodes.setFavorite(&n, nodeInfoLiteIsFavorite(lite)); // through the store, so the save check sees the change
    return FavoriteSync::DONE;
}

size_t MeshCoreCompanion::exportContacts(uint8_t *buf, size_t cap)
{
    concurrency::RecursiveLockGuard guard(lock);
    syncWithNodeDB();
    return MeshCoreNodeFile::save(nodes, buf, cap);
}

size_t MeshCoreCompanion::importContacts(const uint8_t *buf, size_t len)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    const size_t loaded = MeshCoreNodeFile::load(nodes, buf, len);
    if (identityReady) {
        nodes.remove(identityPub);
        if (aliasReady)
            nodes.remove(aliasPub);
    }
    nodes.forEach([&](MeshCoreNode &n) {
        if (n.type != MeshCoreNode::CHAT)
            return;
        if (n.nodeNum == 0) // migrated from the contacts file, which kept no numbers
            nodes.assignNodeNum(&n, MeshCoreNodeStore<1>::nodeNumForName(n.name), PSEUDO_NODE_NUM);
        ensureSenderNode(n.nodeNum, n.name, true, n.pub, meshtastic_Config_DeviceConfig_Role_CLIENT, storedRoute(n));
        retireGroupNode(n.name);
        retireKeyedNode(n.pub, n.nodeNum);
    });
    return loaded;
}

uint32_t MeshCoreCompanion::contactsFingerprint()
{
    concurrency::RecursiveLockGuard guard(lock);
    syncWithNodeDB();
    return MeshCoreNodeFile::fingerprint(nodes);
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
