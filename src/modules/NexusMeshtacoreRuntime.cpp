// SPDX-License-Identifier: AGPL-3.0-only
//
// Meshtacore additions to NexusModule: the low-battery and STORM sleeps, the ready announcement after a wake, the vault v68
// fields (LPCOMP wake level, battery chemistry, ext flags, auto Zero-Hop masks), the Zero-Hop role defaults and the public
// channel silence. They live here, not in NexusModule.cpp, so a merge of the jentastic upstream does not collide with them.

#include "Channels.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "PowerStatus.h"
#include "Router.h"
#include "Throttle.h"
#include "configuration.h"
#include "main.h"
#include "modules/NexusModule.h"
#include "sleep.h"
#ifdef ARCH_ESP32
#include "esp_system.h"
#endif

using namespace meshtastic;

// Copies the v68 fields into a vault image about to be written; applyVaultV68() is the reverse.
void NexusModule::fillVaultV68(NexusVault &v) const
{
    v.lpcomp_wake_ref = lpcomp_wake_ref;
    v.battery_chemistry = battery_chemistry;
    v.ext_flags = ext_flags;
    memcpy(v.zero_hop_auto_router_mask, zero_hop_auto_router_mask, sizeof(zero_hop_auto_router_mask));
    memcpy(v.zero_hop_auto_client_mask, zero_hop_auto_client_mask, sizeof(zero_hop_auto_client_mask));
}

bool NexusModule::hasRealNexusChannel()
{
    return isNexusChannel(findFirstNexusChannel());
}

bool NexusModule::wokeFromBatteryWake()
{
#ifdef ARCH_ESP32
    return esp_reset_reason() == ESP_RST_DEEPSLEEP;
#elif defined(ARCH_NRF52)
    return (readResetReason() & POWER_RESETREAS_LPCOMP_Msk) != 0;
#else
    return false;
#endif
}

bool NexusModule::deferLowBatterySleep(uint32_t batteryMv)
{
    constexpr uint32_t QUEUE_DRAIN_MS = 5000;
    if (lowBatterySleepPending)
        return true;
    if (!hasRealNexusChannel())
        return false; // never announce on a public fallback channel

    char msg[96];
    uint32_t wakeMv = 0;
#ifdef ARCH_NRF52
    wakeMv = nrf52EstimateBatteryLpcompWakeMv();
#endif
    if (wakeMv)
        snprintf(msg, sizeof(msg), "JT: [Sleep] battery %lu mV, wakes at ~%lu mV", (unsigned long)batteryMv,
                 (unsigned long)wakeMv);
    else
        snprintf(msg, sizeof(msg), "JT: [Sleep] battery %lu mV, low-battery deep sleep", (unsigned long)batteryMv);
    sendToNexusChannel(msg, strlen(msg), 0);

    lowBatterySleepPending = true;
    lowBatterySleepDeadlineMs = millis() + QUEUE_DRAIN_MS;
    return true;
}

void NexusModule::serviceLowBatterySleep()
{
    if (lowBatterySleepPending && Throttle::deadlinePassed(lowBatterySleepDeadlineMs)) {
        lowBatterySleepPending = false;
        powerFSM.trigger(EVENT_LOW_BATTERY);
    }
}

void NexusModule::serviceStormSleep()
{
    if (!stormPending || !Throttle::deadlinePassed(stormStartDeadlineMs))
        return;
    stormPending = false;
    LOG_WARN("Nexus: STORM sleep for %lu min", (unsigned long)stormSleepMinutes);
#ifdef ARCH_NRF52
    nrf52RequestTimedSleep();
#endif
    doDeepSleep(stormSleepMinutes * 60UL * 1000UL, false, false);
}

void NexusModule::serviceReadyAnnounce()
{
    constexpr uint32_t READY_ANNOUNCE_UPTIME_MS = 60 * 1000;
    if (!readyAnnouncePending || millis() < READY_ANNOUNCE_UPTIME_MS)
        return;
    readyAnnouncePending = false;
    if (!hasRealNexusChannel())
        return;
    char msg[64];
    const uint32_t mv = powerStatus ? powerStatus->getBatteryVoltageMv() : 0;
    snprintf(msg, sizeof(msg), "JT: [Ready] awake after deep sleep, battery %lu mV", (unsigned long)mv);
    sendToNexusChannel(msg, strlen(msg), 0);
}

void NexusModule::applyLpcompWakeLevel()
{
#ifdef ARCH_NRF52
    if (!nrf52SetBatteryLpcompWakeFraction(lpcomp_wake_ref, lpcomp_wake_ref ? 16 : 0))
        lpcomp_wake_ref = 0;
#endif
}

void NexusModule::applyVaultV68(const NexusVault &v)
{
    vaultV68Loaded = true;
    lpcomp_wake_ref = (v.lpcomp_wake_ref < 16) ? v.lpcomp_wake_ref : 0;
    applyLpcompWakeLevel();
    battery_chemistry = setBatteryChemistry((BatteryChemistry)v.battery_chemistry) ? v.battery_chemistry : 0;
    ext_flags = v.ext_flags;
    memcpy(zero_hop_auto_router_mask, v.zero_hop_auto_router_mask, sizeof(zero_hop_auto_router_mask));
    memcpy(zero_hop_auto_client_mask, v.zero_hop_auto_client_mask, sizeof(zero_hop_auto_client_mask));
}

bool NexusModule::isLiveAutoZeroHopRelay(uint8_t relay_id, bool clientBase)
{
    NodeNum resolved = 0;
    if (!nodeDB->resolveUniqueLastByte(relay_id, /*requireDirectNeighbor=*/false, &resolved))
        return false;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(resolved);
    if (!node || sinceLastSeen(node) > AUTO_ZERO_HOP_MAX_SILENCE_SECS)
        return false;
    if (clientBase)
        return node->role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
    return node->role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
           node->role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
}

void NexusModule::applyZeroHopRoleDefaults(bool vaultHasV68)
{
    constexpr uint8_t ZERO_HOP_EN = 0x01, ZERO_HOP_AUTO_ROUTER = 0x08, ZERO_HOP_AUTO_CLIENT = 0x10;
    if (vaultHasV68)
        return; // the operator's saved choice wins
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
        config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE ||
        config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE)
        zero_hop_config |= ZERO_HOP_EN | ZERO_HOP_AUTO_ROUTER | ZERO_HOP_AUTO_CLIENT;
}

bool NexusModule::isSilencedPublicReply(const meshtastic_MeshPacket &mp)
{
    // pki_encrypted is only set later, at encode time, so ask willUsePki() whether this one will be a PKI DM.
    if (!(ext_flags & EXT_FLAG_PUBLIC_SILENCE) || !isFromUs(&mp) || mp.pki_encrypted || willUsePki(&mp))
        return false;
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag || mp.channel >= MAX_NUM_CHANNELS)
        return false;
    if (!cryptoKeyIsPublic(channels.getKey(mp.channel)))
        return false;
    const bool isReply = mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP || mp.decoded.request_id != 0 ||
                         (mp.decoded.portnum == meshtastic_PortNum_NODEINFO_APP && !isBroadcast(mp.to));
    return isReply;
}
