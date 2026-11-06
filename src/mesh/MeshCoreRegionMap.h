// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MAX_REGION_ENTRIES
#define MAX_REGION_ENTRIES 32
#endif

#define REGION_DENY_FLOOD 0x01
#define REGION_DENY_DIRECT 0x02

struct RegionEntry {
    uint16_t id = 0;
    uint16_t parent = 0;
    uint8_t flags = 0;
    char name[31] = {0};

    bool isWildcard() const { return id == 0; }
};

struct MeshCoreTransportKey {
    uint8_t key[16] = {0};

    uint16_t calcTransportCode(uint8_t payload_type, const uint8_t *payload, size_t payload_len) const;
    bool isNull() const;
};

class MeshCoreRegionMap
{
  private:
    uint16_t next_id = 1;
    uint16_t home_id = 0;
    uint16_t default_id = 0;
    uint16_t num_regions = 0;
    RegionEntry regions[MAX_REGION_ENTRIES];
    RegionEntry wildcard;

    void printChildRegions(int indent, const RegionEntry *parent, char *buf, size_t max_len, size_t &pos) const;
    void getAutoKeyFor(uint16_t id, const char *name, MeshCoreTransportKey &dest) const;

  public:
    MeshCoreRegionMap();

    static bool isNameChar(uint8_t c);

    RegionEntry *putRegion(const char *name, uint16_t parent_id, uint16_t id = 0);
    RegionEntry *findMatch(uint16_t transport_code, uint8_t payload_type, const uint8_t *payload, size_t payload_len,
                           uint8_t mask);
    RegionEntry &getWildcard() { return wildcard; }
    const RegionEntry &getWildcard() const { return wildcard; }

    RegionEntry *findByName(const char *name);
    const RegionEntry *findByName(const char *name) const;
    RegionEntry *findByNamePrefix(const char *prefix);
    const RegionEntry *findByNamePrefix(const char *prefix) const;
    RegionEntry *findById(uint16_t id);
    const RegionEntry *findById(uint16_t id) const;
    /// True when candidate_id is ancestor_id or one of its descendants (re-parenting would make a cycle).
    bool isSelfOrDescendant(uint16_t candidate_id, uint16_t ancestor_id) const;

    RegionEntry *getHomeRegion();
    const RegionEntry *getHomeRegion() const;
    void setHomeRegion(const RegionEntry *home);

    RegionEntry *getDefaultRegion();
    const RegionEntry *getDefaultRegion() const;
    void setDefaultRegion(const RegionEntry *def);

    bool removeRegion(const RegionEntry &region);
    void clear();

    int getCount() const { return num_regions; }
    const RegionEntry *getByIdx(int i) const { return (i >= 0 && i < num_regions) ? &regions[i] : nullptr; }

    int getTransportKeysFor(const RegionEntry &src, MeshCoreTransportKey dest[], int max_num) const;
    int exportNamesTo(char *dest, size_t max_len, uint8_t mask, bool invert = false) const;
    size_t exportTo(char *dest, size_t max_len) const;

    bool processDef(const char *defString, char *reply, size_t replyCap);

    // Serialization
    static constexpr size_t SERIALIZED_MAX_LEN = 12 + (MAX_REGION_ENTRIES * (4 + 31 + 1));
    size_t encode(uint8_t *out, size_t cap) const;
    bool decode(const uint8_t *in, size_t len);
};
