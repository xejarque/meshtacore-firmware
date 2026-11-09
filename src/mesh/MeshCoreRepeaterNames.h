#pragma once

#include "configuration.h" // the ARCH_* macros the memory classes key off
#include "memory/MemClass.h"
#include "meshUtils.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h> // strncasecmp

// 16 B an entry, all in RAM: 0.5 KB at 32 up to 4 KB at 256. A repeater is only named on a path, so far fewer are worth
// remembering than contacts, and the oldest heard is the first to go.
#ifndef MESHCORE_REPEATER_NAMES_MAX
#if MESHTASTIC_MEM_CLASS <= MEM_CLASS_TINY
#define MESHCORE_REPEATER_NAMES_MAX 32
#elif MESHTASTIC_MEM_CLASS == MEM_CLASS_SMALL
#define MESHCORE_REPEATER_NAMES_MAX 64
#elif MESHTASTIC_MEM_CLASS == MEM_CLASS_MEDIUM
#define MESHCORE_REPEATER_NAMES_MAX 128
#else
#define MESHCORE_REPEATER_NAMES_MAX 256
#endif
#endif

/**
 * What a path needs to name a repeater: the start of its public key (the path carries hashes of 1 to 3 bytes of it), a
 * short name and how it was last heard. Every default is zero, so a table of them lands in .bss.
 */
struct MeshCoreRepeaterName {
    static constexpr size_t PREFIX_LEN = 3; // the longest hash a MeshCore path carries
    static constexpr size_t NAME_LEN = 8;   // bytes, zero padded and not terminated when it fills them all
    static constexpr uint8_t HOPS_UNKNOWN = 0xFF;

    uint8_t prefix[PREFIX_LEN] = {};
    char name[NAME_LEN] = {};
    int8_t snrQuarter = 0;   // SNR in quarter dB, clamped: a path label needs no more
    uint8_t hopsPlusOne = 0; // hops the last advert travelled, plus one: 0 is "never heard over RF"
    uint16_t heard = 0;      // recency stamp: 0 marks a free slot

    uint8_t hops() const { return hopsPlusOne ? hopsPlusOne - 1 : HOPS_UNKNOWN; }
    float snr() const { return snrQuarter / 4.0f; }

    /// The name as a terminated string.
    void copyName(char out[NAME_LEN + 1]) const
    {
        memcpy(out, name, NAME_LEN);
        out[NAME_LEN] = 0;
    }
};
static_assert(sizeof(MeshCoreRepeaterName) == 16, "the table is budgeted at 16 B an entry");

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
 * The repeaters this node has heard, by the start of their key, kept apart from the contacts so a busy mountain full of
 * repeaters never takes a slot from someone the user writes to. A full table gives up the repeater heard longest ago.
 */
template <size_t N> class MeshCoreRepeaterNames
{
  public:
    using Entry = MeshCoreRepeaterName;

    static constexpr size_t capacity() { return N; }

    /// Learn or refresh the repeater with this key, heard now. Returns its entry.
    Entry *learn(const uint8_t pub[32], const char *name, float snr, uint8_t hops)
    {
        Entry *e = findExact(pub);
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
        const float q = snr * 4.0f;
        e->snrQuarter = q > 127.0f ? 127 : q < -128.0f ? -128 : (int8_t)q;
        e->hopsPlusOne = hops >= Entry::HOPS_UNKNOWN ? 0 : (uint8_t)(hops + 1);
        touch(e);
        return e;
    }

    /// Restore a saved entry without changing what it was heard with. Nothing is known of its recency but its place in the
    /// order the caller restores them in: the oldest first.
    Entry *restore(const uint8_t prefix[Entry::PREFIX_LEN], const char name[Entry::NAME_LEN], int8_t snrQuarter,
                   uint8_t hopsPlusOne)
    {
        Entry *e = findByPrefixExact(prefix);
        if (!e) {
            e = &entries[victim()];
            *e = Entry();
            memcpy(e->prefix, prefix, Entry::PREFIX_LEN);
        }
        memcpy(e->name, name, Entry::NAME_LEN);
        e->snrQuarter = snrQuarter;
        e->hopsPlusOne = hopsPlusOne;
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

    static size_t nameLength(const Entry &e)
    {
        size_t l = 0;
        while (l < Entry::NAME_LEN && e.name[l])
            l++;
        return l;
    }

    Entry *findExact(const uint8_t pub[32]) { return findByPrefixExact(pub); }

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
