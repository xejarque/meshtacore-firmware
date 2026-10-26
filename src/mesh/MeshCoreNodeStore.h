#pragma once

#include "meshUtils.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MESHCORE_NODES_MAX
#define MESHCORE_NODES_MAX 350 // MeshCore's own MAX_CONTACTS on the ThinkNode M1
#endif

/** A MeshCore node learned from its signed ADVERT: MeshCore's ContactInfo, minus what we do not use. */
struct MeshCoreNode {
    static constexpr size_t NAME_LEN = 32; // MeshCore's ContactInfo.name
    static constexpr size_t MAX_PATH_HOPS = 16; // MeshCore allows 64; real routes stay well below 16
    static constexpr uint8_t PATH_UNKNOWN = 0xFF;

    enum Type : uint8_t { CHAT = 1, REPEATER = 2, ROOM = 3, SENSOR = 4 }; // MeshCore ADV_TYPE_*
    enum Flag : uint8_t { FAVORITE = 1 << 0, HAD_DM = 1 << 1 };

    uint8_t pub[32] = {};
    char name[NAME_LEN] = {};
    uint8_t type = 0;
    uint8_t flags = 0;
    uint8_t outPathLen = PATH_UNKNOWN;
    uint8_t outPath[MAX_PATH_HOPS] = {};
    uint32_t lastAdvert = 0; // the advert timestamp, by THEIR clock
    uint32_t lastMod = 0;    // recency stamp, by our clock; 0 marks a free slot
    uint32_t lastDm = 0;     // recency stamp of the last direct message either way
    int32_t latitudeMicro = 0, longitudeMicro = 0;
    uint32_t nodeNum = 0;

    bool isFavorite() const { return flags & FAVORITE; }
    bool hadDm() const { return flags & HAD_DM; }
    /// Favourites and peers we have messaged are what the user cares about: the last to be evicted, the first saved.
    bool isKept() const { return flags & (FAVORITE | HAD_DM); }
    bool hasOutPath() const { return outPathLen != PATH_UNKNOWN; }

    void setName(const char *s)
    {
        const size_t len = s ? utf8PrefixLen(s, NAME_LEN - 1) : 0;
        if (len)
            memcpy(name, s, len);
        memset(&name[len], 0, NAME_LEN - len);
    }
};

/**
 * Every MeshCore node we know, in RAM, kept apart from the Meshtastic node database so MeshCore traffic never takes a
 * Meshtastic slot. A full table gives up its oldest entry, sparing favourites and peers we have messaged while it can.
 */
template <size_t N> class MeshCoreNodeStore
{
  public:
    /// The node with this key, created if new. Refreshes its recency either way.
    MeshCoreNode *upsert(const uint8_t pub[32])
    {
        MeshCoreNode *n = find(pub);
        if (!n) {
            n = &entries[victim()];
            *n = MeshCoreNode();
            memcpy(n->pub, pub, 32);
        }
        touch(n);
        return n;
    }

    MeshCoreNode *find(const uint8_t pub[32])
    {
        for (auto &e : entries)
            if (e.lastMod && memcmp(e.pub, pub, 32) == 0)
                return &e;
        return nullptr;
    }

    MeshCoreNode *findByNodeNum(uint32_t nodeNum)
    {
        for (auto &e : entries)
            if (e.lastMod && e.nodeNum == nodeNum)
                return &e;
        return nullptr;
    }

    /// Nodes whose public key starts with prefix (MeshCore hashes collide, so there can be several). Returns how many
    /// match; up to max are written to out.
    size_t findByPrefix(const uint8_t *prefix, uint8_t size, MeshCoreNode **out, size_t max)
    {
        size_t total = 0;
        for (auto &e : entries) {
            if (!e.lastMod || memcmp(e.pub, prefix, size) != 0)
                continue;
            if (total < max && out)
                out[total] = &e;
            total++;
        }
        return total;
    }

    void touch(MeshCoreNode *n) { n->lastMod = ++clock; }

    void noteDirectMessage(MeshCoreNode *n)
    {
        n->flags |= MeshCoreNode::HAD_DM;
        n->lastDm = ++clock;
    }

    void setFavorite(MeshCoreNode *n, bool on)
    {
        if (on)
            n->flags |= MeshCoreNode::FAVORITE;
        else
            n->flags &= ~MeshCoreNode::FAVORITE;
    }

    bool remove(const uint8_t pub[32])
    {
        MeshCoreNode *n = find(pub);
        if (n)
            *n = MeshCoreNode();
        return n != nullptr;
    }

    size_t count() const
    {
        size_t n = 0;
        for (const auto &e : entries)
            if (e.lastMod)
                n++;
        return n;
    }

    /// The nodes worth saving, best first: favourites, then peers we have messaged, then the rest, newest first
    /// within each. Returns how many were written to out.
    size_t pickForSaving(const MeshCoreNode **out, size_t max) const
    {
        size_t picked = 0;
        for (; picked < max; picked++) {
            const MeshCoreNode *best = nullptr;
            for (const auto &e : entries)
                if (e.lastMod && !contains(out, picked, &e) && (!best || savesBefore(e, *best)))
                    best = &e;
            if (!best)
                break;
            out[picked] = best;
        }
        return picked;
    }

    template <typename F> void forEach(F fn)
    {
        for (auto &e : entries)
            if (e.lastMod)
                fn(e);
    }

  private:
    MeshCoreNode entries[N];
    uint32_t clock = 0;

    /// A free slot, else the oldest node not kept, else the oldest of all.
    size_t victim() const
    {
        size_t oldest = N, oldestUnkept = N;
        for (size_t i = 0; i < N; i++) {
            const MeshCoreNode &e = entries[i];
            if (!e.lastMod)
                return i;
            if (oldest == N || e.lastMod < entries[oldest].lastMod)
                oldest = i;
            if (!e.isKept() && (oldestUnkept == N || e.lastMod < entries[oldestUnkept].lastMod))
                oldestUnkept = i;
        }
        return oldestUnkept != N ? oldestUnkept : oldest;
    }

    static uint8_t saveRank(const MeshCoreNode &n) { return n.isFavorite() ? 0 : n.hadDm() ? 1 : 2; }

    /// Peers we have messaged rank by the conversation, not by how recently they last advertised.
    static uint32_t recency(const MeshCoreNode &n) { return saveRank(n) == 1 ? n.lastDm : n.lastMod; }

    static bool savesBefore(const MeshCoreNode &a, const MeshCoreNode &b)
    {
        if (saveRank(a) != saveRank(b))
            return saveRank(a) < saveRank(b);
        return recency(a) > recency(b);
    }

    static bool contains(const MeshCoreNode **list, size_t n, const MeshCoreNode *x)
    {
        for (size_t i = 0; i < n; i++)
            if (list[i] == x)
                return true;
        return false;
    }
};
