#include "NexusRescue.h"

#if HAS_NEXUS

#include "Channels.h"
#include "FSCommon.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "SafeFile.h"
#include <ErriezCRC32.h>
#include <string.h>

namespace NexusRescue
{

static constexpr const char *RESCUE_FILE = "/nexus_rescue.bin";
static constexpr char RESCUE_MAGIC[4] = {'J', 'R', 'e', 's'};
static constexpr uint32_t RESCUE_VERSION = 1;

// Raw nanopb structs: the version and the size check reject a copy written by a build with another layout.
struct __attribute__((packed)) RescueCopy {
    char magic[4];
    uint32_t version;
    uint32_t size;
    pb_size_t adminKeyCount;
    meshtastic_Config_SecurityConfig_admin_key_t adminKeys[3];
    meshtastic_ChannelFile channels;
    meshtastic_Config_LoRaConfig lora;
    meshtastic_Config_DeviceConfig_Role role;
    meshtastic_Config_DeviceConfig_RebroadcastMode rebroadcastMode;
    char longName[sizeof(meshtastic_User::long_name)];
    char shortName[sizeof(meshtastic_User::short_name)];
    uint32_t positionBroadcastSecs;
    uint32_t nodeInfoBroadcastSecs;
    uint32_t deviceTelemetrySecs;
    uint32_t environmentTelemetrySecs;
    uint32_t powerTelemetrySecs;
    uint32_t crc;
};

// Static: about 1.5 KB, too much for the nRF52 loop stack.
static RescueCopy stored;
static RescueCopy pending;

static uint32_t bodyCrc(const RescueCopy &c)
{
    return crc32Buffer(&c, offsetof(RescueCopy, crc));
}

static bool loadStored()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(RESCUE_FILE, FILE_O_READ);
    if (!f)
        return false;
    memset(&stored, 0, sizeof(stored));
    const size_t got = f.read((uint8_t *)&stored, sizeof(stored));
    f.close();
    return got == sizeof(stored) && memcmp(stored.magic, RESCUE_MAGIC, sizeof(RESCUE_MAGIC)) == 0 &&
           stored.version == RESCUE_VERSION && stored.size == sizeof(stored) && stored.crc == bodyCrc(stored);
#else
    return false;
#endif
}

static bool writeCopy(const RescueCopy &c)
{
#ifdef FSCom
    auto f = SafeFile(RESCUE_FILE, true);
    const size_t written = f.write((const uint8_t *)&c, sizeof(c));
    const bool closed = f.close();
    return written == sizeof(c) && closed;
#else
    return false;
#endif
}

static bool isEmptyKey(const meshtastic_Config_SecurityConfig_admin_key_t &k)
{
    for (pb_size_t i = 0; i < k.size; i++)
        if (k.bytes[i])
            return false;
    return true;
}

// An empty live slot keeps the stored key: only RESCUE CLEAR forgets an admin key.
static void mergeAdminKeys(RescueCopy &c, bool haveStored)
{
    pb_size_t count = 0;
    for (int i = 0; i < 3; i++) {
        const bool liveSet = i < config.security.admin_key_count && !isEmptyKey(config.security.admin_key[i]);
        if (liveSet)
            c.adminKeys[i] = config.security.admin_key[i];
        else if (haveStored && i < stored.adminKeyCount)
            c.adminKeys[i] = stored.adminKeys[i];
        if (c.adminKeys[i].size && !isEmptyKey(c.adminKeys[i]))
            count = i + 1;
    }
    c.adminKeyCount = count;
}

static void captureLive(RescueCopy &c, bool haveStored)
{
    memset(&c, 0, sizeof(c));
    memcpy(c.magic, RESCUE_MAGIC, sizeof(RESCUE_MAGIC));
    c.version = RESCUE_VERSION;
    c.size = sizeof(c);
    mergeAdminKeys(c, haveStored);
    c.channels = channelFile;
    c.lora = config.lora;
    c.role = config.device.role;
    c.rebroadcastMode = config.device.rebroadcast_mode;
    strncpy(c.longName, owner.long_name, sizeof(c.longName) - 1);
    strncpy(c.shortName, owner.short_name, sizeof(c.shortName) - 1);
    c.positionBroadcastSecs = config.position.position_broadcast_secs;
    c.nodeInfoBroadcastSecs = config.device.node_info_broadcast_secs;
    c.deviceTelemetrySecs = moduleConfig.telemetry.device_update_interval;
    c.environmentTelemetrySecs = moduleConfig.telemetry.environment_update_interval;
    c.powerTelemetrySecs = moduleConfig.telemetry.power_update_interval;
    c.crc = bodyCrc(c);
}

void snapshot()
{
    if (!nodeDB || nodeDB->bootedWithUndecodableConfig())
        return; // a degraded boot runs on defaults: copying them would overwrite the good copy
    const bool haveStored = loadStored();
    captureLive(pending, haveStored);
    if (haveStored && pending.crc == stored.crc)
        return; // unchanged: no flash write
    if (writeCopy(pending))
        LOG_INFO("Nexus: rescue copy updated");
    else
        LOG_ERROR("Nexus: rescue copy write failed");
}

static void applyCopy(const RescueCopy &c)
{
    config.security.admin_key_count = c.adminKeyCount;
    memcpy(config.security.admin_key, c.adminKeys, sizeof(c.adminKeys));
    channelFile = c.channels;
    config.lora = c.lora;
    config.has_lora = true;
    config.device.role = c.role;
    config.device.rebroadcast_mode = c.rebroadcastMode;
    if (c.longName[0])
        strncpy(owner.long_name, c.longName, sizeof(owner.long_name) - 1);
    if (c.shortName[0])
        strncpy(owner.short_name, c.shortName, sizeof(owner.short_name) - 1);
    config.position.position_broadcast_secs = c.positionBroadcastSecs;
    config.device.node_info_broadcast_secs = c.nodeInfoBroadcastSecs;
    moduleConfig.telemetry.device_update_interval = c.deviceTelemetrySecs;
    moduleConfig.telemetry.environment_update_interval = c.environmentTelemetrySecs;
    moduleConfig.telemetry.power_update_interval = c.powerTelemetrySecs;
}

bool reapplyToLive()
{
    if (!loadStored())
        return false;
    applyCopy(stored);
    channels.onConfigChanged();
    return true;
}

bool restoreIfConfigWasMissing()
{
    if (!nodeDB || !nodeDB->bootedWithMissingConfig() || !reapplyToLive())
        return false;
    LOG_WARN("Nexus: config was missing at boot, rescue copy restored");
    nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_CHANNELS | SEGMENT_DEVICESTATE);
    return true;
}

bool clear()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    if (!FSCom.exists(RESCUE_FILE))
        return true;
    return FSCom.remove(RESCUE_FILE);
#else
    return false;
#endif
}

void describe(char *out, size_t outLen)
{
    if (!loadStored()) {
        snprintf(out, outLen, "JT: RESCUE no copy stored");
        return;
    }
    unsigned channelsUsed = 0;
    for (pb_size_t i = 0; i < stored.channels.channels_count; i++)
        if (stored.channels.channels[i].role != meshtastic_Channel_Role_DISABLED)
            channelsUsed++;
    snprintf(out, outLen, "JT: RESCUE keys %u, channels %u, region %d preset %d role %d, owner %.8s",
             (unsigned)stored.adminKeyCount, channelsUsed, (int)stored.lora.region, (int)stored.lora.modem_preset,
             (int)stored.role, stored.shortName);
}

} // namespace NexusRescue

#endif
