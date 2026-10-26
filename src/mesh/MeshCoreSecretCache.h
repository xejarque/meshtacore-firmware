#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * ECDH shared secrets of the peers we talk to, least recently used evicted first. A secret is derived again on a miss, so
 * this only saves the X25519 work; it is kept apart from the node store so each stored node does not carry 32 more bytes.
 */
template <size_t N> class MeshCoreSecretCache
{
  public:
    /// The cached secret for this peer, or null on a miss.
    const uint8_t *find(const uint8_t pub[32])
    {
        for (auto &e : entries)
            if (e.lastUsed && memcmp(e.pub, pub, 32) == 0) {
                e.lastUsed = ++clock;
                return e.secret;
            }
        return nullptr;
    }

    const uint8_t *put(const uint8_t pub[32], const uint8_t secret[32])
    {
        Entry &e = entries[victim()];
        memcpy(e.pub, pub, 32);
        memcpy(e.secret, secret, 32);
        e.lastUsed = ++clock;
        return e.secret;
    }

    /// Forget every secret: they were derived for an identity we no longer have.
    void clear()
    {
        for (auto &e : entries)
            e = Entry();
    }

  private:
    struct Entry {
        uint8_t pub[32] = {};
        uint8_t secret[32] = {};
        uint32_t lastUsed = 0; // 0 marks a free slot
    };
    Entry entries[N];
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
};
