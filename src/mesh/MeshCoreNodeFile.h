#pragma once

#include "MeshCoreNodeStore.h"

/**
 * The image of the few MeshCore nodes kept across a reboot (favourites, then conversations, then the rest), and the
 * migration from the contacts file written before the node store existed.
 */
namespace MeshCoreNodeFile
{
static constexpr size_t SAVED_MAX = 16;

static constexpr uint8_t MAGIC[3] = {'M', 'C', 'N'};
static constexpr uint8_t VERSION = 2;
static constexpr size_t HEADER_LEN = 5;
static constexpr size_t RECORD_LEN = 32 + MeshCoreNode::NAME_LEN + 3 + MeshCoreNode::MAX_PATH_HOPS + 4 * 4;
static constexpr size_t IMAGE_MAX = HEADER_LEN + SAVED_MAX * RECORD_LEN;

// The contacts file before the node store: public key and a 40-byte name per record.
static constexpr uint8_t LEGACY_MAGIC[3] = {'M', 'C', 'C'};
static constexpr uint8_t LEGACY_VERSION = 1;
static constexpr size_t LEGACY_NAME_LEN = 40;
static constexpr size_t LEGACY_RECORD_LEN = 32 + LEGACY_NAME_LEN;

inline uint8_t *putU32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        *p++ = (uint8_t)(v >> (8 * i));
    return p;
}

inline const uint8_t *getU32(const uint8_t *p, uint32_t *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++)
        *v |= (uint32_t)*p++ << (8 * i);
    return p;
}

inline void writeRecord(uint8_t *p, const MeshCoreNode &n)
{
    memcpy(p, n.pub, 32);
    p += 32;
    memcpy(p, n.name, MeshCoreNode::NAME_LEN);
    p += MeshCoreNode::NAME_LEN;
    *p++ = n.type;
    *p++ = n.flags;
    *p++ = n.outPathLen;
    memcpy(p, n.outPath, MeshCoreNode::MAX_PATH_HOPS);
    p += MeshCoreNode::MAX_PATH_HOPS;
    p = putU32(p, n.lastAdvert);
    p = putU32(p, (uint32_t)n.latitudeMicro);
    p = putU32(p, (uint32_t)n.longitudeMicro);
    putU32(p, n.nodeNum);
}

/// Fill n from a record, keeping its recency stamps: the store owns those.
inline void readRecord(const uint8_t *p, MeshCoreNode *n)
{
    p += 32; // the key is already in place
    memcpy(n->name, p, MeshCoreNode::NAME_LEN);
    n->name[MeshCoreNode::NAME_LEN - 1] = 0;
    p += MeshCoreNode::NAME_LEN;
    n->type = *p++;
    n->flags = *p++;
    n->outPathLen = *p++;
    if (n->outPathLen > MeshCoreNode::MAX_PATH_HOPS)
        n->outPathLen = MeshCoreNode::PATH_UNKNOWN;
    memcpy(n->outPath, p, MeshCoreNode::MAX_PATH_HOPS);
    p += MeshCoreNode::MAX_PATH_HOPS;
    uint32_t v;
    p = getU32(p, &n->lastAdvert);
    p = getU32(p, &v);
    n->latitudeMicro = (int32_t)v;
    p = getU32(p, &v);
    n->longitudeMicro = (int32_t)v;
    getU32(p, &n->nodeNum);
}

/// Write the nodes worth keeping, best first. Returns the image length, 0 when cap is too small.
template <size_t N> size_t save(const MeshCoreNodeStore<N> &store, uint8_t *buf, size_t cap)
{
    if (!buf || cap < IMAGE_MAX)
        return 0;
    const MeshCoreNode *picked[SAVED_MAX];
    const size_t count = store.pickForSaving(picked, SAVED_MAX);

    memcpy(buf, MAGIC, sizeof(MAGIC));
    buf[3] = VERSION;
    buf[4] = (uint8_t)count;
    for (size_t i = 0; i < count; i++)
        writeRecord(&buf[HEADER_LEN + i * RECORD_LEN], *picked[i]);
    return HEADER_LEN + count * RECORD_LEN;
}

template <size_t N> size_t loadCurrent(MeshCoreNodeStore<N> &store, const uint8_t *buf, size_t len)
{
    const size_t count = buf[4];
    if (count > SAVED_MAX || len != HEADER_LEN + count * RECORD_LEN)
        return 0;
    // Best first on disk: load the worst first so the best ends up the most recent, and the save order survives.
    for (size_t i = count; i-- > 0;) {
        const uint8_t *rec = &buf[HEADER_LEN + i * RECORD_LEN];
        MeshCoreNode *n = store.upsert(rec);
        readRecord(rec, n);
        if (n->hadDm())
            store.noteDirectMessage(n);
    }
    return count;
}

/// Contacts from the old file come in as chat nodes with no node number yet: the companion assigns one.
template <size_t N> size_t loadLegacy(MeshCoreNodeStore<N> &store, const uint8_t *buf, size_t len)
{
    const size_t count = buf[4];
    if (count > SAVED_MAX || len != HEADER_LEN + count * LEGACY_RECORD_LEN)
        return 0;
    for (size_t i = count; i-- > 0;) {
        const uint8_t *rec = &buf[HEADER_LEN + i * LEGACY_RECORD_LEN];
        char name[LEGACY_NAME_LEN];
        memcpy(name, &rec[32], sizeof(name));
        name[sizeof(name) - 1] = 0;
        MeshCoreNode *n = store.upsert(rec);
        n->type = MeshCoreNode::CHAT;
        n->setName(name);
    }
    return count;
}

/// Changes when the saved set or its content changes, not when only its order does: worth a flash write.
template <size_t N> uint32_t fingerprint(const MeshCoreNodeStore<N> &store)
{
    const MeshCoreNode *picked[SAVED_MAX];
    const size_t count = store.pickForSaving(picked, SAVED_MAX);
    uint32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t rec[RECORD_LEN];
        writeRecord(rec, *picked[i]);
        uint32_t h = 2166136261u; // FNV-1a
        for (size_t k = 0; k < RECORD_LEN; k++)
            h = (h ^ rec[k]) * 16777619u;
        sum += h; // order independent
    }
    return sum ^ (uint32_t)count;
}

/// Load an image of either version into the store. Returns how many nodes it held, 0 when it is not one.
template <size_t N> size_t load(MeshCoreNodeStore<N> &store, const uint8_t *buf, size_t len)
{
    if (!buf || len < HEADER_LEN)
        return 0;
    if (memcmp(buf, MAGIC, sizeof(MAGIC)) == 0 && buf[3] == VERSION)
        return loadCurrent(store, buf, len);
    if (memcmp(buf, LEGACY_MAGIC, sizeof(LEGACY_MAGIC)) == 0 && buf[3] == LEGACY_VERSION)
        return loadLegacy(store, buf, len);
    return 0;
}
} // namespace MeshCoreNodeFile
