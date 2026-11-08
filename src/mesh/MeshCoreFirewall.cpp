// SPDX-License-Identifier: AGPL-3.0-only
//
// The MeshCore half of the Nexus firewall, on the receive path of MeshCoreRepeater: ignored nodes (the NIGN list), the drop rules
// (MeshCoreConfig::DROP_*), the one-advert-per-key gap and the per-repeater hop rate. A frame it drops reaches neither the relay
// nor the companion. With -DNEXUS_EXCLUDE_FIREWALL=1 this file is not built and MeshCoreRepeater.h supplies stubs that drop nothing.

#include "MeshCoreRepeater.h"

#include "Arduino.h"
#include "MeshCoreNodeStore.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include <string.h>

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER && !NEXUS_EXCLUDE_FIREWALL

#include "MeshCoreProtocol.h"

void MeshCoreRepeater::rememberIgnored(const uint8_t *pub, uint32_t num)
{
    for (const IgnoredPrefix &known : ignoredCache) {
        if (known.nodeNum == num)
            return;
    }
    IgnoredPrefix &slot = ignoredCache[ignoredNext];
    ignoredNext = (uint8_t)((ignoredNext + 1) % IGNORED_CACHE_SIZE);
    memcpy(slot.prefix, pub, IGNORED_PREFIX_LEN);
    slot.nodeNum = num;
}

void MeshCoreRepeater::setFirewall(uint8_t dropMask, uint8_t advertGapMin, uint8_t hopRatePerMin)
{
    fwDropMask = dropMask;
    fwAdvertGapMin = advertGapMin;
    fwHopRatePerMin = hopRatePerMin;
}

// The payload types a /mtc DROP rule silences. CONTROL (neighbour discovery) is never one of them.
static uint8_t dropBitFor(uint8_t payloadType)
{
    switch (payloadType) {
    case MC_PAYLOAD_TYPE_ADVERT:
        return MeshCoreConfig::DROP_ADVERT;
    case MC_PAYLOAD_TYPE_REQ:
    case MC_PAYLOAD_TYPE_RESPONSE:
    case MC_PAYLOAD_TYPE_ANON_REQ:
        return MeshCoreConfig::DROP_REQUEST;
    case MC_PAYLOAD_TYPE_TXT_MSG:
        return MeshCoreConfig::DROP_TEXT;
    case MC_PAYLOAD_TYPE_GRP_TXT:
    case MC_PAYLOAD_TYPE_GRP_DATA:
        return MeshCoreConfig::DROP_GROUP;
    case MC_PAYLOAD_TYPE_ACK:
    case MC_PAYLOAD_TYPE_MULTIPART:
        return MeshCoreConfig::DROP_ACK;
    case MC_PAYLOAD_TYPE_PATH:
        return MeshCoreConfig::DROP_PATH;
    case MC_PAYLOAD_TYPE_TRACE:
        return MeshCoreConfig::DROP_TRACE;
    default:
        return 0;
    }
}

bool MeshCoreRepeater::frameFiltered(const uint8_t *raw, const FrameInfo &info)
{
    // Cheap, stateless checks first; the rate limits keep state, so they only see what the others let through.
    // Ignored sources go first: their adverts must reach rememberIgnored() even while DROP MCAD discards every advert,
    // or the hash-based matching of their other traffic never learns them.
    if (isIgnoredSource(raw, info)) {
        rxIgnored++;
        return true;
    }
    const uint8_t bit = dropBitFor(info.payload_type) & fwDropMask;
    if (bit) {
        for (size_t i = 0; i < MeshCoreConfig::DROP_RULE_COUNT; i++) {
            if (MeshCoreConfig::DROP_RULES[i].bit == bit)
                fwRuleDrops[i]++;
        }
        return true;
    }
    if (rateLimited(raw, info)) {
        rxRateLimited++;
        return true;
    }
    return false;
}

bool MeshCoreRepeater::rateLimited(const uint8_t *raw, const FrameInfo &info)
{
    const uint32_t now = Time::getMillis();

    // One advert per key per gap: a node that re-announces itself every few seconds is noise, not presence.
    if (fwAdvertGapMin != 0 && info.payload_type == MC_PAYLOAD_TYPE_ADVERT && info.payload_len >= MC_PUB_KEY_SIZE) {
        const uint32_t key = MeshCoreNodeStore<1>::nodeNumForKey(&raw[info.header_len]);
        AdvertSeen *slot = nullptr;
        for (AdvertSeen &seen : advertSeen) {
            if (seen.key == key) {
                slot = &seen;
                break;
            }
        }
        const uint32_t gapMs = (uint32_t)fwAdvertGapMin * 60000UL;
        if (slot && Throttle::isWithinTimespanMs(slot->atMs, gapMs))
            return true;
        // Only a genuine advert opens the gap; a forged one carrying someone else's key would otherwise silence them.
        if (!verifyAdvert(&raw[info.header_len], info.payload_len))
            return false;
        if (!slot) {
            slot = &advertSeen[advertSeenNext];
            advertSeenNext = (uint8_t)((advertSeenNext + 1) % ADVERT_SEEN_SIZE);
            slot->key = key;
        }
        slot->atMs = now;
    }

    // At most N frames a minute from one neighbouring repeater, named by the last hop of the path (2- or 3-byte hashes).
    const bool flood = info.route_type == MC_ROUTE_FLOOD || info.route_type == MC_ROUTE_TRANSPORT_FLOOD;
    if (fwHopRatePerMin != 0 && flood && info.path_hash_count != 0 && info.path_hash_size >= 2) {
        const uint8_t *lastHop = &raw[info.header_len - info.path_hash_size];
        HopRate *slot = nullptr;
        for (HopRate &hop : hopRates) {
            if (hop.size == info.path_hash_size && memcmp(hop.prefix, lastHop, hop.size) == 0) {
                slot = &hop;
                break;
            }
        }
        if (!slot) {
            slot = &hopRates[hopRatesNext];
            hopRatesNext = (uint8_t)((hopRatesNext + 1) % HOP_RATE_SIZE);
            slot->size = info.path_hash_size;
            memcpy(slot->prefix, lastHop, slot->size);
            slot->windowStartMs = now;
            slot->count = 0;
        }
        if (!Throttle::isWithinTimespanMs(slot->windowStartMs, 60000UL)) {
            slot->windowStartMs = now;
            slot->count = 0;
        }
        if (slot->count >= fwHopRatePerMin)
            return true;
        slot->count++;
    }
    return false;
}

bool MeshCoreRepeater::isIgnoredSource(const uint8_t *raw, const FrameInfo &info)
{
    if (!ignorePredicate)
        return false;
    const uint8_t *payload = &raw[info.header_len];

    // Frames that carry the sender's whole key: the match is exact.
    if (info.payload_type == MC_PAYLOAD_TYPE_ADVERT && info.payload_len >= MC_PUB_KEY_SIZE) {
        const uint32_t num = MeshCoreNodeStore<1>::nodeNumForKey(payload);
        if (!nodeIgnored(num))
            return false;
        rememberIgnored(payload, num); // so the rest of its traffic is recognised by hash
        return true;
    }
    if (info.payload_type == MC_PAYLOAD_TYPE_ANON_REQ && info.payload_len >= 1 + MC_PUB_KEY_SIZE) {
        const uint32_t num = MeshCoreNodeStore<1>::nodeNumForKey(&payload[1]); // dest hash, then the sender key
        if (!nodeIgnored(num))
            return false;
        rememberIgnored(&payload[1], num);
        return true;
    }

    // Direct messages, requests, responses and paths carry a one-byte source hash: 1 in 256 senders shares it. Cutting on
    // it also cuts those few; with a short ignore list that is the price of silencing the node's direct traffic.
    if ((fwDropMask & MeshCoreConfig::DROP_SOURCE) &&
        (info.payload_type == MC_PAYLOAD_TYPE_TXT_MSG || info.payload_type == MC_PAYLOAD_TYPE_REQ ||
         info.payload_type == MC_PAYLOAD_TYPE_RESPONSE || info.payload_type == MC_PAYLOAD_TYPE_PATH) &&
        info.payload_len >= 2) {
        for (IgnoredPrefix &known : ignoredCache) {
            if (known.nodeNum == 0 || known.prefix[0] != payload[1])
                continue;
            if (nodeIgnored(known.nodeNum))
                return true;
            known.nodeNum = 0; // taken off the list since: forget it
        }
    }

    // A flood's last path hash is the node we heard it from. One byte is too short to name anybody.
    const bool flood = info.route_type == MC_ROUTE_FLOOD || info.route_type == MC_ROUTE_TRANSPORT_FLOOD;
    if (!flood || info.path_hash_count == 0 || info.path_hash_size < 2)
        return false;
    const uint8_t *lastHop = &raw[info.header_len - info.path_hash_size];
    for (IgnoredPrefix &known : ignoredCache) {
        if (known.nodeNum == 0 || memcmp(known.prefix, lastHop, info.path_hash_size) != 0)
            continue;
        if (nodeIgnored(known.nodeNum))
            return true;
        known.nodeNum = 0; // taken off the list since: forget it
    }
    return false;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER && !NEXUS_EXCLUDE_FIREWALL
