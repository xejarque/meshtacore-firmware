// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "MeshTypes.h"
#include "RadioInterface.h"
#include "RadioTxHook.h"

struct NexusTxOverride {
    bool inUse = false;
    PacketId id = 0;
    uint8_t sf = 0;
    uint8_t cr = 0;
    uint16_t bw = 0;
    float freq = 0.0f;
    int8_t power = 0x7F;    // 0x7F = no power override
    uint8_t syncword = 0;   // 0 = leave the main syncword unchanged
    bool isFastPath = true; // true = same freq/bw/cr, only SF/power switch
    uint32_t setAt = 0;     // timestamp at last claim/update, for LRU eviction
};

class NexusTxHook : public RadioTxHook
{
  public:
    NexusTxHook();
    ~NexusTxHook() override = default;

    PreTxAction beforeTransmit(RadioInterface *iface, meshtastic_MeshPacket *p) override;
    bool holdsRadio(const meshtastic_MeshPacket *p) override;
    void packetReleased(RadioInterface *iface, const meshtastic_MeshPacket *p) override;

    static void setOverride(PacketId id, uint8_t sf, int8_t power = 0x7F, uint8_t syncword = 0);
    static void setOverrideFull(PacketId id, uint8_t sf, uint16_t bw, uint8_t cr, float freq, int8_t power = 0x7F,
                                uint8_t syncword = 0);
    static bool hasOverride(PacketId id);
    static void clearOverride(PacketId id);

  private:
    static constexpr size_t MAX_OVERRIDES = 8;
    static NexusTxOverride overrides[MAX_OVERRIDES];

    bool fastSwitched = false;
    bool fullSwitched = false;
    bool powerSwitched = false;
    PacketId activeSwitchedId = 0;

    uint8_t originalSf = 0;
    int8_t originalPower = 0;
    uint8_t originalSyncword = 0;
    bool syncSwitched = false;

    // Full-path snapshots
    meshtastic_Config_LoRaConfig originalLora;
    meshtastic_ChannelSettings originalPrimaryChannel;
};

extern NexusTxHook *nexusTxHook;
