// SPDX-License-Identifier: AGPL-3.0-only
#include "MeshCoreRegionMap.h"
#include <ErriezCRC32.h>
#include <SHA256.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

static const char *skipHash(const char *name)
{
    return (name && *name == '#') ? name + 1 : name;
}

uint16_t MeshCoreTransportKey::calcTransportCode(uint8_t payload_type, const uint8_t *payload, size_t payload_len) const
{
    uint16_t code = 0;
    SHA256 sha;
    sha.resetHMAC(key, sizeof(key));
    sha.update(&payload_type, 1);
    if (payload && payload_len > 0)
        sha.update(payload, payload_len);
    sha.finalizeHMAC(key, sizeof(key), &code, 2);
    if (code == 0)
        code = 1;
    else if (code == 0xFFFF)
        code = 0xFFFE;
    return code;
}

bool MeshCoreTransportKey::isNull() const
{
    for (size_t i = 0; i < sizeof(key); i++) {
        if (key[i] != 0)
            return false;
    }
    return true;
}

MeshCoreRegionMap::MeshCoreRegionMap()
{
    next_id = 1;
    home_id = 0;
    default_id = 0;
    num_regions = 0;
    wildcard.id = 0;
    wildcard.parent = 0;
    wildcard.flags = 0; // Default: allow flood
    strcpy(wildcard.name, "*");
}

bool MeshCoreRegionMap::isNameChar(uint8_t c)
{
    return c == '-' || c == '$' || c == '#' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

RegionEntry *MeshCoreRegionMap::putRegion(const char *name, uint16_t parent_id, uint16_t id)
{
    if (!name || !*name)
        return nullptr;

    const char *sp = name;
    while (*sp) {
        if (!isNameChar((uint8_t)*sp))
            return nullptr;
        sp++;
    }

    RegionEntry *region = findByName(name);
    if (region) {
        if (region->id == 0 || isSelfOrDescendant(parent_id, region->id))
            return nullptr;
        region->parent = parent_id;
        return region;
    }

    if (num_regions >= MAX_REGION_ENTRIES)
        return nullptr;

    region = &regions[num_regions++];
    region->flags = 0; // Flood allowed by default
    region->id = (id == 0) ? next_id++ : id;
    if (region->id >= next_id)
        next_id = region->id + 1;

    strncpy(region->name, name, sizeof(region->name) - 1);
    region->name[sizeof(region->name) - 1] = '\0';
    region->parent = parent_id;
    return region;
}

bool MeshCoreRegionMap::isSelfOrDescendant(uint16_t candidate_id, uint16_t ancestor_id) const
{
    // Bounded walk: an existing cycle must not hang the check.
    for (int hops = 0; candidate_id != 0 && hops <= MAX_REGION_ENTRIES; hops++) {
        if (candidate_id == ancestor_id)
            return true;
        const RegionEntry *r = findById(candidate_id);
        if (!r)
            return false;
        candidate_id = r->parent;
    }
    return candidate_id != 0;
}

void MeshCoreRegionMap::getAutoKeyFor(uint16_t id, const char *name, MeshCoreTransportKey &dest) const
{
    (void)id;
    SHA256 sha;
    sha.update(name, strlen(name));
    sha.finalize(dest.key, sizeof(dest.key));
}

int MeshCoreRegionMap::getTransportKeysFor(const RegionEntry &src, MeshCoreTransportKey dest[], int max_num) const
{
    if (max_num <= 0)
        return 0;

    if (src.name[0] == '$') {
        return 0; // Private region without stored key
    } else if (src.name[0] == '#') {
        getAutoKeyFor(src.id, src.name, dest[0]);
        return 1;
    } else {
        char tmp[sizeof(src.name) + 1];
        tmp[0] = '#';
        memcpy(&tmp[1], src.name, sizeof(src.name) - 1);
        tmp[sizeof(src.name)] = '\0';
        getAutoKeyFor(src.id, tmp, dest[0]);
        return 1;
    }
}

RegionEntry *MeshCoreRegionMap::findMatch(uint16_t transport_code, uint8_t payload_type, const uint8_t *payload,
                                          size_t payload_len, uint8_t mask)
{
    for (int i = 0; i < num_regions; i++) {
        RegionEntry *region = &regions[i];
        if ((region->flags & mask) == 0) {
            MeshCoreTransportKey keys[4];
            int num = getTransportKeysFor(*region, keys, 4);
            for (int j = 0; j < num; j++) {
                uint16_t code = keys[j].calcTransportCode(payload_type, payload, payload_len);
                if (transport_code == code)
                    return region;
            }
        }
    }
    return nullptr;
}

RegionEntry *MeshCoreRegionMap::findByName(const char *name)
{
    if (!name)
        return nullptr;
    if (strcmp(name, "*") == 0)
        return &wildcard;

    const char *rawName = skipHash(name);
    for (int i = 0; i < num_regions; i++) {
        if (strcasecmp(rawName, skipHash(regions[i].name)) == 0)
            return &regions[i];
    }
    return nullptr;
}

const RegionEntry *MeshCoreRegionMap::findByName(const char *name) const
{
    if (!name)
        return nullptr;
    if (strcmp(name, "*") == 0)
        return &wildcard;

    const char *rawName = skipHash(name);
    for (int i = 0; i < num_regions; i++) {
        if (strcasecmp(rawName, skipHash(regions[i].name)) == 0)
            return &regions[i];
    }
    return nullptr;
}

RegionEntry *MeshCoreRegionMap::findByNamePrefix(const char *prefix)
{
    if (!prefix)
        return nullptr;
    if (strcmp(prefix, "*") == 0)
        return &wildcard;

    const char *rawPrefix = skipHash(prefix);
    size_t prefixLen = strlen(rawPrefix);
    RegionEntry *partial = nullptr;

    for (int i = 0; i < num_regions; i++) {
        const char *cand = skipHash(regions[i].name);
        if (strcasecmp(rawPrefix, cand) == 0)
            return &regions[i];
        if (strncasecmp(rawPrefix, cand, prefixLen) == 0)
            partial = &regions[i];
    }
    return partial;
}

const RegionEntry *MeshCoreRegionMap::findByNamePrefix(const char *prefix) const
{
    if (!prefix)
        return nullptr;
    if (strcmp(prefix, "*") == 0)
        return &wildcard;

    const char *rawPrefix = skipHash(prefix);
    size_t prefixLen = strlen(rawPrefix);
    const RegionEntry *partial = nullptr;

    for (int i = 0; i < num_regions; i++) {
        const char *cand = skipHash(regions[i].name);
        if (strcasecmp(rawPrefix, cand) == 0)
            return &regions[i];
        if (strncasecmp(rawPrefix, cand, prefixLen) == 0)
            partial = &regions[i];
    }
    return partial;
}

RegionEntry *MeshCoreRegionMap::findById(uint16_t id)
{
    if (id == 0)
        return &wildcard;
    for (int i = 0; i < num_regions; i++) {
        if (regions[i].id == id)
            return &regions[i];
    }
    return nullptr;
}

const RegionEntry *MeshCoreRegionMap::findById(uint16_t id) const
{
    if (id == 0)
        return &wildcard;
    for (int i = 0; i < num_regions; i++) {
        if (regions[i].id == id)
            return &regions[i];
    }
    return nullptr;
}

RegionEntry *MeshCoreRegionMap::getHomeRegion()
{
    return (home_id == 0) ? nullptr : findById(home_id);
}

const RegionEntry *MeshCoreRegionMap::getHomeRegion() const
{
    return (home_id == 0) ? nullptr : findById(home_id);
}

void MeshCoreRegionMap::setHomeRegion(const RegionEntry *home)
{
    home_id = home ? home->id : 0;
}

RegionEntry *MeshCoreRegionMap::getDefaultRegion()
{
    return (default_id == 0) ? nullptr : findById(default_id);
}

const RegionEntry *MeshCoreRegionMap::getDefaultRegion() const
{
    return (default_id == 0) ? nullptr : findById(default_id);
}

void MeshCoreRegionMap::setDefaultRegion(const RegionEntry *def)
{
    default_id = def ? def->id : 0;
}

bool MeshCoreRegionMap::removeRegion(const RegionEntry &region)
{
    if (region.id == 0)
        return false;

    for (int i = 0; i < num_regions; i++) {
        if (regions[i].parent == region.id)
            return false;
    }

    int idx = -1;
    for (int i = 0; i < num_regions; i++) {
        if (regions[i].id == region.id) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return false;

    if (home_id == region.id)
        home_id = 0;
    if (default_id == region.id)
        default_id = 0;

    for (int i = idx; i < num_regions - 1; i++) {
        regions[i] = regions[i + 1];
    }
    num_regions--;
    return true;
}

void MeshCoreRegionMap::clear()
{
    num_regions = 0;
    home_id = 0;
    default_id = 0;
    next_id = 1;
    wildcard.flags = 0;
}

void MeshCoreRegionMap::printChildRegions(int indent, const RegionEntry *parent, char *buf, size_t max_len, size_t &pos) const
{
    char line[64];
    int lineLen = 0;

    int ind = indent;
    while (ind > 0 && lineLen < (int)sizeof(line) - 1) {
        line[lineLen++] = ' ';
        ind--;
    }

    const bool allowFlood = !(parent->flags & REGION_DENY_FLOOD);
    const bool isHome = (parent->id != 0 && parent->id == home_id);
    const bool isDefault = (parent->id != 0 && parent->id == default_id);

    int n = snprintf(line + lineLen, sizeof(line) - lineLen, "%s%s%s%s\n", skipHash(parent->name), isHome ? "^" : "",
                     isDefault ? "*" : "", allowFlood ? " F" : "");
    if (n > 0)
        lineLen += std::min(n, (int)sizeof(line) - 1 - lineLen); // snprintf reports the untruncated length

    if (pos + lineLen < max_len) {
        memcpy(buf + pos, line, lineLen);
        pos += lineLen;
        buf[pos] = '\0';
    }

    // A loaded map may hold a cycle (a region that names itself or a descendant as parent): the depth cap keeps it from
    // recursing until the stack runs out. Real hierarchies are a few levels deep.
    constexpr int MAX_PRINT_DEPTH = 8;
    if (indent >= MAX_PRINT_DEPTH)
        return;
    for (int i = 0; i < num_regions; i++) {
        if (regions[i].parent == parent->id && &regions[i] != parent) {
            printChildRegions(indent + 1, &regions[i], buf, max_len, pos);
        }
    }
}

size_t MeshCoreRegionMap::exportTo(char *dest, size_t max_len) const
{
    if (!dest || max_len == 0)
        return 0;

    size_t pos = 0;
    dest[0] = '\0';
    printChildRegions(0, &wildcard, dest, max_len, pos);
    return pos;
}

int MeshCoreRegionMap::exportNamesTo(char *dest, size_t max_len, uint8_t mask, bool invert) const
{
    if (!dest || max_len == 0)
        return 0;

    dest[0] = '\0';
    size_t pos = 0;
    int count = 0;

    for (int i = 0; i < num_regions; i++) {
        const RegionEntry &r = regions[i];
        bool hasFlag = (r.flags & mask) != 0;
        bool match = invert ? hasFlag : !hasFlag;
        if (match) {
            const char *name = skipHash(r.name);
            size_t nlen = strlen(name);
            if (pos + nlen + 2 < max_len) {
                if (count > 0)
                    dest[pos++] = ' ';
                memcpy(dest + pos, name, nlen);
                pos += nlen;
                dest[pos] = '\0';
                count++;
            }
        }
    }
    return count;
}

static char *skipSpaces(char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static void rtrimSpaces(char *s)
{
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ')
        *--e = '\0';
}

static char *takeToken(char **cursor)
{
    char *p = skipSpaces(*cursor);
    if (*p == '\0') {
        *cursor = p;
        return nullptr;
    }
    char *tok = p;
    while (*p && *p != ' ')
        p++;
    if (*p)
        *p++ = '\0';
    *cursor = p;
    return tok;
}

static char *splitNameJump(char *tok)
{
    for (char *q = tok; *q; q++) {
        if (*q == '|' || *q == ',') {
            *q = '\0';
            char *jump = skipSpaces(q + 1);
            rtrimSpaces(jump);
            return jump;
        }
    }
    return nullptr;
}

bool MeshCoreRegionMap::processDef(const char *defString, char *reply, size_t replyCap)
{
    if (!defString) {
        snprintf(reply, replyCap, "Err - empty def");
        return false;
    }

    char buf[160];
    strncpy(buf, defString, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *payload = skipSpaces(buf);
    rtrimSpaces(payload);
    if (*payload == '\0') {
        snprintf(reply, replyCap, "Err - empty def");
        return false;
    }

    RegionEntry *cursor = &wildcard;
    for (char *tok; (tok = takeToken(&payload)) != nullptr;) {
        char *jump = splitNameJump(tok);
        char *name = skipSpaces(tok);
        if (*name == '\0') {
            snprintf(reply, replyCap, "Err - empty name");
            return false;
        }
        if (jump && *jump == '\0') {
            snprintf(reply, replyCap, "Err - empty jump");
            return false;
        }

        RegionEntry *r = putRegion(name, cursor->id);
        if (!r) {
            snprintf(reply, replyCap, "Err - put failed: %s", name);
            return false;
        }
        r->flags = 0; // Flood allowed

        if (jump) {
            RegionEntry *j = findByNamePrefix(jump);
            if (!j) {
                snprintf(reply, replyCap, "Err - unknown jump: %s", jump);
                return false;
            }
            cursor = j;
        } else {
            cursor = r;
        }
    }
    return true;
}

// Binary layout:
// 4 bytes: Magic 'M', 'C', 'R', 0x01
// 2 bytes: default_id
// 2 bytes: home_id
// 1 byte:  wildcard.flags
// 1 byte:  reserved (0)
// 2 bytes: next_id
// 2 bytes: num_regions
// Each entry:
//   2 bytes: id
//   2 bytes: parent
//   1 byte:  flags
//   31 bytes: name
// 4 bytes: CRC32
static constexpr uint8_t REGION_MAGIC[4] = {'M', 'C', 'R', 0x01};
static constexpr size_t ENTRY_SIZE = 2 + 2 + 1 + 31; // 36 bytes

size_t MeshCoreRegionMap::encode(uint8_t *out, size_t cap) const
{
    const size_t bodyLen = 14 + (num_regions * ENTRY_SIZE);
    const size_t totalLen = bodyLen + 4;
    if (cap < totalLen)
        return 0;

    memcpy(out, REGION_MAGIC, 4);
    out[4] = (uint8_t)default_id;
    out[5] = (uint8_t)(default_id >> 8);
    out[6] = (uint8_t)home_id;
    out[7] = (uint8_t)(home_id >> 8);
    out[8] = wildcard.flags;
    out[9] = 0;
    out[10] = (uint8_t)next_id;
    out[11] = (uint8_t)(next_id >> 8);
    out[12] = (uint8_t)num_regions;
    out[13] = (uint8_t)(num_regions >> 8);

    size_t offset = 14;
    for (int i = 0; i < num_regions; i++) {
        const RegionEntry &r = regions[i];
        out[offset++] = (uint8_t)r.id;
        out[offset++] = (uint8_t)(r.id >> 8);
        out[offset++] = (uint8_t)r.parent;
        out[offset++] = (uint8_t)(r.parent >> 8);
        out[offset++] = r.flags;
        memcpy(out + offset, r.name, 31);
        offset += 31;
    }

    uint32_t crc = crc32Buffer(out, bodyLen);
    for (size_t i = 0; i < 4; i++) {
        out[bodyLen + i] = (uint8_t)(crc >> (8 * i));
    }
    return totalLen;
}

bool MeshCoreRegionMap::decode(const uint8_t *in, size_t len)
{
    if (!in || len < 18)
        return false;
    if (memcmp(in, REGION_MAGIC, 4) != 0)
        return false;

    uint16_t count = (uint16_t)in[12] | ((uint16_t)in[13] << 8);
    if (count > MAX_REGION_ENTRIES)
        return false;

    const size_t bodyLen = 14 + (count * ENTRY_SIZE);
    if (len != bodyLen + 4)
        return false;

    uint32_t storedCrc = 0;
    for (size_t i = 0; i < 4; i++) {
        storedCrc |= (uint32_t)in[bodyLen + i] << (8 * i);
    }
    if (crc32Buffer(in, bodyLen) != storedCrc)
        return false;

    clear();
    default_id = (uint16_t)in[4] | ((uint16_t)in[5] << 8);
    home_id = (uint16_t)in[6] | ((uint16_t)in[7] << 8);
    wildcard.flags = in[8];
    next_id = (uint16_t)in[10] | ((uint16_t)in[11] << 8);
    num_regions = count;

    size_t offset = 14;
    for (int i = 0; i < count; i++) {
        RegionEntry &r = regions[i];
        r.id = (uint16_t)in[offset] | ((uint16_t)in[offset + 1] << 8);
        r.parent = (uint16_t)in[offset + 2] | ((uint16_t)in[offset + 3] << 8);
        r.flags = in[offset + 4];
        memcpy(r.name, in + offset + 5, 31);
        r.name[30] = '\0';
        offset += ENTRY_SIZE;
    }
    return true;
}
