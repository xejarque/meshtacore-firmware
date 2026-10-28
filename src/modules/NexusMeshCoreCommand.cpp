// SPDX-License-Identifier: AGPL-3.0-only
//
// /nexus MESHCORE (alias MCORE): reads and sets the MeshCore relay and ADVERT settings from the Nexus CLI. The settings
// live in RadioLibInterface (MeshCoreConfig); this file parses the operator's words and formats one reply. Like the other
// tactical settings, a change is volatile until /nexus SAVE.

#include "configuration.h"
#include "NodeDB.h"
#include "gps/RTC.h"
#include "modules/NexusModule.h"
#include "mesh/RadioLibInterface.h"
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

// A neighbour list that fits one reply: entries go in while they fit, and the first one that does not closes it with
// "...".
class NeighborList
{
  public:
    NeighborList(char *reply, size_t cap, const char *header) : reply(reply), cap(cap), now(getTime(false))
    {
        const int n = snprintf(reply, cap, "%s", header);
        len = n > 0 && (size_t)n < cap ? (size_t)n : 0;
    }

    void add(const char *name, float snr, uint32_t lastHeard)
    {
        if (full)
            return;
        char ago[8];
        formatAgo(now > lastHeard ? now - lastHeard : 0, ago, sizeof(ago));
        char item[32];
        const int n = snprintf(item, sizeof(item), "%.4s(%.1fdB,%s) ", name[0] ? name : "???", snr, ago);
        if (n > 0 && len + (size_t)n < cap) {
            memcpy(reply + len, item, (size_t)n + 1);
            len += (size_t)n;
            return;
        }
        full = true;
        if (len + 3 < cap) {
            memcpy(reply + len, "...", 4);
            len += 3;
        }
    }

    void finish()
    {
        if (len > 0 && reply[len - 1] == ' ')
            reply[--len] = '\0';
    }

  private:
    char *reply;
    size_t cap;
    size_t len = 0;
    const uint32_t now;
    bool full = false;
};

static bool isDirectNeighbor(const meshtastic_NodeInfoLite *n, uint32_t myNum)
{
    return n && n->num != 0 && n->num != myNum && n->has_hops_away && n->hops_away == 0 && n->last_heard != 0;
}

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
static bool isDirectNeighbor(const MeshCoreNode &n)
{
    return n.lastMod != 0 && n.hops == 0 && n.lastHeard != 0;
}

// A MeshCore neighbour the node DB does not list, so the merged list names it once.
static bool isMeshCoreOnlyNeighbor(const MeshCoreNode &n)
{
    return isDirectNeighbor(n) && !(nodeDB && nodeDB->getMeshNode(n.nodeNum));
}
#endif

void NexusModule::formatNeighbors(char *reply, size_t replyLen)
{
    const uint32_t myNum = myNodeInfo.my_node_num != 0 ? myNodeInfo.my_node_num : (nodeDB ? nodeDB->getNodeNum() : 0);
    const size_t nodeCount = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    size_t count = 0;
    for (size_t i = 0; i < nodeCount; i++)
        if (isDirectNeighbor(nodeDB->getMeshNodeByIndex(i), myNum))
            count++;
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.nodes.forEach([&count](const MeshCoreNode &n) {
        if (isMeshCoreOnlyNeighbor(n))
            count++;
    });
#endif
    if (count == 0) {
        snprintf(reply, replyLen, "JT VECINOS: 0 directos");
        return;
    }

    char header[24];
    snprintf(header, sizeof(header), "JT VECINOS (%u): ", (unsigned)count);
    NeighborList list(reply, replyLen, header);
    for (size_t i = 0; i < nodeCount; i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (isDirectNeighbor(n, myNum))
            list.add(n->short_name[0] ? n->short_name : n->long_name, n->snr, n->last_heard);
    }
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    meshCoreCompanion.nodes.forEach([&list](const MeshCoreNode &n) {
        if (isMeshCoreOnlyNeighbor(n))
            list.add(n.name, n.snr, n.lastHeard);
    });
#endif
    list.finish();
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
                 (unsigned)radio.getMeshCoreConfig().effectiveAdvertHours(),
                 (unsigned)(radio.meshCoreAdvertDueInMs() / 60000UL));
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

static void formatMeshCoreNeighbors(char *reply, size_t replyLen)
{
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    size_t count = 0;
    meshCoreCompanion.nodes.forEach([&count](const MeshCoreNode &n) {
        if (isDirectNeighbor(n))
            count++;
    });
    if (count == 0) {
        snprintf(reply, replyLen, "JT MESHCORE: 0 vecinos directos");
        return;
    }
    char header[32];
    snprintf(header, sizeof(header), "JT MESHCORE (%u vecinos): ", (unsigned)count);
    NeighborList list(reply, replyLen, header);
    meshCoreCompanion.nodes.forEach([&list](const MeshCoreNode &n) {
        if (isDirectNeighbor(n))
            list.add(n.name, n.snr, n.lastHeard);
    });
    list.finish();
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
