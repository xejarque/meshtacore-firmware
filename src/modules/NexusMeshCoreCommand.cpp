// SPDX-License-Identifier: AGPL-3.0-only
//
// /nexus MESHCORE (alias MCORE): reads and sets the MeshCore relay and ADVERT settings from the Nexus CLI. The settings
// live in RadioLibInterface (MeshCoreConfig); this file parses the operator's words and formats one reply. Like the other
// tactical settings, a change is volatile until /nexus SAVE.

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "mesh/PathReport.h"
#include "mesh/RadioLibInterface.h"
#include "modules/NexusModule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/NexusMeshCoreKey.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
#include "mesh/MeshCoreCompanion.h"
#endif

// Nexus's base64 encoder (NexusCommands.cpp), shared so a key reads the same as every other /nexus export.
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
    return n.lastMod != 0 && n.hops == 0 && (n.lastHeard != 0 || n.lastAdvert != 0);
}
#endif

NexusModule::DirectNeighborEntry NexusModule::neighborEntries[32];

size_t NexusModule::formatNeighborLine(const DirectNeighborEntry &entry, uint32_t now, char *out, size_t outLen)
{
    char ago[8];
    formatAgo(now > entry.lastHeard ? now - entry.lastHeard : 0, ago, sizeof(ago));
    const int n = snprintf(out, outLen, "%s (%.1fdB, %s)\n", entry.name, entry.snr, ago);
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
        char item[64];
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
        char item[64];
        totalLen += formatNeighborLine(entries[i], now, item, sizeof(item));
    }

    const bool fitsInOneReply = totalLen + 6 < replyLen && totalLen < NEXUS_MAX_REPLY_BODY - 10;
    if (fitsInOneReply || !activeRequest) {
        appendNeighborLines(entries, count, now, header, reply, replyLen);
        return;
    }

    uint32_t to = activeRequest->to == myNodeInfo.my_node_num ? activeRequest->from : 0xFFFFFFFF;
    startPaging(to, activeRequest->channel, activeRequest->id, title);
    for (size_t i = 0; i < count; i++) {
        char item[64];
        formatNeighborLine(entries[i], now, item, sizeof(item));
        appendPaging(item);
    }
    snprintf(reply, replyLen, "JT: Paging %s (%u)...", title, (unsigned)count);
}

void NexusModule::formatNeighbors(char *reply, size_t replyLen)
{
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : (nodeDB ? nodeDB->getNodeNum() : 0);
    const size_t nodeCount = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    DirectNeighborEntry *entries = neighborEntries;
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
    if (count == 0) {
        snprintf(reply, replyLen, "JT VECINOS: 0 directos");
        return;
    }

    char header[32];
    snprintf(header, sizeof(header), "JT VECINOS (%u):\n", (unsigned)count);
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
    snprintf(reply, replyLen, "JT MESHCORE: relay %s (%s), node %s, alias %s, advert %uh, next %um", relayName(settings.relay),
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
                 "JT MESHCORE STATS:\nrxSeen %lu (fld:%lu dir:%lu dup:%lu drop:%lu)\ntxRelayed %lu txDrop %lu\nAirtime tx:%lums "
                 "rx:%lums\nDefer ch:%lu rad:%lu | DM rx:%lu tx:%lu ack:%lu to:%lu",
                 (unsigned long)relay.rxSeen, (unsigned long)relay.rxFlood, (unsigned long)relay.rxDirectForUs,
                 (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped, (unsigned long)relay.txRelayed,
                 (unsigned long)relay.txDropped, (unsigned long)relay.txAirtimeMs, (unsigned long)relay.rxAirtimeMs,
                 (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio, (unsigned long)meshCoreCompanion.dmRx,
                 (unsigned long)meshCoreCompanion.dmTx, (unsigned long)meshCoreCompanion.dmAcked,
                 (unsigned long)meshCoreCompanion.dmTimedOut);
    } else {
        snprintf(reply, replyLen,
                 "JT MESHCORE: rx %lu dup %lu drop %lu relayed %lu defer %lu+%lu | DM rx %lu tx %lu acked %lu timeout %lu",
                 (unsigned long)relay.rxSeen, (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped,
                 (unsigned long)relay.txRelayed, (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio,
                 (unsigned long)meshCoreCompanion.dmRx, (unsigned long)meshCoreCompanion.dmTx,
                 (unsigned long)meshCoreCompanion.dmAcked, (unsigned long)meshCoreCompanion.dmTimedOut);
    }
#else
    (void)full;
    snprintf(reply, replyLen, "JT MESHCORE: rx %lu dup %lu drop %lu relayed %lu defer %lu+%lu", (unsigned long)relay.rxSeen,
             (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped, (unsigned long)relay.txRelayed,
             (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio);
#endif
}

static void setRelay(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: relay %s (%s)", relayName(radio.getMeshCoreConfig().relay),
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
        snprintf(reply, replyLen, "JT MESHCORE: RELAY takes ON, OFF or AUTO");
        return;
    }
    radio.setMeshCoreRelayMode(mode);
    snprintf(reply, replyLen, "JT MESHCORE: relay %s, now %s", relayName(mode),
             radio.getMeshCoreRepeater().canRelay() ? "on" : "off");
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

static void setAdvert(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: advert every %uh, next in %um",
                 (unsigned)radio.getMeshCoreConfig().effectiveAdvertHours(), (unsigned)(radio.meshCoreAdvertDueInMs() / 60000UL));
        return;
    }
    if (strcasecmp(word, "NOW") == 0) {
        if (radio.requestMeshCoreAdvertNow())
            snprintf(reply, replyLen, "JT MESHCORE: flood ADVERT queued");
        else
            snprintf(reply, replyLen, "JT MESHCORE: ADVERT NOW is spaced 10 min apart");
        return;
    }

    char *end = nullptr;
    const long hours = strtol(word, &end, 10);
    if (end == word || *end != '\0' || !MeshCoreConfig::validAdvertHours((uint32_t)hours)) {
        snprintf(reply, replyLen, "JT MESHCORE: ADVERT takes 1-168 hours, NOW, or nothing");
        return;
    }
    radio.setMeshCoreAdvertHours((uint8_t)hours);
    snprintf(reply, replyLen, "JT MESHCORE: advert every %ldh from the next scheduled flood", hours);
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

void NexusModule::formatMeshCoreNeighbors(char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : (nodeDB ? nodeDB->getNodeNum() : 0);
    DirectNeighborEntry *entries = neighborEntries;
    size_t count = 0;
    meshCoreCompanion.nodes.forEach([&](const MeshCoreNode &n) {
        if (isDirectNeighbor(n) && count < 32) {
            strncpy(entries[count].name, n.name[0] ? n.name : "???", sizeof(entries[count].name) - 1);
            entries[count].name[sizeof(entries[count].name) - 1] = '\0';
            entries[count].snr = n.snr;
            entries[count].lastHeard = n.lastHeard ? n.lastHeard : n.lastAdvert;
            entries[count].nodeNum = n.nodeNum;
            count++;
        }
    });

    if (nodeDB) {
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
            if (n && n->num != 0 && n->num != myNum && (nodeInfoLiteIsMeshCore(n) || (n->num & 0xFF000000) == 0x4D000000) &&
                n->has_hops_away && n->hops_away == 0 && n->last_heard != 0) {
                bool duplicate = false;
                for (size_t j = 0; j < count; j++) {
                    if (entries[j].nodeNum == n->num ||
                        (n->long_name[0] && strncmp(entries[j].name, n->long_name, sizeof(entries[j].name) - 1) == 0)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && count < 32) {
                    const char *srcName = n->long_name[0] ? n->long_name : (n->short_name[0] ? n->short_name : "???");
                    strncpy(entries[count].name, srcName, sizeof(entries[count].name) - 1);
                    entries[count].name[sizeof(entries[count].name) - 1] = '\0';
                    entries[count].snr = n->snr;
                    entries[count].lastHeard = n->last_heard;
                    entries[count].nodeNum = n->num;
                    count++;
                }
            }
        }
    }

    if (count == 0) {
        snprintf(reply, replyLen, "JT MESHCORE: 0 vecinos directos");
        return;
    }

    char header[32];
    snprintf(header, sizeof(header), "JT MESHCORE (%u vecinos):\n", (unsigned)count);
    renderNeighbors(entries, count, header, "MeshCore Vecinos", reply, replyLen);
#else
    snprintf(reply, replyLen, "JT MESHCORE: companion off");
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

static void setPathSize(RadioLibInterface &radio, const char *arg, bool signatureRequired, char *reply, size_t replyLen)
{
    char word[10];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: the floods we originate carry %u-byte path hashes",
                 (unsigned)MeshCoreRepeater::getOwnPathHashSize());
        return;
    }
    uint32_t bytes = 0;
    if (strcasecmp(word, "DEFAULT") == 0) {
        radio.setMeshCorePathHashSize(0);
    } else if (parseWholeNumber(word, &bytes) && MeshCoreConfig::validPathHashSize(bytes)) {
        radio.setMeshCorePathHashSize((uint8_t)bytes);
    } else {
        snprintf(reply, replyLen, "JT MESHCORE: PATHSIZE takes 1, 2 or 3 (bytes per repeater in the path), DEFAULT, or nothing");
        return;
    }
    snprintf(reply, replyLen, "JT MESHCORE: %u-byte path hashes from the next flood we originate",
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
        snprintf(reply, replyLen, "JT MESHCORE: flood max %u, unscoped %u, advert %u", (unsigned)relay.getFloodMax(),
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
            snprintf(reply, replyLen,
                     "JT MESHCORE: FLOODMAX <1-64 any flood> <1-64 unscoped> <1-64 advert>, DEFAULT, or nothing");
            return;
        }
        radio.setMeshCoreFloodLimits((uint8_t)limits[0], (uint8_t)limits[1], (uint8_t)limits[2]);
    }
    snprintf(reply, replyLen, "JT MESHCORE: flood max %u, unscoped %u, advert %u", (unsigned)relay.getFloodMax(),
             (unsigned)relay.getFloodMaxUnscoped(), (unsigned)relay.getFloodMaxAdvert());
    warnIfUnsigned(signatureRequired, reply, replyLen);
}

static void formatIdentity(const RadioLibInterface &radio, char *reply, size_t replyLen)
{
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
    if (!relay.hasIdentity()) {
        snprintf(reply, replyLen, "JT MESHCORE: no identity yet (it is created with the first ADVERT)");
        return;
    }
    const uint8_t *pub = relay.getIdentityPub();
    char hex[2 * 32 + 1];
    for (size_t i = 0; i < 32; i++)
        snprintf(&hex[i * 2], 3, "%02x", pub[i]);
    snprintf(reply, replyLen, "JT MESHCORE: id=%02x%02x%02x%02x type=%u name=%s\npub=%s", pub[0], pub[1], pub[2], pub[3],
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
        snprintf(reply, replyLen, "JT MESHCORE: this radio has no hybrid sync word");
        return;
    }
    char word[8];
    takeWord(arg, word, sizeof(word));
    if (word[0] == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: hybrid sync %s",
                 radio.isMeshCoreHybridSync() ? "ON (Meshtastic and MeshCore heard)" : "OFF (Meshtastic only)");
        return;
    }
    const bool on = strcasecmp(word, "ON") == 0 || strcmp(word, "1") == 0;
    const bool off = strcasecmp(word, "OFF") == 0 || strcmp(word, "0") == 0;
    if (!on && !off) {
        snprintf(reply, replyLen, "JT MESHCORE: HYBRID takes ON, OFF, or nothing");
        return;
    }
    radio.setMeshCoreHybridSync(on);
    snprintf(reply, replyLen, "JT MESHCORE: hybrid sync %s%s", on ? "ON" : "OFF, MeshCore is not heard",
             on ? "" : " until reboot");
    warnIfUnsigned(signatureRequired, reply, replyLen);
#else
    (void)radio;
    (void)arg;
    (void)signatureRequired;
    snprintf(reply, replyLen, "JT MESHCORE: hybrid sync is not compiled in");
#endif
}

static void formatKey(const char *hex, char *reply, size_t replyLen)
{
    char base64[25];
    if (meshCoreKeyHexToBase64(hex, base64, sizeof(base64)))
        snprintf(reply, replyLen, "JT MESHCORE KEY: %s (pega esto como clave del canal MC:<nombre> en la app Meshtastic)",
                 base64);
    else
        snprintf(reply, replyLen, "JT MESHCORE KEY: pon los 32 digitos hex que MeshCore muestra como clave del canal");
}

static void formatRegionTree(const MeshCoreRegionMap &map, char *reply, size_t replyLen)
{
    static char tree[256];
    size_t len = map.exportTo(tree, sizeof(tree));
    if (len == 0) {
        snprintf(reply, replyLen, "MTC REGION: tree empty");
        return;
    }
    if (len + 14 < replyLen) {
        snprintf(reply, replyLen, "MTC REGION:\n%s", tree);
        size_t rlen = strlen(reply);
        if (rlen > 0 && reply[rlen - 1] == '\n')
            reply[rlen - 1] = '\0';
    } else {
        snprintf(reply, replyLen, "MTC REGION (%d regs):\n%.*s...", map.getCount(), (int)(replyLen - 30), tree);
    }
}

void NexusModule::handleRegionCommand(const char *args, char *reply, size_t replyLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "MTC REGION: no radio");
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
        snprintf(reply, replyLen,
                 "MTC REGION: TREE, LIST [ALLOWED|DENIED], GET <n>, PUT <n> [p], REMOVE <n>, ALLOWF <n>, DENYF <n>, HOME [n], "
                 "DEFAULT [n|<null>], DEF <tokens>, CLEAR, SAVE, LOAD");
    } else if (strcasecmp(subcmd, "LIST") == 0) {
        char mode[16];
        takeWord(rest, mode, sizeof(mode));
        static char listBuf[160];
        if (strcasecmp(mode, "ALLOWED") == 0) {
            int count = map.exportNamesTo(listBuf, sizeof(listBuf), REGION_DENY_FLOOD, false);
            snprintf(reply, replyLen, "MTC REGION ALLOWED (%d): %s", count, count > 0 ? listBuf : "-none-");
        } else if (strcasecmp(mode, "DENIED") == 0) {
            int count = map.exportNamesTo(listBuf, sizeof(listBuf), REGION_DENY_FLOOD, true);
            snprintf(reply, replyLen, "MTC REGION DENIED (%d): %s", count, count > 0 ? listBuf : "-none-");
        } else {
            snprintf(reply, replyLen, "MTC REGION: LIST expects ALLOWED or DENIED");
        }
    } else if (strcasecmp(subcmd, "GET") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MTC REGION: GET expects <region_name>");
            return;
        }
        const RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MTC REGION: unknown region '%s'", name);
            return;
        }
        const RegionEntry *parent = map.findById(r->parent);
        const bool isHome = (r->id != 0 && map.getHomeRegion() == r);
        const bool isDefault = (r->id != 0 && map.getDefaultRegion() == r);
        const bool floodAllowed = !(r->flags & REGION_DENY_FLOOD);
        snprintf(reply, replyLen, "MTC REGION: %s (parent: %s) [FLOOD: %s]%s%s", r->name, parent ? parent->name : "*",
                 floodAllowed ? "ALLOWED" : "DENIED", isHome ? " (HOME)" : "", isDefault ? " (DEFAULT)" : "");
    } else if (strcasecmp(subcmd, "PUT") == 0) {
        char name[32];
        char parentName[32];
        const char *rest2 = takeWord(rest, name, sizeof(name));
        takeWord(rest2, parentName, sizeof(parentName));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MTC REGION: PUT expects <name> [parent]");
            return;
        }
        const RegionEntry *parent = &map.getWildcard();
        if (parentName[0] != '\0') {
            parent = map.findByNamePrefix(parentName);
            if (!parent) {
                snprintf(reply, replyLen, "MTC REGION: unknown parent '%s'", parentName);
                return;
            }
        }
        RegionEntry *r = map.putRegion(name, parent->id);
        if (!r) {
            snprintf(reply, replyLen, "MTC REGION: unable to put '%s' (table full or invalid name)", name);
            return;
        }
        r->flags &= ~REGION_DENY_FLOOD; // Flood allowed by default
        snprintf(reply, replyLen, "MTC REGION: %s under %s (flood allowed)", r->name, parent->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "REMOVE") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MTC REGION: REMOVE expects <name>");
            return;
        }
        RegionEntry *r = map.findByName(name);
        if (!r) {
            snprintf(reply, replyLen, "MTC REGION: region '%s' not found", name);
            return;
        }
        if (!map.removeRegion(*r)) {
            snprintf(reply, replyLen, "MTC REGION: cannot remove '%s' (has child regions or is *)", name);
            return;
        }
        snprintf(reply, replyLen, "MTC REGION: removed '%s'", name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "ALLOWF") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MTC REGION: ALLOWF expects <name>");
            return;
        }
        RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MTC REGION: unknown region '%s'", name);
            return;
        }
        r->flags &= ~REGION_DENY_FLOOD;
        snprintf(reply, replyLen, "MTC REGION: %s flood allowed", r->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DENYF") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            snprintf(reply, replyLen, "MTC REGION: DENYF expects <name>");
            return;
        }
        RegionEntry *r = map.findByNamePrefix(name);
        if (!r) {
            snprintf(reply, replyLen, "MTC REGION: unknown region '%s'", name);
            return;
        }
        r->flags |= REGION_DENY_FLOOD;
        snprintf(reply, replyLen, "MTC REGION: %s flood denied", r->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "HOME") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            const RegionEntry *h = map.getHomeRegion();
            snprintf(reply, replyLen, "MTC REGION: home is %s", h ? h->name : "*");
            return;
        }
        if (strcmp(name, "*") == 0 || strcasecmp(name, "<null>") == 0 || strcasecmp(name, "NONE") == 0) {
            map.setHomeRegion(nullptr);
            snprintf(reply, replyLen, "MTC REGION: home is now *");
            warnIfUnsigned(signatureRequired, reply, replyLen);
            return;
        }
        const RegionEntry *h = map.findByNamePrefix(name);
        if (!h) {
            snprintf(reply, replyLen, "MTC REGION: unknown region '%s'", name);
            return;
        }
        map.setHomeRegion(h);
        snprintf(reply, replyLen, "MTC REGION: home is now %s", h->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DEFAULT") == 0) {
        char name[32];
        takeWord(rest, name, sizeof(name));
        if (name[0] == '\0') {
            const RegionEntry *d = map.getDefaultRegion();
            snprintf(reply, replyLen, "MTC REGION: default scope is %s", d ? d->name : "<null>");
            return;
        }
        if (strcasecmp(name, "<null>") == 0 || strcasecmp(name, "NONE") == 0 || strcasecmp(name, "OFF") == 0 ||
            strcmp(name, "*") == 0) {
            map.setDefaultRegion(nullptr);
            snprintf(reply, replyLen, "MTC REGION: default scope cleared");
            warnIfUnsigned(signatureRequired, reply, replyLen);
            return;
        }
        RegionEntry *d = map.findByNamePrefix(name);
        if (!d)
            d = map.putRegion(name, 0);
        if (!d) {
            snprintf(reply, replyLen, "MTC REGION: region table full");
            return;
        }
        d->flags &= ~REGION_DENY_FLOOD;
        map.setDefaultRegion(d);
        snprintf(reply, replyLen, "MTC REGION: default scope is now %s", d->name);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "DEF") == 0) {
        char errBuf[64] = {0};
        if (map.processDef(rest, errBuf, sizeof(errBuf))) {
            snprintf(reply, replyLen, "MTC REGION: def applied (%d regions)", map.getCount());
        } else {
            snprintf(reply, replyLen, "MTC REGION: %s", errBuf);
        }
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "CLEAR") == 0) {
        map.clear();
        snprintf(reply, replyLen, "MTC REGION: regions cleared");
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else if (strcasecmp(subcmd, "SAVE") == 0) {
        bool ok = radio->saveMeshCoreRegions();
        snprintf(reply, replyLen, "MTC REGION: %s", ok ? "regions saved to flash" : "save failed");
    } else if (strcasecmp(subcmd, "LOAD") == 0) {
        bool ok = radio->reloadMeshCoreRegions();
        snprintf(reply, replyLen, "MTC REGION: %s (%d regions)", ok ? "regions loaded" : "load failed", map.getCount());
    } else {
        snprintf(reply, replyLen, "MTC REGION: unknown subcommand '%s', try HELP", subcmd);
    }
}

static void handleMeshCorePathCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: PATH expects <contact|!hex|packetId>");
        return;
    }
    char *end = nullptr;
    unsigned long id = strtoul(arg, &end, 0);
    if (id != 0 && end && *end == '\0') {
        if (meshCoreCompanion.formatRouteForPacket(id, reply, replyLen))
            return;
    }
    const MeshCoreNode *contact = meshCoreCompanion.findContact(arg);
    if (!contact) {
        snprintf(reply, replyLen, "JT MESHCORE: no contact matching '%s'", arg);
        return;
    }
    const MeshCoreCompanion::HeardPath *route = meshCoreCompanion.heardFindByFrom(contact->nodeNum);
    if (!route) {
        snprintf(reply, replyLen, "JT MESHCORE: no route heard for %s", contact->name);
        return;
    }
    static char line[200];
    if (meshCoreCompanion.formatRouteLine(*route, line, sizeof(line))) {
        snprintf(reply, replyLen, "JT MESHCORE: %s › %s", contact->name, line);
    } else {
        snprintf(reply, replyLen, "JT MESHCORE: route format failed for %s", contact->name);
    }
#else
    (void)arg;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
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
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
}

static void handleMeshCoreNodesCommand(uint8_t typeFilter, const char *arg, char *reply, size_t replyLen)
{
    (void)arg;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.formatNodeList(typeFilter, reply, replyLen);
#else
    (void)typeFilter;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
}

static void handleMeshCoreNodeDetailCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: NODE expects <name|!hex>");
        return;
    }
    if (!meshCoreCompanion.formatNodeDetail(arg, reply, replyLen))
        snprintf(reply, replyLen, "JT MESHCORE: node '%s' not found", arg);
#else
    (void)arg;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
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
        snprintf(reply, replyLen, "JT MESHCORE: SEND expects <contact|channel> <text>");
        return;
    }
    const MeshCoreNode *contact = meshCoreCompanion.findContact(target);
    if (contact) {
        RadioInterface *iface = router ? router->getRadioIface() : nullptr;
        if (meshCoreCompanion.sendDirectText(contact->nodeNum, generatePacketId(), text, Time::getMillis(), iface))
            snprintf(reply, replyLen, "JT MESHCORE: DM sent to %s", contact->name);
        else
            snprintf(reply, replyLen, "JT MESHCORE: DM failed to %s", contact->name);
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
                    snprintf(reply, replyLen, "JT MESHCORE: sent on Ch %d (%s)", targetCh, name);
                else
                    snprintf(reply, replyLen, "JT MESHCORE: sent on Ch %d", targetCh);
            } else {
                snprintf(reply, replyLen, "JT MESHCORE: alloc failed");
            }
            return;
        }
    }
    snprintf(reply, replyLen, "JT MESHCORE: target '%s' not found (expected contact or channel)", target);
#else
    (void)args;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
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

static void handleMeshCoreDropCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: DROP expects <hex_prefix|CLEAR>");
        return;
    }
    if (strcasecmp(arg, "CLEAR") == 0) {
        meshCoreCompanion.clearDrops();
        snprintf(reply, replyLen, "JT MESHCORE: drops cleared");
        return;
    }
    uint8_t prefix[4] = {};
    uint8_t plen = 0;
    if (!parseHexBytes(arg, prefix, &plen, 4)) {
        snprintf(reply, replyLen, "JT MESHCORE: DROP expects 1-4 hex bytes (e.g. 4d, a1b2, 0xA1, a1:b2)");
        return;
    }
    if (meshCoreCompanion.addDrop(prefix, plen))
        snprintf(reply, replyLen, "JT MESHCORE: dropped prefix %s", arg);
    else
        snprintf(reply, replyLen, "JT MESHCORE: drop list full (max %u)", (unsigned)MeshCoreCompanion::MAX_MC_DROPS);
#else
    (void)arg;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
}

static void handleMeshCoreUndropCommand(const char *arg, char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        snprintf(reply, replyLen, "JT MESHCORE: UNDROP expects <hex_prefix>");
        return;
    }
    uint8_t prefix[4] = {};
    uint8_t plen = 0;
    if (!parseHexBytes(arg, prefix, &plen, 4)) {
        snprintf(reply, replyLen, "JT MESHCORE: UNDROP expects 1-4 hex bytes");
        return;
    }
    if (meshCoreCompanion.removeDrop(prefix, plen))
        snprintf(reply, replyLen, "JT MESHCORE: undropped prefix %s", arg);
    else
        snprintf(reply, replyLen, "JT MESHCORE: prefix %s not found in drop list", arg);
#else
    (void)arg;
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
}

static void handleMeshCoreDropsCommand(char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.listDrops(reply, replyLen);
#else
    snprintf(reply, replyLen, "JT MESHCORE: companion disabled");
#endif
}

void NexusModule::handleMeshCoreCommand(const char *args, char *reply, size_t replyLen)
{
    RadioLibInterface *radio = RadioLibInterface::instance;
    if (!radio) {
        snprintf(reply, replyLen, "JT MESHCORE: no radio");
        return;
    }
    char word[16];
    const char *rest = takeWord(args, word, sizeof(word));
    const bool signatureRequired = securityRequiresSignature();

    if (word[0] == '\0' || strcasecmp(word, "HELP") == 0) {
        snprintf(reply, replyLen,
                 "JT MESHCORE: INFO, STATS [FULL], NODES, CONTACTS, REPEATERS, NODE <id>, PATH <target>, TRACE <target>, SEND "
                 "<target> <msg>, DROP [hex|CLEAR], UNDROP <hex>, DROPS, RELAY [ON|OFF|AUTO], ADVERT [1-168|NOW], REGION [...], "
                 "VECINOS, IDENTITY, KEY <hex>, PATHSIZE [1-3], FLOODMAX [g u a], HYBRID [ON|OFF], PPING [...], PING <target>, "
                 "RESET. SAVE "
                 "keeps them.");
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
        snprintf(reply, replyLen, "JT MESHCORE: relay AUTO, advert %uh, %u-byte paths, default flood limits",
                 (unsigned)MeshCoreConfig::DEFAULT_ADVERT_HOURS, (unsigned)MeshCoreConfig::DEFAULT_PATH_HASH_SIZE);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else {
        snprintf(reply, replyLen, "JT MESHCORE: unknown '%s', try HELP", word);
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
    int l = 0;
    if (custom_pping_active && pping_header[0] != '\0') {
        l = snprintf(rMsg, rMsgLen, "%s\n", pping_header);
    } else {
        l = snprintf(rMsg, rMsgLen, "🤖MeshCore Ping🤖\n");
    }

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    static char routeLine[200];
    routeLine[0] = '\0';
    const float fErr = ping_freqerr ? mp.rx_frequency_error : 0.0f;
    if (meshCoreCompanion.formatRouteForPacket(mp.id, routeLine, sizeof(routeLine), fErr)) {
        l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, "%s", routeLine);
        return true;
    }
#endif

    uint32_t hopsUsed = (mp.hop_start >= mp.hop_limit) ? (mp.hop_start - mp.hop_limit) : 0;
    if (hopsUsed == 0) {
        l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, "⚡ directo");
    } else {
        const char *glyph = hopGlyph(hopsUsed);
        if (glyph)
            l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, "%s", glyph);
        else
            l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, "%u saltos", (unsigned)hopsUsed);
    }
    char sig[64];
    formatSignal(sig, sizeof(sig), mp.rx_snr, mp.has_rx_rssi, mp.rx_rssi);
    l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, "%s", sig);
    if (ping_freqerr && mp.rx_frequency_error != 0.0f) {
        l += snprintf(rMsg + l, rMsgLen > (size_t)l ? rMsgLen - l : 0, " · Δf %.0fHz", (double)mp.rx_frequency_error);
    }
    return true;
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

    if (*args == '\0') {
        char botStatus[32] = "not found";
        if (botCh >= 0) {
            snprintf(botStatus, sizeof(botStatus), "Ch %d (%s)", botCh, ping_channels[botCh] ? "ON" : "OFF");
        }
        snprintf(reply, replyLen, "JT MESHCORE PPING:\nGlobal:%d MC:#bot:%s RSSI:%d SNR:%d FE:%d Emoji:%d DM:%d Custom:%d Cmd:%s",
                 public_ping ? 1 : 0, botStatus, ping_rssi ? 1 : 0, ping_snr ? 1 : 0, ping_freqerr ? 1 : 0, (int)ping_emoji_mode,
                 ping_allow_dm ? 1 : 0, custom_pping_active ? 1 : 0, ping_trigger);
    } else if (strcasecmp(args, "ON") == 0) {
        public_ping = true;
        if (botCh >= 0) {
            ping_channels[botCh] = true;
            snprintf(reply, replyLen, "JT MESHCORE: PPING ON (MC:#bot on Ch %d)", botCh);
        } else {
            snprintf(reply, replyLen, "JT MESHCORE: PPING ON (MC:#bot channel not configured)");
        }
    } else if (strcasecmp(args, "OFF") == 0) {
        for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
            if (channels.isMeshCoreChannel(i))
                ping_channels[i] = false;
        }
        snprintf(reply, replyLen, "JT MESHCORE: PPING OFF on MeshCore channels");
    } else if (strncasecmp(args, "CMD ", 4) == 0 || strncasecmp(args, "TRIGGER ", 8) == 0 || strcasecmp(args, "CMD") == 0 ||
               strcasecmp(args, "TRIGGER") == 0) {
        const char *c = (strncasecmp(args, "TRIGGER", 7) == 0) ? (args + 7) : (args + 3);
        while (*c == ' ')
            c++;
        if (*c == '\0') {
            snprintf(reply, replyLen, "JT MESHCORE: Ping command is '%s'", ping_trigger);
        } else if (strcasecmp(c, "DEFAULT") == 0) {
            strncpy(ping_trigger, "/ping", sizeof(ping_trigger) - 1);
            ping_trigger[sizeof(ping_trigger) - 1] = '\0';
            snprintf(reply, replyLen, "JT MESHCORE: Ping command reset to '/ping'");
        } else {
            size_t len = 0;
            while (c[len] && !isspace((unsigned char)c[len]) && len < sizeof(ping_trigger) - 1) {
                ping_trigger[len] = c[len];
                len++;
            }
            ping_trigger[len] = '\0';
            snprintf(reply, replyLen, "JT MESHCORE: Ping command set to '%s'", ping_trigger);
        }
    } else if (strncasecmp(args, "HEADER ", 7) == 0) {
        const char *h = args + 7;
        while (*h == ' ')
            h++;
        if (strcasecmp(h, "OFF") == 0) {
            custom_pping_active = false;
            snprintf(reply, replyLen, "JT MESHCORE: Custom Header DISABLED");
        } else {
            memset(pping_header, 0, sizeof(pping_header));
            strncpy(pping_header, h, 63);
            custom_pping_active = true;
            snprintf(reply, replyLen, "JT MESHCORE: Custom Header set to '%s'", pping_header);
        }
    } else if (strncasecmp(args, "EMOJI ", 6) == 0) {
        const char *e = args + 6;
        while (*e == ' ')
            e++;
        if (strcasecmp(e, "OFF") == 0) {
            ping_emoji_mode = 0;
            snprintf(reply, replyLen, "JT MESHCORE: Ping response in TEXT mode");
        } else if (strcasecmp(e, "INDIRECT") == 0 || strcasecmp(e, "INDR") == 0) {
            ping_emoji_mode = 1;
            snprintf(reply, replyLen, "JT MESHCORE: Ping response in EMOJI for indirect");
        } else if (strcasecmp(e, "ALWAYS") == 0 || strcasecmp(e, "ALW") == 0) {
            ping_emoji_mode = 2;
            snprintf(reply, replyLen, "JT MESHCORE: Ping response in ALWAYS-EMOJI mode");
        } else {
            snprintf(reply, replyLen, "JT MESHCORE: PPING EMOJI <OFF|INDIRECT|ALWAYS>");
        }
    } else if (strcasecmp(args, "RSSI ON") == 0) {
        ping_rssi = true;
        snprintf(reply, replyLen, "JT MESHCORE: Ping RSSI reporting ENABLED");
    } else if (strcasecmp(args, "RSSI OFF") == 0) {
        ping_rssi = false;
        snprintf(reply, replyLen, "JT MESHCORE: Ping RSSI reporting DISABLED");
    } else if (strcasecmp(args, "SNR ON") == 0) {
        ping_snr = true;
        snprintf(reply, replyLen, "JT MESHCORE: Ping SNR reporting ENABLED");
    } else if (strcasecmp(args, "SNR OFF") == 0) {
        ping_snr = false;
        snprintf(reply, replyLen, "JT MESHCORE: Ping SNR reporting DISABLED");
    } else if (strcasecmp(args, "FREQERR ON") == 0) {
        ping_freqerr = true;
        snprintf(reply, replyLen, "JT MESHCORE: Ping FreqErr reporting ENABLED");
    } else if (strcasecmp(args, "FREQERR OFF") == 0) {
        ping_freqerr = false;
        snprintf(reply, replyLen, "JT MESHCORE: Ping FreqErr reporting DISABLED");
    } else if (strcasecmp(args, "ALLOWDM ON") == 0) {
        ping_allow_dm = true;
        snprintf(reply, replyLen, "JT MESHCORE: Ping DM replies ENABLED");
    } else if (strcasecmp(args, "ALLOWDM OFF") == 0) {
        ping_allow_dm = false;
        snprintf(reply, replyLen, "JT MESHCORE: Ping DM replies DISABLED");
    } else if (strcasecmp(args, "DIRECT IGNORE ON") == 0 || strcasecmp(args, "IGNORE DIRECT ON") == 0) {
        ping_ignore_direct = true;
        snprintf(reply, replyLen, "JT MESHCORE: Direct Ignore ENABLED");
    } else if (strcasecmp(args, "DIRECT IGNORE OFF") == 0 || strcasecmp(args, "IGNORE DIRECT OFF") == 0) {
        ping_ignore_direct = false;
        snprintf(reply, replyLen, "JT MESHCORE: Direct Ignore DISABLED");
    } else if (strcasecmp(args, "IGNORE REMOTE ON") == 0 || strcasecmp(args, "REMOTE IGNORE ON") == 0) {
        ping_ignore_remote = true;
        snprintf(reply, replyLen, "JT MESHCORE: Remote Ignore ENABLED");
    } else if (strcasecmp(args, "IGNORE REMOTE OFF") == 0 || strcasecmp(args, "REMOTE IGNORE OFF") == 0) {
        ping_ignore_remote = false;
        snprintf(reply, replyLen, "JT MESHCORE: Remote Ignore DISABLED");
    } else if (strncasecmp(args, "CHAN ", 5) == 0) {
        const char *cPtr = args + 5;
        bool enable = true;
        if (strncasecmp(cPtr, "ON ", 3) == 0) {
            enable = true;
            cPtr += 3;
        } else if (strncasecmp(cPtr, "OFF ", 4) == 0) {
            enable = false;
            cPtr += 4;
        }
        while (*cPtr == ' ')
            cPtr++;
        int targetCh = -1;
        if (isdigit((unsigned char)*cPtr) && *(cPtr + 1) == '\0') {
            targetCh = *cPtr - '0';
        } else {
            for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
                const char *name = channels.getName(i);
                if (name && strcasecmp(name, cPtr) == 0) {
                    targetCh = (int)i;
                    break;
                }
            }
        }
        if (targetCh >= 0 && (size_t)targetCh < MAX_NUM_CHANNELS) {
            ping_channels[targetCh] = enable;
            snprintf(reply, replyLen, "JT MESHCORE: Ping on Ch %d (%s) set to %d", targetCh, channels.getName(targetCh), enable);
        } else {
            snprintf(reply, replyLen, "JT MESHCORE: Channel '%s' not found", cPtr);
        }
    } else {
        snprintf(reply, replyLen, "JT MESHCORE: PPING ON|OFF|CMD <cmd>|RSSI|SNR|FREQERR|EMOJI|HEADER|ALLOWDM|CHAN");
    }
}

void NexusModule::handleMeshCorePingSend(const char *target, char *reply, size_t replyLen)
{
    while (*target == ' ')
        target++;

    if (*target == '\0' || strcasecmp(target, "STATUS") == 0 || strcasecmp(target, "ON") == 0 || strcasecmp(target, "OFF") == 0 ||
        strncasecmp(target, "CMD", 3) == 0 || strncasecmp(target, "TRIGGER", 7) == 0 || strncasecmp(target, "HEADER", 6) == 0 ||
        strncasecmp(target, "EMOJI", 5) == 0 || strncasecmp(target, "RSSI", 4) == 0 || strncasecmp(target, "SNR", 3) == 0 ||
        strncasecmp(target, "FREQERR", 7) == 0 || strncasecmp(target, "ALLOWDM", 7) == 0 || strncasecmp(target, "CHAN", 4) == 0) {
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
            snprintf(reply, replyLen, "JT MESHCORE: Ping sent to Ch %d (%s)", targetCh, channels.getName(targetCh));
        } else {
            snprintf(reply, replyLen, "JT MESHCORE: Packet alloc failed");
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
            snprintf(reply, replyLen, "JT MESHCORE: Ping sent to %s (!%08x)", contact->name, (unsigned)contact->nodeNum);
        } else {
            snprintf(reply, replyLen, "JT MESHCORE: Packet alloc failed");
        }
        return;
    }
#endif

    snprintf(reply, replyLen, "JT MESHCORE: Unknown channel or contact '%s'", target);
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

void NexusModule::handleMeshCoreCommand(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "JT MESHCORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::handleRegionCommand(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "MTC REGION: not in this build (MeshCore sniffer off)");
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
    snprintf(reply, replyLen, "JT MESHCORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::handleMeshCorePingSend(const char *, char *reply, size_t replyLen)
{
    snprintf(reply, replyLen, "JT MESHCORE: not in this build (MeshCore sniffer off)");
}

void NexusModule::saveMeshCoreSettings() {}

void NexusModule::loadMeshCoreSettings() {}

#endif

} // namespace meshtastic
