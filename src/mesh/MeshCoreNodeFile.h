#pragma once

#include "MeshCoreNodeStore.h"
#include "MeshCoreRepeaterNames.h"

/**
 * The image of the few MeshCore nodes kept across a reboot (favourites, then conversations, then the rest), and the
 * migration from the contacts file written before the node store existed.
 *
 * The file is that image, then optionally the repeater names: a count byte and that many 13-byte records, oldest first, so
 * that reading them in order leaves the newest the most recent. The caller writes and reads that tail in pieces (the loop
 * stack of a small board cannot hold a table's worth of records), so this file only describes its records.
 */
namespace MeshCoreNodeFile
{
static constexpr size_t SAVED_MAX = 16;

static constexpr uint8_t MAGIC[3] = {'M', 'C', 'N'};
static constexpr uint8_t VERSION = 3; // 3: repeater names may follow the contacts; the contacts are laid out as in 2
static constexpr uint8_t PREVIOUS_VERSION = 2;
static constexpr size_t HEADER_LEN = 5;
static constexpr size_t RECORD_LEN = 32 + MeshCoreNode::NAME_LEN + 3 + MeshCoreNode::MAX_PATH_HOPS + 4 * 4;
static constexpr size_t IMAGE_MAX = HEADER_LEN + SAVED_MAX * RECORD_LEN;

/// A repeater name on disk: key prefix, name, SNR in quarter dB, hops plus one. The saved count is one byte.
static constexpr size_t REPEATER_RECORD_LEN = MeshCoreRepeaterName::PREFIX_LEN + MeshCoreRepeaterName::NAME_LEN + 2;
static constexpr size_t REPEATERS_SAVED_MAX = 255;

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

inline void writeRepeaterRecord(uint8_t *p, const MeshCoreRepeaterName &e)
{
    memcpy(p, e.prefix, MeshCoreRepeaterName::PREFIX_LEN);
    p += MeshCoreRepeaterName::PREFIX_LEN;
    memcpy(p, e.name, MeshCoreRepeaterName::NAME_LEN);
    p += MeshCoreRepeaterName::NAME_LEN;
    *p++ = (uint8_t)e.snrQuarter;
    *p = e.hopsPlusOne;
}

/// Restore one record into the table. A record with no key or no name names nothing and is dropped.
template <size_t N> bool readRepeaterRecord(MeshCoreRepeaterNames<N> &table, const uint8_t *p)
{
    const uint8_t *name = p + MeshCoreRepeaterName::PREFIX_LEN;
    bool keyed = false, named = false;
    for (size_t i = 0; i < MeshCoreRepeaterName::PREFIX_LEN; i++)
        keyed |= p[i] != 0;
    for (size_t i = 0; i < MeshCoreRepeaterName::NAME_LEN; i++)
        named |= name[i] != 0;
    if (!keyed || !named)
        return false;
    const uint8_t *tail = name + MeshCoreRepeaterName::NAME_LEN;
    table.restore(p, (const char *)name, (int8_t)tail[0], tail[1]);
    return true;
}

inline void writeRecord(uint8_t *p, const MeshCoreNode &n)
{
    memcpy(p, n.pub, 32);
    p += 32;
    memcpy(p, n.name, MeshCoreNode::NAME_LEN);
    p += MeshCoreNode::NAME_LEN;
    *p++ = n.type;
    *p++ = n.flags & ~MeshCoreNode::HAS_OUT_PATH; // carried by the length, as MeshCore saves it
    *p++ = n.hasOutPath() ? n.outPathLen : MeshCoreNode::PATH_UNKNOWN;
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
    n->flags = *p++ & MeshCoreNode::KNOWN_FLAGS & ~MeshCoreNode::HAS_OUT_PATH;
    n->outPathLen = *p++;
    if (n->outPathLen <= MeshCoreNode::MAX_PATH_HOPS)
        n->flags |= MeshCoreNode::HAS_OUT_PATH;
    else
        n->outPathLen = 0;
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
    size_t loaded = 0;
    for (size_t i = count; i-- > 0;) {
        const uint8_t *rec = &buf[HEADER_LEN + i * RECORD_LEN];
        if (rec[32 + MeshCoreNode::NAME_LEN] == MeshCoreNode::REPEATER)
            continue; // an earlier build kept repeaters here; they have their own table now and are learned again
        MeshCoreNode *n = store.upsert(rec);
        readRecord(rec, n);
        if (n->hadDm())
            store.noteDirectMessage(n);
        loaded++;
    }
    return loaded;
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
        // lastAdvert and the position move on almost every advert heard; a flash write for those alone would wear the
        // flash for nothing. They are the three u32 before the trailing nodeNum.
        memset(&rec[RECORD_LEN - 16], 0, 12);
        uint32_t h = 2166136261u; // FNV-1a
        for (size_t k = 0; k < RECORD_LEN; k++)
            h = (h ^ rec[k]) * 16777619u;
        sum += h; // order independent
    }
    return sum ^ (uint32_t)count;
}

/// How many bytes of the file the nodes image takes, 0 when it does not start with one. What follows it, in a file of the
/// current version, is the repeater names.
inline size_t imageLength(const uint8_t *buf, size_t len)
{
    if (!buf || len < HEADER_LEN)
        return 0;
    const size_t count = buf[4];
    if (memcmp(buf, MAGIC, sizeof(MAGIC)) == 0 && (buf[3] == VERSION || buf[3] == PREVIOUS_VERSION))
        return count <= SAVED_MAX && len >= HEADER_LEN + count * RECORD_LEN ? HEADER_LEN + count * RECORD_LEN : 0;
    if (memcmp(buf, LEGACY_MAGIC, sizeof(LEGACY_MAGIC)) == 0 && buf[3] == LEGACY_VERSION)
        return count <= SAVED_MAX && len >= HEADER_LEN + count * LEGACY_RECORD_LEN ? HEADER_LEN + count * LEGACY_RECORD_LEN : 0;
    return 0;
}

/// Load an image of either version into the store. Returns how many nodes it held, 0 when it is not one.
template <size_t N> size_t load(MeshCoreNodeStore<N> &store, const uint8_t *buf, size_t len)
{
    if (!buf || len < HEADER_LEN)
        return 0;
    if (memcmp(buf, MAGIC, sizeof(MAGIC)) == 0 && (buf[3] == VERSION || buf[3] == PREVIOUS_VERSION))
        return loadCurrent(store, buf, len);
    if (memcmp(buf, LEGACY_MAGIC, sizeof(LEGACY_MAGIC)) == 0 && buf[3] == LEGACY_VERSION)
        return loadLegacy(store, buf, len);
    return 0;
}
} // namespace MeshCoreNodeFile
