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
 *
 * Known limitation: the two protocols are not fully distinguishable by header bytes alone (both
 * look random). A MeshCore-looking frame that is NOT addressed to/from us could still be a
 * Meshtastic unicast between two other nodes (a few percent of overheard DMs by length); those
 * are sniffed instead of routed. Broadcasts and DMs to/from this node are never misclassified.
 */
class MeshCoreRepeater
{
  public:
    static constexpr size_t MAX_FRAME_LEN = 255; // MeshCore MAX_TRANS_UNIT

    /** MeshCore MAX_PATH_SIZE: the flood path budget in bytes, shared by every MeshCore node. */
    static constexpr size_t MAX_PATH_SIZE = 64;

    /** Parsed view of a MeshCore frame header (see MeshCore Dispatcher::tryParsePacket). */
    struct FrameInfo {
        uint8_t route_type;   // 0x00 transport-flood, 0x01 flood, 0x02 direct, 0x03 transport-direct
        uint8_t payload_type; // PAYLOAD_TYPE_* (0x00-0x0B, 0x0F)
        uint8_t payload_ver;  // must be 0 (PAYLOAD_VER_1)
        uint8_t path_hash_size;
        uint8_t path_hash_count;
        uint8_t header_len;  // bytes consumed by header + transport codes + path
        uint8_t payload_len; // remaining bytes
    };

    /**
     * True if the raw LoRa frame looks like a valid MeshCore packet. Conservative on purpose:
     * must be >= 2 bytes, <= 255, payload version 0, a known payload type, and a valid path_len
     * byte (path mode 3 is reserved). A frame whose Meshtastic to/from NodeNum equals ourNodeNum
     * is never MeshCore (crc32(pubkey) collision odds are 2^-32), so a Meshtastic unicast to or
     * from this node is kept out of the sniffer even when its bytes happen to parse.
     */
    static bool looksLikeMeshCoreFrame(const uint8_t *raw, size_t len, uint32_t ourNodeNum, FrameInfo *out);

    /** Parse a MeshCore frame header. Returns false if the frame is malformed. */
    static bool parse(const uint8_t *raw, size_t len, FrameInfo *out);

    /**
     * Handle a received MeshCore frame (raw bytes as demodulated). Sniffs always; relays floods
     * if enabled. airtimeMs is the frame's on-air time (RadioInterface::getPacketTime), used to
     * size the adaptive relay delay.
     */
    void handleReceivedFrame(const uint8_t *raw, size_t len, int8_t snr, uint32_t airtimeMs);

    /** True when a raw MeshCore frame is queued for transmission. */
    bool hasPendingFrame() const { return pendingLen > 0; }

    /** True when the pending frame's adaptive delay has elapsed and it may go out now. */
    bool isRelayDue() const;

    /** Milliseconds until the pending relay is due (0 when due or nothing pending). */
    uint32_t relayDueInMs() const;

    /** Copy the pending raw frame into buf; returns its length (0 if none). */
    size_t takePendingFrame(uint8_t *buf, size_t bufLen);

    /** Send or drop the pending frame: folds its dupe sample into the contention estimate. */
    void clearPendingFrame();

    /** Adaptive jitter factor in [0.40, 1.00] (0.5 before warmup), from the observed dupes. */
    float getFloodDelayFactor();

    // Counters for observability
    uint32_t rxSeen = 0, rxFlood = 0, rxDup = 0, rxDropped = 0, txRelayed = 0, txDropped = 0;

  private:
    static constexpr uint8_t MAX_PATH_HASH_SIZE = 3; // MeshCore allows 1..3
    static constexpr uint8_t MAX_PATH_HASH_COUNT = 63;
    static constexpr size_t DEDUP_ENTRIES = 64;
    static constexpr uint32_t DEDUP_WINDOW_MS = 15000;

    // MeshCore's flood_max_advert default (NodePrefs.h): ADVERT floods stop being forwarded
    // after this many hops. flood_max itself (64) is implied by the 6-bit hop count plus the
    // MAX_PATH_SIZE bound above.
    static constexpr uint8_t MAX_FLOOD_HOPS_ADVERT = 8;

    // Adaptive relay delay (design follows ZephCore's ContentionTracker / computeAdaptiveFloodDelay,
    // MIT): jitter is 20ms + rand(0, 5 x airtime x factor), capped at 6 x airtime and 2000ms;
    // hearing a duplicate of the pending frame postpones it by 0.2 x airtime per dupe, capped at
    // 12 x airtime / 2000ms cumulative.
    static constexpr uint32_t DELAY_FLOOR_MS = 20;
    static constexpr float JITTER_AIRTIME_MULT = 5.0f;
    static constexpr float JITTER_AIRTIME_CAP = 6.0f;
    static constexpr uint32_t JITTER_CAP_MS = 2000;
    static constexpr float REACTIVE_BACKOFF_AIRTIME = 0.2f;
    static constexpr float REACTIVE_AIRTIME_CAP = 12.0f;
    static constexpr uint32_t REACTIVE_HARD_CAP_MS = 2000;
    static constexpr uint32_t DUPE_WINDOW_MS = 10000;

    // Contention estimate: EMA (x256 fixed point) of the per-relay dupe counts.
    static constexpr int EMA_SHIFT = 3;      // alpha = 1/8
    static constexpr int WARMUP_SAMPLES = 4; // samples before the EMA is trusted
    static constexpr float MIN_FLOOD_FACTOR = 0.40f;
    static constexpr float MAX_FLOOD_FACTOR = 1.00f;
    static constexpr float FLOOD_EST_HALFPOINT = 4.0f; // factor = 0.60 at est = 4
    // The estimate decays when the relay goes idle: one 1/8 step per period once the relay has
    // been quiet for STALE_MS, applied lazily whenever the factor is read (no timer).
    static constexpr uint32_t STALE_MS = 300000;
    static constexpr uint32_t DECAY_PERIOD_MS = 5000;
    static constexpr uint32_t MAX_DECAY_CATCHUP = 8;

    struct DedupEntry {
        uint32_t hash;
        uint32_t seenAtMs;
    };

    /** Observation for the frame currently being relayed: its dupes are the contention sample. */
    struct PendingRelay {
        uint32_t hash32 = 0;
        uint32_t firstSeenMs = 0;
        uint32_t airtimeMs = 0;
        uint16_t reactiveAddedMs = 0;
        uint8_t dupeCount = 0;
    };

    DedupEntry dedup[DEDUP_ENTRIES];
    uint8_t dedupNext = 0;

    uint8_t pending[MAX_FRAME_LEN];
    size_t pendingLen = 0;
    PendingRelay pendingRelay;
    uint32_t pendingDeadlineMs = 0;

    uint32_t emaX256 = 0;
    uint16_t finalizedSamples = 0;
    uint32_t lastRelayMs = 0;
    uint32_t lastDecayMs = 0;

    static uint32_t hashFrame(const uint8_t *raw, size_t len, const FrameInfo &info);
    bool wasRecentlySeen(uint32_t hash);
    void onDupeHeard(uint32_t hash);
    bool canRelay() const;
    void queueRelay(const uint8_t *raw, size_t len, uint32_t hash32, uint32_t airtimeMs);
    void foldContentionSample();
    void applyLazyDecay();
    uint32_t adaptiveDelayMs(uint32_t airtimeMs);
};

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
