// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jennifer Sanchez
//
// Licensed under the GNU Affero General Public License v3.
// See LICENSE-AGPL for the full license text.
//
// The Meshtastic half of the Nexus firewall: the rules that decide whether a packet is dropped on its way in (decoded and opaque)
// and on its way out, the rate limits, the invalid-packet filters, the signature scan and the NIGN list. NexusModule.cpp keeps
// the packet statistics and the TRACE / META alerts, which are not firewall, and calls into here. With -DNEXUS_EXCLUDE_FIREWALL=1
// this whole file compiles to nothing and NexusModule.h supplies inline stubs that drop nothing.

#include "configuration.h"

#if !NEXUS_EXCLUDE_FIREWALL

#include "Channels.h"
#include "NodeDB.h"
#include "Router.h" // BITFIELD_OK_TO_MQTT_MASK
#include "mesh/AdminKeys.h"
#include "mesh/Throttle.h"
#include "modules/NexusCommandParse.h"
#include "modules/NexusModule.h"

using namespace meshtastic;

static inline bool nexus_is_favorite(const meshtastic_NodeInfoLite *n)
{
    return nodeInfoLiteIsFavorite(n);
}

/*
 * This function is an additional filter for invalid packets to drop
 * The origin of the function was when we started seeing nodes appearing in Null Island
 * So if you wanna completely drop this packets with this you can.
 */

bool NexusModule::shouldInvalidatePacket(const meshtastic_MeshPacket &mp) {
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return false;

    uint32_t port = mp.decoded.portnum;

    // Rule 5: NodeID mismatch (From header vs NodeInfo payload)
    if (inv_id_mismatch && port == meshtastic_PortNum_NODEINFO_APP) {
        static meshtastic_User user;  // ~160B; static to relieve stack pressure
        memset(&user, 0, sizeof(user));
        if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_User_msg, &user)) {
            uint32_t payloadId = 0;
            if (user.id[0] == '!') {
                payloadId = strtoul(user.id + 1, NULL, 16);
            }
            if (payloadId != 0 && payloadId != mp.from) {
                LOG_WARN("MTC: Invalid ID Mismatch - Header:!%08x vs Payload:!%08x", (unsigned int)mp.from, (unsigned int)payloadId);
                inv_drop_id++;
                return true;
            }
        }
    }

    // Rule 4: HW Unset
    if (inv_hw_unset && port == meshtastic_PortNum_NODEINFO_APP) {
        static meshtastic_User user;  // Reused across inv blocks; mutually exclusive here
        memset(&user, 0, sizeof(user));
        if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_User_msg, &user)) {
            if (user.hw_model == meshtastic_HardwareModel_UNSET) {
                LOG_WARN("MTC: Invalid HW Unset from !%08x", (unsigned int)mp.from);
                inv_drop_hw++;
                return true;
            }
        }
    }

    // Position Rules (1, 2, 3)
    if (port == meshtastic_PortNum_POSITION_APP) {
        static meshtastic_Position pos;  // ~180B; static to relieve stack pressure
        memset(&pos, 0, sizeof(pos));
        if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Position_msg, &pos)) {
            float lat = (float)pos.latitude_i / 1e7f;
            float lon = (float)pos.longitude_i / 1e7f;

            // Rule 1: Null Island drop (gated by inv_null_island).
            // Catches both exact (0,0) and near (0,0) coords (external bug injector uses 0,0 pure).
            // Note: legitimate "no fix" packets report lat=0 && lon=0, but they're rare and acceptable to drop when flag is ON.
            if (inv_null_island) {
                bool exactZero = (pos.latitude_i == 0 && pos.longitude_i == 0);
                bool nearZero = (fabs(lat) < 0.01f && fabs(lon) < 0.01f);
                if (exactZero || nearZero) {
                    LOG_WARN("MTC: Invalid Null Island from !%08x (lat=%d, lon=%d)", (unsigned int)mp.from, pos.latitude_i, pos.longitude_i);
                    inv_drop_null++;
                    return true;
                }
            }

            // Rule 2: Outside Spain
            // Peninsula: 36N to 44N, 9.5W to 4.5E
            // Canary: 27N to 30N, 19W to 13W
            if (inv_out_spain) {
                bool inPeninsula = (lat >= 35.0f && lat <= 44.0f && lon >= -10.0f && lon <= 5.0f);
                bool inCanary = (lat >= 27.0f && lat <= 30.0f && lon >= -19.0f && lon <= -13.0f);
                if (!inPeninsula && !inCanary) {
                    LOG_WARN("MTC: Invalid Outside Spain from !%08x (%.4f, %.4f)", (unsigned int)mp.from, lat, lon);
                    inv_drop_spain++;
                    return true;
                }
            }

            // Rule 3: Outside Germany
            // Approx: 47N to 55N, 5.5E to 15.5E
            if (inv_out_germany) {
                bool inGermany = (lat >= 47.0f && lat <= 55.5f && lon >= 5.5f && lon <= 15.5f);
                if (!inGermany) {
                    LOG_WARN("MTC: Invalid Outside Germany from !%08x (%.4f, %.4f)", (unsigned int)mp.from, lat, lon);
                    inv_drop_germany++;
                    return true;
                }
            }
        }
    }

    return false;
}

bool NexusModule::checkRateLimit(const meshtastic_MeshPacket &mp) {
    // Tokens refill at limit/3600 per ms.
    // pkts/hr / 3600.0f = pkts/sec.
    // pkts/sec / 1000.0f = pkts/ms.
    // float refill_per_ms = (float)limit / 3600000.0f;

    uint32_t port = 0;
    bool isEncrypted = (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag);
    if (isEncrypted) {
        if (!drop_rl_private) return true;
    } else if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        port = mp.decoded.portnum;
    } else {
        return true; // Unknown variant, don't RL
    }

    int idx = -1;
    uint16_t limit = 0;
    if (isEncrypted) {
        idx = 6;
        limit = rl_private_limit;
    } else if (port == meshtastic_PortNum_TEXT_MESSAGE_APP || port == meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP) {
        if (!drop_rl_text) return true;
        idx = 0;
        limit = rl_text_limit;
    } else if (port == meshtastic_PortNum_TRACEROUTE_APP) {
        if (!drop_rl_trace) return true;
        idx = 1;
        limit = rl_trace_limit;
    } else if (port == meshtastic_PortNum_NODEINFO_APP) {
        if (!drop_rl_nodeinfo) return true;
        idx = 3;
        limit = rl_nodeinfo_limit;
    } else if (port == meshtastic_PortNum_POSITION_APP) {
        if (!drop_rl_position) return true;
        idx = 4;
        limit = rl_position_limit;
    } else if (port == meshtastic_PortNum_TELEMETRY_APP) {
        if (!drop_rl_telemetry) return true;
        idx = 5;
        limit = rl_telemetry_limit;
    } else {
        if (!drop_rl_other) return true;
        idx = 2;
        limit = rl_other_limit;
    }

    if (idx == -1 || limit == 0) return true;

    // v267: seen-counter increments only when the RL is actually engaged (idx resolved,
    // limit > 0). Bypasses (drop_rl_* = OFF) are not counted because the RL is not
    // regulating that bucket. seen = pass + drop by arithmetic.
    switch (idx) {
        case 0: rl_seen_text++; break;
        case 1: rl_seen_trace++; break;
        case 2: rl_seen_other++; break;
        case 3: rl_seen_nodeinfo++; break;
        case 4: rl_seen_position++; break;
        case 5: rl_seen_telemetry++; break;
        case 6: rl_seen_private++; break;
    }

    // Refill tokens for all buckets based on elapsed time
    uint32_t now = Time::getMillis();
    uint32_t elapsed = (now - rl_last_refill);
    rl_last_refill = now;
    float refill = (float)elapsed / 3600000.0f; // milliseconds to hours
    static const uint16_t* limits[7] = {
        &rl_text_limit, &rl_trace_limit, &rl_other_limit,
        &rl_nodeinfo_limit, &rl_position_limit, &rl_telemetry_limit, &rl_private_limit
    };
    for (int i=0; i<7; i++) {
        rl_tokens[i] += refill * (float)(*limits[i]);
        if (rl_tokens[i] > (float)(*limits[i])) rl_tokens[i] = (float)(*limits[i]); // Burst limit = 1hr worth
    }

    if (rl_tokens[idx] >= 1.0f) {
        rl_tokens[idx] -= 1.0f;
        switch (idx) {
            case 0: rl_pass_text++; break;
            case 1: rl_pass_trace++; break;
            case 2: rl_pass_other++; break;
            case 3: rl_pass_nodeinfo++; break;
            case 4: rl_pass_position++; break;
            case 5: rl_pass_telemetry++; break;
            case 6: rl_pass_private++; break;
        }
        return true; // Pass
    }

    LOG_WARN("MTC: Rate limit exceeded (bucket %d, Limit: %u pkts/hr)", idx, (unsigned int)limit);
    switch (idx) {
        case 0: rl_drop_text++; break;
        case 1: rl_drop_trace++; break;
        case 2: rl_drop_other++; break;
        case 3: rl_drop_nodeinfo++; break;
        case 4: rl_drop_position++; break;
        case 5: rl_drop_telemetry++; break;
        case 6: rl_drop_private++; break;
    }
    return false; // Drop
}

bool NexusModule::isAdminKeyPacket(const meshtastic_MeshPacket &mp)
{
    if (!mp.pki_encrypted || mp.public_key.size != 32)
        return false;
    return isAuthorizedAdmin(mp.public_key.bytes) || AdminKeys::isAuthorized(mp.public_key.bytes);
}

bool NexusModule::isNexusIgnored(uint32_t nodeID) {
    if (nodeID == 0) return false;
    for (uint16_t i = 0; i < nexus_ignore_count; i++) {
        if (nexus_ignore_list[i] == nodeID) return true;
    }
    return false;
}

bool NexusModule::firewallDropsPacket(const meshtastic_MeshPacket &mp)
{
    bool isNexusChannel = this->isNexusChannel(mp.channel);

    // Packets authenticated with an admin key are never dropped, whatever the rules say (ignore list,
    // relay drop, PKI/admin/port drops, rate limits, signatures...).
    if (isAdminKeyPacket(mp)) {
        fw_nexus_bypass++;
        return false;
    }

    // Nexus Trusted Channels bypass ALL firewall drops
    if (isNexusChannel) {
        fw_nexus_bypass++;
        return false;
    }

    // Nexus: Global Favorite Bypass
    // If active, any node marked as 'favorite' in NodeDB bypasses ALL subsequent firewall rules.
    // Checks both sender (mp.from) and recipient (mp.to) for maximum flexibility in 1:1 
    if (fav_bypass) {
        auto fromNode = nodeDB->getMeshNode(mp.from);
        if (fromNode && nexus_is_favorite(fromNode)) {
            fw_fav_bypass++;
            return false;
        }

        if (mp.to != 0xFFFFFFFF) {
            auto toNode = nodeDB->getMeshNode(mp.to);
            if (toNode && nexus_is_favorite(toNode)) {
                fw_fav_bypass++;
                return false;
            }
        }
    }

    // 0.5 Nexus Ignore List Check
    if (isNexusIgnored(mp.from)) {
        LOG_DEBUG("MTC: Packet dropped from Nexus-Ignored node !%08x", (unsigned int)mp.from);
        fw_drop_nexus_ignored++;
        return true;
    }

    // Nexus: Relay Node Drop Filter (256-bit mask)
    uint8_t relayId = (uint8_t)(mp.relay_node & 0xFF);
    if (isRelayDropped(relayId)) {
        LOG_DEBUG("MTC: Packet dropped from Relay-Dropped node *%02X", relayId);
        fw_drop_relay++;
        return true;
    }

    // 0. PreHop Filter (Validation of hop_start metrics)
    if (drop_prehop && !isFromUs(&mp)) {
        if (classifyHopStart(mp) != HopStartStatus::VALID) {
            fw_drop_prehop++;
            return true;
        }
    }

    // 0. PKI/DM Filter
    if (mp.pki_encrypted) {
        bool isLocalPKI = (mp.from == myNodeInfo.my_node_num || mp.to == myNodeInfo.my_node_num);
        // drop_pki_local auto-couples to drop_pki (global): an operator enabling global PKI drop
        // almost never wants their own PKI DMs through. Local drop fires first so the local
        // counter tracks local PKI specifically regardless of which toggle triggered it. Foreign
        // PKI handled by shouldDropPrivate() since un-decodable PKI DMs land on the opaque path.
        if ((drop_pki_local || drop_pki) && isLocalPKI) {
            fw_drop_pki_local++;
            return true;
        }
        if (drop_pki) {
            fw_drop_pki++;
            return true;
        }
        if (drop_pki_remote && !isLocalPKI) {
            fw_drop_pki_remote++;
            return true;
        }
        return false; // Bypass other rules if not explicitly dropped
    }

    // Whitelist/Blacklist filter is exclusively for encrypted packets (shouldDropPrivate).
    // shouldDropPacket() only sees decoded packets; decrypted wire-level channel filtering
    // is handled at the encrypted path. see shouldDropPrivate() for the actual checks.

    // 2. Mesh Congestion Filters (Hop-based)
    if (drop_hop6 && mp.hop_start == 6) {
        fw_drop_hop6++;
        return true;
    }
    if (drop_hop7 && mp.hop_start == 7) {
        fw_drop_hop7++;
        return true;
    }

    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        uint32_t port = mp.decoded.portnum;

        // 0. Stealth ACKs (Filter incoming acknowledgments if requested)
        if (drop_acks && port == meshtastic_PortNum_ROUTING_APP) {
            fw_drop_acks++;
            return true;
        }

        // 3. Block standard protocol requests (want_response = true)
        // This prevents the node from automatically formulating a reply to specific queries.
        if (mp.decoded.want_response) {
            if (port == meshtastic_PortNum_NODEINFO_APP && drop_req_nodeinfo) { fw_drop_req_nodeinfo++; return true; }
            if (port == meshtastic_PortNum_POSITION_APP && drop_req_position) { fw_drop_req_position++; return true; }
            if (port == meshtastic_PortNum_TELEMETRY_APP && drop_req_telemetry) { fw_drop_req_telemetry++; return true; }
        }

        // 3a. Broadcast-specific request filters (v267)
        // These only fire on requests sent to broadcast (mp.to == NODENUM_BROADCAST).
        // DM/targeted requests pass through regardless, so an operator can still
        // probe a single node directly even with these drops active.
        if (mp.decoded.want_response && mp.to == NODENUM_BROADCAST) {
            if (port == meshtastic_PortNum_NODEINFO_APP  && drop_br_nodeinfo)  { fw_drop_br_nodeinfo++;  return true; }
            if (port == meshtastic_PortNum_TELEMETRY_APP && drop_br_telemetry) { fw_drop_br_telemetry++; return true; }
            if (port == meshtastic_PortNum_POSITION_APP  && drop_br_position)  { fw_drop_br_position++;  return true; }
        }

        // 3b. Rate Limiting (Nexus v28)
        if (!checkRateLimit(mp)) return true;

        // 3c. Invalid Filters (Nexus v29)
        if (shouldInvalidatePacket(mp)) return true;

        // 4. Block specific ports from being repeated/processed
        if (port == meshtastic_PortNum_NODEINFO_APP && drop_nodeinfo) { fw_drop_nodeinfo++; return true; }
        if (port == meshtastic_PortNum_POSITION_APP) {
            if (drop_position) { fw_drop_position++; return true; }
            static meshtastic_Position pos;  // ~180B; static to relieve stack pressure
            memset(&pos, 0, sizeof(pos));
            if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Position_msg, &pos)) {
                if (drop_pos_manual && pos.location_source == meshtastic_Position_LocSource_LOC_MANUAL) { fw_drop_pos_manual++; return true; }
                if (drop_pos_gps && (pos.location_source == meshtastic_Position_LocSource_LOC_INTERNAL || pos.location_source == meshtastic_Position_LocSource_LOC_EXTERNAL)) { fw_drop_pos_gps++; return true; }
                if (drop_pos_other && pos.location_source > meshtastic_Position_LocSource_LOC_EXTERNAL) { fw_drop_pos_other++; return true; }
            }
        }
        if (port == meshtastic_PortNum_TELEMETRY_APP) {
            if (drop_telemetry) { fw_drop_telemetry++; return true; }
            // Telemetry union holds the largest variant (LocalStats); ~200B total.
            static meshtastic_Telemetry tele;
            memset(&tele, 0, sizeof(tele));
            if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Telemetry_msg, &tele)) {
                if (drop_tele_device && tele.which_variant == meshtastic_Telemetry_device_metrics_tag) { fw_drop_tele_device++; return true; }
                if (drop_tele_power && tele.which_variant == meshtastic_Telemetry_power_metrics_tag) { fw_drop_tele_power++; return true; }
                if (drop_tele_env && tele.which_variant == meshtastic_Telemetry_environment_metrics_tag) { fw_drop_tele_env++; return true; }
                if (drop_tele_other && tele.which_variant != meshtastic_Telemetry_device_metrics_tag &&
                    tele.which_variant != meshtastic_Telemetry_power_metrics_tag && tele.which_variant != meshtastic_Telemetry_environment_metrics_tag) { fw_drop_tele_other++; return true; }
            }
        }
        if (drop_no_mqtt && mp.decoded.has_bitfield && !(mp.decoded.bitfield & BITFIELD_OK_TO_MQTT_MASK)) { fw_drop_no_mqtt++; return true; }
        if (port == meshtastic_PortNum_NEIGHBORINFO_APP && drop_neighbor) { fw_drop_neighbor++; return true; }
        if (port == meshtastic_PortNum_TRACEROUTE_APP && drop_traceroute) { fw_drop_traceroute++; return true; }

        // 4b. Block Store and Forward
        if (drop_store_forward && (port == meshtastic_PortNum_STORE_FORWARD_APP || port == meshtastic_PortNum_STORE_FORWARD_PLUSPLUS_APP)) {
            fw_drop_store_forward++;
            return true;
        }

        // 4c. Block Admin, Text, KeyVer, Alert
        if (port == meshtastic_PortNum_ADMIN_APP && drop_admin) { fw_drop_admin++; return true; }
        if ((port == meshtastic_PortNum_TEXT_MESSAGE_APP || port == meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP) && drop_text) { fw_drop_text++; return true; }
        if (port == meshtastic_PortNum_KEY_VERIFICATION_APP && drop_keyver) { fw_drop_keyver++; return true; }
        if (port == meshtastic_PortNum_ALERT_APP && drop_alert) { fw_drop_alert++; return true; }
        if (port == meshtastic_PortNum_WAYPOINT_APP && drop_waypoint) { fw_drop_waypoint++; return true; }

        // 5. Granular TRACEROUTE filtering
        if (port == meshtastic_PortNum_TRACEROUTE_APP) {
            // Nexus: TEMPORARY BYPASS during active /mtc TRACE command
            if (trace_active) return false;

            bool isLocal = (mp.from == myNodeInfo.my_node_num || mp.to == myNodeInfo.my_node_num);
            uint8_t hopsUsed = (mp.hop_start >= mp.hop_limit) ? (mp.hop_start - mp.hop_limit) : 0;
            bool isDirect = (hopsUsed == 0);

            if (drop_traceroute) { fw_drop_traceroute++; return true; }
            if (drop_traceroute_local && isLocal) { fw_drop_traceroute_local++; return true; }
            if (drop_traceroute_remote && !isLocal) { fw_drop_traceroute_remote++; return true; }

            // 5b. Direct TraceRoute Filtering (0 hops used)
            if (isDirect) {
                if (drop_traceroute_direct) { fw_drop_traceroute_direct++; return true; }
                if (drop_traceroute_direct_local && isLocal) { fw_drop_traceroute_direct_local++; return true; }
                if (drop_traceroute_direct_remote && !isLocal) { fw_drop_traceroute_direct_remote++; return true; }
            }
        }
    }

    // Signature firewall (v39): scan payload bytes for any configured signature
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        size_t plen = mp.decoded.payload.size;
        const uint8_t *pbuf = mp.decoded.payload.bytes;
        for (int s = 0; s < 8; s++) {
            if (nexus_signatures[s].len == 0) continue;
            if (plen < nexus_signatures[s].len) continue;
            // Naive substring match (O(n*m), m<=16, n<=240 => trivial)
            for (size_t off = 0; off <= plen - nexus_signatures[s].len; off++) {
                bool match = true;
                for (uint8_t i = 0; i < nexus_signatures[s].len; i++) {
                    if (pbuf[off + i] != nexus_signatures[s].bytes[i]) { match = false; break; }
                }
                if (match) {
                    fw_drop_signature++;
                    return true;
                }
            }
        }
    }

#ifdef NEXUS_PRIVATE_BUILD
    // Nexus-28006 Step C (2026-09-24) — private extension point.
    // Empty pass-through today; future private-only drop rules land here
    // without touching the public firewall body above.
    if (privateShouldDropPacket(mp)) {
        return true;
    }
#endif

    return false;
}

bool NexusModule::firewallDropsPrivate(const meshtastic_MeshPacket &mp)
{

    // Admin-key packets are never dropped (see isAdminKeyPacket).
    if (isAdminKeyPacket(mp)) {
        fw_nexus_bypass++;
        return false;
    }

    // Nexus: Relay Node Drop Filter (256-bit mask)
    uint8_t relayId = (uint8_t)(mp.relay_node & 0xFF);
    if (isRelayDropped(relayId)) {
        fw_drop_relay++;
        return true;
    }
    // 0. Favorite Bypass (Header-level info available for encrypted)
    if (fav_bypass) {
        auto fromNode = nodeDB->getMeshNode(mp.from);
        if (fromNode && nexus_is_favorite(fromNode)) return false;
        if (mp.to != 0xFFFFFFFF) {
            auto toNode = nodeDB->getMeshNode(mp.to);
            if (toNode && nexus_is_favorite(toNode)) return false;
        }
    }


    // 0.2 Nexus Ignore List Check
    if (isNexusIgnored(mp.from)) {
        LOG_DEBUG("MTC: Private packet dropped from Nexus-Ignored node !%08x", (unsigned int)mp.from);
        fw_drop_nexus_ignored++;
        return true;
    }

    // 1. PreHop Filter — two branches: genuine mp.pki_encrypted (decoded path sees local PKI,
    // opaque path never does) and the foreign-PKI heuristic for un-decodable PKI DMs where
    // mp.pki_encrypted is false but the wire shape signals PKI from another node.
    if (mp.pki_encrypted) {
        // Genuine PKI on the opaque path: this fires only for already-decoded PKI packets that
        // came back through this path (rare; mostly internal/MQTT-uplink replays). drop_pki_local
        // deliberately omitted — a local PKI packet is decodable by us and lands in
        // shouldDropPacket instead, where its drop_pki_local rule (with the auto-couple to
        // drop_pki) handles it.
        if (drop_pki) { fw_drop_pki++; return true; }
        if (drop_pki_remote) { fw_drop_pki_remote++; return true; }
        return false; // Bypass other rules — mp.channel is 0 for PKI, blacklist hashes never match.
    }
    if (looksLikeForeignPKI(mp)) {
        // Foreign PKI we couldn't decode. mp.channel is 0 on the wire (PKI signature) so the
        // channel blacklist/whitelist below can catch it via PRBLOCK ADD 0; drop_private also
        // fires because which_payload_variant == encrypted_tag. drop_pki_local omitted (no
        // local PKI on this path by definition).
        if (drop_pki) { fw_drop_pki++; return true; }
        if (drop_pki_remote) { fw_drop_pki_remote++; return true; }
        // Fall through to channel rules: PRBLOCK ADD 0, whitelist, drop_private, hop6/7.
    }

    // 1. Whitelist/Blacklist
    for(int i=0; i<16; i++) {
        if (nexus_ch_blacklist[i] != 0 && mp.channel == nexus_ch_blacklist[i]) { fw_drop_priv_blacklist++; return true; }
    }
    bool whitelistActive = false;
    bool onWhitelist = false;
    for(int i=0; i<16; i++) {
        if (nexus_ch_whitelist[i] != 0) { whitelistActive = true; if (mp.channel == nexus_ch_whitelist[i]) onWhitelist = true; }
    }
    if (whitelistActive && !onWhitelist) { fw_drop_priv_whitelist++; return true; }

    // 2. Rate Limiting (Nexus v28). checkRateLimit() handles both the
    //    decoded and encrypted variants; in this function we always
    //    hit the encrypted/PR bucket (idx=6). Engaged iff drop_rl_private
    //    is on AND rl_private_limit > 0; otherwise it's a no-op that
    //    doesn't tick rl_seen_private. Sits BEFORE the global drop_private
    //    so RL can drop a packet that drop_private would also drop
    //    (they're independent rules — RL limits throughput, drop_private
    //    is a blanket firewall on/off).
    if (!checkRateLimit(mp)) return true;

    // 3. Global Drop Private (encrypted-only - fw_drop_private counter is reserved for this rule)
    if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
        if (drop_private) { fw_drop_private++; return true; }
    }

    // 4. Hop Filter
    if (drop_hop6 && mp.hop_start == 6) { fw_drop_hop6++; return true; }
    if (drop_hop7 && mp.hop_start == 7) { fw_drop_hop7++; return true; }

#ifdef NEXUS_PRIVATE_BUILD
    // Nexus-28006 Step C (2026-09-24) — private extension point.
    if (privateShouldDropPrivate(mp)) {
        return true;
    }
#endif

    return false;
}

bool NexusModule::firewallDropsOutput(const meshtastic_MeshPacket &mp)
{
    // 0. Trusted Channel Bypass (trusted channels always allowed out)
    for (uint8_t i = 0; i < MAX_NUM_CHANNELS; i++) {
        meshtastic_Channel *c = &channels.getByIndex(i);
        if (c->role != meshtastic_Channel_Role_DISABLED) {
            if (mp.channel == i) {
                if (strcasecmp(c->settings.name, "Nexus") == 0 || strcasecmp(c->settings.name, "Nexus2") == 0) return false;
#ifdef NEXUS_TRUSTED_CHANNEL_1
                if (strcasecmp(c->settings.name, NEXUS_TRUSTED_CHANNEL_1) == 0) return false;
#endif
#ifdef NEXUS_TRUSTED_CHANNEL_2
                if (strcasecmp(c->settings.name, NEXUS_TRUSTED_CHANNEL_2) == 0) return false;
#endif
                for (int j = 0; j < 2; j++) {
                    if (trusted_ch_active[j] && strcasecmp(c->settings.name, trusted_ch_names[j]) == 0) return false;
                }
            }
        }
    }

    // 1. Favorite Bypass (If we are sending to a friend, don't block it)
    if (fav_bypass) {
        if (mp.to != 0xFFFFFFFF) {
            auto toNode = nodeDB->getMeshNode(mp.to);
            if (toNode && nexus_is_favorite(toNode)) return false;
        }
    }

    // 2. Port-based Outbound Filters
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        uint32_t port = mp.decoded.portnum;
        if (drop_out_nodeinfo && port == meshtastic_PortNum_NODEINFO_APP) { fw_drop_out_nodeinfo++; return true; }
        if (drop_out_position && port == meshtastic_PortNum_POSITION_APP) { fw_drop_out_position++; return true; }
        if (drop_out_telemetry && port == meshtastic_PortNum_TELEMETRY_APP) { fw_drop_out_telemetry++; return true; }
        if (drop_out_traceroute && port == meshtastic_PortNum_TRACEROUTE_APP) { fw_drop_out_traceroute++; return true; }
        if (drop_out_admin && port == meshtastic_PortNum_ADMIN_APP) { fw_drop_out_admin++; return true; }
        if (drop_out_routing && port == meshtastic_PortNum_ROUTING_APP) { fw_drop_out_routing++; return true; }
    }

#ifdef NEXUS_PRIVATE_BUILD
    // Nexus-28006 Step C (2026-09-24) — private extension point.
    if (privateShouldDropOutput(mp)) {
        return true;
    }
#endif

    return false;
}

#endif // !NEXUS_EXCLUDE_FIREWALL
