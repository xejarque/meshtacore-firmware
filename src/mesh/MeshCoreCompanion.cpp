#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "NodeDB.h"
#include "Router.h"
#include "TypeConversions.h"
#include "meshUtils.h"
#include <AES.h>
#include <SHA256.h>
#include <string.h>

MeshCoreCompanion meshCoreCompanion;

static size_t utf8PrefixLen(const char *s, size_t maxBytes)
{
    if (!s)
        return 0;
    size_t len = strlen(s);
    if (len <= maxBytes)
        return len;
    size_t n = maxBytes;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80)
        n--; // do not split a multi-byte character
    return n;
}

static size_t aesEncrypt(const uint8_t secret[32], const uint8_t *src, size_t len, uint8_t *dst)
{
    AES128 aes;
    if (!aes.setKey(secret, 16))
        return 0;

    size_t i = 0;
    for (; i + 16 <= len; i += 16)
        aes.encryptBlock(&dst[i], &src[i]);
    if (i < len) {
        uint8_t block[16] = {};
        memcpy(block, &src[i], len - i);
        aes.encryptBlock(&dst[i], block);
        i += 16;
    }
    return i;
}

static size_t aesDecrypt(const uint8_t secret[32], const uint8_t *src, size_t len, uint8_t *dst)
{
    if (len == 0 || len % 16 != 0)
        return 0;
    AES128 aes;
    if (!aes.setKey(secret, 16))
        return 0;
    for (size_t i = 0; i < len; i += 16)
        aes.decryptBlock(&dst[i], &src[i]);
    return len;
}

static uint32_t readLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void writeLe32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

size_t MeshCoreCompanion::normalizedKeyLen(const uint8_t *key, size_t keyLen)
{
    if (!key || (keyLen != 16 && keyLen != 32))
        return 0;
    if (keyLen == 32) {
        uint8_t tail = 0;
        for (size_t i = 16; i < 32; i++)
            tail |= key[i];
        if (tail == 0)
            return 16;
    }
    return keyLen;
}

bool MeshCoreCompanion::channelHash(const uint8_t *key, size_t keyLen, uint8_t *out)
{
    size_t len = normalizedKeyLen(key, keyLen);
    if (len == 0 || !out)
        return false;

    SHA256 sha;
    uint8_t digest[32];
    sha.reset();
    sha.update(key, len);
    sha.finalize(digest, sizeof(digest));
    *out = digest[0];
    return true;
}

size_t MeshCoreCompanion::encodeGroupText(const uint8_t *key, size_t keyLen, const char *sender, uint32_t timestamp,
                                          const char *text, uint8_t *out, size_t outCap, bool *truncated)
{
    size_t len = normalizedKeyLen(key, keyLen);
    if (len == 0 || !out)
        return 0;

    uint8_t secret[32] = {};
    memcpy(secret, key, len);

    size_t senderLen = utf8PrefixLen(sender, MAX_TEXT_LEN - 2);
    size_t prefixLen = senderLen + 2; // ": "
    size_t textLen = utf8PrefixLen(text, MAX_TEXT_LEN - prefixLen);
    if (truncated)
        *truncated = text && textLen < strlen(text);

    uint8_t plain[5 + MAX_TEXT_LEN];
    writeLe32(plain, timestamp);
    plain[4] = 0; // TXT_TYPE_PLAIN
    if (senderLen)
        memcpy(&plain[5], sender, senderLen);
    plain[5 + senderLen] = ':';
    plain[5 + senderLen + 1] = ' ';
    if (textLen)
        memcpy(&plain[5 + prefixLen], text, textLen);

    const size_t plainLen = 5 + prefixLen + textLen;
    uint8_t cipher[MAX_CIPHER_LEN];
    const size_t cipherLen = aesEncrypt(secret, plain, plainLen, cipher);
    if (cipherLen == 0 || 5 + cipherLen > outCap)
        return 0;

    uint8_t hash;
    if (!channelHash(key, keyLen, &hash))
        return 0;

    out[0] = (0x05 << 2) | 0x01;                        // GRP_TXT, ROUTE_FLOOD
    out[1] = MeshCoreRepeater::OWN_FLOOD_PATH_LEN_BYTE; // zero hops, 2-byte hashes
    out[2] = hash;

    SHA256 hmac;
    hmac.resetHMAC(secret, sizeof(secret));
    hmac.update(cipher, cipherLen);
    hmac.finalizeHMAC(secret, sizeof(secret), &out[3], 2);

    memcpy(&out[5], cipher, cipherLen);
    return 5 + cipherLen;
}

bool MeshCoreCompanion::decodeGroupText(const uint8_t *key, size_t keyLen, const uint8_t *frame, size_t len,
                                        const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                        uint32_t *timestampOut)
{
    size_t keySize = normalizedKeyLen(key, keyLen);
    if (keySize == 0 || !frame || info.payload_type != 0x05 || info.header_len >= len)
        return false;
    if (info.payload_len < 1 + 2 + 16)
        return false;

    const uint8_t *payload = &frame[info.header_len];
    uint8_t hash;
    if (!channelHash(key, keyLen, &hash) || payload[0] != hash)
        return false;

    const size_t cipherLen = info.payload_len - 3;
    if (cipherLen % 16 != 0 || cipherLen > MAX_CIPHER_LEN)
        return false;

    uint8_t secret[32] = {};
    memcpy(secret, key, keySize);

    uint8_t mac[2];
    SHA256 hmac;
    hmac.resetHMAC(secret, sizeof(secret));
    hmac.update(&payload[3], cipherLen);
    hmac.finalizeHMAC(secret, sizeof(secret), mac, sizeof(mac));
    if (memcmp(mac, &payload[1], sizeof(mac)) != 0)
        return false;

    uint8_t plain[MAX_CIPHER_LEN];
    const size_t plainLen = aesDecrypt(secret, &payload[3], cipherLen, plain);
    if (plainLen < 5 || (plain[4] >> 2) != 0) // only TXT_TYPE_PLAIN
        return false;

    if (textOut && textOutCap) {
        size_t bodyLen = plainLen - 5;
        size_t end = 0;
        while (end < bodyLen && plain[5 + end] != 0)
            end++;
        size_t copyLen = end < textOutCap ? end : textOutCap - 1;
        memcpy(textOut, &plain[5], copyLen);
        textOut[copyLen] = 0;
        sanitizeUtf8(textOut, textOutCap);
    }
    if (timestampOut)
        *timestampOut = readLe32(plain);
    return true;
}

void MeshCoreCompanion::ensurePseudoNode()
{
    if (!nodeDB)
        return;
    meshtastic_NodeInfoLite *lite = nodeDB->getOrCreateMeshNode(PSEUDO_NODE_NUM);
    if (!lite || nodeInfoLiteHasUser(lite))
        return;

    meshtastic_User user = {};
    strncpy(user.long_name, "MeshCore", sizeof(user.long_name) - 1);
    strncpy(user.short_name, "MC", sizeof(user.short_name) - 1);
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    user.has_is_unmessagable = true;
    user.is_unmessagable = true;
    TypeConversions::CopyUserToNodeInfoLite(lite, user);
}

void MeshCoreCompanion::hopFields(uint8_t pathHashCount, uint8_t *hopStart, uint8_t *hopLimit)
{
    const uint8_t hops = pathHashCount < HOP_START ? pathHashCount : HOP_START;
    *hopStart = HOP_START; // nonzero: a zero hop_start makes clients show '?' instead of the hop count
    *hopLimit = HOP_START - hops;
}

uint32_t MeshCoreCompanion::nextTxTimestamp(uint32_t now)
{
    lastTxTimestamp = now > lastTxTimestamp ? now : lastTxTimestamp + 1;
    return lastTxTimestamp;
}

bool MeshCoreCompanion::cachedChannelHash(ChannelIndex ch, const uint8_t *key, size_t keyLen, uint8_t *out)
{
    if (ch >= MAX_NUM_CHANNELS)
        return channelHash(key, keyLen, out);

    HashCacheEntry &entry = hashCache[ch];
    if (entry.keyLen != keyLen || memcmp(entry.key, key, keyLen) != 0) {
        if (!channelHash(key, keyLen, &entry.hash)) {
            entry.keyLen = 0;
            return false;
        }
        memcpy(entry.key, key, keyLen);
        entry.keyLen = keyLen;
    }
    *out = entry.hash;
    return true;
}

void MeshCoreCompanion::handleFrame(const uint8_t *raw, size_t len, float snr, int32_t rssi)
{
    if (!raw || !router)
        return;

    MeshCoreRepeater::FrameInfo info;
    if (!MeshCoreRepeater::parse(raw, len, &info) || info.payload_type != 0x05)
        return;

    rxSeen++;
    if (info.header_len >= len || info.payload_len < 1 + 2 + 16) {
        rxIgnored++;
        return;
    }
    const uint32_t hash = MeshCoreRepeater::hashFrame(raw, len, info);
    if (wasRecentlySeen(hash)) {
        rxDup++;
        return;
    }

    for (ChannelIndex i = 0; i < MAX_NUM_CHANNELS; i++) {
        uint8_t key[32];
        const size_t keyLen = channels.getMeshCoreKey(i, key);
        if (keyLen == 0)
            continue;

        uint8_t hash8;
        if (!cachedChannelHash(i, key, keyLen, &hash8) || raw[info.header_len] != hash8)
            continue; // not this channel: skip the HMAC and AES work

        char text[MAX_TEXT_LEN + 1];
        uint32_t timestamp = 0;
        if (!decodeGroupText(key, keyLen, raw, len, info, text, sizeof(text), &timestamp))
            continue;

        meshtastic_MeshPacket *p = packetPool.allocZeroed();
        if (!p)
            return;

        rememberFrame(hash);
        rxDecoded++;
        ensurePseudoNode(); // a node-DB reset or eviction may have removed it since boot

        p->from = PSEUDO_NODE_NUM;
        p->to = NODENUM_BROADCAST;
        p->id = generatePacketId();
        p->channel = i;
        p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        const size_t textLen = strlen(text);
        p->decoded.payload.size = textLen <= sizeof(p->decoded.payload.bytes) ? textLen : sizeof(p->decoded.payload.bytes);
        memcpy(p->decoded.payload.bytes, text, p->decoded.payload.size);
        p->decoded.has_bitfield = true;
        p->decoded.bitfield = 0;
        // Hops are the repeaters the frame crossed (its path). A hop_limit below hop_start also keeps this
        // synthetic packet's relay gate closed, and a next hop that is not us closes it for direct frames.
        MeshCoreCompanion::hopFields(info.path_hash_count, &p->hop_start, &p->hop_limit);
        const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
        p->next_hop = (uint8_t)(ourRelay + 1);
        if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
            p->next_hop = 1;
        p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
        // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
        if (info.path_hash_count == 0) {
            p->rx_snr = snr;
            p->has_rx_rssi = true;
            p->rx_rssi = rssi;
        }

        LOG_INFO("MeshCore group text ch=%u ts=%u len=%u", (unsigned)i, timestamp, (unsigned)p->decoded.payload.size);
        router->enqueueReceivedMessage(p);
        return;
    }

    rxIgnored++;
}

void MeshCoreCompanion::noteTransmitted(const uint8_t *raw, size_t len)
{
    MeshCoreRepeater::FrameInfo info;
    if (raw && MeshCoreRepeater::parse(raw, len, &info))
        rememberFrame(MeshCoreRepeater::hashFrame(raw, len, info));
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
