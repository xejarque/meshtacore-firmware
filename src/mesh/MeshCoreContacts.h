#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** A MeshCore peer we can exchange direct messages with, learned from its signed ADVERT. */
struct MeshCoreContact {
    static constexpr size_t NAME_LEN = 40;   // meshtastic_User.long_name
    static constexpr size_t PATH_BYTES_MAX = 64; // MeshCore MAX_PATH_SIZE

    uint8_t pub[32] = {};
    char name[NAME_LEN] = {};
    uint32_t nodeNum = 0;
    uint8_t secret[32] = {}; // ECDH shared secret, valid once hasSecret
    bool hasSecret = false;
    uint32_t lastUsed = 0; // LRU stamp; 0 marks a free slot

    // Route to this contact: the hash sequence a MeshCore TRACE walks, next hop first. Learned from the PATH frame the
    // peer returns after a flooded message, or, until one arrives, from the path of its last ADVERT (reversed).
    uint8_t path[PATH_BYTES_MAX] = {};
    uint8_t pathBytes = 0;      // 0 with pathValid is a direct neighbour
    uint8_t pathHashSize = 1;   // 1..3 bytes per hash
    bool pathValid = false;
    bool pathFromAdvert = false;
    uint32_t pathMs = 0; // when the route was learned
};

/**
 * Fixed-size contact table, least recently used entry evicted first. Lives in RAM; the companion saves the
 * favourite ones to flash.
 */
template <size_t N> class MeshCoreContacts
{
  public:
    /// Node number standing in for a MeshCore name: 'M' in the top byte, 24 bits hashed from the name. A contact and
    /// the sender of a channel text with the same name share it, so the client shows one node, not two.
    static uint32_t nodeNumFor(const char *name)
    {
        uint32_t h = 2166136261u; // FNV-1a
        for (const char *p = name; p && *p; p++)
            h = (h ^ (uint8_t)*p) * 16777619u;
        return 0x4D000000 | ((h ^ (h >> 24)) & 0x00FFFFFF);
    }

    /// Node number for a repeater, which has no contact entry: hashed from its public key.
    static uint32_t nodeNumForKey(const uint8_t pub[32])
    {
        uint32_t h = 2166136261u; // FNV-1a
        for (size_t i = 0; i < 32; i++)
            h = (h ^ pub[i]) * 16777619u;
        return 0x4D000000 | ((h ^ (h >> 24)) & 0x00FFFFFF);
    }

    /// Add the contact, or refresh its name. A full table gives up its least recently used entry.
    MeshCoreContact *upsert(const uint8_t pub[32], const char *name)
    {
        MeshCoreContact *c = findByPub(pub);
        if (!c) {
            c = &entries[victim()];
            *c = MeshCoreContact();
            memcpy(c->pub, pub, 32);
            c->nodeNum = freeNodeNum(name);
        }
        if (name) {
            strncpy(c->name, name, sizeof(c->name) - 1);
            c->name[sizeof(c->name) - 1] = 0;
        }
        touch(c);
        return c;
    }

    MeshCoreContact *findByPub(const uint8_t pub[32])
    {
        for (auto &e : entries)
            if (e.lastUsed && memcmp(e.pub, pub, 32) == 0)
                return &e;
        return nullptr;
    }

    /// The contact with exactly this name, or null when none or several have it (a name alone cannot pick one).
    MeshCoreContact *findUniqueByName(const char *name)
    {
        MeshCoreContact *found = nullptr;
        for (auto &e : entries) {
            if (e.lastUsed && strcmp(e.name, name) == 0) {
                if (found)
                    return nullptr;
                found = &e;
            }
        }
        return found;
    }

    MeshCoreContact *findByNodeNum(uint32_t nodeNum)
    {
        for (auto &e : entries)
            if (e.lastUsed && e.nodeNum == nodeNum)
                return &e;
        return nullptr;
    }

    /// Contacts whose public key starts with hash (the 1-byte hash collides, so there can be several).
    size_t findByHash(uint8_t hash, MeshCoreContact **out, size_t max)
    {
        size_t n = 0;
        for (auto &e : entries)
            if (e.lastUsed && e.pub[0] == hash && n < max)
                out[n++] = &e;
        return n;
    }

    /// Contacts whose public key starts with prefix (a MeshCore path hop is a 1..3 byte key prefix).
    size_t findByPubPrefix(const uint8_t *prefix, size_t len, MeshCoreContact **out, size_t max)
    {
        size_t n = 0;
        if (!prefix || len == 0 || len > 32)
            return 0;
        for (auto &e : entries)
            if (e.lastUsed && memcmp(e.pub, prefix, len) == 0 && n < max)
                out[n++] = &e;
        return n;
    }

    void touch(MeshCoreContact *c) { c->lastUsed = ++clock; }

    template <typename F> void forEach(F fn)
    {
        for (auto &e : entries)
            if (e.lastUsed)
                fn(e);
    }

  private:
    MeshCoreContact entries[N];
    uint32_t clock = 0;

    size_t victim() const
    {
        size_t best = 0;
        for (size_t i = 0; i < N; i++) {
            if (!entries[i].lastUsed)
                return i;
            if (entries[i].lastUsed < entries[best].lastUsed)
                best = i;
        }
        return best;
    }

    /// The hashed node number, moved on if another contact already owns it.
    uint32_t freeNodeNum(const char *name)
    {
        uint32_t num = nodeNumFor(name);
        while (findByNodeNum(num) || num == 0x4D430001) // 0x4D430001: the generic "MeshCore" node
            num = 0x4D000000 | ((num + 1) & 0x00FFFFFF);
        return num;
    }
};
