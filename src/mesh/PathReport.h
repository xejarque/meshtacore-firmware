#pragma once

#include "meshUtils.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Shared vocabulary of the two path reports (MeshCore path hashes and the Meshtastic ".path" command). Both render
 * node names into client markdown and describe a hop count, so the rules live here once.
 */

/// Strip leading and trailing blanks from a command typed into a text box.
inline void trimInPlace(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
}

/// A node name as a markdown label: no delimiters or control bytes, cut on a UTF-8 boundary.
inline size_t cleanPathLabel(const char *src, char *out, size_t cap, size_t maxBytes)
{
    if (!src || !out || cap == 0)
        return 0;
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++) {
        const char c = *p;
        if ((uint8_t)c < 0x20 || c == '*' || c == '_' || c == '~' || c == '`' || c == '[' || c == ']')
            continue;
        out[o++] = c;
    }
    out[o] = 0;
    const size_t keep = utf8PrefixLen(out, maxBytes);
    out[keep] = 0;
    return keep;
}

/// Drop the " (MC)" suffix that tells a MeshCore copy of a node from the Meshtastic one of the same name.
inline void stripMeshCoreSuffix(char *label)
{
    const size_t l = label ? strlen(label) : 0;
    if (l > 5 && strcmp(&label[l - 5], " (MC)") == 0)
        label[l - 5] = 0;
}

/// Hop count as a glyph: a bolt for direct, keycaps for 1-10. Null when the count has no glyph.
inline const char *hopGlyph(int hops)
{
    static const char *const GLYPHS[] = {"⚡", "1️⃣", "2️⃣", "3️⃣", "4️⃣", "5️⃣", "6️⃣", "7️⃣", "8️⃣", "9️⃣", "🔟"};
    return hops >= 0 && (size_t)hops < sizeof(GLYPHS) / sizeof(GLYPHS[0]) ? GLYPHS[hops] : nullptr;
}
