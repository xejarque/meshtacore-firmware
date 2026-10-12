#pragma once

#include "configuration.h"
#include <stddef.h>
#include <stdint.h>

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER

/**
 * Minimal, protocol-agnostic relay for MeshCore (meshcore-dev/MeshCore) LoRa frames.
 *
 * MeshCore frames carry no Meshtastic PacketHeader: the on-air bytes are exactly its own
 * 1-byte packet header (route type in bits 0-1, payload type in bits 2-5, version in bits 6-7),
 * optionally 4 bytes of transport codes, one path_len byte, the path, then the payload. Flood
 * frames are relayed transparently (never decrypted - MeshCore does the same in its own
 * simple_repeater example), with dedup and a hop-count bound. This only makes sense when the
 * Meshtastic node is on the same frequency and spreading factor as the MeshCore network.
 */
class MeshCoreRepeater
{
  public:
    static constexpr size_t MAX_FRAME_LEN = 255; // MeshCore MAX_TRANS_UNIT

    /** Parsed view of a MeshCore frame header (see MeshCore Dispatcher::tryParsePacket). */
    struct FrameInfo {
        uint8_t route_type;   // 0x00 transport-flood, 0x01 flood, 0x02 direct, 0x03 transport-direct
        uint8_t payload_type; // PAYLOAD_TYPE_* (0x00-0x0B, 0x0F)
        uint8_t payload_ver;  // must be 0 (PAYLOAD_VER_1)
        uint8_t path_hash_size;
        uint8_t path_hash_count;
        uint8_t header_len;   // bytes consumed by header + transport codes + path
        uint8_t payload_len;  // remaining bytes
    };

    /**
     * True if the raw LoRa frame looks like a valid MeshCore packet. Conservative on purpose:
     * must be >= 2 bytes, <= 255, payload version 0, a known payload type, and a valid path_len
     * byte (path mode 3 is reserved). Returns false for any Meshtastic frame with a 12-byte header.
     */
    static bool looksLikeMeshCoreFrame(const uint8_t *raw, size_t len, FrameInfo *out);

    /** Parse a MeshCore frame header. Returns false if the frame is malformed or not flood-routed. */
    static bool parse(const uint8_t *raw, size_t len, FrameInfo *out);

    /** Handle a received MeshCore frame (raw bytes as demodulated). Sniffs always; relays floods if enabled. */
    void handleReceivedFrame(const uint8_t *raw, size_t len, int8_t snr);

    /** True when a raw MeshCore frame is queued for transmission. */
    bool hasPendingFrame() const { return pendingLen > 0; }

    /** Copy the pending raw frame into buf; returns its length (0 if none). */
    size_t takePendingFrame(uint8_t *buf, size_t bufLen);

    void onFrameSent() { pendingLen = 0; }

    // Counters for observability
    uint32_t rxSeen = 0, rxFlood = 0, rxDup = 0, rxDropped = 0, txRelayed = 0;

  private:
    static constexpr uint8_t MAX_PATH_HASH_SIZE = 3; // MeshCore allows 1..3
    static constexpr uint8_t MAX_PATH_HASH_COUNT = 63;
    static constexpr size_t DEDUP_ENTRIES = 32;
    static constexpr uint32_t RELAY_COOLDOWN_MS = 250; // min gap between MeshCore relays

    struct DedupEntry {
        uint32_t hash;
        uint32_t seenAtMs;
    };

    DedupEntry dedup[DEDUP_ENTRIES];
    uint8_t dedupNext = 0;

    uint8_t pending[MAX_FRAME_LEN];
    size_t pendingLen = 0;
    uint32_t lastRelayMs = 0;

    static uint32_t hashFrame(const uint8_t *raw, size_t len);
    bool wasRecentlySeen(uint32_t hash);
    bool canRelay() const;
    void queueRelay(const uint8_t *raw, size_t len);
};

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
