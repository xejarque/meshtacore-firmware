// SPDX-License-Identifier: AGPL-3.0-only
//
// What the companion reports in text: the .path and .trace answers, the PING line, the node listing and detail, and
// the hash drops that hide a repeater from them.

#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "MeshCoreProtocol.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "PingReport.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <AES.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <SHA512.h>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "MeshCoreCompanionWire.h"

#if MESHTASTIC_MESHCORE_PATH_COMMAND
static const char *pathSenderName(uint32_t nodeNum)
{
    if (!nodeDB || !nodeNum)
        return nullptr;
    const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(nodeNum);
    return n && nodeInfoLiteHasUser(n) && n->long_name[0] ? n->long_name : nullptr;
}

/// Most names shown for one hop; a wider match is summarised as "+N".
static constexpr size_t PATH_HOP_NAMES_MAX = 3;
static_assert(PATH_HOP_NAMES_MAX == 3, "hopNameScratch in MeshCoreCompanion.h holds that many names");
#endif

#if MESHTASTIC_MESHCORE_PATH_COMMAND
// FNV-1a over the whole route: the stored path is cut at PATH_BYTES, so this tells long routes apart.
static uint32_t routeHash(const uint8_t *path, size_t bytes)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; path && i < bytes; i++)
        hash = (hash ^ path[i]) * 16777619u;
    return hash;
}

void MeshCoreCompanion::noteHeardRoute(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel,
                                       const uint8_t *path, uint8_t hashSize, uint8_t hopCount, uint8_t routeType, float snr,
                                       int32_t rssi)
{
    const size_t bytes = (size_t)hopCount * hashSize;
    const size_t keep = bytes < sizeof(heard[0].path) ? bytes : sizeof(heard[0].path);
    const uint32_t pathHash = routeHash(path, bytes);

    HeardPath *owner = nullptr;
    for (HeardPath &h : heard) {
        if (!h.packetId || h.frameHash != frameHash)
            continue;
        if (!owner)
            owner = &h;
        // The same route heard twice only refreshes its signal: a flood arrives once per route, not once per copy.
        if (!h.own && h.hashSize == hashSize && h.hopCount == hopCount && h.pathHash == pathHash) {
            h.snr = snr;
            h.rssi = rssi;
            return;
        }
    }
    // A later copy carries no id of its own: it belongs to the text the first copy delivered to the client.
    if (!packetId) {
        if (!owner)
            return;
        packetId = owner->packetId;
        fromNum = owner->fromNum;
        channel = owner->channel;
    }

    HeardPath *slot = nullptr;
    if (owner) {
        // A text already kept: one more route, if there is room or it is shorter than the longest kept.
        slot = heardRouteSlot(frameHash, hopCount, frameHash == heardOwn ? PATH_ROUTES_OWN : PATH_ROUTES_PER_TEXT);
    } else {
        // A new text takes the place of the oldest received one, routes and all.
        heardForget(heardIncoming[heardIncomingNext]);
        heardIncoming[heardIncomingNext] = frameHash;
        heardIncomingNext = (heardIncomingNext + 1) % PATH_INCOMING_TEXTS;
        slot = heardRouteSlot(frameHash, hopCount, PATH_ROUTES_PER_TEXT);
    }
    if (!slot)
        return;

    HeardPath &h = *slot;
    h = HeardPath();
    h.packetId = packetId;
    h.frameHash = frameHash;
    h.fromNum = fromNum;
    h.channel = channel;
    h.routeType = routeType;
    h.hashSize = hashSize;
    h.hopCount = hopCount;
    h.pathHash = pathHash;
    h.snr = snr;
    h.rssi = rssi;
    h.heardMs = millis();
    if (keep && path)
        memcpy(h.path, path, keep);
}

void MeshCoreCompanion::heardForget(uint32_t frameHash)
{
    if (!frameHash)
        return;
    for (HeardPath &h : heard)
        if (h.packetId && h.frameHash == frameHash)
            h = HeardPath();
}

MeshCoreCompanion::HeardPath *MeshCoreCompanion::heardRouteSlot(uint32_t frameHash, uint8_t hopCount, size_t maxRoutes)
{
    HeardPath *free = nullptr;
    HeardPath *longest = nullptr;
    size_t routes = 0;
    for (HeardPath &h : heard) {
        if (!h.packetId) {
            if (!free)
                free = &h;
        } else if (h.frameHash == frameHash && !h.own) {
            routes++;
            if (!longest || h.hopCount > longest->hopCount)
                longest = &h;
        }
    }
    if (routes < maxRoutes && free)
        return free;
    return (longest && hopCount < longest->hopCount) ? longest : nullptr;
}

void MeshCoreCompanion::noteSentText(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel)
{
    // Only the last text we sent is kept: the previous one, echoes included, makes room for it.
    heardForget(heardOwn);
    heardOwn = frameHash;
    HeardPath *slot = heardRouteSlot(frameHash, 0, 1);
    if (!slot)
        return;
    HeardPath &h = *slot;
    h = HeardPath();
    h.packetId = packetId;
    h.frameHash = frameHash;
    h.fromNum = fromNum;
    h.channel = channel;
    h.heardMs = millis();
    h.own = true;
}

size_t MeshCoreCompanion::heardRouteCount(uint32_t frameHash) const
{
    concurrency::RecursiveLockGuard guard(lock);
    size_t n = 0;
    for (const HeardPath &h : heard)
        if (h.packetId && !h.own && h.frameHash == frameHash)
            n++;
    return n;
}

const MeshCoreCompanion::HeardPath *MeshCoreCompanion::heardFind(uint32_t packetId) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!packetId)
        return nullptr;
    for (const HeardPath &h : heard)
        if (h.packetId == packetId)
            return &h;
    return nullptr;
}

const MeshCoreCompanion::HeardPath *MeshCoreCompanion::heardFindByFrom(uint32_t fromNum) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!fromNum)
        return nullptr;
    const HeardPath *best = nullptr;
    for (const HeardPath &h : heard) {
        if (h.packetId && h.fromNum == fromNum) {
            if (!best || (int32_t)(h.heardMs - best->heardMs) > 0)
                best = &h;
        }
    }
    return best;
}

/// A node the companion announced for a MeshCore peer: a MeshCore peer node holding a MeshCore key.
static bool isMeshCoreNode(const meshtastic_NodeInfoLite *n)
{
    return nodeInfoLiteIsMeshCorePeer(n) && n->public_key.size == 32 && n->hw_model == meshtastic_HardwareModel_UNSET;
}

size_t MeshCoreCompanion::pathHopNames(const uint8_t *hash, uint8_t size, const char **out, size_t max) const
{
    size_t total = 0;
    auto addName = [&](const char *name) {
        if (!name || !name[0])
            return;
        for (size_t k = 0; k < total && k < max; k++)
            if (strcmp(out[k], name) == 0)
                return; // the same node listed twice is not a second candidate
        if (total < max && out)
            out[total] = name;
        total++;
    };

    const MeshCoreNode *known[PATH_HOP_NAMES_MAX];
    const size_t knownCount = nodes.findByPrefix(hash, size, known, PATH_HOP_NAMES_MAX);
    for (size_t i = 0; i < knownCount && i < PATH_HOP_NAMES_MAX; i++)
        addName(known[i]->name);
    // Repeaters are kept as a key prefix and a short name. One the node DB still holds is already listed by its whole name,
    // and the short form would only be a second candidate for the same hop.
    const MeshCoreRepeaterName *heard[PATH_HOP_NAMES_MAX];
    const size_t heardCount = repeaters.findByPrefix(hash, size, heard, PATH_HOP_NAMES_MAX);
    bool heardInNodeDb[PATH_HOP_NAMES_MAX] = {};
    // One pass over the node DB serves both lists. MeshCore nodes announced before the store existed are only there; a
    // Meshtastic node's key has nothing to do with a MeshCore hash, so only MeshCore nodes are looked at.
    for (size_t i = 0; nodeDB && i < nodeDB->getNumMeshNodes(); i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!isMeshCoreNode(n) || n->long_name[0] == 0)
            continue;
        for (size_t r = 0; r < heardCount && r < PATH_HOP_NAMES_MAX; r++)
            if (memcmp(n->public_key.bytes, heard[r]->prefix, MeshCoreRepeaterName::PREFIX_LEN) == 0)
                heardInNodeDb[r] = true;
        if (memcmp(n->public_key.bytes, hash, size) == 0 && !nodes.find(n->public_key.bytes))
            addName(n->long_name);
    }
    size_t scratchUsed = 0;
    for (size_t i = 0; i < heardCount && i < PATH_HOP_NAMES_MAX; i++) {
        if (heardInNodeDb[i])
            continue;
        heard[i]->copyName(hopNameScratch[scratchUsed]);
        addName(hopNameScratch[scratchUsed++]);
    }
    return total;
}

/// "A|B" for the names behind a hop, or "" when none. Duplicates collapse; beyond two names the rest become "+N".
static void formatHopLabel(const char *const *names, size_t count, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    char cleaned[PATH_HOP_NAMES_MAX][40];
    size_t have = 0;
    for (size_t i = 0; i < count && i < PATH_HOP_NAMES_MAX; i++) {
        if (cleanPathLabel(names[i], cleaned[have], sizeof(cleaned[have]), sizeof(cleaned[have]) - 1) == 0)
            continue;
        stripMeshCoreSuffix(cleaned[have]);
        if (cleaned[have][0])
            have++;
    }
    if (have == 0)
        return;

    const size_t limit = have > 2 ? 2 : have;
    size_t o = 0;
    for (size_t i = 0; i < limit; i++) {
        if (i && o + 1 < cap)
            out[o++] = '|';
        const size_t l = strlen(cleaned[i]);
        if (o + l + 1 >= cap)
            break;
        memcpy(&out[o], cleaned[i], l);
        o += l;
    }
    if (have > limit) {
        const int n = snprintf(&out[o], cap - o, "+%u", (unsigned)(have - limit));
        if (n > 0 && (size_t)n < cap - o)
            o += (size_t)n;
    }
    out[o] = 0;
}

static constexpr size_t REPORT_CHUNK_CAP = sizeof(((meshtastic_MeshPacket *)nullptr)->decoded.payload.bytes);

/**
 * Packs the report into client-sized messages. Text is added one atomic token at a time - a glyph, a hex hop, a bold
 * name, a " · SNR ..." group - and a token always opens and closes inside the same message. A line that does not fit
 * continues in the next message, so nothing is cut and clients never see unmatched markdown markers.
 */
struct ReportWriter {
    MeshCoreCompanion::ReportSink sink;
    void *ctx;
    char buf[REPORT_CHUNK_CAP + 1] = {};
    size_t len = 0;
    unsigned parts = 0;

    void flush()
    {
        if (!len)
            return;
        sink(ctx, buf, len);
        parts++;
        len = 0;
        buf[0] = 0;
    }

    void startPart()
    {
        if (len == 0 && parts > 0) { // a piece that continues a split report
            memcpy(buf, "↳ ", 4);
            len = 4;
            buf[len] = 0;
        }
    }

    /// Append one token; firstOfLine also consumes the newline owed before it. A token that cannot fit alone is the
    /// only thing that gets split, and that needs a name or hop far longer than the report allows.
    void token(const char *text, bool firstOfLine = false)
    {
        const size_t n = strlen(text);
        if (firstOfLine && len) {
            if (len + 1 + n > REPORT_CHUNK_CAP)
                flush();
            else {
                buf[len++] = '\n';
                buf[len] = 0;
            }
        }
        if (len + n > REPORT_CHUNK_CAP)
            flush();
        startPart();
        if (len + n > REPORT_CHUNK_CAP) {
            addSplit(text);
            return;
        }
        memcpy(&buf[len], text, n);
        len += n;
        buf[len] = 0;
    }

    void addSplit(const char *text)
    {
        size_t n = strlen(text);
        while (n) {
            startPart();
            const size_t room = REPORT_CHUNK_CAP - len;
            if (n <= room) {
                memcpy(&buf[len], text, n);
                len += n;
                buf[len] = 0;
                return;
            }
            size_t cut = room;
            while (cut > 0 && ((uint8_t)text[cut] & 0xC0) == 0x80)
                cut--; // never split a UTF-8 character
            if (cut == 0)
                cut = room;
            memcpy(&buf[len], text, cut);
            len += cut;
            buf[len] = 0;
            flush();
            text += cut;
            n -= cut;
        }
    }
};

bool MeshCoreCompanion::formatRouteLine(const HeardPath &route, char *out, size_t cap, float freqErr, uint8_t signal,
                                        const char *header) const
{
    if (!out || cap == 0)
        return false;

    // Metrics joined by " · "; a direct route leads with the signal icon and tags the frequency error.
    auto formatSignal = [&](char *dst, size_t dstCap, bool direct) {
        size_t n = 0;
        const char *sep = direct ? "📶 " : "";
        auto add = [&](const char *fmt, auto... args) {
            if (n >= dstCap - 1)
                return;
            int w = snprintf(dst + n, dstCap - n, fmt, args...);
            if (w > 0)
                n += (size_t)w < dstCap - n ? (size_t)w : dstCap - n - 1;
        };
        if (signal & SIGNAL_SNR) {
            add("%sSNR: %.1f dB", sep, (double)route.snr);
            sep = " · ";
        }
        if (signal & SIGNAL_RSSI) {
            add("%sRSSI: %d dBm", sep, (int)route.rssi);
            sep = " · ";
        }
        if (signal & SIGNAL_FREQERR)
            add("%s%sΔf %.0fHz", sep, direct ? "🔬 " : "", (double)freqErr);
        dst[n < dstCap ? n : dstCap - 1] = '\0';
        return n;
    };

    // The header, when there is one, shares the first line with the hop count: the route itself starts below.
    const char *lead = (header && header[0]) ? header : "";
    const bool direct = route.hopCount == 0;
    size_t len = 0;
    if (direct) {
        int n = lead[0] ? snprintf(out, cap, "%s · ⚡ directo\n", lead) : snprintf(out, cap, "⚡ directo\n");
        if (n > 0)
            len = (size_t)n < cap ? (size_t)n : cap - 1;
        if (cap - len > 1)
            len += formatSignal(out + len, cap - len, true);
    } else {
        int n = lead[0] ? snprintf(out, cap, "%s · 📡 %u saltos\n", lead, (unsigned)route.hopCount)
                        : snprintf(out, cap, "📡 %u saltos\n", (unsigned)route.hopCount);
        if (n > 0)
            len = (size_t)n < cap ? (size_t)n : cap - 1;

        // The signal line goes last, so a long route must not push it out: set its room aside before the hop names, which are
        // dropped whole (never cut mid-name) once they no longer fit.
        char sig[80];
        const size_t sigLen = formatSignal(sig, sizeof(sig), false);
        const size_t reserve = (sigLen > 0 && sigLen + 16 < cap) ? sigLen : 0;
        const size_t hopCap = cap - reserve;

        const size_t stored = route.hashSize ? sizeof(route.path) / route.hashSize : 0;
        const size_t total = route.hopCount < stored ? route.hopCount : stored;
        for (size_t hop = 0; hop < total; hop++) {
            if (len >= hopCap - 1)
                break;
            const uint8_t *hash = &route.path[hop * route.hashSize];
            const char *names[PATH_HOP_NAMES_MAX];
            const size_t nameCount = pathHopNames(hash, route.hashSize, names, PATH_HOP_NAMES_MAX);
            char one[96];
            formatHopLabel(names, nameCount, one, sizeof(one));
            if (!one[0])
                formatHopHex(hash, route.hashSize, one, sizeof(one));
            else {
                char cleanOne[48];
                cleanNeighborName(one, cleanOne, sizeof(cleanOne), 14);
                strncpy(one, cleanOne, sizeof(one) - 1);
                one[sizeof(one) - 1] = '\0';
            }
            int nHop = snprintf(out + len, hopCap - len, "%s\n", one);
            if (nHop > 0) {
                if ((size_t)nHop < hopCap - len)
                    len += (size_t)nHop;
                else {
                    out[len] = '\0'; // this name does not fit: drop it whole
                    break;
                }
            }
        }

        if (reserve > 0) {
            memcpy(out + len, sig, sigLen + 1);
            len += sigLen;
        } else if (len < cap - 1) {
            len += formatSignal(out + len, cap - len, false);
        }
    }

    // A byte-wise truncation may have cut a multi-byte character in half: drop the orphan lead/continuation bytes.
    if (len >= cap - 1) {
        size_t cut = len;
        while (cut > 0 && ((uint8_t)out[cut - 1] & 0xC0) == 0x80)
            cut--;
        if (cut > 0 && ((uint8_t)out[cut - 1] & 0x80) != 0) {
            const uint8_t lead = (uint8_t)out[cut - 1];
            const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            cut = (len - (cut - 1) >= need) ? len : cut - 1;
        } else {
            cut = len;
        }
        len = cut;
        out[len] = '\0';
    }

    return len > 0 && len < cap;
}

bool MeshCoreCompanion::formatRouteForPacket(uint32_t packetId, char *out, size_t cap, float freqErr, uint8_t signal,
                                             const char *header) const
{
    concurrency::RecursiveLockGuard guard(lock);
    const HeardPath *entry = heardFind(packetId);
    if (!entry)
        return false;
    return formatRouteLine(*entry, out, cap, freqErr, signal, header);
}

bool MeshCoreCompanion::formatPingLine(const HeardPath &route, char *out, size_t cap, const char *sender, uint8_t signal) const
{
    if (!out || cap == 0)
        return false;
    out[0] = '\0';

    char tail[24];
    const size_t tailLen = PingReport::signalTail(tail, sizeof(tail), signal & SIGNAL_SNR, signal & SIGNAL_RSSI, route.snr,
                                                  (int)route.rssi);
    size_t n = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (n >= cap - 1)
            return;
        const int w = snprintf(out + n, cap - n, fmt, args...);
        if (w > 0)
            n = (size_t)w < cap - n ? n + (size_t)w : cap - 1;
    };

    if (sender && sender[0])
        add("@[%s] ", sender);
    if (route.hopCount == 0)
        add("direct");
    else
        add("%u hop%s", (unsigned)route.hopCount, route.hopCount == 1 ? "" : "s");

    // One hop per line, the way the packet carries them. The signal sticks to the last one, so its room is kept back and a
    // route that does not fit ends in "\n..".
    bool listed = false;
    const size_t stored = route.hashSize ? sizeof(route.path) / route.hashSize : 0;
    const size_t total = route.hopCount < stored ? route.hopCount : stored;
    for (size_t hop = 0; route.hopCount > 0 && hop < total; hop++) {
        const uint8_t *hash = &route.path[hop * route.hashSize];
        const char *names[PATH_HOP_NAMES_MAX];
        const size_t nameCount = pathHopNames(hash, route.hashSize, names, PATH_HOP_NAMES_MAX);
        char label[48];
        label[0] = '\0';
        if (nameCount > 0) {
            char cleaned[48];
            formatHopLabel(names, 1, cleaned, sizeof(cleaned)); // the first name alone: a shared hash gets a "?" below
            cleanNeighborName(cleaned, label, sizeof(label), PingReport::NAME_MAX_BYTES);
            if (label[0] && nameCount > 1 && strlen(label) + 2 < sizeof(label))
                strcat(label, "?");
        }
        if (!label[0])
            PingReport::hexLabel(hash, route.hashSize, label, sizeof(label));

        const size_t need = 1 + strlen(PingReport::ARROW) + strlen(label);
        const size_t more = hop + 1 < total ? 3 : 0; // room for "\n.."
        if (n + need + more + tailLen >= cap) {
            add("\n..");
            listed = true;
            break;
        }
        add("\n%s%s", PingReport::ARROW, label);
        listed = true;
    }

    if (tailLen > 0) {
        if (!listed)
            add(" ");
        add("%s", tail);
    }
    return n > 0 && n < cap;
}

bool MeshCoreCompanion::formatPingForPacket(uint32_t packetId, char *out, size_t cap, const char *sender, uint8_t signal) const
{
    concurrency::RecursiveLockGuard guard(lock);
    const HeardPath *entry = heardFind(packetId);
    if (!entry)
        return false;
    return formatPingLine(*entry, out, cap, sender, signal);
}

size_t MeshCoreCompanion::formatPathReport(const HeardPath *const *routes, size_t count, const char *sender, ReportSink sink,
                                           void *ctx) const
{
    if (!routes || count == 0 || !sink)
        return 0;

    ReportWriter w{sink, ctx};

    char header[128];
    formatReportHeader(header, sizeof(header), sender, count);
    w.token(header, true);
    uint32_t firstHeardMs = routes[0]->heardMs;

    for (size_t r = 0; r < count; r++) {
        const HeardPath &route = *routes[r];
        char token[128];
        if ((int32_t)(route.heardMs - firstHeardMs) < 0)
            firstHeardMs = route.heardMs; // the text is as old as its first copy

        // The glyph is the hop count: no "N salto(s)" text to repeat it.
        if (route.hopCount == 0) {
            w.token("⚡ directo", true);
        } else {
            if (const char *glyph = hopGlyph(route.hopCount))
                snprintf(token, sizeof(token), "%s", glyph);
            else
                snprintf(token, sizeof(token), "%u saltos", (unsigned)route.hopCount);
            w.token(token, true);

            const size_t stored = route.hashSize ? sizeof(route.path) / route.hashSize : 0;
            const size_t total = route.hopCount < stored ? route.hopCount : stored;
            for (size_t hop = 0; hop < total; hop++) {
                const uint8_t *hash = &route.path[hop * route.hashSize];
                const char *names[PATH_HOP_NAMES_MAX];
                const size_t nameCount = pathHopNames(hash, route.hashSize, names, PATH_HOP_NAMES_MAX);
                char one[96];
                formatHopLabel(names, nameCount, one, sizeof(one));
                if (!one[0])
                    formatHopHex(hash, route.hashSize, one, sizeof(one));
                snprintf(token, sizeof(token), hop ? " › %s" : " %s", one);
                w.token(token);
            }
        }
        formatSignal(token, sizeof(token), route.snr, true, route.rssi);
        w.token(token);
    }
    const bool direct = routes[0]->channel == HeardPath::DIRECT_CHANNEL;
    char where[96];
    formatHeardWhere(where, sizeof(where), direct, direct ? nullptr : channels.getName(routes[0]->channel), routes[0]->channel,
                     false, (millis() - firstHeardMs) / 1000);
    w.token(where, true);
    w.flush();
    return w.parts;
}

void MeshCoreCompanion::injectReportChunk(void *ctx, const char *text, size_t len)
{
    auto *r = static_cast<ReportInject *>(ctx);
    r->self->injectText(r->fromNum, r->to, r->channel, text, r->replyId, r->direct, r->contact, r->hops, r->snr, r->rssi);
    LOG_INFO("MeshCore .path report piece: %u B", (unsigned)len);
}

bool MeshCoreCompanion::handlePathCommand(const meshtastic_MeshPacket *p, bool direct)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    if (!p || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;
    if (!direct && !channels.isMeshCoreChannel((ChannelIndex)p->channel))
        return false;

    char text[sizeof(p->decoded.payload.bytes) + 1];
    const size_t len =
        p->decoded.payload.size < sizeof(p->decoded.payload.bytes) ? p->decoded.payload.size : sizeof(p->decoded.payload.bytes);
    memcpy(text, p->decoded.payload.bytes, len);
    text[len] = 0;
    trimInPlace(text);
    if (strcasecmp(text, ".path") != 0 && strcasecmp(text, ".ruta") != 0)
        return false;
    // Only a reply is answered: a plain ".path" is an ordinary text and goes out.
    if (!p->decoded.reply_id)
        return false;

    // The text being replied to: every copy heard of it is a route. Our own transmission is the entry they hang from, not one.
    const HeardPath *entry = heardFind(p->decoded.reply_id);

    // Every copy of that text is a separate route: the report lists them all, the shortest first.
    const HeardPath *routes[PATH_ROUTES_OWN > PATH_ROUTES_PER_TEXT ? PATH_ROUTES_OWN : PATH_ROUTES_PER_TEXT];
    constexpr size_t routeCap = sizeof(routes) / sizeof(routes[0]);
    size_t routeCount = 0;
    if (entry) {
        for (const HeardPath &h : heard)
            if (h.packetId && !h.own && h.frameHash == entry->frameHash && routeCount < routeCap)
                routes[routeCount++] = &h;
        for (size_t i = 1; i < routeCount; i++) { // insertion sort: tiny, and stable on equal hop counts
            const HeardPath *key = routes[i];
            size_t j = i;
            while (j > 0 && routes[j - 1]->hopCount > key->hopCount) {
                routes[j] = routes[j - 1];
                j--;
            }
            routes[j] = key;
        }
    }
    const HeardPath *best = routeCount ? routes[0] : nullptr;

    LOG_INFO("MeshCore .path: %u routes, %u MeshCore nodes known", (unsigned)routeCount, (unsigned)nodes.count());

    // Never on air: mark the command delivered and answer it here, in the same channel / chat. A long report goes out
    // as several messages, so the sender must be in place before the first piece is streamed.
    reportDelivery(direct ? p->to : PSEUDO_NODE_NUM, p->id, meshtastic_Routing_Error_NONE);
    const MeshCoreNode *c = direct ? contactByNodeNum(p->to) : nullptr;
    if (direct) {
        if (!c)
            return true; // the contact is gone; the command still must not go out
        ensureSenderNode(c->nodeNum, c->name, true, c->pub, meshtastic_Config_DeviceConfig_Role_CLIENT,
                         best ? MeshCoreRx(best->hopCount, best->snr, best->rssi) : MeshCoreRx::unmeasured(0));
    } else {
        ensurePseudoNode();
    }

    const uint32_t fromNum = c ? c->nodeNum : PSEUDO_NODE_NUM;
    const uint32_t to = direct ? nodeDB->getNodeNum() : NODENUM_BROADCAST;
    const uint8_t channel = direct ? 0 : p->channel;
    const uint32_t replyId = entry ? entry->packetId : p->id;
    const uint8_t hops = best ? best->hopCount : 0;
    const float snr = best ? best->snr : 0;
    const int32_t rssi = best ? best->rssi : 0;

    // A text of ours nobody has re-flooded yet has an entry but no route: that is no data either.
    if (routeCount) {
        ReportInject inject = {this, fromNum, to, replyId, channel, direct, c, hops, snr, rssi};
        formatPathReport(routes, routeCount, pathSenderName(entry->fromNum), injectReportChunk, &inject);
    } else {
        char report[sizeof(p->decoded.payload.bytes) + 1];
        if (entry && entry->own)
            snprintf(report, sizeof(report), "🛰️ *Sin ecos*: tu mensaje fue enviado, pero ningún repetidor lo ha reemitido aún.");
        else
            snprintf(report, sizeof(report), "🛰️ *Sin datos*: no he visto la ruta de ese mensaje.");
        sanitizeUtf8(report, sizeof(report));
        injectText(fromNum, to, channel, report, replyId, direct, c, hops, snr, rssi);
    }
    LOG_INFO("MeshCore path command answered locally (direct=%d, routes=%u)", direct, (unsigned)routeCount);
    return true;
}

static const MeshCoreNode *findNodeForTrace(const MeshCoreNodeStore<MESHCORE_NODES_MAX> &nodes, const char *name)
{
    if (!name || !name[0])
        return nullptr;
    const MeshCoreNode *match = nullptr;
    nodes.forEach([&](const MeshCoreNode &n) {
        if (match) // forEach visits used slots only
            return;
        if (strcasecmp(n.name, name) == 0) {
            match = &n;
            return;
        }
        char clean[MeshCoreNode::NAME_LEN];
        cleanPathLabel(n.name, clean, sizeof(clean), sizeof(clean) - 1);
        stripMeshCoreSuffix(clean);
        if (strcasecmp(clean, name) == 0) {
            match = &n;
        }
    });
    if (match)
        return match;

    if (name[0] == '!' || (name[0] >= '0' && name[0] <= '9') || (name[0] >= 'a' && name[0] <= 'f') ||
        (name[0] >= 'A' && name[0] <= 'F')) {
        const char *hexStr = (name[0] == '!') ? name + 1 : name;
        char *endp = nullptr;
        unsigned long val = strtoul(hexStr, &endp, 16);
        if (endp && *endp == 0 && val != 0) {
            if (val > 0xFFFFFF) {
                match = nodes.findByNodeNum((uint32_t)val);
                if (match)
                    return match;
            }
            if (strlen(hexStr) == 2) {
                const uint8_t hByte = (uint8_t)val;
                nodes.forEach([&](const MeshCoreNode &n) {
                    if (n.lastMod && n.pub[0] == hByte && !match)
                        match = &n;
                });
                if (match)
                    return match;
            }
        }
    }

    if (nodeDB) {
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNodeByIndex(i);
            if (!nodeInfoLiteIsMeshCorePeer(lite))
                continue;
            char clean[48];
            cleanPathLabel(lite->long_name, clean, sizeof(clean), sizeof(clean) - 1);
            stripMeshCoreSuffix(clean);
            if (strcasecmp(clean, name) == 0 || strcasecmp(lite->short_name, name) == 0) {
                match = nodes.findByNodeNum(lite->num);
                if (match)
                    return match;
            }
        }
    }
    return match;
}

static bool hexDigitValue(char c, uint8_t *v)
{
    if (c >= '0' && c <= '9')
        *v = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
        *v = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
        *v = (uint8_t)(c - 'A' + 10);
    else
        return false;
    return true;
}

MeshCoreCompanion::TargetLookup MeshCoreCompanion::resolveTraceTarget(const char *typed, TraceTarget &out, char *candidates,
                                                                      size_t candidatesCap) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (candidates && candidatesCap)
        candidates[0] = 0;
    out = TraceTarget();
    auto take = [&](uint8_t hash, const char *name) {
        out.hash = hash;
        strncpy(out.name, name, sizeof(out.name) - 1);
        return TargetLookup::FOUND;
    };

    if (const MeshCoreNode *n = findNodeForTrace(nodes, typed))
        return take(n->pub[0], n->name);

    // A repeater the node DB still holds, by its whole name.
    for (size_t i = 0; nodeDB && typed && i < nodeDB->getNumMeshNodes(); i++) {
        const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNodeByIndex(i);
        if (!isMeshCoreNode(lite) || lite->long_name[0] == 0)
            continue;
        char clean[48];
        cleanPathLabel(lite->long_name, clean, sizeof(clean), sizeof(clean) - 1);
        stripMeshCoreSuffix(clean);
        if (strcasecmp(clean, typed) == 0 || strcasecmp(lite->short_name, typed) == 0)
            return take(lite->public_key.bytes[0], lite->long_name);
    }

    // The repeaters kept as a short name, then as the start of a key typed in hex.
    constexpr size_t LISTED = 4;
    const MeshCoreRepeaterName *found[LISTED];
    size_t total = repeaters.findByName(typed, found, LISTED);
    bool byHex = false;
    if (total == 0 && typed) {
        const char *digits = typed[0] == '!' ? typed + 1 : typed;
        const size_t nDigits = strlen(digits);
        uint8_t prefix[MeshCoreRepeaterName::PREFIX_LEN] = {};
        bool hex = nDigits == 2 || nDigits == 4 || nDigits == 6;
        for (size_t i = 0; hex && i < nDigits; i++) {
            uint8_t hi;
            hex = hexDigitValue(digits[i], &hi);
            if (hex)
                prefix[i / 2] = (uint8_t)((prefix[i / 2] << 4) | hi);
        }
        if (hex) {
            byHex = true;
            total = repeaters.findByPrefix(prefix, (uint8_t)(nDigits / 2), found, LISTED);
        }
    }
    if (total == 0)
        return TargetLookup::NOT_FOUND;

    char name[MeshCoreRepeaterName::NAME_LEN + 1];
    if (total == 1) {
        found[0]->copyName(name);
        return take(found[0]->prefix[0], name);
    }
    // Several repeaters start with the hex key typed, so they all share its first byte, however many there are: the trace can
    // only name that byte, so they are one target. (Only LISTED of them fit in found[], but the count is not what matters.)
    if (byHex)
        return take(found[0]->prefix[0], typed[0] == '!' ? typed + 1 : typed);

    size_t len = 0;
    for (size_t i = 0; candidates && i < total && i < LISTED && len + 1 < candidatesCap; i++) {
        found[i]->copyName(name);
        const int n = snprintf(candidates + len, candidatesCap - len, "%s%s [%02x%02x%02x]", i ? ", " : "", name,
                               found[i]->prefix[0], found[i]->prefix[1], found[i]->prefix[2]);
        if (n <= 0 || (size_t)n >= candidatesCap - len)
            break;
        len += (size_t)n;
    }
    return TargetLookup::AMBIGUOUS;
}

const MeshCoreNode *MeshCoreCompanion::findContact(const char *nameOrHex) const
{
    concurrency::RecursiveLockGuard guard(lock);
    return findNodeForTrace(nodes, nameOrHex);
}

size_t MeshCoreCompanion::formatNodeList(uint8_t typeFilter, char *out, size_t cap) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!out || cap == 0)
        return 0;
    out[0] = 0;
    // Contacts live in the node store and repeaters in their own table: a filter names which of the two it wants.
    const bool wantStore = typeFilter != MeshCoreNode::REPEATER;
    const bool wantRepeaters = typeFilter == 0 || typeFilter == MeshCoreNode::REPEATER;
    size_t count = wantRepeaters ? repeaters.count() : 0;
    if (wantStore) {
        nodes.forEach([&](const MeshCoreNode &n) {
            if (!n.lastMod)
                return;
            if (typeFilter && n.type != typeFilter)
                return;
            count++;
        });
    }

    const char *title = (typeFilter == MeshCoreNode::CHAT)       ? "Contacts"
                        : (typeFilter == MeshCoreNode::REPEATER) ? "Repeaters"
                                                                 : "Nodes";
    if (count == 0) {
        snprintf(out, cap, "MESHTACORE: no %s learned yet", title);
        return strlen(out);
    }
    size_t len = snprintf(out, cap, "MESHTACORE %s (%u):\n", title, (unsigned)count);

    auto routeGlyph = [](uint8_t hops, char *route, size_t routeCap) {
        route[0] = 0;
        if (hops == 0)
            snprintf(route, routeCap, "⚡");
        else if (hops != MeshCoreNode::HOPS_UNKNOWN)
            snprintf(route, routeCap, "%uh", (unsigned)hops);
    };
    auto append = [&](int nChars) {
        if (nChars > 0 && (size_t)nChars < cap - len)
            len += nChars;
        else
            len = cap - 1;
    };

    if (wantStore) {
        nodes.forEach([&](const MeshCoreNode &n) {
            if (!n.lastMod || len >= cap - 1)
                return;
            if (typeFilter && n.type != typeFilter)
                return;
            char hexPub[9];
            snprintf(hexPub, sizeof(hexPub), "%02x%02x%02x%02x", n.pub[0], n.pub[1], n.pub[2], n.pub[3]);
            char route[16];
            routeGlyph(n.hops(), route, sizeof(route));
            append(snprintf(out + len, cap - len, "!%08x %s [%s] %s SNR:%.1f RSSI:%d\n", (unsigned)n.nodeNum, n.name, hexPub,
                            route, (double)n.snr, (int)n.rssi));
        });
    }
    // Newest first. A repeater is kept as a key prefix and a short name, so it has no RSSI to show.
    for (const MeshCoreRepeaterName *r = wantRepeaters ? repeaters.newest() : nullptr; r && len < cap - 1;
         r = repeaters.nextOlder(r->heard)) {
        char name[MeshCoreRepeaterName::NAME_LEN + 1];
        r->copyName(name);
        char route[16];
        routeGlyph(r->hops(), route, sizeof(route));
        append(snprintf(out + len, cap - len, "%s [%02x%02x%02x] %s SNR:%.1f\n", name, r->prefix[0], r->prefix[1], r->prefix[2],
                        route, (double)r->snr()));
    }
    if (len > 0 && out[len - 1] == '\n')
        out[--len] = 0;
    return len;
}

bool MeshCoreCompanion::formatNodeDetail(const char *nameOrHex, char *out, size_t cap) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!nameOrHex || !out || cap == 0)
        return false;
    const MeshCoreNode *n = findContact(nameOrHex);
    if (!n) {
        // Not a contact: a repeater, which is kept as a key prefix and a short name. Several can answer to the same letters.
        const MeshCoreRepeaterName *heard[2];
        const size_t total = repeaters.findByName(nameOrHex, heard, 2);
        if (total == 0)
            return false;
        if (total > 1) {
            snprintf(out, cap, "'%s' matches several repeaters: type more of the name", nameOrHex);
            return true;
        }
        char name[MeshCoreRepeaterName::NAME_LEN + 1];
        heard[0]->copyName(name);
        snprintf(out, cap, "%s (REPEATER) key:%02x%02x%02x.. hops:%u SNR:%.1f", name, heard[0]->prefix[0], heard[0]->prefix[1],
                 heard[0]->prefix[2], (unsigned)(heard[0]->hops() == MeshCoreRepeaterName::HOPS_UNKNOWN ? 0 : heard[0]->hops()),
                 (double)heard[0]->snr());
        return true;
    }

    char hexPub[17];
    snprintf(hexPub, sizeof(hexPub), "%02x%02x%02x%02x%02x%02x%02x%02x", n->pub[0], n->pub[1], n->pub[2], n->pub[3], n->pub[4],
             n->pub[5], n->pub[6], n->pub[7]);
    const char *typeName = (n->type == MeshCoreNode::CHAT)       ? "CHAT"
                           : (n->type == MeshCoreNode::REPEATER) ? "REPEATER"
                           : (n->type == MeshCoreNode::ROOM)     ? "ROOM"
                                                                 : "OTHER";

    snprintf(out, cap, "!%08x %s (%s) key:%s.. hops:%u SNR:%.1f RSSI:%d", (unsigned)n->nodeNum, n->name, typeName, hexPub,
             (unsigned)(n->hops() == MeshCoreNode::HOPS_UNKNOWN ? 0 : n->hops()), (double)n->snr, (int)n->rssi);
    return true;
}

bool MeshCoreCompanion::addDrop(const uint8_t *prefix, uint8_t len)
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!prefix || len == 0 || len > 4)
        return false;
    for (size_t i = 0; i < MAX_MC_DROPS; i++) {
        if (drops[i].len == len && memcmp(drops[i].prefix, prefix, len) == 0)
            return true;
    }
    for (size_t i = 0; i < MAX_MC_DROPS; i++) {
        if (drops[i].len == 0) {
            memcpy(drops[i].prefix, prefix, len);
            drops[i].len = len;
            return true;
        }
    }
    return false;
}

bool MeshCoreCompanion::removeDrop(const uint8_t *prefix, uint8_t len)
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!prefix || len == 0 || len > 4)
        return false;
    for (size_t i = 0; i < MAX_MC_DROPS; i++) {
        if (drops[i].len == len && memcmp(drops[i].prefix, prefix, len) == 0) {
            drops[i].len = 0;
            return true;
        }
    }
    return false;
}

void MeshCoreCompanion::clearDrops()
{
    concurrency::RecursiveLockGuard guard(lock);
    memset(drops, 0, sizeof(drops));
}

size_t MeshCoreCompanion::listDrops(char *out, size_t cap) const
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!out || cap == 0)
        return 0;
    size_t len = snprintf(out, cap, "MESHTACORE drops: ");
    size_t count = 0;
    for (size_t i = 0; i < MAX_MC_DROPS; i++) {
        if (drops[i].len > 0) {
            char hex[9] = "";
            for (size_t k = 0; k < drops[i].len; k++)
                snprintf(hex + k * 2, sizeof(hex) - k * 2, "%02x", drops[i].prefix[k]);
            int n = snprintf(out + len, cap - len, "%s%s", count ? ", " : "", hex);
            if (n > 0)
                len += n;
            count++;
        }
    }
    if (count == 0)
        snprintf(out, cap, "MESHTACORE drops: none");
    return strlen(out);
}

bool MeshCoreCompanion::isDropped(const uint8_t *hash, uint8_t hashLen) const
{
    if (!hash || hashLen == 0)
        return false;
    for (size_t i = 0; i < MAX_MC_DROPS; i++) {
        if (drops[i].len > 0 && drops[i].len <= hashLen) {
            if (memcmp(drops[i].prefix, hash, drops[i].len) == 0)
                return true;
        }
    }
    return false;
}

bool MeshCoreCompanion::startTrace(const char *args, uint32_t fromNum, uint32_t to, uint8_t channel, uint32_t replyId,
                                   bool direct, const MeshCoreNode *c, char *statusReply, size_t statusCap)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    if (activeTrace.active) {
        if (statusReply && statusCap)
            snprintf(statusReply, statusCap, "MESHTACORE: trace already in progress");
        else
            injectText(fromNum, to, channel, "🛰️ *Trace en curso*: espera a que termine.", replyId, direct, c, 0, 0, 0);
        return true;
    }

    const char *pStr = args ? args : "";
    bool symmetric = false;
    char targetNames[ActiveTrace::MAX_TRACE_HOPS][MeshCoreNode::NAME_LEN] = {};
    TraceTarget targets[ActiveTrace::MAX_TRACE_HOPS];
    size_t targetCount = 0;

    while (*pStr) {
        while (*pStr == ' ' || *pStr == '\t')
            pStr++;
        if (!*pStr)
            break;

        if (strncmp(pStr, "...", 3) == 0) {
            symmetric = true;
            pStr += 3;
            continue;
        }
        if ((uint8_t)pStr[0] == 0xE2 && (uint8_t)pStr[1] == 0xA6) {
            symmetric = true;
            pStr += 2;
            continue;
        }

        char name[MeshCoreNode::NAME_LEN] = {};
        size_t nlen = 0;
        if (*pStr == '@') {
            pStr++;
            if (*pStr == '[') {
                pStr++;
                while (*pStr && *pStr != ']' && nlen < sizeof(name) - 1)
                    name[nlen++] = *pStr++;
                if (*pStr == ']')
                    pStr++;
            } else {
                while (*pStr && *pStr != ' ' && *pStr != '\t' && nlen < sizeof(name) - 1) {
                    if (strncmp(pStr, "...", 3) == 0) {
                        symmetric = true;
                        pStr += 3;
                        break;
                    }
                    name[nlen++] = *pStr++;
                }
            }
        } else {
            while (*pStr && *pStr != ' ' && *pStr != '\t' && nlen < sizeof(name) - 1) {
                if (strncmp(pStr, "...", 3) == 0) {
                    symmetric = true;
                    pStr += 3;
                    break;
                }
                name[nlen++] = *pStr++;
            }
        }

        trimInPlace(name);
        if (name[0] != 0) {
            size_t l = strlen(name);
            if (l >= 3 && strcmp(&name[l - 3], "...") == 0) {
                symmetric = true;
                name[l - 3] = 0;
                trimInPlace(name);
            }
            if (name[0] != 0 && targetCount < (ActiveTrace::MAX_TRACE_HOPS / 2)) {
                strncpy(targetNames[targetCount++], name, sizeof(targetNames[0]) - 1);
            }
        }
    }

    if (targetCount == 0) {
        if (statusReply && statusCap) {
            snprintf(statusReply, statusCap, "MESHTACORE: TRACE expects target node/repeater");
        } else {
            char help[128];
            snprintf(help, sizeof(help), "🛰️ *Uso*: ..trace @nodo1 @nodo2 ... (termina en ... para vuelta simétrica)");
            injectText(fromNum, to, channel, help, replyId, direct, c, 0, 0, 0);
        }
        return true;
    }

    for (size_t i = 0; i < targetCount; i++) {
        char candidates[96];
        const TargetLookup found = resolveTraceTarget(targetNames[i], targets[i], candidates, sizeof(candidates));
        if (found != TargetLookup::FOUND) {
            const bool ambiguous = found == TargetLookup::AMBIGUOUS;
            if (statusReply && statusCap) {
                if (ambiguous)
                    snprintf(statusReply, statusCap, "MESHTACORE: '%s' is ambiguous: %s", targetNames[i], candidates);
                else
                    snprintf(statusReply, statusCap, "MESHTACORE: node not found '%s'", targetNames[i]);
            } else {
                char err[160];
                if (ambiguous)
                    snprintf(err, sizeof(err), "🛰️ *Nombre ambiguo*: @%s → %s", targetNames[i], candidates);
                else
                    snprintf(err, sizeof(err), "🛰️ *Nodo no encontrado*: @%s", targetNames[i]);
                injectText(fromNum, to, channel, err, replyId, direct, c, 0, 0, 0);
            }
            return true;
        }
    }

    uint8_t hopHashes[ActiveTrace::MAX_TRACE_HOPS];
    char hopLabels[ActiveTrace::MAX_TRACE_HOPS][MeshCoreNode::NAME_LEN];
    size_t totalHops = 0;

    for (size_t i = 0; i < targetCount; i++) {
        hopHashes[totalHops] = targets[i].hash;
        cleanPathLabel(targets[i].name, hopLabels[totalHops], sizeof(hopLabels[0]), sizeof(hopLabels[0]) - 1);
        stripMeshCoreSuffix(hopLabels[totalHops]);
        totalHops++;
    }

    if (symmetric) {
        if (targetCount > 1) {
            for (size_t i = targetCount - 1; i > 0; i--) {
                hopHashes[totalHops] = targets[i - 1].hash;
                cleanPathLabel(targets[i - 1].name, hopLabels[totalHops], sizeof(hopLabels[0]), sizeof(hopLabels[0]) - 1);
                stripMeshCoreSuffix(hopLabels[totalHops]);
                totalHops++;
            }
        }
        const uint8_t ourHash = aliasReady ? aliasPub[0] : identityPub[0];
        hopHashes[totalHops] = ourHash;
        strncpy(hopLabels[totalHops], "Nosotros", sizeof(hopLabels[0]) - 1);
        totalHops++;
    }

    const uint32_t tag = generatePacketId();
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t flen = 0;
    frame[flen++] = MC_HEADER(MC_PAYLOAD_TYPE_TRACE, MC_ROUTE_DIRECT);
    frame[flen++] = 0;
    memcpy(&frame[flen], &tag, 4);
    flen += 4;
    const uint32_t auth_code = 0;
    memcpy(&frame[flen], &auth_code, 4);
    flen += 4;
    frame[flen++] = 0; // flags = 0 (1-byte hash size)
    for (size_t i = 0; i < totalHops; i++)
        frame[flen++] = hopHashes[i];

    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (!iface || !iface->queueMeshCoreFrame(frame, flen)) {
        if (statusReply && statusCap) {
            snprintf(statusReply, statusCap, "MESHTACORE: radio not available or TX not allowed");
        } else {
            char err[128];
            snprintf(err, sizeof(err), "🛰️ *Error*: Radio no disponible o transmisión no permitida.");
            injectText(fromNum, to, channel, err, replyId, direct, c, 0, 0, 0);
        }
        return true;
    }

    activeTrace.active = true;
    activeTrace.symmetric = symmetric;
    activeTrace.tag = tag;
    activeTrace.startMs = Time::getMillis();
    uint32_t timeoutMs = 6000 + (totalHops * 3500);
    if (timeoutMs > 30000)
        timeoutMs = 30000;
    activeTrace.deadlineMs = activeTrace.startMs + timeoutMs;
    activeTrace.replyId = replyId;
    activeTrace.fromNum = fromNum;
    activeTrace.to = to;
    activeTrace.channel = channel;
    activeTrace.direct = direct;
    activeTrace.totalHops = (uint8_t)totalHops;
    for (size_t i = 0; i < totalHops; i++) {
        activeTrace.hopHashes[i] = hopHashes[i];
        strncpy(activeTrace.hopNames[i], hopLabels[i], sizeof(activeTrace.hopNames[0]) - 1);
    }

    if (statusReply && statusCap) {
        snprintf(statusReply, statusCap, "MESHTACORE: trace started (%u hops%s)", (unsigned)totalHops,
                 symmetric ? ", symmetric" : "");
    } else {
        char startMsg[128];
        if (symmetric)
            snprintf(startMsg, sizeof(startMsg), "🛰️ *Iniciando trace* (%u saltos con vuelta)...", (unsigned)totalHops);
        else
            snprintf(startMsg, sizeof(startMsg), "🛰️ *Iniciando trace* (%u saltos, solo ida)...", (unsigned)totalHops);
        injectText(fromNum, to, channel, startMsg, replyId, direct, c, 0, 0, 0);
    }
    return true;
}

bool MeshCoreCompanion::handleTraceCommand(const meshtastic_MeshPacket *p, bool direct)
{
    concurrency::RecursiveLockGuard guard(lock);
    storeChanges++;
    if (!p || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;
    if (!direct && !channels.isMeshCoreChannel((ChannelIndex)p->channel))
        return false;

    char text[sizeof(p->decoded.payload.bytes) + 1];
    const size_t len =
        p->decoded.payload.size < sizeof(p->decoded.payload.bytes) ? p->decoded.payload.size : sizeof(p->decoded.payload.bytes);
    memcpy(text, p->decoded.payload.bytes, len);
    text[len] = 0;
    trimInPlace(text);

    if (strncasecmp(text, "..trace", 7) != 0)
        return false;
    if (text[7] != 0 && text[7] != ' ' && text[7] != '\t')
        return false;

    reportDelivery(direct ? p->to : PSEUDO_NODE_NUM, p->id, meshtastic_Routing_Error_NONE);
    const MeshCoreNode *c = direct ? contactByNodeNum(p->to) : nullptr;
    if (direct) {
        if (!c)
            return true;
        ensureSenderNode(c->nodeNum, c->name, true, c->pub, meshtastic_Config_DeviceConfig_Role_CLIENT);
    } else {
        ensurePseudoNode();
    }

    const uint32_t fromNum = c ? c->nodeNum : PSEUDO_NODE_NUM;
    const uint32_t to = direct ? nodeDB->getNodeNum() : NODENUM_BROADCAST;
    const uint8_t channel = direct ? 0 : p->channel;
    const uint32_t replyId = p->id;

    return startTrace(text + 7, fromNum, to, channel, replyId, direct, c);
}

void MeshCoreCompanion::handleTraceFrame(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                         int32_t rssi)
{
    if (!activeTrace.active || info.payload_len < 9)
        return;
    const uint8_t *payload = &raw[info.header_len];
    uint32_t tag = 0;
    memcpy(&tag, payload, 4);
    if (tag != activeTrace.tag)
        return;

    const uint8_t *pathSnrs = pathOf(raw, info);

    const bool isFinalHop = (activeTrace.symmetric && info.path_hash_count >= activeTrace.totalHops - 1) ||
                            (!activeTrace.symmetric && info.path_hash_count >= activeTrace.totalHops);

    if (isFinalHop) {
        char report[512];
        size_t rlen = 0;
        int n = snprintf(report, sizeof(report), "🛰️ *Trace completado* (%u saltos):\n", (unsigned)activeTrace.totalHops);
        if (n > 0)
            rlen += (size_t)n;

        for (size_t h = 0; h < activeTrace.totalHops && rlen + 64 < sizeof(report); h++) {
            const char *fromName = (h == 0) ? "Nosotros" : activeTrace.hopNames[h - 1];
            const char *toName = activeTrace.hopNames[h];
            float hopSnr = 0.0f;
            if (h < (size_t)info.path_hash_count)
                hopSnr = ((float)(int8_t)pathSnrs[h]) / 4.0f;
            else
                hopSnr = snr;
            n = snprintf(report + rlen, sizeof(report) - rlen, "• %s › %s: SNR %.1f dB\n", fromName, toName, (double)hopSnr);
            if (n > 0)
                rlen += (size_t)n;
        }

        sanitizeUtf8(report, sizeof(report));
        const MeshCoreNode *c = activeTrace.direct ? contactByNodeNum(activeTrace.to) : nullptr;
        injectText(activeTrace.fromNum, activeTrace.to, activeTrace.channel, report, activeTrace.replyId, activeTrace.direct, c,
                   activeTrace.totalHops, snr, rssi);
        activeTrace.active = false;
    }
}

void MeshCoreCompanion::checkTraceTimeout()
{
    if (activeTrace.active && Throttle::deadlinePassed(activeTrace.deadlineMs)) {
        activeTrace.active = false;
        char msg[128];
        snprintf(msg, sizeof(msg), "🛰️ *Trace sin respuesta*: no se recibió respuesta tras %u s.",
                 (unsigned)((Time::getMillis() - activeTrace.startMs) / 1000));
        const MeshCoreNode *c = activeTrace.direct ? contactByNodeNum(activeTrace.to) : nullptr;
        injectText(activeTrace.fromNum, activeTrace.to, activeTrace.channel, msg, activeTrace.replyId, activeTrace.direct, c, 0,
                   0, 0);
    }
}
#endif

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
