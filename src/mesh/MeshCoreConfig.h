#pragma once

#include <ErriezCRC32.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * The MeshCore settings Nexus changes at run time: whether this node relays MeshCore floods, how often it floods
 * its own ADVERT, the path hash size of the floods it originates, and how deep a flood may go before it is not relayed.
 * Kept in a file of its own (/prefs/meshcore_config.bin, written by /mtc SAVE), not in the Nexus vault.
 *
 * Every field's zero is the built-in behaviour, so a default config is all zeros and a missing file changes nothing.
 */
struct MeshCoreConfig {
    enum RelayMode : uint8_t {
        RELAY_AUTO = 0, // the Meshtastic role and rebroadcast policy decide
        RELAY_OFF = 1,  // never relay MeshCore floods
        RELAY_ON = 2,   // relay them even when the role or the rebroadcast policy would not
    };

    static constexpr uint8_t DEFAULT_ADVERT_HOURS = 12; // MeshCore's flood ADVERT period, as shipped
    static constexpr uint8_t MIN_ADVERT_HOURS = 1;      // every node re-sends a flood, so it cannot be more frequent
    static constexpr uint8_t MAX_ADVERT_HOURS = 168;

    // Path hash size of the floods this node originates: how many bytes each repeater appends to the path. MeshCore allows 1..3.
    static constexpr uint8_t DEFAULT_PATH_HASH_SIZE = 2;
    static constexpr uint8_t MIN_PATH_HASH_SIZE = 1;
    static constexpr uint8_t MAX_PATH_HASH_SIZE = 3;

    // Flood depth limits (MeshCore flood_max, flood_max_unscoped, flood_max_advert): a flood that has crossed this many
    // repeaters is not relayed. The hop count is 6 bits, so 64 is the same as no limit.
    static constexpr uint8_t DEFAULT_FLOOD_MAX = 64;
    static constexpr uint8_t DEFAULT_FLOOD_MAX_UNSCOPED = 64;
    static constexpr uint8_t DEFAULT_FLOOD_MAX_ADVERT = 8;
    static constexpr uint8_t MIN_FLOOD_MAX = 1;
    static constexpr uint8_t MAX_FLOOD_MAX = 64;

    // Firewall rules, set with /mtc DROP <tag> like Nexus's. A set bit drops that kind of frame before the relay or the
    // companion sees it. Tags in kDropRules (below).
    static constexpr uint8_t DROP_ADVERT = 1 << 0;   // MCAD
    static constexpr uint8_t DROP_REQUEST = 1 << 1;  // MCRQ  requests, responses and anonymous requests
    static constexpr uint8_t DROP_TEXT = 1 << 2;     // MCTX  direct text messages
    static constexpr uint8_t DROP_GROUP = 1 << 3;    // MCGR  group text and data
    static constexpr uint8_t DROP_ACK = 1 << 4;      // MCAK  acknowledgements
    static constexpr uint8_t DROP_PATH = 1 << 5;     // MCPA  returned paths
    static constexpr uint8_t DROP_TRACE = 1 << 6;    // MCTR  traces
    static constexpr uint8_t DROP_SOURCE = 1 << 7;   // MCSRC also cut an ignored node's direct messages by its 1-byte source hash
    static constexpr size_t DROP_RULE_COUNT = 8;
    struct DropRule {
        const char *tag;
        uint8_t bit;
    };
    static constexpr DropRule DROP_RULES[DROP_RULE_COUNT] = {
        {"MCAD", DROP_ADVERT}, {"MCRQ", DROP_REQUEST}, {"MCTX", DROP_TEXT}, {"MCGR", DROP_GROUP},
        {"MCAK", DROP_ACK},    {"MCPA", DROP_PATH},    {"MCTR", DROP_TRACE}, {"MCSRC", DROP_SOURCE},
    };

    RelayMode relay = RELAY_AUTO;
    uint8_t advertHours = 0;      // 0 = DEFAULT_ADVERT_HOURS
    uint8_t pathHashSize = 0;     // 0 = DEFAULT_PATH_HASH_SIZE
    uint8_t floodMax = 0;         // 0 = DEFAULT_FLOOD_MAX
    uint8_t floodMaxUnscoped = 0; // 0 = DEFAULT_FLOOD_MAX_UNSCOPED
    uint8_t floodMaxAdvert = 0;   // 0 = DEFAULT_FLOOD_MAX_ADVERT
    uint8_t dropMask = 0;         // DROP_* bits; 0 drops nothing
    uint8_t advertGapMin = 0;     // minutes between two adverts of one key that are kept; 0 = no limit
    uint8_t hopRatePerMin = 0;    // frames a minute accepted from one last-hop repeater; 0 = no limit

    /// The ADVERT period in hours, with the built-in default filled in.
    uint8_t effectiveAdvertHours() const { return advertHours ? advertHours : DEFAULT_ADVERT_HOURS; }

    /// True for an ADVERT period the user may set.
    static bool validAdvertHours(uint32_t hours) { return hours >= MIN_ADVERT_HOURS && hours <= MAX_ADVERT_HOURS; }

    /// The values in effect, with the built-in defaults filled in.
    uint8_t effectivePathHashSize() const { return pathHashSize ? pathHashSize : DEFAULT_PATH_HASH_SIZE; }
    uint8_t effectiveFloodMax() const { return floodMax ? floodMax : DEFAULT_FLOOD_MAX; }
    uint8_t effectiveFloodMaxUnscoped() const { return floodMaxUnscoped ? floodMaxUnscoped : DEFAULT_FLOOD_MAX_UNSCOPED; }
    uint8_t effectiveFloodMaxAdvert() const { return floodMaxAdvert ? floodMaxAdvert : DEFAULT_FLOOD_MAX_ADVERT; }

    /// True for a path hash size or a flood limit the user may set.
    static bool validPathHashSize(uint32_t bytes) { return bytes >= MIN_PATH_HASH_SIZE && bytes <= MAX_PATH_HASH_SIZE; }
    static bool validFloodMax(uint32_t hops) { return hops >= MIN_FLOOD_MAX && hops <= MAX_FLOOD_MAX; }

    // Image layout: magic, version, relay, advert hours, path hash size, flood max, unscoped, advert, then a CRC-32
    // (little-endian) over the first BODY_LEN bytes. Version 3 adds the drop mask, the advert gap and the hop rate;
    // version 1 stopped after the advert hours and version 2 after the flood limits. Both are still read.
    static constexpr uint8_t MAGIC[3] = {'M', 'C', 'F'};
    static constexpr uint8_t VERSION = 3;
    static constexpr size_t BODY_LEN_V1 = 6;
    static constexpr size_t BODY_LEN_V2 = 10;
    static constexpr size_t BODY_LEN = 13;
    static constexpr size_t IMAGE_LEN = BODY_LEN + 4;
    static constexpr size_t IMAGE_LEN_V1 = BODY_LEN_V1 + 4;

    /// Write the image of c into out. Returns IMAGE_LEN, or 0 if cap is too small.
    static size_t encode(const MeshCoreConfig &c, uint8_t *out, size_t cap)
    {
        if (cap < IMAGE_LEN)
            return 0;
        memcpy(out, MAGIC, sizeof(MAGIC));
        out[3] = VERSION;
        out[4] = (uint8_t)c.relay;
        out[5] = c.advertHours;
        out[6] = c.pathHashSize;
        out[7] = c.floodMax;
        out[8] = c.floodMaxUnscoped;
        out[9] = c.floodMaxAdvert;
        out[10] = c.dropMask;
        out[11] = c.advertGapMin;
        out[12] = c.hopRatePerMin;
        const uint32_t crc = crc32Buffer(out, BODY_LEN);
        for (size_t i = 0; i < 4; i++)
            out[BODY_LEN + i] = (uint8_t)(crc >> (8 * i));
        return IMAGE_LEN;
    }

    /// Read an image written by encode(). Returns false, and leaves *c untouched, for a wrong length, magic, version,
    /// checksum or value: a bad file applies nothing, so the node keeps the built-in behaviour as a whole.
    static bool decode(const uint8_t *buf, size_t len, MeshCoreConfig *c)
    {
        if (len < 4 || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0)
            return false;
        const uint8_t version = buf[3];
        const size_t bodyLen = version == VERSION ? BODY_LEN : version == 2 ? BODY_LEN_V2 : version == 1 ? BODY_LEN_V1 : 0;
        if (bodyLen == 0 || len != bodyLen + 4)
            return false;
        uint32_t stored = 0;
        for (size_t i = 0; i < 4; i++)
            stored |= (uint32_t)buf[bodyLen + i] << (8 * i);
        if (crc32Buffer(buf, bodyLen) != stored)
            return false;

        MeshCoreConfig parsed;
        const uint8_t relay = buf[4];
        const uint8_t hours = buf[5];
        if (relay > RELAY_ON || (hours != 0 && !validAdvertHours(hours)))
            return false;
        parsed.relay = (RelayMode)relay;
        parsed.advertHours = hours;
        if (version >= 2) {
            parsed.pathHashSize = buf[6];
            parsed.floodMax = buf[7];
            parsed.floodMaxUnscoped = buf[8];
            parsed.floodMaxAdvert = buf[9];
            if ((parsed.pathHashSize != 0 && !validPathHashSize(parsed.pathHashSize)) ||
                (parsed.floodMax != 0 && !validFloodMax(parsed.floodMax)) ||
                (parsed.floodMaxUnscoped != 0 && !validFloodMax(parsed.floodMaxUnscoped)) ||
                (parsed.floodMaxAdvert != 0 && !validFloodMax(parsed.floodMaxAdvert)))
                return false;
        }
        if (version >= 3) {
            parsed.dropMask = buf[10];
            parsed.advertGapMin = buf[11];
            parsed.hopRatePerMin = buf[12];
        }
        *c = parsed;
        return true;
    }
};
