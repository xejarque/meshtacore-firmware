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

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
#include "mesh/MeshCoreCompanion.h"
#endif

namespace meshtastic
{

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
        if (n.lastMod != 0 && n.hops == 0 && n.lastHeard != 0)
            count++;
    });
    if (count == 0) {
        snprintf(reply, replyLen, "JT MESHCORE: 0 vecinos directos");
        return;
    }
    size_t len = snprintf(reply, replyLen, "JT MESHCORE (%u vecinos): ", (unsigned)count);
    const uint32_t now = getTime(false);
    meshCoreCompanion.nodes.forEach([&](const MeshCoreNode &n) {
        if (n.lastMod != 0 && n.hops == 0 && n.lastHeard != 0) {
            const uint32_t ago = (now > n.lastHeard) ? (now - n.lastHeard) : 0;
            char agoStr[8];
            if (ago < 60)
                snprintf(agoStr, sizeof(agoStr), "%us", (unsigned)ago);
            else if (ago < 3600)
                snprintf(agoStr, sizeof(agoStr), "%um", (unsigned)(ago / 60));
            else if (ago < 86400)
                snprintf(agoStr, sizeof(agoStr), "%uh", (unsigned)(ago / 3600));
            else
                snprintf(agoStr, sizeof(agoStr), "%ud", (unsigned)(ago / 86400));

            const char *name = (n.name[0] != '\0') ? n.name : "???";
            char item[32];
            const int itemLen = snprintf(item, sizeof(item), "%.4s(%.1fdB,%s) ", name, n.snr, agoStr);
            if (len + itemLen < replyLen - 1) {
                memcpy(reply + len, item, itemLen);
                len += itemLen;
                reply[len] = '\0';
            } else if (len + 4 < replyLen && (len < 3 || strcmp(reply + len - 3, "...") != 0)) {
                strcpy(reply + len, "...");
                len += 3;
            }
        }
    });
    if (len > 0 && reply[len - 1] == ' ')
        reply[len - 1] = '\0';
#else
    snprintf(reply, replyLen, "JT MESHCORE: companion off");
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
        snprintf(reply, replyLen, "JT MESHCORE: INFO, STATS, RELAY [ON|OFF|AUTO], ADVERT [1-168|NOW], VECINOS, RESET. SAVE keeps them.");
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
