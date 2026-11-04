// SPDX-License-Identifier: AGPL-3.0-only
//
// Meshtacore additions to the /nexus CLI: RESCUE, CHEM, STORM, SETURL, PUBSILENCE, VWAKE, VOLT and MESHCORE. They live here, not
// in NexusCommands.cpp, so a merge of the jentastic upstream does not collide with them: the dispatch chain in
// handleNexusCommand() carries a single call to handleMeshtacoreCommand().

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Power.h"
#include "RadioLibInterface.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "configuration.h"
#include "main.h"
#include "memGet.h"
#include "mesh/generated/meshtastic/apponly.pb.h"
#include "modules/NexusCommandParse.h"
#include "modules/NexusModule.h"
#include "modules/NexusRescue.h"
#include "sleep.h"
#include <pb_decode.h>
#include <string.h>
#include <strings.h>

using namespace meshtastic;

/**
 * Runs the Meshtacore command in `cmd`, leaving its answer in replyMsg.
 * @return false when `cmd` is not one of ours, so the caller keeps matching.
 */
bool NexusModule::handleMeshtacoreCommand(const meshtastic_MeshPacket &mp, uint32_t replyId, const char *cmd)
{
    if (strcasecmp(cmd, "MESHCORE") == 0 || strncasecmp(cmd, "MESHCORE ", 9) == 0 || strcasecmp(cmd, "MCORE") == 0 ||
        strncasecmp(cmd, "MCORE ", 6) == 0) {
        const char *args = strchr(cmd, ' ');
        handleMeshCoreCommand(args ? args + 1 : "", replyMsg, sizeof(replyMsg));
    } else if (strcasecmp(cmd, "REGION") == 0 || strncasecmp(cmd, "REGION ", 7) == 0) {
        const char *args = strchr(cmd, ' ');
        handleRegionCommand(args ? args + 1 : "", replyMsg, sizeof(replyMsg));
    } else if (strcasecmp(cmd, "VOLT") == 0 || strcasecmp(cmd, "VOLTAGE") == 0 || strncasecmp(cmd, "VOLT ", 5) == 0 ||
               strncasecmp(cmd, "VOLTAGE ", 8) == 0) {
        // /nexus VOLT [LIST|WAKE|<1..4> [...]]: voltage threshold rules (NexusMeshtacoreVolt.cpp).
        handleVoltCommand(mp, replyId, cmd);
    } else if (strcasecmp(cmd, "RESCUE") == 0 || strcasecmp(cmd, "RESCUE SHOW") == 0) {
        // /nexus RESCUE [SHOW]: the copy restored after a factory reset or a wiped /prefs.
        NexusRescue::describe(replyMsg, sizeof(replyMsg));
    } else if (strcasecmp(cmd, "RESCUE CLEAR") == 0) {
        snprintf(replyMsg, sizeof(replyMsg), "JT: RESCUE %s", NexusRescue::clear() ? "copy deleted" : "delete failed");
    } else if (strcasecmp(cmd, "RESCUE SAVE") == 0) {
        NexusRescue::snapshot();
        NexusRescue::describe(replyMsg, sizeof(replyMsg));
    } else if (strncasecmp(cmd, "CHEM ", 5) == 0 || strcasecmp(cmd, "CHEM") == 0) {
        // /nexus CHEM [VARIANT|LIPO|NIMH|SODIUM|LIFEPO4]: battery OCV curve and low-voltage cutoff. Persist with SAVE.
        handleChemCommand(cmd[4] ? cmd + 5 : "");
    } else if (strncasecmp(cmd, "STORM ", 6) == 0 || strcasecmp(cmd, "STORM") == 0) {
        // /nexus STORM <minutes 1-1440>|OFF: radio off and deep sleep, then reboot.
        handleStormCommand(cmd[5] ? cmd + 6 : "");
    } else if (strncasecmp(cmd, "SETURL ", 7) == 0) {
        // /nexus SETURL <channel URL>: merge the shared channels by name, keep Nexus channels and TX power.
        // The command buffer keeps 127 bytes: a URL that fills it was cut and must not half-apply.
        if (strlen(this->cmdBuf) >= sizeof(this->cmdBuf) - 1) // the whole message, prefix included, filled it
            snprintf(replyMsg, sizeof(replyMsg), "JT: SETURL URL too long for one command");
        else
            handleSetUrlCommand(cmd + 7);
    } else if (strncasecmp(cmd, "PUBSILENCE ", 11) == 0 || strcasecmp(cmd, "PUBSILENCE") == 0) {
        // /nexus PUBSILENCE [ON|OFF]: no ACKs, replies or unicast NodeInfo from us on public-key channels.
        const char *p = cmd[10] ? cmd + 11 : "";
        while (*p == ' ')
            p++;
        const bool enable = (strcasecmp(p, "ON") == 0 || strcasecmp(p, "1") == 0);
        const bool disable = (strcasecmp(p, "OFF") == 0 || strcasecmp(p, "0") == 0);
        if (*p && !enable && !disable) {
            snprintf(replyMsg, sizeof(replyMsg), "JT: PUBSILENCE expects ON or OFF");
        } else {
            if (*p)
                ext_flags = enable ? (ext_flags | EXT_FLAG_PUBLIC_SILENCE) : (ext_flags & ~EXT_FLAG_PUBLIC_SILENCE);
            snprintf(replyMsg, sizeof(replyMsg), "JT: Public channel silence %s",
                     (ext_flags & EXT_FLAG_PUBLIC_SILENCE) ? "ON" : "OFF");
        }
    } else if (strncasecmp(cmd, "VWAKE ", 6) == 0 || strcasecmp(cmd, "VWAKE") == 0) {
        // /nexus VWAKE [<n>/<d>|DEFAULT]: LPCOMP battery wake threshold, a fraction of VDD
        // seen after the battery divider. Persist with SAVE.
        handleVwakeCommand(cmd[5] ? cmd + 6 : "");
    } else if (strcasecmp(cmd, "HEALTH") == 0 || strncasecmp(cmd, "HEALTH ", 7) == 0 || strcasecmp(cmd, "SALUT") == 0 ||
               strncasecmp(cmd, "SALUT ", 6) == 0 || strcasecmp(cmd, "SYS") == 0 || strcasecmp(cmd, "UPTIME") == 0 ||
               strcasecmp(cmd, "UP") == 0 || strcasecmp(cmd, "UTIL") == 0) {
        handleHealthCommand();
    } else {
        return false;
    }
    return true;
}

void NexusModule::handleVwakeCommand(const char *arg)
{
#ifdef ARCH_NRF52
    while (*arg == ' ')
        arg++;
    if (!nrf52HasBatteryLpcompWake()) {
        snprintf(replyMsg, sizeof(replyMsg), "JT: VWAKE not supported on this board");
        return;
    }
    if (strcasecmp(arg, "DEFAULT") == 0) {
        lpcomp_wake_ref = 0;
        applyLpcompWakeLevel();
    } else if (*arg) {
        unsigned num = 0, den = 0;
        if (sscanf(arg, "%u/%u", &num, &den) != 2 || (den != 8 && den != 16) || num == 0 || num * (16 / den) >= 16 ||
            !nrf52SetBatteryLpcompWakeFraction(num, den)) {
            snprintf(replyMsg, sizeof(replyMsg), "JT: VWAKE expects n/8 or n/16 below 1, or DEFAULT");
            return;
        }
        lpcomp_wake_ref = (uint8_t)(num * (16 / den));
    }
    uint8_t num = 0, den = 0;
    nrf52GetBatteryLpcompWakeFraction(&num, &den);
    const uint32_t mv = nrf52EstimateBatteryLpcompWakeMv();
    if (mv)
        snprintf(replyMsg, sizeof(replyMsg), "JT: VWAKE %u/%u VDD%s (~%lu mV battery)", num, den,
                 lpcomp_wake_ref ? "" : " default", (unsigned long)mv);
    else
        snprintf(replyMsg, sizeof(replyMsg), "JT: VWAKE %u/%u VDD%s", num, den, lpcomp_wake_ref ? "" : " default");
#else
    (void)arg;
    snprintf(replyMsg, sizeof(replyMsg), "JT: VWAKE not supported on this board");
#endif
}

bool NexusModule::isTrustedNexusChannelName(const char *name)
{
#ifdef NEXUS_TRUSTED_CHANNEL_1
    if (strcasecmp(name, NEXUS_TRUSTED_CHANNEL_1) == 0)
        return true;
#endif
#ifdef NEXUS_TRUSTED_CHANNEL_2
    if (strcasecmp(name, NEXUS_TRUSTED_CHANNEL_2) == 0)
        return true;
#endif
    return strcasecmp(name, "Nexus") == 0 || strcasecmp(name, "Nexus2") == 0;
}

// Returns the slot written, or -1 when the channel was refused (reason appended to notes).
int NexusModule::mergeSharedChannel(const meshtastic_ChannelSettings &settings, bool asPrimary, char *notes, size_t notesLen)
{
    const size_t used = strlen(notes);
    int slot = -1;
    if (asPrimary) {
        if (isTrustedNexusChannelName(channels.getByIndex(0).settings.name)) {
            snprintf(notes + used, notesLen - used, " primary kept (Nexus);");
            return -1;
        }
        slot = 0;
    } else {
        for (int i = 1; i < MAX_NUM_CHANNELS && slot < 0; i++) {
            const meshtastic_Channel &ch = channels.getByIndex(i);
            if (ch.role != meshtastic_Channel_Role_DISABLED && strcasecmp(ch.settings.name, settings.name) == 0)
                slot = i;
        }
        for (int i = 1; i < MAX_NUM_CHANNELS && slot < 0; i++) {
            if (channels.getByIndex(i).role == meshtastic_Channel_Role_DISABLED)
                slot = i;
        }
        if (slot < 0) {
            snprintf(notes + used, notesLen - used, " '%s' no free slot;", settings.name);
            return -1;
        }
        if (isTrustedNexusChannelName(channels.getByIndex(slot).settings.name)) {
            snprintf(notes + used, notesLen - used, " '%s' kept (Nexus);", settings.name);
            return -1;
        }
    }
    meshtastic_Channel ch = channels.getByIndex(slot);
    ch.index = slot;
    ch.role = asPrimary ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
    ch.has_settings = true;
    ch.settings = settings;
    channels.setChannel(ch);
    if (cryptoKeyIsPublic(channels.getKey(slot))) {
        const size_t now = strlen(notes);
        snprintf(notes + now, notesLen - now, " ch%d public key, readable by anyone;", slot);
    }
    return slot;
}

void NexusModule::handleSetUrlCommand(const char *url)
{
    while (*url == ' ')
        url++;
    const char *hashPart = strchr(url, '#');
    if (hashPart)
        url = hashPart + 1;

    // Static (BSS): nRF52 loopTask has a 4 KB stack. The command buffer bounds the URL anyway.
    static unsigned char buffer[256];
    static meshtastic_ChannelSet set;
    size_t dlen = 0;
    memset(&set, 0, sizeof(set));
    if (nexus_base64_decode(buffer, sizeof(buffer), &dlen, url) != 0 ||
        !pb_decode_from_bytes(buffer, dlen, &meshtastic_ChannelSet_msg, &set) || set.settings_count == 0) {
        snprintf(replyMsg, sizeof(replyMsg), "JT: SETURL could not decode the channel URL");
        return;
    }

    static char notes[160];
    notes[0] = '\0';
    int applied = 0;
    for (pb_size_t i = 0; i < set.settings_count; i++) {
        if (mergeSharedChannel(set.settings[i], i == 0, notes, sizeof(notes)) >= 0)
            applied++;
    }
    if (applied) {
        channels.onConfigChanged();
        nodeDB->saveToDisk(SEGMENT_CHANNELS);
        if (service)
            service->configChanged.notifyObservers(nullptr);
    }

    bool loraChanged = false;
    if (set.has_lora_config && pendingLoRaConfig) {
        const size_t used = strlen(notes);
        snprintf(notes + used, sizeof(notes) - used, " LoRa skipped, another change pending;");
    } else if (set.has_lora_config) {
        meshtastic_Config pc = meshtastic_Config_init_default;
        pc.which_payload_variant = meshtastic_Config_lora_tag;
        pc.payload_variant.lora = set.lora_config;
        pc.payload_variant.lora.tx_power = config.lora.tx_power; // the shared link never sets our power
        if (pc.payload_variant.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
            pc.payload_variant.lora.region = config.lora.region; // an unset region would mute the radio
        pendingConfig = pc;
        pendingLoRaApplyAt = Time::getMillis() + 5000;
        pendingLoRaConfig = true;
        setIntervalFromNow(50);
        loraChanged = true;
    }
    snprintf(replyMsg, sizeof(replyMsg), "JT: SETURL %d/%u channels%s.%s", applied, (unsigned)set.settings_count,
             loraChanged ? ", LoRa in 5s" : "", notes);
}

void NexusModule::handleStormCommand(const char *arg)
{
    constexpr uint32_t STORM_MAX_MINUTES = 24 * 60;
    constexpr uint32_t STORM_NOTICE_MS = 10 * 1000;
    while (*arg == ' ')
        arg++;
    if (strcasecmp(arg, "OFF") == 0) {
        snprintf(replyMsg, sizeof(replyMsg), "JT: STORM %s", stormPending ? "cancelled" : "not pending");
        stormPending = false;
        return;
    }
    if (!*arg) {
        snprintf(replyMsg, sizeof(replyMsg), stormPending ? "JT: STORM pending, %lu min" : "JT: STORM idle",
                 (unsigned long)stormSleepMinutes);
        return;
    }
    uint32_t minutesU = 0;
    const bool minutesOk = parseUInt(arg, minutesU, STORM_MAX_MINUTES);
    const long minutes = (long)minutesU;
    if (!minutesOk || minutes < 1) {
        snprintf(replyMsg, sizeof(replyMsg), "JT: STORM expects 1-%lu minutes or OFF", (unsigned long)STORM_MAX_MINUTES);
        return;
    }
    stormSleepMinutes = (uint32_t)minutes;
    stormStartDeadlineMs = millis() + STORM_NOTICE_MS;
    stormPending = true;
    snprintf(replyMsg, sizeof(replyMsg), "JT: STORM radio off for %ld min in 10 s (STORM OFF cancels)", minutes);
}

void NexusModule::handleChemCommand(const char *arg)
{
    while (*arg == ' ')
        arg++;
    if (*arg) {
        uint8_t chosen = (uint8_t)BatteryChemistry::COUNT;
        for (uint8_t c = 0; c < (uint8_t)BatteryChemistry::COUNT; c++)
            if (strcasecmp(arg, batteryChemistryName((BatteryChemistry)c)) == 0)
                chosen = c;
        if (!setBatteryChemistry((BatteryChemistry)chosen)) {
            snprintf(replyMsg, sizeof(replyMsg), "JT: CHEM expects VARIANT, LIPO, NIMH, SODIUM or LIFEPO4");
            return;
        }
        battery_chemistry = chosen;
    }
    snprintf(replyMsg, sizeof(replyMsg), "JT: CHEM %s, full %u mV, cutoff %u mV per cell",
             batteryChemistryName((BatteryChemistry)battery_chemistry), activeBatteryOcv[0],
             activeBatteryOcv[NUM_OCV_POINTS - 1]);
}

void NexusModule::handleHealthCommand()
{
    uint64_t ms = Time::getMillisMonotonic();
    uint32_t s = (uint32_t)(ms / 1000ULL);
    uint32_t d = s / 86400;
    s %= 86400;
    uint32_t h = s / 3600;
    s %= 3600;
    uint32_t m = s / 60;
    s %= 60;
    char upBuf[24];
    if (d > 0)
        snprintf(upBuf, sizeof(upBuf), "%ud %02uh %02um", d, h, m);
    else
        snprintf(upBuf, sizeof(upBuf), "%uh %02um %02us", h, m, (unsigned)s);

    static meshtastic_Telemetry dm = meshtastic_Telemetry_init_zero;
    char batBuf[24] = "N/A";
    if (getNexusDeviceTelemetry(&dm) && dm.which_variant == meshtastic_Telemetry_device_metrics_tag) {
        const auto &dev = dm.variant.device_metrics;
        if (dev.has_battery_level && dev.battery_level > 100) {
            if (dev.has_voltage && dev.voltage > 0.1f)
                snprintf(batBuf, sizeof(batBuf), "Powered(%.2fV)", dev.voltage);
            else
                snprintf(batBuf, sizeof(batBuf), "Powered");
        } else if (dev.has_battery_level && dev.has_voltage) {
            snprintf(batBuf, sizeof(batBuf), "%.2fV (%u%%)", dev.voltage, (unsigned)dev.battery_level);
        } else if (dev.has_battery_level) {
            snprintf(batBuf, sizeof(batBuf), "%u%%", (unsigned)dev.battery_level);
        } else if (dev.has_voltage && dev.voltage > 0.1f) {
            snprintf(batBuf, sizeof(batBuf), "%.2fV", dev.voltage);
        }
    }

    uint32_t freeH = memGet.getFreeHeap();
    uint32_t totH = memGet.getHeapSize();
    char heapBuf[24];
    if (totH > 0)
        snprintf(heapBuf, sizeof(heapBuf), "%uK/%uK", (unsigned)(freeH / 1024), (unsigned)(totH / 1024));
    else
        snprintf(heapBuf, sizeof(heapBuf), "%uK free", (unsigned)(freeH / 1024));

    float temp = getDieTemp();
    const char *reason = getResetReason();
    uint32_t rbt = myNodeInfo.reboot_count;
    uint32_t unexp = getUnexpectedRebootCount();

    char bootBuf[36];
    if (unexp > 0)
        snprintf(bootBuf, sizeof(bootBuf), "%s (x%u, %u unexp)", reason, (unsigned)rbt, (unsigned)unexp);
    else
        snprintf(bootBuf, sizeof(bootBuf), "%s (x%u)", reason, (unsigned)rbt);

    float chUtil = (airTime != NULL) ? airTime->channelUtilizationPercent() : 0.0f;
    float txUtil = (airTime != NULL) ? airTime->utilizationTXPercent() : 0.0f;
    int32_t noise = (RadioLibInterface::instance) ? RadioLibInterface::instance->getAverageNoiseFloor() : -120;

    snprintf(replyMsg, sizeof(replyMsg),
             "MTC HEALTH:\nUp: %s\nBoot: %s\nBat: %s\nHeap: %s\nTemp: %.1fC\nChU: %.1f%% TxU: %.1f%% Noise: %ddBm", upBuf,
             bootBuf, batBuf, heapBuf, temp, chUtil, txUtil, (int)noise);
}
