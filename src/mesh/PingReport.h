#pragma once
// The text of a PPING answer, in the shape ZephCore (branch zephtacore, helpers/PingBot.h) gives it:
//   MeshCore:   "@[Ana] 2 hops\n↓Montjuic\n↓3f9a(-88/+3.3)"    direct: "@[Ana] direct (-88/+3.3)"
//   Meshtastic: "@[Ana] 2 hops, last Montjuic, SNR 7.5 dB"      direct: "@[Ana] direct, SNR 7.5 dB"
// Only the pieces both protocols share live here; each protocol assembles its own line.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace PingReport
{

/// The arrow that opens each hop line of a MeshCore route.
static constexpr const char *ARROW = "\xE2\x86\x93";

/// Longest hop name printed, in bytes (a name is never cut in the middle of a UTF-8 character).
static constexpr size_t NAME_MAX_BYTES = 14;

/// SNR in tenths of a dB, truncated toward zero. The radio reports quarter dB, so going through x4 keeps 7.5 as 75, not 74.
inline int snrTenths(float snr)
{
    return (int)lroundf(snr * 4.0f) * 10 / 4;
}

/// "(RSSI/SNR)" with only the metrics asked for: "(-88/+3.3)", "(-88)", "(+3.3)". Empty when neither is on.
inline size_t signalTail(char *out, size_t cap, bool snr, bool rssi, float snrDb, int rssiDbm)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!snr && !rssi)
        return 0;
    const int tenths = snrTenths(snrDb);
    const int absTenths = tenths < 0 ? -tenths : tenths;
    int n = snprintf(out, cap, "(");
    if (rssi && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, "%d", rssiDbm);
    if (rssi && snr && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, "/");
    if (snr && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, "%c%d.%d", tenths < 0 ? '-' : '+', absTenths / 10, absTenths % 10);
    if (n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, ")");
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

/// "7.5" / "-2.5": the SNR with its tenth, as the Meshtastic line prints it ("SNR 7.5 dB").
inline void snrPlain(char *out, size_t cap, float snrDb)
{
    const int tenths = snrTenths(snrDb);
    const int absTenths = tenths < 0 ? -tenths : tenths;
    snprintf(out, cap, "%s%d.%d", tenths < 0 ? "-" : "", absTenths / 10, absTenths % 10);
}

/// A hop no node claims, as lower-case hex of its hash bytes: "3f9a".
inline void hexLabel(const uint8_t *hash, size_t size, char *out, size_t cap)
{
    size_t o = 0;
    if (cap == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < size && o + 3 <= cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%02x", (unsigned)hash[i]);
}

/// The Meshtastic line, which carries only the last relay: "@[Ana] 2 hops, last Montjuic, SNR 7.5 dB".
/// sender and lastRelay may be null or empty; hops 0 reads "direct".
inline size_t meshtasticLine(char *out, size_t cap, const char *sender, unsigned hops, const char *lastRelay, bool snr, bool rssi,
                             float snrDb, int rssiDbm)
{
    if (!out || cap == 0)
        return 0;
    size_t n = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (n >= cap - 1)
            return;
        const int w = snprintf(out + n, cap - n, fmt, args...);
        if (w > 0)
            n = (size_t)w < cap - n ? n + (size_t)w : cap - 1;
    };
    out[0] = '\0';
    if (sender && sender[0])
        add("@[%s] ", sender);
    if (hops == 0) {
        add("direct");
    } else {
        add("%u hop%s", hops, hops == 1 ? "" : "s");
        if (lastRelay && lastRelay[0])
            add(", last %s", lastRelay);
    }
    if (snr) {
        char text[12];
        snrPlain(text, sizeof(text), snrDb);
        add(", SNR %s dB", text);
    }
    if (rssi)
        add(", RSSI %d dBm", rssiDbm);
    return n;
}

} // namespace PingReport
