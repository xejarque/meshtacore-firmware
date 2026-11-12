// SPDX-License-Identifier: AGPL-3.0-only
//
// /mtc MESHCORE (alias MCORE): reads and sets the MeshCore relay and ADVERT settings from the Nexus CLI. The settings
// live in RadioLibInterface (MeshCoreConfig); this file parses the operator's words and formats one reply. Like the other
// tactical settings, a change is volatile until /mtc SAVE.

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "mesh/PathReport.h"
#include "mesh/RadioLibInterface.h"
#include "mesh/Throttle.h"
#include "modules/NexusModule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/NexusMeshCoreFirewall.h"
#include "modules/NexusMeshCoreKey.h"

#include "mesh/MeshCoreRepeaterNames.h" // the compact entries the neighbour listings read
#include "mesh/MeshCoreNodeStore.h" // MeshCoreNode, which the NODES/CONTACTS/REPEATERS commands name even without a companion
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
#include "mesh/MeshCoreCompanion.h"
#endif

// Nexus's base64 encoder (NexusCommands.cpp), shared so a key reads the same as every other /mtc export.
void nexus_base64_encode(char *out, const uint8_t *in, size_t in_len, bool url_safe);

namespace meshtastic
{

static bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool parseHexPsk(unsigned char *buffer, size_t maxLen, size_t *outLen, const char *input)
{
    if (!input || !*input)
        return false;
    const char *hex = input;
    if (strncasecmp(hex, "0x", 2) == 0)
        hex += 2;
    size_t slen = strlen(hex);
    if ((slen != 32 && slen != 64) || (slen / 2 > maxLen))
        return false;
    for (size_t i = 0; i < slen; i++) {
        if (!isHexDigit(hex[i]))
            return false;
    }
    auto nib = [](char c) -> uint8_t { return (c >= 'a') ? (c - 'a' + 10) : ((c >= 'A') ? (c - 'A' + 10) : (c - '0')); };
    for (size_t i = 0; i < slen; i += 2)
        buffer[i / 2] = (nib(hex[i]) << 4) | nib(hex[i + 1]);
    if (outLen)
        *outLen = slen / 2;
    return true;
}

bool meshCoreKeyHexToBase64(const char *hex, char *out, size_t outLen)
{
    static constexpr size_t KEY_LEN = 16;
    if (!hex || !out || outLen < 4 * ((KEY_LEN + 2) / 3) + 1)
        return false;
    while (*hex == ' ')
        hex++;
    if (strncasecmp(hex, "0x", 2) == 0)
        hex += 2;

    char digits[2 * KEY_LEN + 1];
    size_t n = 0;
    for (; *hex; hex++) {
        if (*hex == ' ' || *hex == '-' || *hex == ':')
            continue;
        if (!isHexDigit(*hex) || n == 2 * KEY_LEN)
            return false;
        digits[n++] = *hex;
    }
    digits[n] = '\0';

    uint8_t key[KEY_LEN];
    size_t keyLen = 0;
    if (n != 2 * KEY_LEN || !parseHexPsk(key, sizeof(key), &keyLen, digits))
        return false;
    nexus_base64_encode(out, key, keyLen, false);
    return true;
}

// How long ago a neighbour was heard, in its largest unit: "5s", "12m", "3h", "2d".
static void formatAgo(uint32_t secs, char *out, size_t len)
{
    if (secs < 60)
        snprintf(out, len, "%us", (unsigned)secs);
    else if (secs < 3600)
        snprintf(out, len, "%um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, len, "%uh", (unsigned)(secs / 3600));
    else
        snprintf(out, len, "%ud", (unsigned)(secs / 86400));
}

static bool isDirectNeighbor(const meshtastic_NodeInfoLite *n, uint32_t myNum)
{
    return n && n->num != 0 && n->num != myNum && !nodeInfoLiteIsMeshCore(n) && (n->num & 0xFF000000) != 0x4D000000 &&
           n->has_hops_away && n->hops_away == 0 && n->last_heard != 0;
}

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
static bool isDirectNeighbor(const MeshCoreNode &n)
{
    return n.lastMod != 0 && n.hops() == 0 && (n.lastHeard != 0 || n.lastAdvert != 0);
}
#endif

void NexusModule::cleanNeighborName(const char *src, char *out, size_t cap, size_t maxBytes)
{
    ::cleanNeighborName(src, out, cap, maxBytes);
}

NexusModule::DirectNeighborEntry NexusModule::neighborEntries[40];

static void formatHexPrefix(const uint8_t prefix[MeshCoreRepeaterName::PREFIX_LEN], char *out, size_t outLen)
{
    snprintf(out, outLen, "%02X%02X%02X", prefix[0], prefix[1], prefix[2]);
}

/// A neighbour listed from a compact table entry: its short name, or its key prefix when it announced none.
[[maybe_unused]] static NexusModule::DirectNeighborEntry neighborFromEntry(const MeshCoreRepeaterName &e, uint32_t ageSecs, uint32_t now)
{
    NexusModule::DirectNeighborEntry entry = {};
    char name[MeshCoreRepeaterName::NAME_LEN + 1];
    e.copyName(name);
    if (name[0])
        strncpy(entry.name, name, sizeof(entry.name) - 1);
    else
        formatHexPrefix(e.prefix, entry.name, sizeof(entry.name));
    entry.snr = e.snr();
    entry.lastHeard = now > ageSecs ? now - ageSecs : 0;
    entry.nodeNum = e.nodeNum;
    return entry;
}

[[maybe_unused]] static bool listsNeighbor(const NexusModule::DirectNeighborEntry *entries, size_t count, uint32_t nodeNum, const char *name)
{
    for (size_t j = 0; j < count; j++)
        if (entries[j].nodeNum == nodeNum || (name && name[0] && strncmp(entries[j].name, name, sizeof(entries[j].name) - 1) == 0))
            return true;
    return false;
}

// Keeps the list sorted newest first and, once full, lets a newer neighbour push out the oldest one.
static void insertNewestNeighbor(NexusModule::DirectNeighborEntry *entries, size_t *count, size_t cap,
                                 const NexusModule::DirectNeighborEntry &entry)
{
    size_t at = *count < cap ? (*count)++ : cap;
    if (at == cap) {
        if (entries[cap - 1].lastHeard >= entry.lastHeard)
            return; // older than everything kept
        at = cap - 1;
    }
    while (at > 0 && entries[at - 1].lastHeard < entry.lastHeard) {
        entries[at] = entries[at - 1];
        at--;
    }
    entries[at] = entry;
}

size_t NexusModule::formatNeighborLine(const DirectNeighborEntry &entry, uint32_t now, char *out, size_t outLen)
{
    char ago[8];
    formatAgo(now > entry.lastHeard ? now - entry.lastHeard : 0, ago, sizeof(ago));
    char cleanName[24];
    cleanNeighborName(entry.name[0] ? entry.name : "???", cleanName, sizeof(cleanName), 20);
    const int n = snprintf(out, outLen, "%s %.1fdB %s\n", cleanName, entry.snr, ago);
    if (n < 0)
        return 0;
    return (size_t)n < outLen ? (size_t)n : outLen - 1;
}

void NexusModule::appendNeighborLines(const DirectNeighborEntry *entries, size_t count, uint32_t now, const char *header,
                                      char *reply, size_t replyLen)
{
    size_t len = snprintf(reply, replyLen, "%s", header);
    if (len >= replyLen)
        len = replyLen - 1;
    for (size_t i = 0; i < count; i++) {
        char item[96];
        const size_t iLen = formatNeighborLine(entries[i], now, item, sizeof(item));
        if (len + iLen >= replyLen)
            break;
        memcpy(reply + len, item, iLen + 1);
        len += iLen;
    }
    if (len > 0 && reply[len - 1] == '\n')
        reply[--len] = '\0';
}

void NexusModule::renderNeighbors(const DirectNeighborEntry *entries, size_t count, const char *header, const char *title,
                                  char *reply, size_t replyLen)
{
    const uint32_t now = getTime(false);
    size_t totalLen = strlen(header);

    for (size_t i = 0; i < count; i++) {
        char item[96];
        totalLen += formatNeighborLine(entries[i], now, item, sizeof(item));
    }

    const bool fitsInOneReply = totalLen + 6 < replyLen && totalLen < NEXUS_MAX_REPLY_BODY - 10;
    if (fitsInOneReply || !activeRequest) {
        appendNeighborLines(entries, count, now, header, reply, replyLen);
        return;
    }

    uint32_t to = activeRequest->to == myNodeInfo.my_node_num ? activeRequest->from : 0xFFFFFFFF;
    char pagedTitle[48];
    snprintf(pagedTitle, sizeof(pagedTitle), "%s (%u)", title, (unsigned)count);
    startPaging(to, activeRequest->channel, activeRequest->id, pagedTitle);
    for (size_t i = 0; i < count; i++) {
        char item[96];
        formatNeighborLine(entries[i], now, item, sizeof(item));
        appendPaging(item);
    }
    snprintf(reply, replyLen, "Paging %s (%u)...", title, (unsigned)count);
}

/// The Meshtastic nodes heard directly, into entries (at most 32); returns how many.
static size_t collectMeshtasticNeighbors(NexusModule::DirectNeighborEntry *entries)
{
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : (nodeDB ? nodeDB->getNodeNum() : 0);
    const size_t nodeCount = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    size_t count = 0;
    for (size_t i = 0; i < nodeCount; i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (isDirectNeighbor(n, myNum) && count < 32) {
            const char *srcName = n->long_name[0] ? n->long_name : (n->short_name[0] ? n->short_name : "???");
            strncpy(entries[count].name, srcName, sizeof(entries[count].name) - 1);
            entries[count].name[sizeof(entries[count].name) - 1] = '\0';
            entries[count].snr = n->snr;
            entries[count].lastHeard = n->last_heard;
            entries[count].nodeNum = n->num;
            count++;
        }
    }
    return count;
}

void NexusModule::formatNeighbors(char *reply, size_t replyLen)
{
    DirectNeighborEntry *entries = neighborEntries;
    const size_t count = collectMeshtasticNeighbors(entries);
    if (count == 0) {
        snprintf(reply, replyLen, "Vecinos directos: 0");
        return;
    }

    char header[48];
    snprintf(header, sizeof(header), "Vecinos directos (%u):\n", (unsigned)count);
    renderNeighbors(entries, count, header, "Vecinos", reply, replyLen);
}

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER

static const char *relayName(MeshCoreConfig::RelayMode mode)
{
    switch (mode) {
    case MeshCoreConfig::RELAY_OFF:
        return "OFF";
    case MeshCoreConfig::RELAY_ON:
        return "ON";
    default:
        return "AUTO";
    }
}

// Copies the first word of args into word, cut to fit, and returns the text after it with its blanks skipped.
static const char *takeWord(const char *args, char *word, size_t wordLen)
{
    while (*args == ' ')
        args++;
    size_t n = 0;
    while (args[n] != '\0' && args[n] != ' ') {
        if (n + 1 < wordLen)
            word[n] = args[n];
        n++;
    }
    word[n < wordLen ? n : wordLen - 1] = '\0';
    const char *rest = args + n;
    while (*rest == ' ')
        rest++;
    return rest;
}

// While REQ_SIG is off, anyone who reaches a Nexus channel can change these settings: a write says so.
static void warnIfUnsigned(bool signatureRequired, char *reply, size_t replyLen)
{
    const size_t used = strlen(reply);
    if (!signatureRequired && used < replyLen)
        snprintf(reply + used, replyLen - used, " (REQ_SIG off: any Nexus channel can change this)");
}

static void formatInfo(const RadioLibInterface &radio, char *reply, size_t replyLen)
{
    const MeshCoreConfig &settings = radio.getMeshCoreConfig();
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
    const bool asRepeater = relay.getNodeType() == MeshCoreRepeater::ADV_TYPE_REPEATER;
    snprintf(reply, replyLen, "MESHTACORE: relay %s (%s), node %s, alias %s, advert %uh, next %um", relayName(settings.relay),
             relay.canRelay() ? "on" : "off", asRepeater ? "repeater" : "chat", relay.usesAlias() ? "on" : "off",
             (unsigned)settings.effectiveAdvertHours(), (unsigned)(radio.meshCoreAdvertDueInMs() / 60000UL));
    size_t used = strlen(reply);
    snprintf(reply + used, replyLen - used, ", path %uB, flood %u/%u/%u", (unsigned)MeshCoreRepeater::getOwnPathHashSize(),
             (unsigned)relay.getFloodMax(), (unsigned)relay.getFloodMaxUnscoped(), (unsigned)relay.getFloodMaxAdvert());
#if MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC
    used = strlen(reply);
    if (radio.isMeshCoreHybridSync())
        snprintf(reply + used, replyLen - used, ", sync 0x%04X/0x12", (unsigned)MESHTASTIC_MESHCORE_RX_SYNCWORD);
    else
        snprintf(reply + used, replyLen - used, ", hybrid sync OFF");
#endif
}

static void formatStats(const RadioLibInterface &radio, const char *arg, char *reply, size_t replyLen)
{
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
    while (arg && *arg == ' ')
        arg++;
    bool full = (arg && (strcasecmp(arg, "FULL") == 0 || strcasecmp(arg, "RELAY") == 0));

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    if (full) {
        snprintf(reply, replyLen,
                 "MESHTACORE STATS:\nrxSeen %lu (fld:%lu dir:%lu dup:%lu drop:%lu)\ntxRelayed %lu txDrop %lu\nAirtime tx:%lums "
                 "rx:%lums\nDefer ch:%lu rad:%lu | DM rx:%lu tx:%lu ack:%lu to:%lu",
                 (unsigned long)relay.rxSeen, (unsigned long)relay.rxFlood, (unsigned long)relay.rxDirectForUs,
                 (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped, (unsigned long)relay.txRelayed,
                 (unsigned long)relay.txDropped, (unsigned long)relay.txAirtimeMs, (unsigned long)relay.rxAirtimeMs,
                 (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio, (unsigned long)meshCoreCompanion.dmRx,
                 (unsigned long)meshCoreCompanion.dmTx, (unsigned long)meshCoreCompanion.dmAcked,
                 (unsigned long)meshCoreCompanion.dmTimedOut);
    } else {
        snprintf(reply, replyLen,
                 "MESHTACORE: rx %lu dup %lu drop %lu relayed %lu defer %lu+%lu | DM rx %lu tx %lu acked %lu timeout %lu",
                 (unsigned long)relay.rxSeen, (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped,
                 (unsigned long)relay.txRelayed, (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio,
                 (unsigned long)meshCoreCompanion.dmRx, (unsigned long)meshCoreCompanion.dmTx,
                 (unsigned long)meshCoreCompanion.dmAcked, (unsigned long)meshCoreCompanion.dmTimedOut);
    }
#else
    (void)full;
    snprintf(reply, replyLen, "MESHTACORE: rx %lu dup %lu drop %lu relayed %lu defer %lu+%lu", (unsigned long)relay.rxSeen,
             (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped, (unsigned long)relay.txRelayed,
             (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio);
#endif
}

static void setRelay(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: relay %s (%s)", relayName(radio.getMeshCoreConfig().relay),
                 radio.getMeshCoreRepeater().canRelay() ? "on" : "off");
        return;
    }

    MeshCoreConfig::RelayMode mode;
    if (strcasecmp(word, "ON") == 0)
        mode = MeshCoreConfig::RELAY_ON;
    else if (strcasecmp(word, "OFF") == 0)
        mode = MeshCoreConfig::RELAY_OFF;
    else if (strcasecmp(word, "AUTO") == 0)
        mode = MeshCoreConfig::RELAY_AUTO;
    else {
        snprintf(reply, replyLen, "MESHTACORE: RELAY takes ON, OFF or AUTO");
        return;
    }
    radio.setMeshCoreRelayMode(mode);
    snprintf(reply, replyLen, "MESHTACORE: relay %s, now %s", relayName(mode),
             radio.getMeshCoreRepeater().canRelay() ? "on" : "off");
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

static void setAdvert(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: advert every %uh, next in %um",
                 (unsigned)radio.getMeshCoreConfig().effectiveAdvertHours(), (unsigned)(radio.meshCoreAdvertDueInMs() / 60000UL));
        return;
    }
    if (strcasecmp(word, "NOW") == 0) {
        if (radio.requestMeshCoreAdvertNow())
            snprintf(reply, replyLen, "MESHTACORE: flood ADVERT queued");
        else
            snprintf(reply, replyLen, "MESHTACORE: ADVERT NOW is spaced 10 min apart");
        return;
    }

    char *end = nullptr;
    const long hours = strtol(word, &end, 10);
    if (end == word || *end != '\0' || !MeshCoreConfig::validAdvertHours((uint32_t)hours)) {
        snprintf(reply, replyLen, "MESHTACORE: ADVERT takes 1-168 hours, NOW, or nothing");
        return;
    }
    radio.setMeshCoreAdvertHours((uint8_t)hours);
    snprintf(reply, replyLen, "MESHTACORE: advert every %ldh from the next scheduled flood", hours);
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
static constexpr size_t NEIGHBOR_LIST_MAX = 32; // newest first, like the relay's own table

/// Contacts whose last advert reached us without crossing a repeater.
static void addContactNeighbors(NexusModule::DirectNeighborEntry *entries, size_t *count)
{
    meshCoreCompanion.nodes.forEach([&](const MeshCoreNode &n) {
        if (!isDirectNeighbor(n))
            return;
        NexusModule::DirectNeighborEntry entry = {};
        strncpy(entry.name, n.name[0] ? n.name : "???", sizeof(entry.name) - 1);
        entry.snr = n.snr;
        entry.lastHeard = n.lastHeard ? n.lastHeard : n.lastAdvert;
        entry.nodeNum = n.nodeNum;
        insertNewestNeighbor(entries, count, NEIGHBOR_LIST_MAX, entry);
    });
}

/// MeshCore nodes the node DB holds as heard directly, with their whole names.
static void addNodeDbNeighbors(NexusModule::DirectNeighborEntry *entries, size_t *count)
{
    if (!nodeDB)
        return;
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : nodeDB->getNodeNum();
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == 0 || n->num == myNum || !(nodeInfoLiteIsMeshCore(n) || (n->num & 0xFF000000) == 0x4D000000) ||
            !n->has_hops_away || n->hops_away != 0 || n->last_heard == 0 || listsNeighbor(entries, *count, n->num, n->long_name))
            continue;
        const char *srcName = n->long_name[0] ? n->long_name : (n->short_name[0] ? n->short_name : "???");
        NexusModule::DirectNeighborEntry entry = {};
        strncpy(entry.name, srcName, sizeof(entry.name) - 1);
        entry.snr = n->snr;
        entry.lastHeard = n->last_heard;
        entry.nodeNum = n->num;
        insertNewestNeighbor(entries, count, NEIGHBOR_LIST_MAX, entry);
    }
}

/// Repeaters heard directly this boot that the node DB no longer holds (it evicts MeshCore nodes first): their short name.
static void addRepeaterTableNeighbors(NexusModule::DirectNeighborEntry *entries, size_t *count)
{
    const MeshCoreCompanion::RepeaterTable &table = meshCoreCompanion.repeaterNames();
    const uint32_t uptime = Time::getUptimeSecs();
    const uint32_t now = getTime(false);
    for (size_t i = 0; i < table.capacity(); i++) {
        const MeshCoreRepeaterName *e = table.at(i);
        if (!e || e->hops() != 0 || !e->heardThisBoot() || listsNeighbor(entries, *count, e->nodeNum, nullptr))
            continue;
        insertNewestNeighbor(entries, count, NEIGHBOR_LIST_MAX, neighborFromEntry(*e, e->ageSecs(uptime), now));
    }
}
#endif

/// The MeshCore peers heard directly, into entries; false when this build has no MeshCore neighbour table.
static bool collectMeshCoreNeighbors(NexusModule::DirectNeighborEntry *entries, size_t *count)
{
    *count = 0;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    addContactNeighbors(entries, count);
    addNodeDbNeighbors(entries, count); // before the table: the node DB has the whole name
    addRepeaterTableNeighbors(entries, count);
    return true;
#elif MESHTASTIC_ENABLE_MESHCORE_NEIGHBORS
    RadioLibInterface *radio = RadioLibInterface::instance;
    static MeshCoreRepeater::NeighborView heard[MeshCoreRepeater::NEIGHBOR_MAX]; // 8 B each: pointers into the relay table
    *count = radio ? radio->getMeshCoreRepeater().copyNeighbors(heard, MeshCoreRepeater::NEIGHBOR_MAX) : 0;
    const uint32_t now = getTime(false);
    for (size_t i = 0; i < *count; i++)
        entries[i] = neighborFromEntry(*heard[i].node, heard[i].ageSecs, now);
    return true;
#else
    (void)entries;
    return false;
#endif
}

void NexusModule::formatMeshCoreNeighbors(char *reply, size_t replyLen)
{
    DirectNeighborEntry *entries = neighborEntries;
    size_t count = 0;
    if (!collectMeshCoreNeighbors(entries, &count)) {
        snprintf(reply, replyLen, "Vecinos MeshCore: companion off");
        return;
    }
    if (count == 0) {
        snprintf(reply, replyLen, "Vecinos MeshCore: 0 directos");
        return;
    }
    char header[48];
    snprintf(header, sizeof(header), "Vecinos MeshCore (%u):\n", (unsigned)count);
    renderNeighbors(entries, count, header, "Vecinos MeshCore", reply, replyLen);
}

void NexusModule::formatAllNeighbors(char *reply, size_t replyLen)
{
    if (!activeRequest) {
        formatNeighbors(reply, replyLen); // no chat to page into: the Meshtastic list alone fits a reply
        return;
    }
    const uint32_t now = getTime(false);
    const uint32_t to = activeRequest->to == myNodeInfo.my_node_num ? activeRequest->from : 0xFFFFFFFF;
    startPaging(to, activeRequest->channel, activeRequest->id, "Vecinos");
    // One titled section of the combined listing, its lines appended to the page being built.
    auto appendSection = [&](const char *label, const DirectNeighborEntry *list, size_t total) {
        char head[40];
        snprintf(head, sizeof(head), total ? "%s (%u):\n" : "%s: 0\n", label, (unsigned)total);
        appendPaging(head);
        for (size_t i = 0; i < total; i++) {
            char item[96];
            formatNeighborLine(list[i], now, item, sizeof(item));
            appendPaging(item);
        }
    };

    // One table serves both lists in turn: the first is already in the page when the second is collected.
    DirectNeighborEntry *entries = neighborEntries;
    size_t count = collectMeshtasticNeighbors(entries);
    appendSection("Meshtastic", entries, count);
    if (collectMeshCoreNeighbors(entries, &count))
        appendSection("MeshCore", entries, count);
    snprintf(reply, replyLen, "Paging Vecinos...");
}

// Accepts "4d", "!a1b2", "0xA1B2", "a1:b2" or "a1 b2": optional "!"/"0x" prefix, bytes optionally split by ':' or ' '.
static bool parseHexBytes(const char *str, uint8_t *out, uint8_t *lenOut, uint8_t maxLen)
{
    while (*str == ' ' || *str == '!')
        str++;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
        str += 2;
    uint8_t count = 0;
    while (*str) {
        if ((*str == ':' || *str == ' ') && count > 0) {
            str++;
            continue;
        }
        if (!isxdigit((unsigned char)str[0]) || !isxdigit((unsigned char)str[1]) || count >= maxLen)
            return false;
        char byteBuf[3] = {str[0], str[1], 0};
        out[count++] = (uint8_t)strtoul(byteBuf, nullptr, 16);
        str += 2;
    }
    if (count == 0)
        return false;
    *lenOut = count;
    return true;
}

void NexusModule::handleMeshCoreFavCommand(const char *args, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    char word[16];
    const char *rest = takeWord(args, word, sizeof(word));

    if (word[0] == '\0') {
        // No argument lists what is already kept: this command can only add or remove, never clear in bulk.
        char list[128];
        size_t len = 0;
        list[0] = '\0';
        meshCoreCompanion.nodes.forEach([&](const MeshCoreNode &n) {
            if (!n.isFavorite() || len >= sizeof(list) - 20)
                return;
            char hex[10];
            formatHexPrefix(n.pub, hex, sizeof(hex));
            len += snprintf(list + len, sizeof(list) - len, "%s%s", len ? ", " : "", hex);
        });
        snprintf(reply, replyLen, "MESHTACORE: FAV takes <hex_prefix> [ON|OFF]. %s%s", len ? "Kept: " : "No favourites yet",
                 list);
        return;
    }

    uint8_t prefix[4] = {};
    uint8_t plen = 0;
    if (!parseHexBytes(word, prefix, &plen, sizeof(prefix))) {
        snprintf(reply, replyLen, "MESHTACORE: FAV expects 1-4 hex bytes of the public key (e.g. 4d, a1b2)");
        return;
    }

    MeshCoreNode *matches[8];
    const size_t found = meshCoreCompanion.nodes.findByPrefix(prefix, plen, matches, sizeof(matches) / sizeof(matches[0]));
    if (found == 0) {
        snprintf(reply, replyLen, "MESHTACORE: no contact starts with '%s'", word);
        return;
    }
    if (found > 1) {
        // MeshCore hashes collide within a prefix: name the candidates instead of guessing which one was meant.
        char list[128];
        size_t len = 0;
        list[0] = '\0';
        for (size_t i = 0; i < found && len < sizeof(list) - 20; i++) {
            char hex[10];
            formatHexPrefix(matches[i]->pub, hex, sizeof(hex));
            len += snprintf(list + len, sizeof(list) - len, "%s%s", len ? ", " : "", hex);
        }
        snprintf(reply, replyLen, "MESHTACORE: '%s' matches %u contacts: %s", word, (unsigned)found, list);
        return;
    }

    bool on = true;
    char opt[8];
    takeWord(rest, opt, sizeof(opt));
    if (strcasecmp(opt, "OFF") == 0)
        on = false;
    else if (opt[0] != '\0' && strcasecmp(opt, "ON") != 0) {
        snprintf(reply, replyLen, "MESHTACORE: FAV takes ON or OFF");
        return;
    }

    MeshCoreNode *n = matches[0];
    meshCoreCompanion.nodes.setFavorite(n, on);
    meshCoreCompanion.nodes.touch(n); // a saved favourite ranks first, so keep it recent too
    char hex[10];
    formatHexPrefix(n->pub, hex, sizeof(hex));
    snprintf(reply, replyLen, "MESHTACORE: %s %s (%s)%s", on ? "kept" : "released", n->name[0] ? n->name : "sin nombre", hex,
             on ? ", saved on the next contacts write" : "");
#else
    (void)args;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

// Reads a whole decimal number from word. False for an empty word, letters or trailing text.
static bool parseWholeNumber(const char *word, uint32_t *value)
{
    char *end = nullptr;
    const long parsed = strtol(word, &end, 10);
    if (end == word || *end != '\0' || parsed < 0)
        return false;
    *value = (uint32_t)parsed;
    return true;
}

void NexusModule::handleMeshCoreDiscoverCommand(const char *args, const ReplyContext &requester, char *reply, size_t replyLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "MESHTACORE: no radio");
        return;
    }
    if (meshCoreDiscoverPending) {
        const uint32_t now = Time::getMillis();
        const uint32_t leftMs = Throttle::deadlinePassedAt(now, meshCoreDiscoverUntil) ? 0 : meshCoreDiscoverUntil - now;
        snprintf(reply, replyLen, "🔎 Descubriendo ya (%us restantes)", (unsigned)((leftMs + 999) / 1000));
        return;
    }

    // MeshCore's discover.neighbors waits 60 s because it answers over the CLI synchronously; here the listing comes
    // as a second message, so 10 s is enough for the repeater's randomised reply delay.
    uint32_t seconds = 10;
    char word[8];
    takeWord(args, word, sizeof(word));
    if (word[0] != '\0') {
        if (!parseWholeNumber(word, &seconds) || seconds < 5 || seconds > 60) {
            snprintf(reply, replyLen, "MESHTACORE: DESCUBRIR takes 5-60 seconds, or nothing");
            return;
        }
    }

    if (!radio->getMeshCoreRepeater().startDiscover(seconds * 1000UL)) {
        snprintf(reply, replyLen, "MESHTACORE: no se pudo lanzar la búsqueda (radio ocupada)");
        return;
    }
    // Only a scan that really started takes over the reply context: a refused or repeated request must not redirect
    // the listing the first requester is waiting for.
    meshCoreDiscoverReply = requester;
    meshCoreDiscoverWindowMs = seconds * 1000UL;
    meshCoreDiscoverUntil = radio->getMeshCoreRepeater().discoverDeadlineMs(); // one deadline, one clock read
    meshCoreDiscoverPending = true;
    snprintf(reply, replyLen, "🔎 Descubriendo vecinos MeshCore (%us)...", (unsigned)seconds);
}

void NexusModule::finishMeshCoreDiscover()
{
    meshCoreDiscoverPending = false;
    if (!RadioLibInterface::instance)
        return;

    MeshCoreRepeater::DiscoveredPeer found[MeshCoreRepeater::DISCOVER_RESULTS_MAX];
    const size_t count =
        RadioLibInterface::instance->getMeshCoreRepeater().finishDiscover(found, sizeof(found) / sizeof(found[0]));

    DirectNeighborEntry *entries = neighborEntries;
    size_t listed = 0;
    size_t added = 0; // peers the node store had never seen
    const uint32_t now = getTime(false);
    for (size_t i = 0; i < count && listed < 32; i++) {
        const MeshCoreRepeater::DiscoveredPeer &peer = found[i];
        char name[sizeof(entries[0].name)];
        name[0] = '\0';

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
        // Store it where every other MeshCore node lives, so CONTACTS and VECINOS see it too. A full table is left
        // untouched: a scan must never evict a contact the operator saved or talked to.
        const bool wasKnown = meshCoreCompanion.nodes.find(peer.pub) != nullptr;
        MeshCoreNode *n = meshCoreCompanion.nodes.upsertIfRoom(peer.pub);
        if (n) {
            // The discover answer is unsigned: it may name a new node's type, never overwrite one an ADVERT set.
            if (!wasKnown) {
                added++;
                n->type = peer.type;
            }
            n->snr = peer.rxSnr;
            n->setHops(0); // it answered a zero-hop request: it is a direct neighbour by definition
            n->lastHeard = now;
            if (n->nodeNum == 0)
                meshCoreCompanion.nodes.assignNodeNum(n, MeshCoreNodeStore<MESHCORE_NODES_MAX>::nodeNumForKey(peer.pub),
                                                      MeshCoreCompanion::PSEUDO_NODE_NUM);
            if (n->name[0])
                strncpy(name, n->name, sizeof(name) - 1);
        }
#endif
        if (!name[0]) // no ADVERT yet: the answer carries no name, so show the key prefix
            formatHexPrefix(peer.pub, name, sizeof(name));

        strncpy(entries[listed].name, name, sizeof(entries[listed].name) - 1);
        entries[listed].name[sizeof(entries[listed].name) - 1] = '\0';
        entries[listed].snr = peer.rxSnr;
        entries[listed].lastHeard = now;
        entries[listed].nodeNum = 0;
        listed++;
    }

    char body[NEXUS_MAX_REPLY_BODY];
    if (listed == 0) {
        snprintf(body, sizeof(body), "Vecinos MeshCore: nadie respondió (%us)",
                 (unsigned)(meshCoreDiscoverWindowMs / 1000));
    } else {
        snprintf(body, sizeof(body), "Vecinos MeshCore (%u, %u nuevos):\n", (unsigned)listed, (unsigned)added);
        // One line per peer, built with the same renderer as VECINOS so both listings read alike.
        size_t shown = 0;
        for (; shown < listed; shown++) {
            char item[96];
            formatNeighborLine(entries[shown], now, item, sizeof(item));
            if (strlen(body) + strlen(item) + 24 >= sizeof(body)) // room left for the "and N more" line
                break;
            strncat(body, item, sizeof(body) - strlen(body) - 1);
        }
        if (shown < listed) {
            const size_t used = strlen(body);
            snprintf(body + used, sizeof(body) - used, "… y %u más", (unsigned)(listed - shown));
        }
    }

    const size_t len = strlen(body);
    size_t outLen = len;
    if (outLen > 0 && body[outLen - 1] == '\n')
        body[--outLen] = '\0';
    sendReply(meshCoreDiscoverReply, body, outLen);
}

static void setPathSize(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[10];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: the floods we originate carry %u-byte path hashes",
                 (unsigned)MeshCoreRepeater::getOwnPathHashSize());
        return;
    }
    uint32_t bytes = 0;
    if (strcasecmp(word, "DEFAULT") == 0) {
        radio.setMeshCorePathHashSize(0);
    } else if (parseWholeNumber(word, &bytes) && MeshCoreConfig::validPathHashSize(bytes)) {
        radio.setMeshCorePathHashSize((uint8_t)bytes);
    } else {
        snprintf(reply, replyLen, "MESHTACORE: PATHSIZE takes 1, 2 or 3 (bytes per repeater in the path), DEFAULT, or nothing");
        return;
    }
    snprintf(reply, replyLen, "MESHTACORE: %u-byte path hashes from the next flood we originate",
             (unsigned)MeshCoreRepeater::getOwnPathHashSize());
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

static void setFloodMax(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
    // Up to four words are read so that a stray fourth one is refused instead of ignored.
    char words[4][10];
    const char *rest = arg;
    for (auto &word : words)
        rest = takeWord(rest, word, sizeof(word));

    if (words[0][0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: flood max %u, unscoped %u, advert %u", (unsigned)relay.getFloodMax(),
                 (unsigned)relay.getFloodMaxUnscoped(), (unsigned)relay.getFloodMaxAdvert());
        return;
    }
    if (strcasecmp(words[0], "DEFAULT") == 0) {
        radio.setMeshCoreFloodLimits(0, 0, 0);
    } else {
        uint32_t limits[3] = {};
        bool valid = words[3][0] == '\0';
        for (size_t i = 0; i < 3; i++)
            valid = valid && parseWholeNumber(words[i], &limits[i]) && MeshCoreConfig::validFloodMax(limits[i]);
        if (!valid) {
            snprintf(reply, replyLen, "MESHTACORE: FLOODMAX <1-64 any flood> <1-64 unscoped> <1-64 advert>, DEFAULT, or nothing");
            return;
        }
        radio.setMeshCoreFloodLimits((uint8_t)limits[0], (uint8_t)limits[1], (uint8_t)limits[2]);
    }
    snprintf(reply, replyLen, "MESHTACORE: flood max %u, unscoped %u, advert %u", (unsigned)relay.getFloodMax(),
             (unsigned)relay.getFloodMaxUnscoped(), (unsigned)relay.getFloodMaxAdvert());
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

static void formatIdentity(const RadioLibInterface &radio, char *reply, size_t replyLen)
{
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
    if (!relay.hasIdentity()) {
        snprintf(reply, replyLen, "MESHTACORE: no identity yet (it is created with the first ADVERT)");
        return;
    }
    const uint8_t *pub = relay.getIdentityPub();
    char hex[2 * 32 + 1];
    for (size_t i = 0; i < 32; i++)
        snprintf(&hex[i * 2], 3, "%02x", pub[i]);
    snprintf(reply, replyLen, "MESHTACORE: id=%02x%02x%02x%02x type=%u name=%s\npub=%s", pub[0], pub[1], pub[2], pub[3],
             (unsigned)relay.getNodeType(), owner.long_name[0] ? owner.long_name : "(none)", hex);
    if (relay.usesAlias()) {
        const uint8_t *alias = relay.getAliasPub();
        const size_t used = strlen(reply);
        if (used < replyLen)
            snprintf(reply + used, replyLen - used, "\nrelay alias=%02x%02x%02x%02x", alias[0], alias[1], alias[2], alias[3]);
    }
}

static void setHybrid(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC
    if (!radio.supportsMeshCoreHybridSync()) {
        snprintf(reply, replyLen, "MESHTACORE: this radio has no hybrid sync word");
        return;
    }
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: hybrid sync %s",
                 radio.isMeshCoreHybridSync() ? "ON (Meshtastic and MeshCore heard)" : "OFF (Meshtastic only)");
        return;
    }
    const bool on = strcasecmp(word, "ON") == 0 || strcmp(word, "1") == 0;
    const bool off = strcasecmp(word, "OFF") == 0 || strcmp(word, "0") == 0;
    if (!on && !off) {
        snprintf(reply, replyLen, "MESHTACORE: HYBRID takes ON, OFF, or nothing");
        return;
    }
    radio.setMeshCoreHybridSync(on);
    snprintf(reply, replyLen, "MESHTACORE: hybrid sync %s%s", on ? "ON" : "OFF, MeshCore is not heard",
             on ? "" : " until reboot");
    warnIfUnsigned(signatureRequired, reply, replyLen);
#else
    (void)radio;
    (void)arg;
    (void)signatureRequired;
    snprintf(reply, replyLen, "MESHTACORE: hybrid sync is not compiled in");
#endif
}

static void formatKey(const char *hex, char *reply, size_t replyLen)
{
    char base64[25];
    if (meshCoreKeyHexToBase64(hex, base64, sizeof(base64)))
        snprintf(reply, replyLen, "MESHTACORE KEY: %s (pega esto como clave del canal MC:<nombre> en la app Meshtastic)", base64);
    else
        snprintf(reply, replyLen, "MESHTACORE KEY: pon los 32 digitos hex que MeshCore muestra como clave del canal");
}

static void formatRegionTree(const MeshCoreRegionMap &map, char *reply, size_t replyLen)
{
    static char tree[256];
    size_t len = map.exportTo(tree, sizeof(tree));
    if (len == 0) {
        snprintf(reply, replyLen, "MESHTACORE REGION: tree empty");
        return;
    }
    if (len + 14 < replyLen) {
        snprintf(reply, replyLen, "MESHTACORE REGION:\n%s", tree);
        size_t rlen = strlen(reply);
        if (rlen > 0 && reply[rlen - 1] == '\n')
            reply[rlen - 1] = '\0';
    } else {
        snprintf(reply, replyLen, "MESHTACORE REGION (%d regs):\n%.*s...", map.getCount(), (int)(replyLen - 30), tree);
    }
}

void NexusModule::handleRegionCommand(const char *args, char *reply, size_t replyLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "MESHTACORE REGION: no radio");
        return;
    }
    MeshCoreRepeater &relay = radio->getMeshCoreRepeater();
    MeshCoreRegionMap &map = relay.getRegionMap();
    const bool signatureRequired = securityRequiresSignature();

    char subcmd[16];
    const char *rest = takeWord(args, subcmd, sizeof(subcmd));

    if (subcmd[0] == '\0' || strcasecmp(subcmd, "TREE") == 0 || strcasecmp(subcmd, "SHOW") == 0) {
        formatRegionTree(map, reply, replyLen);
    } else if (strcasecmp(subcmd, "HELP") == 0) {
        snprintf(
            reply, replyLen,
            "MESHTACORE REGION: TREE, LIST [ALLOWED|DENIED], GET <n>, PUT <n> [p], REMOVE <n>, ALLOWF <n>, DENYF <n>, HOME [n], "
            "DEFAULT [n|<null>], DEF <tokens>, CLEAR, SAVE, LOAD");
    } else if (strcasecmp(subcmd, "LIST") == 0) {
        char mode[16];
        takeWord(rest, mode, sizeof(mode));
        static char listBuf[160];
        if (strcasecmp(mode, "ALLOWED") == 0) {
            int count = map.exportNamesTo(listBuf, sizeof(listBuf), REGION_DENY_FLOOD, false);
            snprintf(reply, replyLen, "MESHTACORE REGION ALLOWED (%d): %s", count, count > 0 ? listBuf : "-none-");
        } else if (strcasecmp(mode, "DENIED") == 0) {
            int count = map.exportNamesTo(listBuf, sizeof(listBuf), REGION_DENY_FLOOD, true);
            snprintf(reply, replyLen, "MESHTACORE REGION DENIED (%d): %s", count, count > 0 ? listBuf : "-none-");
        } else {
            snprintf(reply, replyLen, "MESHTACORE REGION: LIST expects ALLOWED or DENIED");
        }
    } else if (strcasecmp(subcmd, "GET") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MESHTACORE REGION: GET expects <region_name>");
            return;
        }
        const RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MESHTACORE REGION: unknown region '%s'", name);
            return;
        }
        const RegionEntry *parent = map.findById(r->parent);
        const bool isHome = (r->id != 0 && map.getHomeRegion() == r);
        const bool isDefault = (r->id != 0 && map.getDefaultRegion() == r);
        const bool floodAllowed = !(r->flags & REGION_DENY_FLOOD);
        snprintf(reply, replyLen, "MESHTACORE REGION: %s (parent: %s) [FLOOD: %s]%s%s", r->name, parent ? parent->name : "*",
                 floodAllowed ? "ALLOWED" : "DENIED", isHome ? " (HOME)" : "", isDefault ? " (DEFAULT)" : "");
    } else if (strcasecmp(subcmd, "PUT") == 0) {
        char name[32];
        char parentName[32];
        const char *rest2 = takeWord(rest, name, sizeof(name));
        takeWord(rest2, parentName, sizeof(parentName));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MESHTACORE REGION: PUT expects <name> [parent]");
            return;
        }
        const RegionEntry *parent = &map.getWildcard();
        if (parentName[0] != '\0') {
            parent = map.findByNamePrefix(parentName);
            if (!parent) {
                snprintf(reply, replyLen, "MESHTACORE REGION: unknown parent '%s'", parentName);
                return;
            }
        }
        RegionEntry *r = map.putRegion(name, parent->id);
        if (!r) {
            snprintf(reply, replyLen, "MESHTACORE REGION: unable to put '%s' (table full or invalid name)", name);
            return;
        }
        r->flags &= ~REGION_DENY_FLOOD; // Flood allowed by default
        snprintf(reply, replyLen, "MESHTACORE REGION: %s under %s (flood allowed)", r->name, parent->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "REMOVE") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MESHTACORE REGION: REMOVE expects <name>");
            return;
        }
        RegionEntry *r = map.findByName(name);
        if (!r) {
            snprintf(reply, replyLen, "MESHTACORE REGION: region '%s' not found", name);
            return;
        }
        if (!map.removeRegion(*r)) {
            snprintf(reply, replyLen, "MESHTACORE REGION: cannot remove '%s' (has child regions or is *)", name);
            return;
        }
        snprintf(reply, replyLen, "MESHTACORE REGION: removed '%s'", name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "ALLOWF") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MESHTACORE REGION: ALLOWF expects <name>");
            return;
        }
        RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MESHTACORE REGION: unknown region '%s'", name);
            return;
        }
        r->flags &= ~REGION_DENY_FLOOD;
        snprintf(reply, replyLen, "MESHTACORE REGION: %s flood allowed", r->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DENYF") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MESHTACORE REGION: DENYF expects <name>");
            return;
        }
        RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MESHTACORE REGION: unknown region '%s'", name);
            return;
        }
        r->flags |= REGION_DENY_FLOOD;
        snprintf(reply, replyLen, "MESHTACORE REGION: %s flood denied", r->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "HOME") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            const RegionEntry *h = map.getHomeRegion();
            snprintf(reply, replyLen, "MESHTACORE REGION: home is %s", h ? h->name : "*");
            return;
        }
        if (strcmp(name, "*") == 0 || strcasecmp(name, "<null>") == 0 || strcasecmp(name, "NONE") == 0) {
            map.setHomeRegion(nullptr);
            snprintf(reply, replyLen, "MESHTACORE REGION: home is now *");
            warnIfUnsigned(signatureRequired, reply, replyLen);
            return;
        }
        const RegionEntry *h = map.findByNamePrefix(name);
        if (!h) {
            snprintf(reply, replyLen, "MESHTACORE REGION: unknown region '%s'", name);
            return;
        }
        map.setHomeRegion(h);
        snprintf(reply, replyLen, "MESHTACORE REGION: home is now %s", h->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DEFAULT") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            const RegionEntry *d = map.getDefaultRegion();
            snprintf(reply, replyLen, "MESHTACORE REGION: default scope is %s", d ? d->name : "<null>");
            return;
        }
        if (strcasecmp(name, "<null>") == 0 || strcasecmp(name, "NONE") == 0 || strcasecmp(name, "OFF") == 0 ||
            strcmp(name, "*") == 0) {
            map.setDefaultRegion(nullptr);
            snprintf(reply, replyLen, "MESHTACORE REGION: default scope cleared");
            warnIfUnsigned(signatureRequired, reply, replyLen);
            return;
        }
        RegionEntry *d = map.findByNamePrefix(name);
        if (!d)
            d = map.putRegion(name, 0);
        if (!d) {
            snprintf(reply, replyLen, "MESHTACORE REGION: region table full");
            return;
        }
        d->flags &= ~REGION_DENY_FLOOD;
        map.setDefaultRegion(d);
        snprintf(reply, replyLen, "MESHTACORE REGION: default scope is now %s", d->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DEF") == 0) {
        char errBuf[64] = {0};
        if (map.processDef(rest, errBuf, sizeof(errBuf))) {
            snprintf(reply, replyLen, "MESHTACORE REGION: def applied (%d regions)", map.getCount());
        } else {
            snprintf(reply, replyLen, "MESHTACORE REGION: %s", errBuf);
        }
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "CLEAR") == 0) {
        map.clear();
        snprintf(reply, replyLen, "MESHTACORE REGION: regions cleared");
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "SAVE") == 0) {
        bool ok = radio->saveMeshCoreRegions();
        snprintf(reply, replyLen, "MESHTACORE REGION: %s", ok ? "regions saved to flash" : "save failed");
    } else if (strcasecmp(subcmd, "LOAD") == 0) {
        bool ok = radio->reloadMeshCoreRegions();
        snprintf(reply, replyLen, "MESHTACORE REGION: %s (%d regions)", ok ? "regions loaded" : "load failed", map.getCount());
    } else {
        snprintf(reply, replyLen, "MESHTACORE REGION: unknown subcommand '%s', try HELP", subcmd);
    }
}

static void handleMeshCorePathCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: PATH expects <contact|!hex|packetId>");
        return;
    }
    char *end = nullptr;
    unsigned long id = strtoul(arg, &end, 0);
    if (id != 0 && end && *end == '\0') {
        if (meshCoreCompanion.formatRouteForPacket(id, reply, replyLen, 0.0f,
                                                   MeshCoreCompanion::SIGNAL_SNR | MeshCoreCompanion::SIGNAL_RSSI))
            return;
    }
    const MeshCoreNode *contact = meshCoreCompanion.findContact(arg);
    if (!contact) {
        snprintf(reply, replyLen, "MESHTACORE: no contact matching '%s'", arg);
        return;
    }
    const MeshCoreCompanion::HeardPath *route = meshCoreCompanion.heardFindByFrom(contact->nodeNum);
    if (!route) {
        snprintf(reply, replyLen, "MESHTACORE: no route heard for %s", contact->name);
        return;
    }
    static char line[NEXUS_MAX_REPLY];
    if (meshCoreCompanion.formatRouteLine(*route, line, sizeof(line), 0.0f,
                                          MeshCoreCompanion::SIGNAL_SNR | MeshCoreCompanion::SIGNAL_RSSI)) {
        snprintf(reply, replyLen, "MESHTACORE: %s › %s", contact->name, line);
    } else {
        snprintf(reply, replyLen, "MESHTACORE: route format failed for %s", contact->name);
    }
#else
    (void)arg;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreTraceCommand(const char *args, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    uint8_t targetChannel = 0;
    for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
        if (channels.isMeshCoreChannel(i)) {
            targetChannel = (uint8_t)i;
            break;
        }
    }
    meshCoreCompanion.startTrace(args, MeshCoreCompanion::PSEUDO_NODE_NUM, NODENUM_BROADCAST, targetChannel, 0, false, nullptr,
                                 reply, replyLen);
#else
    (void)args;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreNodesCommand(uint8_t typeFilter, const char *arg, char *reply, size_t replyLen)
{
    (void)arg;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.formatNodeList(typeFilter, reply, replyLen);
#else
    (void)typeFilter;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreNodeDetailCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: NODE expects <name|!hex>");
        return;
    }
    if (!meshCoreCompanion.formatNodeDetail(arg, reply, replyLen))
        snprintf(reply, replyLen, "MESHTACORE: node '%s' not found", arg);
#else
    (void)arg;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreSendCommand(const char *args, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    char target[32];
    const char *text = takeWord(args, target, sizeof(target));
    while (text && *text == ' ')
        text++;
    if (target[0] == '\0' || !text || text[0] == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: SEND expects <contact|channel> <text>");
        return;
    }
    const MeshCoreNode *contact = meshCoreCompanion.findContact(target);
    if (contact) {
        RadioInterface *iface = router ? router->getRadioIface() : nullptr;
        if (meshCoreCompanion.sendDirectText(contact->nodeNum, generatePacketId(), text, Time::getMillis(), iface))
            snprintf(reply, replyLen, "MESHTACORE: DM sent to %s", contact->name);
        else
            snprintf(reply, replyLen, "MESHTACORE: DM failed to %s", contact->name);
        return;
    }
    // A channel name wins over a bare digit, so a channel literally named "1" stays addressable by name.
    int targetCh = -1;
    for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
        auto &ch = channels.getByIndex(i);
        if (ch.role != meshtastic_Channel_Role_DISABLED && strcasecmp(ch.settings.name, target) == 0) {
            targetCh = (int)i;
            break;
        }
    }
    if (targetCh < 0 && isdigit((unsigned char)*target) && *(target + 1) == '\0')
        targetCh = *target - '0';

    if (targetCh >= 0 && (size_t)targetCh < MAX_NUM_CHANNELS) {
        auto &ch = channels.getByIndex((ChannelIndex)targetCh);
        if (ch.role != meshtastic_Channel_Role_DISABLED) {
            meshtastic_MeshPacket *tp = router ? router->allocForSending() : nullptr;
            if (tp) {
                tp->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
                tp->to = NODENUM_BROADCAST;
                tp->channel = targetCh;
                size_t tlen = strlen(text);
                if (tlen > sizeof(tp->decoded.payload.bytes))
                    tlen = sizeof(tp->decoded.payload.bytes);
                memcpy(tp->decoded.payload.bytes, text, tlen);
                tp->decoded.payload.size = tlen;
                if (service)
                    service->sendToMesh(tp);
                const char *name = channels.getName(targetCh);
                if (name && name[0])
                    snprintf(reply, replyLen, "MESHTACORE: sent on Ch %d (%s)", targetCh, name);
                else
                    snprintf(reply, replyLen, "MESHTACORE: sent on Ch %d", targetCh);
            } else {
                snprintf(reply, replyLen, "MESHTACORE: alloc failed");
            }
            return;
        }
    }
    snprintf(reply, replyLen, "MESHTACORE: target '%s' not found (expected contact or channel)", target);
#else
    (void)args;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreDropCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: DROP expects <hex_prefix|CLEAR>");
        return;
    }
    if (strcasecmp(arg, "CLEAR") == 0) {
        meshCoreCompanion.clearDrops();
        snprintf(reply, replyLen, "MESHTACORE: drops cleared");
        return;
    }
    uint8_t prefix[4] = {};
    uint8_t plen = 0;
    if (!parseHexBytes(arg, prefix, &plen, 4)) {
        snprintf(reply, replyLen, "MESHTACORE: DROP expects 1-4 hex bytes (e.g. 4d, a1b2, 0xA1, a1:b2)");
        return;
    }
    if (meshCoreCompanion.addDrop(prefix, plen))
        snprintf(reply, replyLen, "MESHTACORE: dropped prefix %s", arg);
    else
        snprintf(reply, replyLen, "MESHTACORE: drop list full (max %u)", (unsigned)MeshCoreCompanion::MAX_MC_DROPS);
#else
    (void)arg;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

static void handleMeshCoreUndropCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "MESHTACORE: UNDROP expects <hex_prefix>");
        return;
    }
    uint8_t prefix[4] = {};
    uint8_t plen = 0;
    if (!parseHexBytes(arg, prefix, &plen, 4)) {
        snprintf(reply, replyLen, "MESHTACORE: UNDROP expects 1-4 hex bytes");
        return;
    }
    if (meshCoreCompanion.removeDrop(prefix, plen))
        snprintf(reply, replyLen, "MESHTACORE: undropped prefix %s", arg);
    else
        snprintf(reply, replyLen, "MESHTACORE: prefix %s not found in drop list", arg);
#else
    (void)arg;
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

bool meshCoreFirewallSetRule(const char *tag, bool on)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    for (const MeshCoreConfig::DropRule &rule : MeshCoreConfig::DROP_RULES) {
        if (strcasecmp(tag, rule.tag) != 0)
            continue;
        if (!radio)
            return true; // a known tag on a node without a radio: nothing to change
        const MeshCoreConfig cfg = radio->currentMeshCoreConfig();
        const uint8_t mask = on ? (cfg.dropMask | rule.bit) : (cfg.dropMask & (uint8_t)~rule.bit);
        radio->setMeshCoreFirewall(mask, cfg.advertGapMin, cfg.hopRatePerMin);
        return true;
    }
    return false;
}

bool meshCoreFirewallCommand(const char *cmd, char *reply, size_t replyLen)
{
    const bool gap = strncasecmp(cmd, "MCGAP", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ');
    const bool rate = strncasecmp(cmd, "MCRATE", 6) == 0 && (cmd[6] == '\0' || cmd[6] == ' ');
    if (!gap && !rate)
        return false;
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "MESHTACORE: no radio");
        return true;
    }
    const MeshCoreConfig cfg = radio->currentMeshCoreConfig();
    const char *arg = cmd + (gap ? 5 : 6);
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        if (gap)
            snprintf(reply, replyLen, "MESHTACORE: MCGAP %u min (0 = off), one advert per key in that time", cfg.advertGapMin);
        else
            snprintf(reply, replyLen, "MESHTACORE: MCRATE %u frames/min (0 = off) from one neighbouring repeater",
                     cfg.hopRatePerMin);
        return true;
    }
    uint32_t value = 0;
    if (!parseWholeNumber(arg, &value) || value > 255) {
        snprintf(reply, replyLen, "MESHTACORE: %s takes 0-255", gap ? "MCGAP" : "MCRATE");
        return true;
    }
    radio->setMeshCoreFirewall(cfg.dropMask, gap ? (uint8_t)value : cfg.advertGapMin, rate ? (uint8_t)value : cfg.hopRatePerMin);
    snprintf(reply, replyLen, "MESHTACORE: %s %u%s", gap ? "MCGAP" : "MCRATE", (unsigned)value, value == 0 ? " (off)" : "");
    return true;
}

size_t meshCoreFirewallActiveTags(char *out, size_t outLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    size_t len = 0;
    if (outLen)
        out[0] = '\0';
    if (!radio)
        return 0;
    // The configured mask, not the repeater's: the saved settings load lazily, and this reads them in first.
    const uint8_t mask = radio->currentMeshCoreConfig().dropMask;
    for (const MeshCoreConfig::DropRule &rule : MeshCoreConfig::DROP_RULES) {
        if ((mask & rule.bit) == 0)
            continue;
        const int n = snprintf(out + len, outLen - len, "%s ", rule.tag);
        if (n < 0 || (size_t)n >= outLen - len)
            break;
        len += (size_t)n;
    }
    return len;
}

size_t meshCoreFirewallLimits(char *out, size_t outLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        if (outLen)
            out[0] = '\0';
        return 0;
    }
    const MeshCoreConfig cfg = radio->currentMeshCoreConfig();
    const int n = snprintf(out, outLen, "MCGAP:%u MCRATE:%u", cfg.advertGapMin, cfg.hopRatePerMin);
    return n < 0 ? 0 : ((size_t)n < outLen ? (size_t)n : outLen - 1);
}

size_t meshCoreFirewallCounters(char *out, size_t outLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        if (outLen)
            out[0] = '\0';
        return 0;
    }
    const MeshCoreRepeater &repeater = radio->getMeshCoreRepeater();
    size_t len = (size_t)snprintf(out, outLen, "NIGN:%lu LIMIT:%lu", (unsigned long)repeater.rxIgnored,
                                  (unsigned long)repeater.rxRateLimited);
    for (size_t i = 0; i < MeshCoreConfig::DROP_RULE_COUNT && len < outLen; i++) {
        if (repeater.getRuleDrops(i) == 0)
            continue;
        const int n = snprintf(out + len, outLen - len, " %s:%lu", MeshCoreConfig::DROP_RULES[i].tag,
                               (unsigned long)repeater.getRuleDrops(i));
        if (n < 0 || (size_t)n >= outLen - len)
            break;
        len += (size_t)n;
    }
    return len < outLen ? len : outLen - 1;
}

static void handleMeshCoreDropsCommand(char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.listDrops(reply, replyLen);
#else
    snprintf(reply, replyLen, "MESHTACORE: companion disabled");
#endif
}

void NexusModule::handleMeshCoreCommand(const char *args, char *reply, size_t replyLen, const meshtastic_MeshPacket *mp,
                                        uint32_t replyId, bool toPhoneOnly)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "MESHTACORE: no radio");
        return;
    }
    char word[16];
    const char *rest = takeWord(args, word, sizeof(word));
    const bool signatureRequired = securityRequiresSignature();

    if (word[0] == '\0' || strcasecmp(word, "HELP") == 0) {
        snprintf(reply, replyLen,
                 "MESHTACORE: INFO, STATS [FULL], NODES, CONTACTS, REPEATERS, NODE <id>, PATH <target>, TRACE <target>, SEND "
                 "<target> <msg>, DROP [hex|CLEAR], UNDROP <hex>, DROPS, RELAY [ON|OFF|AUTO], ADVERT [1-168|NOW], REGION [...], "
                 "VECINOS, DESCUBRIR [5-60], FAV <hex> [ON|OFF], IDENTITY, KEY <hex>, PATHSIZE [1-3], FLOODMAX [g u a], "
                 "HYBRID [ON|OFF], PPING [...], PING <target>, RESET. SAVE keeps them.");
    } else if (strcasecmp(word, "INFO") == 0) {
        formatInfo(*radio, reply, replyLen);
    } else if (strcasecmp(word, "STATS") == 0) {
        formatStats(*radio, rest, reply, replyLen);
    } else if (strcasecmp(word, "NODES") == 0) {
        handleMeshCoreNodesCommand(0, rest, reply, replyLen);
    } else if (strcasecmp(word, "CONTACTS") == 0) {
        handleMeshCoreNodesCommand(MeshCoreNode::CHAT, rest, reply, replyLen);
    } else if (strcasecmp(word, "REPEATERS") == 0) {
        handleMeshCoreNodesCommand(MeshCoreNode::REPEATER, rest, reply, replyLen);
    } else if (strcasecmp(word, "NODE") == 0) {
        handleMeshCoreNodeDetailCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "PATH") == 0 || strcasecmp(word, "RUTA") == 0) {
        handleMeshCorePathCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "TRACE") == 0 || strcasecmp(word, "TRAZA") == 0) {
        handleMeshCoreTraceCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "SEND") == 0) {
        handleMeshCoreSendCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "DROP") == 0) {
        handleMeshCoreDropCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "UNDROP") == 0) {
        handleMeshCoreUndropCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "DROPS") == 0) {
        handleMeshCoreDropsCommand(reply, replyLen);
    } else if (strcasecmp(word, "RELAY") == 0) {
        setRelay(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "ADVERT") == 0) {
        setAdvert(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "REGION") == 0) {
        handleRegionCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "VECINOS") == 0 || strcasecmp(word, "NEIGHBOURS") == 0 || strcasecmp(word, "NEIGHBORS") == 0) {
        formatMeshCoreNeighbors(reply, replyLen);
    } else if (strcasecmp(word, "DESCUBRIR") == 0 || strcasecmp(word, "DISCOVER") == 0 || strcasecmp(word, "BUSCAR") == 0) {
        // The listing arrives in a second message, so the context has to survive until the window closes.
        ReplyContext requester = meshCoreDiscoverReply;
        if (mp) {
            const bool isDM = mp->to == myNodeInfo.my_node_num;
            requester = {isDM ? mp->from : 0xFFFFFFFF, mp->channel, replyId, toPhoneOnly, toPhoneOnly || !isDM};
        }
        handleMeshCoreDiscoverCommand(rest, requester, reply, replyLen);
    } else if (strcasecmp(word, "FAV") == 0) {
        handleMeshCoreFavCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "KEY") == 0) {
        formatKey(rest, reply, replyLen);
    } else if (strcasecmp(word, "PATHSIZE") == 0) {
        setPathSize(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "FLOODMAX") == 0) {
        setFloodMax(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "IDENTITY") == 0 || strcasecmp(word, "ID") == 0) {
        formatIdentity(*radio, reply, replyLen);
    } else if (strcasecmp(word, "HYBRID") == 0) {
        setHybrid(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "PPING") == 0) {
        handleMeshCorePpingCommand(rest, reply, replyLen);
    } else if (strcasecmp(word, "PING") == 0) {
        handleMeshCorePingSend(rest, reply, replyLen);
    } else if (strcasecmp(word, "RESET") == 0) {
        radio->setMeshCoreRelayMode(MeshCoreConfig::RELAY_AUTO);
        radio->setMeshCoreAdvertHours(0);
        radio->setMeshCorePathHashSize(0);
        radio->setMeshCoreFloodLimits(0, 0, 0);
        radio->setMeshCoreFirewall(0, 0, 0); // every MeshCore drop rule off, no advert gap, no hop rate limit
        snprintf(reply, replyLen, "MESHTACORE: relay AUTO, advert %uh, %u-byte paths, default flood limits, firewall off",
                 (unsigned)MeshCoreConfig::DEFAULT_ADVERT_HOURS, (unsigned)MeshCoreConfig::DEFAULT_PATH_HASH_SIZE);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else {
        snprintf(reply, replyLen, "MESHTACORE: unknown '%s', try HELP", word);
    }
}

bool NexusModule::isMeshCoreBotChannel(uint32_t channel)
{
    if (channel >= MAX_NUM_CHANNELS || !channels.isMeshCoreChannel((ChannelIndex)channel))
        return false;
    const char *chName = channels.getName(channel);
    return chName && strcasecmp(chName, "MC:#bot") == 0;
}

bool NexusModule::isMeshCorePing(const meshtastic_MeshPacket &mp, bool isPingDM)
{
    if (channels.isMeshCoreChannel((ChannelIndex)mp.channel))
        return true;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    if (isPingDM && (meshCoreCompanion.hasContact(mp.from) || (mp.from >> 24) == 0x4D))
        return true;
#endif
    return false;
}

bool NexusModule::formatMeshCorePingReply(const meshtastic_MeshPacket &mp, char *rMsg, size_t rMsgLen)
{
    if (!rMsg || rMsgLen == 0)
        return false;
    rMsg[0] = '\0';

    char sender[40];
    pingSenderName(mp, sender, sizeof(sender));
    const uint8_t signal = (ping_snr ? MeshCoreCompanion::SIGNAL_SNR : 0) | (ping_rssi ? MeshCoreCompanion::SIGNAL_RSSI : 0);

    bool built = false;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    built = meshCoreCompanion.formatPingForPacket(mp.id, rMsg, rMsgLen, sender, signal);
    if (!built) {
        // No route was heard for this packet: the same shape with the hop count taken from the packet and no names.
        MeshCoreCompanion::HeardPath none;
        none.hopCount = (mp.hop_start >= mp.hop_limit) ? (uint8_t)(mp.hop_start - mp.hop_limit) : 0;
        none.snr = mp.rx_snr;
        none.rssi = mp.rx_rssi;
        built = meshCoreCompanion.formatPingLine(none, rMsg, rMsgLen, sender, signal);
    }
#endif
    // The optional extras close the answer, each on its own line: ZephCore's text stays untouched above them.
    if (built && ping_freqerr) {
        const size_t len = strlen(rMsg);
        if (len + 1 < rMsgLen)
            snprintf(rMsg + len, rMsgLen - len, "\nΔf %.0fHz", (double)mp.rx_frequency_error);
    }
    return built;
}

void NexusModule::handleMeshCorePpingCommand(const char *args, char *reply, size_t replyLen)
{
    while (*args == ' ')
        args++;

    int botCh = -1;
    for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
        if (isMeshCoreBotChannel(i)) {
            botCh = (int)i;
            break;
        }
    }

    if (*args == '\0' || strcasecmp(args, "STATUS") == 0) {
        char botStatus[32] = "not found";
        if (botCh >= 0) {
            snprintf(botStatus, sizeof(botStatus), "Ch %d (%s)", botCh, ping_channels[botCh] ? "ON" : "OFF");
        }
        snprintf(reply, replyLen, "MESHTACORE PPING:\nGlobal:%d MC:#bot:%s RSSI:%d SNR:%d FE:%d Emoji:%d DM:%d Cmd:%s",
                 public_ping ? 1 : 0, botStatus, ping_rssi ? 1 : 0, ping_snr ? 1 : 0, ping_freqerr ? 1 : 0, (int)ping_emoji_mode,
                 ping_allow_dm ? 1 : 0, ping_trigger);
    } else if (strcasecmp(args, "ON") == 0) {
        // Only the MeshCore side: /mtc PPING owns public_ping.
        if (botCh >= 0) {
            ping_channels[botCh] = true;
            snprintf(reply, replyLen, "MESHTACORE: PPING ON (MC:#bot on Ch %d)", botCh);
        } else {
            snprintf(reply, replyLen, "MESHTACORE: PPING ON (MC:#bot channel not configured)");
        }
    } else if (strcasecmp(args, "OFF") == 0) {
        for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
            if (channels.isMeshCoreChannel(i))
                ping_channels[i] = false;
        }
        snprintf(reply, replyLen, "MESHTACORE: PPING OFF on MeshCore channels");
    } else if (applyPpingSetting(args, true, "MESHTACORE", reply, replyLen)) {
        // Shared reporting settings: see NexusCommands.cpp.
    } else {
        snprintf(reply, replyLen,
                 "MESHTACORE: PPING ON|OFF|CMD <cmd>|RSSI|SNR|FREQERR|EMOJI|ALLOWDM|CHAN|FLAGS|SETHEX|GETHEX");
    }
}

void NexusModule::handleMeshCorePingSend(const char *target, char *reply, size_t replyLen)
{
    while (*target == ' ')
        target++;

    // A contact called "Cmdr" must still be pinged, so a PPING word only counts when it is the whole first token.
    auto isPpingWord = [&](const char *word) {
        const size_t n = strlen(word);
        return strncasecmp(target, word, n) == 0 && (target[n] == '\0' || target[n] == ' ');
    };
    static const char *const ppingWords[] = {"STATUS", "ON",     "OFF",     "CMD",     "TRIGGER", "EMOJI",
                                             "RSSI",   "SNR",    "FREQERR", "ALLOWDM", "CHAN",    "FLAGS",  "OM",
                                             "SETHEX", "GETHEX", "DIRECT",  "IGNORE",  "REMOTE"};
    bool isPpingSetting = *target == '\0';
    for (const char *word : ppingWords)
        isPpingSetting = isPpingSetting || isPpingWord(word);
    if (isPpingSetting) {
        handleMeshCorePpingCommand(target, reply, replyLen);
        return;
    }

    int targetCh = -1;
    if (isdigit((unsigned char)*target) && *(target + 1) == '\0') {
        targetCh = *target - '0';
    } else {
        for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
            const char *name = channels.getName(i);
            if (name && strcasecmp(name, target) == 0) {
                targetCh = (int)i;
                break;
            }
        }
    }

    if (targetCh >= 0 && (size_t)targetCh < MAX_NUM_CHANNELS) {
        meshtastic_MeshPacket *tp = router ? router->allocForSending() : nullptr;
        if (tp) {
            tp->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
            tp->to = NODENUM_BROADCAST;
            tp->channel = targetCh;
            const char *pingMsg = (ping_trigger[0] != '\0') ? ping_trigger : "/ping";
            memcpy(tp->decoded.payload.bytes, pingMsg, strlen(pingMsg));
            tp->decoded.payload.size = strlen(pingMsg);
            if (service)
                service->sendToMesh(tp);
            snprintf(reply, replyLen, "MESHTACORE: Ping sent to Ch %d (%s)", targetCh, channels.getName(targetCh));
        } else {
            snprintf(reply, replyLen, "MESHTACORE: Packet alloc failed");
        }
        return;
    }

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    const MeshCoreNode *contact = meshCoreCompanion.findContact(target);
    if (contact) {
        meshtastic_MeshPacket *tp = router ? router->allocForSending() : nullptr;
        if (tp) {
            tp->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
            tp->to = contact->nodeNum;
            tp->channel = 0;
            const char *pingMsg = (ping_trigger[0] != '\0') ? ping_trigger : "/ping";
            memcpy(tp->decoded.payload.bytes, pingMsg, strlen(pingMsg));
            tp->decoded.payload.size = strlen(pingMsg);
            if (service)
                service->sendToMesh(tp);
            snprintf(reply, replyLen, "MESHTACORE: Ping sent to %s (!%08x)", contact->name, (unsigned)contact->nodeNum);
        } else {
            snprintf(reply, replyLen, "MESHTACORE: Packet alloc failed");
        }
        return;
    }
#endif

    snprintf(reply, replyLen, "MESHTACORE: Unknown channel or contact '%s'", target);
}

void NexusModule::saveMeshCoreSettings()
{
    if (RadioLibInterface::instance)
        RadioLibInterface::instance->saveMeshCoreConfig();
}

void NexusModule::loadMeshCoreSettings()
{
    if (RadioLibInterface::instance)
        RadioLibInterface::instance->reloadMeshCoreConfig();
}

#else // !MESHTASTIC_ENABLE_MESHCORE_SNIFFER

void NexusModule::handleMeshCoreCommand(const char *, char *reply, size_t replyLen, const meshtastic_MeshPacket *, uint32_t, bool)
{
    snprintf(reply, replyLen, "MESHTACORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::handleRegionCommand(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MESHTACORE REGION: not in this build (MeshCore sniffer off)");
}

bool NexusModule::isMeshCoreBotChannel(uint32_t)
{
    return false;
}

bool NexusModule::isMeshCorePing(const meshtastic_MeshPacket &, bool)
{
    return false;
}

bool NexusModule::formatMeshCorePingReply(const meshtastic_MeshPacket &, char *, size_t)
{
    return false;
}

void NexusModule::handleMeshCorePpingCommand(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MESHTACORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::handleMeshCorePingSend(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MESHTACORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::formatMeshCoreNeighbors(char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "Vecinos MeshCore: companion off");
}

void NexusModule::formatAllNeighbors(char *reply, size_t replyLen)
{
    formatNeighbors(reply, replyLen);
}

void NexusModule::handleMeshCoreDiscoverCommand(const char *, const ReplyContext &, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MESHTACORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::finishMeshCoreDiscover() {}

void NexusModule::handleMeshCoreFavCommand(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MESHTACORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::saveMeshCoreSettings() {}

void NexusModule::loadMeshCoreSettings() {}

#endif

} // namespace meshtastic
