#pragma once

#include <ErriezCRC32.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * The MeshCore settings Nexus changes at run time: whether this node relays MeshCore floods, and how often it floods
 * its own ADVERT. Kept in a file of its own (/prefs/meshcore_config.bin, written by /nexus SAVE), not in the Nexus vault.
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

    RelayMode relay = RELAY_AUTO;
    uint8_t advertHours = 0; // 0 = DEFAULT_ADVERT_HOURS

    /// The ADVERT period in hours, with the built-in default filled in.
    uint8_t effectiveAdvertHours() const { return advertHours ? advertHours : DEFAULT_ADVERT_HOURS; }

    /// True for an ADVERT period the user may set.
    static bool validAdvertHours(uint32_t hours) { return hours >= MIN_ADVERT_HOURS && hours <= MAX_ADVERT_HOURS; }

    // Image layout: magic, version, relay, advert hours, then a CRC-32 (little-endian) over the first BODY_LEN bytes.
    static constexpr uint8_t MAGIC[3] = {'M', 'C', 'F'};
    static constexpr uint8_t VERSION = 1;
    static constexpr size_t BODY_LEN = 6;
    static constexpr size_t IMAGE_LEN = BODY_LEN + 4;

    /// Write the image of c into out. Returns IMAGE_LEN, or 0 if cap is too small.
    static size_t encode(const MeshCoreConfig &c, uint8_t *out, size_t cap)
    {
        if (cap < IMAGE_LEN)
            return 0;
        memcpy(out, MAGIC, sizeof(MAGIC));
        out[3] = VERSION;
        out[4] = (uint8_t)c.relay;
        out[5] = c.advertHours;
        const uint32_t crc = crc32Buffer(out, BODY_LEN);
        for (size_t i = 0; i < 4; i++)
            out[BODY_LEN + i] = (uint8_t)(crc >> (8 * i));
        return IMAGE_LEN;
    }

    /// Read an image written by encode(). Returns false, and leaves *c untouched, for a wrong length, magic, version,
    /// checksum or value: a bad file applies nothing, so the node keeps the built-in behaviour as a whole.
    static bool decode(const uint8_t *buf, size_t len, MeshCoreConfig *c)
    {
        if (len != IMAGE_LEN || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0 || buf[3] != VERSION)
            return false;
        uint32_t stored = 0;
        for (size_t i = 0; i < 4; i++)
            stored |= (uint32_t)buf[BODY_LEN + i] << (8 * i);
        if (crc32Buffer(buf, BODY_LEN) != stored)
            return false;
        const uint8_t relay = buf[4];
        const uint8_t hours = buf[5];
        if (relay > RELAY_ON || (hours != 0 && !validAdvertHours(hours)))
            return false;
        c->relay = (RelayMode)relay;
        c->advertHours = hours;
        return true;
    }
};
