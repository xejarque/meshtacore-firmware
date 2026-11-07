#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// True when cmd is exactly `word`, or `word` followed by whitespace and arguments. SAVEX or LOADFOO are not commands.
inline bool isCommandWord(const char *cmd, const char *word)
{
    const size_t n = strlen(word);
    return strncasecmp(cmd, word, n) == 0 && (cmd[n] == '\0' || isspace((unsigned char)cmd[n]));
}

// Which radio a command root addresses: /mt Meshtastic, /mc MeshCore, /mtc both.
enum CmdScope : uint8_t { SCOPE_MT, SCOPE_MC, SCOPE_BOTH };

// The three command roots. `root` takes the -device/-node/-mac/-group suffixes (or none, for a broadcast); `local` is the
// short local-only form (/mtl, /mcl, /mtcl): it never leaves the radio and carries no suffixes.
struct CmdRoot {
    const char *root;
    const char *local;
    CmdScope scope;
};

inline const CmdRoot *cmdRoots(size_t *count)
{
    static const CmdRoot ROOTS[] = {{"/mt", "/mtl", SCOPE_MT}, {"/mc", "/mcl", SCOPE_MC}, {"/mtc", "/mtcl", SCOPE_BOTH}};
    *count = sizeof(ROOTS) / sizeof(ROOTS[0]);
    return ROOTS;
}

// Matches the local-only roots (/mtl, /mcl, /mtcl) as a whole word.
// Returns the command body in *tail (never null; empty when the root carries no arguments) and its scope in *scope, or false
// when cmd is not one.
inline bool matchLocalShortcut(const char *cmd, const char **tail, CmdScope *scope)
{
    if (!cmd)
        return false;
    size_t count = 0;
    const CmdRoot *roots = cmdRoots(&count);
    for (size_t i = 0; i < count; i++) {
        const size_t n = strlen(roots[i].local);
        if (strncasecmp(cmd, roots[i].local, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' '))
            continue;
        const char *p = cmd + n;
        while (*p == ' ')
            p++;
        if (tail)
            *tail = p;
        if (scope)
            *scope = roots[i].scope;
        return true;
    }
    return false;
}

// Adapts a command body to the root it came in on. /mc runs MeshCore commands, so it prepends MCORE (into buf), unless the
// body already starts with MCORE or MESHCORE. /mt refuses the MESHCORE/MCORE family and returns nullptr so the caller can
// point the operator at /mc. /mtc runs the body as typed. A /mc body too long for buf also returns nullptr: running a
// truncated command would do something else than what was sent. Callers tell the two apart by scope.
inline const char *applyCommandScope(CmdScope scope, const char *body, char *buf, size_t bufLen)
{
    if (scope == SCOPE_MC) {
        if (body[0] == '\0')
            return "MCORE";
        if (isCommandWord(body, "MCORE") || isCommandWord(body, "MESHCORE"))
            return body;
        const int written = snprintf(buf, bufLen, "MCORE %s", body);
        return (written >= 0 && (size_t)written < bufLen) ? buf : nullptr;
    }
    if (scope == SCOPE_MT && (isCommandWord(body, "MESHCORE") || isCommandWord(body, "MCORE")))
        return nullptr;
    return body;
}

// A RELAYS argument naming one relay by hex: `*AB`, `0xAB` or a bare two-digit hex byte. A pair of decimal digits is a top-N
// count, so a bare byte must hold a letter (`5A`, `A5`, `FF`).
inline bool isRelayIdArg(const char *p)
{
    if (*p == '*')
        return isxdigit((unsigned char)p[1]);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return isxdigit((unsigned char)p[2]);
    return isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && p[2] == '\0' &&
           !(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]));
}

// Reads an ON/OFF argument (ON, 1, OFF, 0). Anything else leaves `out` untouched and returns false: a typo must never switch
// a setting.
inline bool parseOnOff(const char *p, bool &out)
{
    if (strcasecmp(p, "ON") == 0 || strcmp(p, "1") == 0) {
        out = true;
        return true;
    }
    if (strcasecmp(p, "OFF") == 0 || strcmp(p, "0") == 0) {
        out = false;
        return true;
    }
    return false;
}

// Strict number parsers: the whole argument must be the number (surrounding spaces allowed). atoi/strtoul read "abc" as 0,
// which would write 0 into a setting; these leave `out` untouched and return false instead.
inline const char *skipSpaces(const char *p)
{
    while (*p == ' ')
        p++;
    return p;
}

inline bool parseUInt(const char *p, uint32_t &out, uint32_t maxValue = UINT32_MAX)
{
    p = skipSpaces(p);
    if (!isdigit((unsigned char)*p))
        return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = strtoull(p, &end, 10);
    if (errno != 0 || *skipSpaces(end) != '\0' || v > maxValue)
        return false;
    out = (uint32_t)v;
    return true;
}

inline bool parseFloat(const char *p, float &out, float minValue, float maxValue)
{
    p = skipSpaces(p);
    if (*p == '\0')
        return false;
    char *end = nullptr;
    errno = 0;
    const float v = strtof(p, &end);
    if (errno != 0 || end == p || *skipSpaces(end) != '\0' || !(v >= minValue && v <= maxValue))
        return false;
    out = v;
    return true;
}

// A relay or node id byte: `AB`, `*AB` or `0xAB`, one or two hex digits.
inline bool parseHexByte(const char *p, uint8_t &out)
{
    p = skipSpaces(p);
    if (*p == '*')
        p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    if (!isxdigit((unsigned char)*p))
        return false;
    char *end = nullptr;
    const unsigned long v = strtoul(p, &end, 16);
    if (*skipSpaces(end) != '\0' || v > 0xFF)
        return false;
    out = (uint8_t)v;
    return true;
}

// Token readers for multi-argument commands: read one space-delimited token, advance `p` past it and the spaces after it. A
// missing or malformed token fails without moving the output.
inline bool takeUInt(const char *&p, uint32_t &out, uint32_t maxValue = UINT32_MAX)
{
    p = skipSpaces(p);
    const char *end = p;
    while (*end && *end != ' ')
        end++;
    char buf[16];
    const size_t n = (size_t)(end - p);
    if (n == 0 || n >= sizeof(buf))
        return false;
    memcpy(buf, p, n);
    buf[n] = '\0';
    if (!parseUInt(buf, out, maxValue))
        return false;
    p = skipSpaces(end);
    return true;
}

inline bool takeFloat(const char *&p, float &out, float minValue, float maxValue)
{
    p = skipSpaces(p);
    const char *end = p;
    while (*end && *end != ' ')
        end++;
    char buf[24];
    const size_t n = (size_t)(end - p);
    if (n == 0 || n >= sizeof(buf))
        return false;
    memcpy(buf, p, n);
    buf[n] = '\0';
    if (!parseFloat(buf, out, minValue, maxValue))
        return false;
    p = skipSpaces(end);
    return true;
}

// First space-delimited token as a signed integer, or `invalid` when it is not entirely a number. Pick `invalid` outside the
// range the caller accepts and the existing range check refuses it.
inline int tokenInt(const char *p, int invalid = -1)
{
    p = skipSpaces(p);
    const char *end = p;
    while (*end && *end != ' ')
        end++;
    char buf[16];
    const size_t n = (size_t)(end - p);
    if (n == 0 || n >= sizeof(buf))
        return invalid;
    memcpy(buf, p, n);
    buf[n] = '\0';
    const bool negative = buf[0] == '-';
    uint32_t magnitude = 0;
    if (!parseUInt(buf + (negative ? 1 : 0), magnitude, 1000000000u))
        return invalid;
    return negative ? -(int)magnitude : (int)magnitude;
}

// First space-delimited token as a float, or NaN when it is not entirely a number: every range comparison on NaN is false, so
// the caller's existing bounds check refuses it.
inline float tokenFloat(const char *p)
{
    p = skipSpaces(p);
    const char *end = p;
    while (*end && *end != ' ')
        end++;
    char buf[24];
    const size_t n = (size_t)(end - p);
    float v = 0;
    if (n == 0 || n >= sizeof(buf))
        return NAN;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return parseFloat(buf, v, -1e9f, 1e9f) ? v : NAN;
}

// Decimal, 0x-hex or octal like strtoul(base 0), but the whole argument must be the number.
inline bool parseUIntAuto(const char *p, uint32_t &out)
{
    p = skipSpaces(p);
    if (!isdigit((unsigned char)*p))
        return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long v = strtoul(p, &end, 0);
    if (errno != 0 || *skipSpaces(end) != '\0' || v > UINT32_MAX)
        return false;
    out = (uint32_t)v;
    return true;
}

// Up to 16 hex digits, optional 0x prefix, nothing else.
inline bool parseHex64(const char *p, uint64_t &out)
{
    p = skipSpaces(p);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    if (!isxdigit((unsigned char)*p))
        return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = strtoull(p, &end, 16);
    if (errno != 0 || *skipSpaces(end) != '\0')
        return false;
    out = (uint64_t)v;
    return true;
}

// True when all `len` characters are hex digits.
inline bool isAllHex(const char *p, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)p[i]))
            return false;
    return true;
}
