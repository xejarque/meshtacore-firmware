#pragma once

#include "MeshCoreNodeStore.h"
#include <ErriezCRC32.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Image of the MeshCore admin passwords in /prefs/meshcore_admin.bin. The password is stored in the clear: it is what
 * the repeater expects, and encrypting it under a key that lives on the same flash would only hide it from the logs.
 */
namespace MeshCoreAdminFile
{
static constexpr size_t MAX_CREDENTIALS = 8;
static constexpr size_t PASSWORD_LEN = 16; // 15 characters + NUL, MeshCore's limit

static constexpr uint8_t MAGIC[3] = {'M', 'C', 'A'};
static constexpr uint8_t VERSION = 1;
static constexpr size_t HEADER_LEN = 5;
static constexpr size_t RECORD_LEN = 32 + 1 + MeshCoreNode::NAME_LEN + 1 + PASSWORD_LEN;
static constexpr size_t IMAGE_MAX = HEADER_LEN + MAX_CREDENTIALS * RECORD_LEN + 4;

struct Credential {
    uint8_t pub[32] = {};
    uint8_t type = 0;
    char name[MeshCoreNode::NAME_LEN] = {};
    uint8_t passLen = 0;
    char pass[PASSWORD_LEN] = {};
};

inline const uint8_t *getU32(const uint8_t *p, uint32_t *v)
{
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return p + 4;
}

inline uint8_t *putU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

inline void writeRecord(uint8_t *p, const Credential &c)
{
    memcpy(p, c.pub, 32);
    p += 32;
    *p++ = c.type;
    memcpy(p, c.name, MeshCoreNode::NAME_LEN);
    p += MeshCoreNode::NAME_LEN;
    *p++ = c.passLen;
    memcpy(p, c.pass, PASSWORD_LEN);
}

/// Returns false for anything not a complete, checksummed image of a known version.
inline bool readRecord(const uint8_t *p, Credential *c)
{
    memcpy(c->pub, p, 32);
    p += 32;
    c->type = *p++;
    memcpy(c->name, p, MeshCoreNode::NAME_LEN);
    c->name[MeshCoreNode::NAME_LEN - 1] = 0;
    p += MeshCoreNode::NAME_LEN;
    c->passLen = *p++;
    if (c->passLen >= PASSWORD_LEN)
        return false;
    memcpy(c->pass, p, PASSWORD_LEN);
    c->pass[c->passLen] = 0;
    return c->type == MeshCoreNode::REPEATER || c->type == MeshCoreNode::ROOM;
}

inline size_t encode(const Credential *creds, size_t count, uint8_t *buf, size_t cap)
{
    if (!buf || cap < IMAGE_MAX || count > MAX_CREDENTIALS || (count && !creds))
        return 0;

    memcpy(buf, MAGIC, sizeof(MAGIC));
    buf[3] = VERSION;
    buf[4] = (uint8_t)count;
    for (size_t i = 0; i < count; i++)
        writeRecord(&buf[HEADER_LEN + i * RECORD_LEN], creds[i]);

    const size_t bodyLen = HEADER_LEN + count * RECORD_LEN;
    putU32(&buf[bodyLen], crc32Buffer(buf, bodyLen));
    return bodyLen + 4;
}

inline size_t decode(const uint8_t *buf, size_t len, Credential *creds, size_t max, bool *valid = nullptr)
{
    if (valid)
        *valid = false;
    if (!buf || !creds || len < HEADER_LEN + 4 || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0 || buf[3] != VERSION)
        return 0;

    const size_t count = buf[4];
    if (count > MAX_CREDENTIALS || count > max || len != HEADER_LEN + count * RECORD_LEN + 4)
        return 0;

    const size_t bodyLen = HEADER_LEN + count * RECORD_LEN;
    uint32_t stored = 0;
    getU32(&buf[bodyLen], &stored);
    if (crc32Buffer(buf, bodyLen) != stored)
        return 0;

    for (size_t i = 0; i < count; i++)
        if (!readRecord(&buf[HEADER_LEN + i * RECORD_LEN], &creds[i]))
            return 0;
    if (valid)
        *valid = true;
    return count;
}
} // namespace MeshCoreAdminFile
