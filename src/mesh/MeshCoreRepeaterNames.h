#pragma once

#include "MeshCoreNodeStore.h" // nodeNumForKey
#include "configuration.h"     // the ARCH_* macros the memory classes key off
#include "memory/MemClass.h"
#include "meshUtils.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h> // strncasecmp

// 16 B plus the name an entry, all in RAM. A repeater is only named on a path, so far fewer are worth remembering than
// contacts, and the oldest heard is the first to go. The ThinkNode M1 (nRF52 with a screen and a GPS) is the tight one,
// measured with 88 % of its heap used: it keeps 48.
#ifndef MESHCORE_REPEATER_NAMES_MAX
#if MESHTASTIC_MEM_CLASS <= MEM_CLASS_TINY
#define MESHCORE_REPEATER_NAMES_MAX 32
#elif MESHTASTIC_MEM_CLASS == MEM_CLASS_SMALL
#if HAS_SCREEN && defined(ARCH_NRF52) && defined(GPS_RX_PIN)
#define MESHCORE_REPEATER_NAMES_MAX 48
#else
#define MESHCORE_REPEATER_NAMES_MAX 64
#endif
#elif MESHTASTIC_MEM_CLASS == MEM_CLASS_MEDIUM
#define MESHCORE_REPEATER_NAMES_MAX 128
#else
#define MESHCORE_REPEATER_NAMES_MAX 256
#endif
#endif

// Bytes of name an entry keeps. The whole name lives in the node DB while the node is there; this one only stands in once
// the node DB has evicted it, so an nRF52 keeps 12 bytes and boards with RAM to spare keep MeshCore's whole 32.
#ifndef MESHCORE_REPEATER_NAME_LEN
#if defined(ARCH_NRF52)
#define MESHCORE_REPEATER_NAME_LEN 12
#else
#define MESHCORE_REPEATER_NAME_LEN 32
#endif
#endif

/**
 * A MeshCore node heard over RF, known by the start of its key: what a path needs to name it (the path carries hashes of 1 to
 * 3 bytes of the key), the node number it is announced under, a short name and how and when it was last heard. The
 * companion keeps repeaters in a table of them; a relay without a companion keeps its direct neighbours in one. Every
 * default is zero, so a table of them lands in .bss.
 */
struct MeshCoreRepeaterName {
    static constexpr size_t PREFIX_LEN = 3; // the longest hash a MeshCore path carries
    static constexpr size_t NAME_LEN = MESHCORE_REPEATER_NAME_LEN; // zero padded, not terminated when it fills them all
    static constexpr uint8_t HOPS_UNKNOWN = 0xFF;
    static constexpr uint8_t TYPE_REPEATER = 2; // MeshCore ADV_TYPE_REPEATER

    uint8_t prefix[PREFIX_LEN] = {};
    uint8_t type = 0;       // MeshCore ADV_TYPE_*; 0 when restored from a file that did not keep it
    uint32_t nodeNum = 0;   // MeshCoreNodeStore::nodeNumForKey of the whole key; 0 when restored from such a file
    uint32_t heardAt = 0;   // Time::getUptimeSecs() + 1 when last heard; 0 when only restored from flash this boot
    char name[NAME_LEN] = {};
    int8_t snrQuarter = 0;   // SNR in quarter dB, clamped: a path label needs no more
    uint8_t hopsPlusOne = 0; // hops the last advert travelled, plus one: 0 is "never heard over RF"
    uint16_t heard = 0;      // recency stamp: 0 marks a free slot

    uint8_t hops() const { return hopsPlusOne ? hopsPlusOne - 1 : HOPS_UNKNOWN; }
    float snr() const { return snrQuarter / 4.0f; }
    bool heardThisBoot() const { return heardAt != 0; }
    /// Seconds since it was last heard, given the uptime now. Meaningless unless heardThisBoot().
    uint32_t ageSecs(uint32_t uptimeSecs) const { return uptimeSecs + 1 > heardAt ? uptimeSecs + 1 - heardAt : 0; }

    /// The name as a terminated string.
    void copyName(char out[NAME_LEN + 1]) const
    {
        memcpy(out, name, NAME_LEN);
        out[NAME_LEN] = 0;
    }
};
static_assert(sizeof(MeshCoreRepeaterName) == 16 + MeshCoreRepeaterName::NAME_LEN, "an entry is budgeted at 16 B plus its name");

/// How a node was heard: what learn() records besides its key and name.
struct MeshCoreHeard {
    float snr = 0;
    uint8_t hops = MeshCoreRepeaterName::HOPS_UNKNOWN;
    uint8_t type = MeshCoreRepeaterName::TYPE_REPEATER;
    uint32_t uptimeSecs = 0; // Time::getUptimeSecs() when heard
};

namespace MeshCoreRepeaterLabel
{
/**
 * The few bytes of a repeater name worth keeping. Repeater names open with pictograms (a mountain, a tower) that would
 * eat the whole budget, so emoji, symbols and variation selectors are skipped, as are markdown delimiters, control bytes
 * and the blanks left at the ends. The cut falls on a UTF-8 boundary. A name made only of pictograms keeps its leading
 * bytes as they are rather than none. out holds NAME_LEN + 1 bytes. Returns the length.
 */
inline size_t make(const char *src, char *out)
{
    constexpr size_t cap = MeshCoreRepeaterName::NAME_LEN;
    size_t len = 0;
    out[0] = 0;
    if (!src)
        return 0;
    for (const uint8_t *p = (const uint8_t *)src; *p;) {
        const uint8_t c = *p;
        const size_t seq = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        size_t have = 1;
        while (have < seq && (p[have] & 0xC0) == 0x80)
            have++;
        const bool pictogram = c >= 0xF0 || c == 0xE2 || (c == 0xEF && have == 3 && p[1] == 0xB8);
        const bool markdown = c < 0x20 || c == '*' || c == '_' || c == '~' || c == '`' || c == '[' || c == ']';
        const bool leadingBlank = c == ' ' && len == 0;
        if (!pictogram && !markdown && !leadingBlank) {
            if (len + have > cap)
                break;
            memcpy(&out[len], p, have);
            len += have;
        }
        p += have;
    }
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = 0;
    if (len == 0 && src[0]) {
        len = utf8PrefixLen(src, cap);
        memcpy(out, src, len);
        out[len] = 0;
    }
    return len;
}
} // namespace MeshCoreRepeaterLabel

/**
 * The nodes this node has heard, by the start of their key. The companion keeps repeaters in one, apart from the contacts,
 * so a busy mountain full of repeaters never takes a slot from someone the user writes to; a relay without a companion
 * keeps its direct neighbours in one. A full table gives up the node heard longest ago.
 */
template <size_t N> class MeshCoreRepeaterNames
{
  public:
    using Entry = MeshCoreRepeaterName;

    static constexpr size_t capacity() { return N; }

    /// Learn or refresh the node with this key, heard as `how` says. Returns its entry.
    Entry *learn(const uint8_t pub[32], const char *name, const MeshCoreHeard &how)
    {
        Entry *e = findByPrefixExact(pub);
        if (!e) {
            e = &entries[victim()];
            *e = Entry();
            memcpy(e->prefix, pub, Entry::PREFIX_LEN);
        }
        char label[Entry::NAME_LEN + 1];
        const size_t len = MeshCoreRepeaterLabel::make(name, label);
        if (len) {
            memset(e->name, 0, sizeof(e->name));
            memcpy(e->name, label, len);
        }
        e->type = how.type;
        e->nodeNum = MeshCoreNodeStore<1>::nodeNumForKey(pub);
        e->heardAt = how.uptimeSecs + 1;
        const float q = how.snr * 4.0f;
        e->snrQuarter = q > 127.0f ? 127 : q < -128.0f ? -128 : (int8_t)q;
        e->hopsPlusOne = how.hops >= Entry::HOPS_UNKNOWN ? 0 : (uint8_t)(how.hops + 1);
        touch(e);
        return e;
    }

    /// Restore a saved entry as it was saved, except that it has not been heard this boot. Nothing is known of its recency
    /// but its place in the order the caller restores them in: the oldest first.
    Entry *restore(const Entry &saved)
    {
        Entry *e = findByPrefixExact(saved.prefix);
        if (!e)
            e = &entries[victim()];
        *e = saved;
        e->heardAt = 0;
        touch(e);
        return e;
    }

    /// Entries whose key starts with the first size bytes of prefix. Returns how many match; up to max are written to out.
    size_t findByPrefix(const uint8_t *prefix, uint8_t size, const Entry **out, size_t max) const
    {
        if (!prefix || size == 0)
            return 0;
        if (size > Entry::PREFIX_LEN)
            size = Entry::PREFIX_LEN; // a longer hash is matched on what is kept of it
        size_t total = 0;
        for (const Entry &e : entries) {
            if (!e.heard || memcmp(e.prefix, prefix, size) != 0)
                continue;
            if (out && total < max)
                out[total] = &e;
            total++;
        }
        return total;
    }

    /// Entries whose kept name starts with what the user typed, read the same way a name is kept (pictograms skipped, cut at
    /// NAME_LEN), so a long name matches the short form it was stored as. Returns how many match; up to max go to out.
    size_t findByName(const char *typed, const Entry **out, size_t max) const
    {
        char label[Entry::NAME_LEN + 1];
        const size_t len = MeshCoreRepeaterLabel::make(typed, label);
        if (len == 0)
            return 0;
        size_t total = 0;
        for (const Entry &e : entries) {
            if (!e.heard || nameLength(e) < len || strncasecmp(e.name, label, len) != 0)
                continue;
            if (out && total < max)
                out[total] = &e;
            total++;
        }
        return total;
    }

    size_t count() const
    {
        size_t n = 0;
        for (const Entry &e : entries)
            if (e.heard)
                n++;
        return n;
    }

    /// The entry in slot i, or null for a free one.
    const Entry *at(size_t i) const { return i < N && entries[i].heard ? &entries[i] : nullptr; }

    /// Entries heard most recently first, up to max. Returns how many were written.
    size_t newestFirst(const Entry **out, size_t max) const
    {
        size_t n = 0;
        for (const Entry &e : entries) {
            if (!e.heard || max == 0)
                continue;
            size_t at = 0; // where e belongs among the newer ones already kept
            while (at < n && out[at]->heard > e.heard)
                at++;
            if (at >= max)
                continue; // older than everything kept
            for (size_t k = (n < max ? n : max - 1); k > at; k--)
                out[k] = out[k - 1];
            out[at] = &e;
            if (n < max)
                n++;
        }
        return n;
    }

    /// The repeater with `rank` others heard more recently (0 is the newest), or null past the last. Walking the ranks from
    /// the oldest to the newest lets a save write the table in order without room for a sorted copy of it.
    const Entry *nthNewest(size_t rank) const
    {
        for (const Entry &e : entries) {
            if (!e.heard)
                continue;
            size_t newer = 0;
            for (const Entry &o : entries)
                if (o.heard > e.heard)
                    newer++;
            if (newer == rank)
                return &e;
        }
        return nullptr;
    }

    /// Stamps are unique, so the table can be walked by recency one O(N) step at a time with no sorted copy: start from
    /// newest() (or nthNewest()) and follow nextOlder() / nextNewer() until null.
    const Entry *newest() const { return nextOlder(NO_STAMP_ABOVE); }

    /// The repeater heard just before the one stamped `heard`, or null if none is older.
    const Entry *nextOlder(uint32_t heard) const
    {
        const Entry *best = nullptr;
        for (const Entry &e : entries) {
            if (e.heard && e.heard < heard && (!best || e.heard > best->heard))
                best = &e;
        }
        return best;
    }

    /// The repeater heard just after the one stamped `heard`, or null if none is newer.
    const Entry *nextNewer(uint32_t heard) const
    {
        const Entry *best = nullptr;
        for (const Entry &e : entries) {
            if (e.heard > heard && (!best || e.heard < best->heard))
                best = &e;
        }
        return best;
    }

    /// Changes when the set of repeaters or a name changes, not when one is merely heard again: only that is worth a flash
    /// write.
    uint32_t fingerprint() const
    {
        uint32_t sum = 0;
        size_t used = 0;
        for (const Entry &e : entries) {
            if (!e.heard)
                continue;
            uint32_t h = 2166136261u; // FNV-1a
            for (size_t i = 0; i < Entry::PREFIX_LEN; i++)
                h = (h ^ e.prefix[i]) * 16777619u;
            for (size_t i = 0; i < Entry::NAME_LEN; i++)
                h = (h ^ (uint8_t)e.name[i]) * 16777619u;
            sum += h; // order independent
            used++;
        }
        return sum ^ (uint32_t)used;
    }

    void clear()
    {
        for (Entry &e : entries)
            e = Entry();
        clock = 0;
    }

  private:
    Entry entries[N];
    uint16_t clock = 0;
    static constexpr uint32_t NO_STAMP_ABOVE = 0x10000; // above every 16-bit stamp

    static size_t nameLength(const Entry &e)
    {
        size_t l = 0;
        while (l < Entry::NAME_LEN && e.name[l])
            l++;
        return l;
    }

    Entry *findByPrefixExact(const uint8_t *prefix)
    {
        for (Entry &e : entries)
            if (e.heard && memcmp(e.prefix, prefix, Entry::PREFIX_LEN) == 0)
                return &e;
        return nullptr;
    }

    /// Stamp e as the newest. The 16-bit clock wraps after 65535 stamps: when it is about to, every stamp is renumbered in
    /// the same order from 1, which keeps who is oldest.
    void touch(Entry *e)
    {
        if (clock == 0xFFFF)
            renumber();
        e->heard = ++clock;
    }

    void renumber()
    {
        uint16_t next = 0;
        uint16_t floor = 0;
        for (;;) {
            Entry *oldest = nullptr;
            for (Entry &e : entries)
                if (e.heard > floor && (!oldest || e.heard < oldest->heard))
                    oldest = &e;
            if (!oldest)
                break;
            floor = oldest->heard;
            oldest->heard = ++next;
        }
        clock = next;
    }

    /// A free slot, else the one heard longest ago.
    size_t victim() const
    {
        size_t oldest = 0;
        for (size_t i = 0; i < N; i++) {
            if (!entries[i].heard)
                return i;
            if (entries[i].heard < entries[oldest].heard)
                oldest = i;
        }
        return oldest;
    }
};
