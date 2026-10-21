#pragma once

#include "configuration.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "MeshCoreDedup.h"
#include "MeshCoreRepeater.h"
#include <stddef.h>
#include <stdint.h>

/**
 * MeshCore group-text bridge for the Meshtastic client.
 *
 * Channels named "MC:..." carry a MeshCore group PSK. Text sent on one is encoded as a MeshCore
 * GRP_TXT flood (timestamp + "name: text", AES-128 + 2-byte HMAC); GRP_TXT frames we can decrypt
 * are injected into the Meshtastic router as a broadcast from PSEUDO_NODE_NUM, so the phone shows
 * them on that channel alongside Meshtastic traffic.
 */
class MeshCoreCompanion
{
  public:
    /// Node number all bridged MeshCore messages appear to come from.
    static constexpr uint32_t PSEUDO_NODE_NUM = 0x4D430001;

    /// MeshCore MAX_TEXT_LEN: the encrypted body's limit, including the "name: " prefix.
    static constexpr size_t MAX_TEXT_LEN = 160;

    /// Largest ciphertext a valid GRP_TXT carries: 5-byte header + MAX_TEXT_LEN, padded to the AES block.
    static constexpr size_t MAX_CIPHER_LEN = 176;

    /// Effective secret length (16 or 32): a 32-byte key with an all-zero second half is a 16-byte secret.
    /// Returns 0 for any other key length.
    static size_t normalizedKeyLen(const uint8_t *key, size_t keyLen);

    /// First byte of SHA-256 over the effective secret, MeshCore's GroupChannel::hash.
    static bool channelHash(const uint8_t *key, size_t keyLen, uint8_t *out);

    /**
     * Encode a GRP_TXT flood frame. sender is prefixed as "sender: "; text is truncated to
     * MAX_TEXT_LEN on a UTF-8 boundary; *truncated (optional) reports whether text lost its tail.
     * Returns the frame length, or 0 if it does not fit.
     */
    static size_t encodeGroupText(const uint8_t *key, size_t keyLen, const char *sender, uint32_t timestamp, const char *text,
                                  uint8_t *out, size_t outCap, bool *truncated = nullptr);

    /**
     * Decrypt and validate a GRP_TXT frame for key. textOut receives the decrypted "sender: text"
     * body (NUL-terminated, UTF-8 sanitised). Returns false on a wrong channel hash, bad MAC,
     * malformed ciphertext or unsupported text type.
     */
    static bool decodeGroupText(const uint8_t *key, size_t keyLen, const uint8_t *frame, size_t len,
                                const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                uint32_t *timestampOut);

    /// Meshtastic hop fields for a frame that crossed pathHashCount repeaters. Clients show hops as
    /// hop_start - hop_limit, so 0 hops stays "direct" and anything beyond HOP_START is capped there.
    static constexpr uint8_t HOP_START = 7;
    static void hopFields(uint8_t pathHashCount, uint8_t *hopStart, uint8_t *hopLimit);

    /// Create the synthetic "MeshCore" node if it is missing, so the client has a sender name to render.
    void ensurePseudoNode();

    /// Timestamp for the next outgoing text: strictly increasing, so two texts sent within the same
    /// second still encrypt differently and are not dropped as duplicates by repeaters.
    uint32_t nextTxTimestamp(uint32_t now);

    /// Consume a received MeshCore frame: decode GRP_TXT on any MC: channel and inject it.
    void handleFrame(const uint8_t *raw, size_t len, float snr, int32_t rssi);

    /// Mark one of our own transmitted frames seen, so hearing it back does not inject it.
    void noteTransmitted(const uint8_t *raw, size_t len);

    uint32_t rxSeen = 0, rxDecoded = 0, rxDup = 0, rxIgnored = 0, txSent = 0;

  private:
    static constexpr size_t DEDUP_ENTRIES = 64;

    /// Cached channelHash() per channel slot, recomputed when the key bytes change.
    struct HashCacheEntry {
        uint8_t key[32] = {};
        size_t keyLen = 0;
        uint8_t hash = 0;
    };

    MeshCoreDedup<DEDUP_ENTRIES> dedup; // hashFrame() never returns 0, the empty-slot marker
    HashCacheEntry hashCache[MAX_NUM_CHANNELS];
    uint32_t lastTxTimestamp = 0;

    bool cachedChannelHash(ChannelIndex ch, const uint8_t *key, size_t keyLen, uint8_t *out);
    bool wasRecentlySeen(uint32_t hash) const { return dedup.contains(hash); }
    void rememberFrame(uint32_t hash) { dedup.remember(hash); }
};

extern MeshCoreCompanion meshCoreCompanion;

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
