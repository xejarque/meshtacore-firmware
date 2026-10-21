#pragma once

#include "configuration.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

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

    /// First byte of SHA-256 over the effective secret, MeshCore's GroupChannel::hash.
    static bool channelHash(const uint8_t *key, size_t keyLen, uint8_t *out);

    /**
     * Encode a GRP_TXT flood frame. sender is prefixed as "sender: "; text is truncated to
     * MAX_TEXT_LEN on a UTF-8 boundary. Returns the frame length, or 0 if it does not fit.
     */
    static size_t encodeGroupText(const uint8_t *key, size_t keyLen, const char *sender, uint32_t timestamp, const char *text,
                                  uint8_t *out, size_t outCap);

    /**
     * Decrypt and validate a GRP_TXT frame for key. textOut receives the decrypted "sender: text"
     * body (NUL-terminated, UTF-8 sanitised). Returns false on a wrong channel hash, bad MAC,
     * malformed ciphertext or unsupported text type.
     */
    static bool decodeGroupText(const uint8_t *key, size_t keyLen, const uint8_t *frame, size_t len,
                                const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                uint32_t *timestampOut);

    /// Create the synthetic "MeshCore" node once, so the client has a sender name to render.
    void ensurePseudoNode();

    /// Consume a received MeshCore frame: decode GRP_TXT on any MC: channel and inject it.
    void handleFrame(const uint8_t *raw, size_t len, float snr, int32_t rssi);

    /// Mark one of our own transmitted frames seen, so hearing it back does not inject it.
    void noteTransmitted(const uint8_t *raw, size_t len);

    uint32_t rxSeen = 0, rxDecoded = 0, rxDup = 0, rxIgnored = 0, txSent = 0;

  private:
    static constexpr size_t DEDUP_ENTRIES = 32;

    uint32_t dedup[DEDUP_ENTRIES] = {}; // 0 = empty slot; hashFrame() never returns 0
    uint8_t dedupNext = 0;

    static size_t normalizedKeyLen(const uint8_t *key, size_t keyLen);
    bool wasRecentlySeen(uint32_t hash) const;
    void rememberFrame(uint32_t hash);
};

extern MeshCoreCompanion meshCoreCompanion;

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
