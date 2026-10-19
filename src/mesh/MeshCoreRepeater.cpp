#include "MeshCoreRepeater.h"

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

void MeshCoreRepeater::handleReceivedFrame(const uint8_t *raw, size_t len, int8_t snr)
{
    (void)snr; // logged by the caller together with RSSI
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

    uint32_t hash = hashFrame(raw, len);
    if (wasRecentlySeen(hash)) {
        rxDup++;
        return;
    }

    if (!canRelay())
        return;

    queueRelay(raw, len);
#endif
}

uint32_t MeshCoreRepeater::hashFrame(const uint8_t *raw, size_t len)
{
    // FNV-1a over the frame; small frames make collisions unlikely, and the dedup ring is only a
    // rate-limiter for a broadcast storm, not a correctness guarantee.
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= raw[i];
        hash *= 16777619u;
    }
    return hash;
}

bool MeshCoreRepeater::wasRecentlySeen(uint32_t hash)
{
    const uint32_t now = Time::getMillis();
    for (size_t i = 0; i < DEDUP_ENTRIES; i++) {
        if (dedup[i].hash == hash && Throttle::isWithinTimespanMs(dedup[i].seenAtMs, 15000)) {
            return true;
        }
    }
    dedup[dedupNext].hash = hash;
    dedup[dedupNext].seenAtMs = now;
    dedupNext = (dedupNext + 1) % DEDUP_ENTRIES;
    return false;
}

bool MeshCoreRepeater::canRelay() const
{
    // Same rebroadcast policy as Meshtastic's own relay (FloodingRouter::isRebroadcaster): a mute
    // node or a node with rebroadcast disabled relays nothing, MeshCore floods included.
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE ||
        config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE)
        return false;

    // Serialize the re-transmissions: the LoRa channel is half-duplex and shared with Meshtastic
    // traffic, so a raw relay must not stomp on our own TX or on another relay. lastRelayMs == 0
    // means "never relayed": an inactive cooldown must not suppress the first relay after boot.
    if (hasPendingFrame())
        return false;
    return lastRelayMs == 0 || !Throttle::isWithinTimespanMs(lastRelayMs, RELAY_COOLDOWN_MS);
}

void MeshCoreRepeater::queueRelay(const uint8_t *raw, size_t len)
{
    if (len > sizeof(pending))
        return;
    memcpy(pending, raw, len);
    pendingLen = len;
    lastRelayMs = Time::getMillis();
    txRelayed++;
}

size_t MeshCoreRepeater::takePendingFrame(uint8_t *buf, size_t bufLen)
{
    if (pendingLen == 0 || bufLen < pendingLen)
        return 0;
    memcpy(buf, pending, pendingLen);
    return pendingLen;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
