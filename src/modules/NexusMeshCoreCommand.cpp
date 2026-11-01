// SPDX-License-Identifier: AGPL-3.0-only
//
// /nexus MESHCORE (alias MCORE): reads and sets the MeshCore relay and ADVERT settings from the Nexus CLI. The settings
// live in RadioLibInterface (MeshCoreConfig); this file parses the operator's words and formats one reply. Like the other
// tactical settings, a change is volatile until /nexus SAVE.

#include "NodeDB.h"
#include "configuration.h"
#include "gps/RTC.h"
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

void NexusModule::renderNeighbors(const DirectNeighborEntry *entries, size_t count, const char *header, const char *title,
                                  char *reply, size_t replyLen)
{
    const uint32_t now = getTime(false);
    size_t totalLen = strlen(header);
    char items[32][64];

    for (size_t i = 0; i < count; i++) {
        char ago[8];
        formatAgo(now > entries[i].lastHeard ? now - entries[i].lastHeard : 0, ago, sizeof(ago));
        int n = snprintf(items[i], sizeof(items[i]), "%s (%.1fdB, %s)\n", entries[i].name, entries[i].snr, ago);
        if (n > 0)
            totalLen += (size_t)n;
    }

    if (totalLen + 6 < replyLen && totalLen < NEXUS_MAX_REPLY_BODY - 10) {
        size_t len = snprintf(reply, replyLen, "%s", header);
        for (size_t i = 0; i < count && len < replyLen; i++) {
            size_t iLen = strlen(items[i]);
            if (len + iLen < replyLen) {
                memcpy(reply + len, items[i], iLen + 1);
                len += iLen;
            }
        }
        if (len > 0 && reply[len - 1] == '\n')
            reply[--len] = '\0';
        return;
    }

    uint32_t to = activeRequest ? (activeRequest->to == myNodeInfo.my_node_num ? activeRequest->from : 0xFFFFFFFF) : 0xFFFFFFFF;
    uint32_t ch = activeRequest ? activeRequest->channel : 0;
    uint32_t rId = activeRequest ? activeRequest->id : 0;
    startPaging(to, ch, rId, title);
    for (size_t i = 0; i < count; i++)
        appendPaging(items[i]);

    if (activeRequest) {
        snprintf(reply, replyLen, "JT: Paging %s (%u)...", title, (unsigned)count);
    } else {
        size_t len = snprintf(reply, replyLen, "%s", header);
        for (size_t i = 0; i < count && len < replyLen; i++) {
            size_t iLen = strlen(items[i]);
            if (len + iLen < replyLen) {
                memcpy(reply + len, items[i], iLen + 1);
                len += iLen;
            }
        }
        if (len > 0 && reply[len - 1] == '\n')
            reply[--len] = '\0';
    }
}

void NexusModule::formatNeighbors(char *reply, size_t replyLen)
{
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : (nodeDB ? nodeDB->getNodeNum() : 0);
    const size_t nodeCount = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    DirectNeighborEntry entries[32];
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
#if MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC
    const size_t used = strlen(reply);
    snprintf(reply + used, replyLen - used, ", sync 0x%04X/0x12", (unsigned)MESHTASTIC_MESHCORE_RX_SYNCWORD);
#endif
}

static void formatStats(const RadioLibInterface &radio, char *reply, size_t replyLen)
{
    const MeshCoreRepeater &relay = radio.getMeshCoreRepeater();
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    snprintf(reply, replyLen,
             "JT MESHCORE: rx %lu dup %lu drop %lu relayed %lu defer %lu+%lu | DM rx %lu tx %lu acked %lu timeout %lu",
             (unsigned long)relay.rxSeen, (unsigned long)relay.rxDup, (unsigned long)relay.rxDropped,
             (unsigned long)relay.txRelayed, (unsigned long)relay.deferChannel, (unsigned long)relay.deferRadio,
             (unsigned long)meshCoreCompanion.dmRx, (unsigned long)meshCoreCompanion.dmTx,
             (unsigned long)meshCoreCompanion.dmAcked, (unsigned long)meshCoreCompanion.dmTimedOut);
#else
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
    DirectNeighborEntry entries[32];
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

static void formatKey(const char *hex, char *reply, size_t replyLen)
{
    char base64[25];
    if (meshCoreKeyHexToBase64(hex, base64, sizeof(base64)))
        snprintf(reply, replyLen, "JT MESHCORE KEY: %s (pega esto como clave del canal MC:<nombre> en la app Meshtastic)",
                 base64);
    else
        snprintf(reply, replyLen, "JT MESHCORE KEY: pon los 32 digitos hex que MeshCore muestra como clave del canal");
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
        snprintf(
            reply, replyLen,
            "JT MESHCORE: INFO, STATS, RELAY [ON|OFF|AUTO], ADVERT [1-168|NOW], VECINOS, KEY <hex>, RESET. SAVE keeps them.");
    } else if (strcasecmp(word, "INFO") == 0) {
        formatInfo(*radio, reply, replyLen);
    } else if (strcasecmp(word, "STATS") == 0) {
        formatStats(*radio, reply, replyLen);
    } else if (strcasecmp(word, "RELAY") == 0) {
        setRelay(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "ADVERT") == 0) {
        setAdvert(*radio, rest, signatureRequired, reply, replyLen);
    } else if (strcasecmp(word, "VECINOS") == 0 || strcasecmp(word, "NEIGHBOURS") == 0 || strcasecmp(word, "NEIGHBORS") == 0) {
        formatMeshCoreNeighbors(reply, replyLen);
    } else if (strcasecmp(word, "KEY") == 0) {
        formatKey(rest, reply, replyLen);
    } else if (strcasecmp(word, "RESET") == 0) {
        radio->setMeshCoreRelayMode(MeshCoreConfig::RELAY_AUTO);
        radio->setMeshCoreAdvertHours(0);
        snprintf(reply, replyLen, "JT MESHCORE: relay AUTO, advert %uh", (unsigned)MeshCoreConfig::DEFAULT_ADVERT_HOURS);
        warnIfUnsigned(signatureRequired, reply, replyLen);
    } else {
        snprintf(reply, replyLen, "JT MESHCORE: unknown '%s', try HELP", word);
    }
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

void NexusModule::saveMeshCoreSettings() {}

void NexusModule::loadMeshCoreSettings() {}

#endif

} // namespace meshtastic
