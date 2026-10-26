#pragma once

#include "meshUtils.h"
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/**
 * Shared vocabulary of the two path reports (MeshCore path hashes and the Meshtastic ".path" command). Both render
 * node names into client markdown and describe a hop count, so the rules live here once. Markdown is kept to what helps
 * reading: bold for the sender only, code for a hop known by its hash alone, italics for a note.
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

/// First line of a report: "🛰️ Ruta de **who**", plus " · N rutas" when the text arrived over several. who may be empty.
inline void formatReportHeader(char *out, size_t cap, const char *who, size_t routes)
{
    char label[48];
    const bool named = cleanPathLabel(who, label, sizeof(label), sizeof(label) - 1) > 0;
    if (named)
        stripMeshCoreSuffix(label);
    int n = snprintf(out, cap, named && label[0] ? "🛰️ Ruta de **%s**" : "🛰️ Ruta", label);
    if (routes > 1 && n > 0 && (size_t)n < cap)
        snprintf(&out[n], cap - n, " · %u rutas", (unsigned)routes);
}

/// " · SNR x dB · RSSI y dBm", the signal a route was heard with. RSSI is left out when unknown.
inline void formatSignal(char *out, size_t cap, float snr, bool hasRssi, int32_t rssi)
{
    if (hasRssi)
        snprintf(out, cap, " · SNR %.1f dB · RSSI %d dBm", (double)snr, (int)rssi);
    else
        snprintf(out, cap, " · SNR %.1f dB", (double)snr);
}

/// Last line of a report: where the text was heard and how long ago. channelName may be null (shown by number).
inline void formatHeardWhere(char *out, size_t cap, bool direct, const char *channelName, unsigned channel, bool viaMqtt,
                             uint32_t ageSecs)
{
    char label[48];
    int n;
    if (direct)
        n = snprintf(out, cap, "Mensaje directo");
    else if (cleanPathLabel(channelName, label, sizeof(label), sizeof(label) - 1) > 0)
        n = snprintf(out, cap, "Canal %s", label);
    else
        n = snprintf(out, cap, "Canal %u", channel);
    if (n > 0 && (size_t)n < cap && viaMqtt)
        n += snprintf(&out[n], cap - n, " · vía MQTT");
    if (n > 0 && (size_t)n < cap)
        snprintf(&out[n], cap - n, " · hace %u s", (unsigned)ageSecs);
}

/// A hop no node claims, as upper-case hex of its hash bytes (at most 4): "`A1B2`".
inline void formatHopHex(const uint8_t *hash, size_t size, char *out, size_t cap)
{
    char hex[9] = {};
    for (size_t i = 0; i < size && i < 4; i++)
        snprintf(&hex[i * 2], 3, "%02X", (unsigned)hash[i]);
    snprintf(out, cap, "`%s`", hex);
}
