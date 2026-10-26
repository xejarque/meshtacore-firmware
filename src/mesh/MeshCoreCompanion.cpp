#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#ifdef CRYPTO_AES_NO_DECRYPT
#error "MeshCore needs AES decryption: add -UCRYPTO_AES_NO_DECRYPT to this env (the nRF52 base defines it)"
#endif

#include "Channels.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "gps/RTC.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <AES.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <SHA512.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if !(MESHTASTIC_EXCLUDE_XEDDSA)
#include "XEdDSA.h" // fe_* field arithmetic for the Ed25519 -> X25519 public key map
#endif

MeshCoreCompanion meshCoreCompanion;

/// Where the path lives in a parsed frame: it sits between the header and the payload.
static const uint8_t *pathOf(const uint8_t *raw, const MeshCoreRepeater::FrameInfo &info)
{
    return &raw[info.header_len - (size_t)info.path_hash_count * info.path_hash_size];
}

#if MESHTASTIC_MESHCORE_PATH_COMMAND
static const char *pathSenderName(uint32_t nodeNum)
{
    if (!nodeDB || !nodeNum)
        return nullptr;
    const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(nodeNum);
    return n && nodeInfoLiteHasUser(n) && n->long_name[0] ? n->long_name : nullptr;
}

/// Most names shown for one hop; a wider match is summarised as "+N".
static constexpr size_t PATH_HOP_NAMES_MAX = 3;
#endif

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

bool MeshCoreCompanion::sharedSecret(const uint8_t seed[32], const uint8_t peerPub[32], uint8_t out[32])
{
#if !(MESHTASTIC_EXCLUDE_XEDDSA)
    if (!seed || !peerPub || !out)
        return false;

    // Peer: Montgomery u = (1 + y) / (1 - y) from the Edwards y in the public key.
    fe y, one, num, den;
    fe_frombytes(y, peerPub);
    fe_1(one);
    fe_add(num, one, y);
    fe_sub(den, one, y);
    fe_invert(den, den);
    fe_mul(num, num, den);
    uint8_t u[32];
    fe_tobytes(u, num);

    // Us: the Ed25519 scalar is the clamped first half of SHA-512(seed).
    SHA512 sha;
    uint8_t digest[64];
    sha.reset();
    sha.update(seed, 32);
    sha.finalize(digest, sizeof(digest));
    digest[0] &= 248;
    digest[31] &= 63;
    digest[31] |= 64;

    const bool ok = Curve25519::eval(out, digest, u) && !Curve25519::isWeakPoint(out);
    memset(digest, 0, sizeof(digest));
    return ok;
#else
    return false;
#endif
}

// MeshCore datagram (TXT_MSG, PATH): dest hash | src hash | MAC(2) | AES-128 blocks, MAC = HMAC-SHA256 over the cipher.
static size_t sealDatagram(const uint8_t secret[32], uint8_t payloadType, uint8_t destHash, uint8_t srcHash, const uint8_t *plain,
                           size_t plainLen, uint8_t *out, size_t outCap)
{
    uint8_t cipher[MeshCoreCompanion::MAX_CIPHER_LEN];
    if (plainLen > sizeof(cipher))
        return 0;
    const size_t cipherLen = aesEncrypt(secret, plain, plainLen, cipher);
    if (cipherLen == 0 || 6 + cipherLen > outCap)
        return 0;

    out[0] = (payloadType << 2) | 0x01;                 // ROUTE_FLOOD
    out[1] = MeshCoreRepeater::OWN_FLOOD_PATH_LEN_BYTE; // zero hops, 2-byte hashes
    out[2] = destHash;
    out[3] = srcHash;

    SHA256 hmac;
    hmac.resetHMAC(secret, 32);
    hmac.update(cipher, cipherLen);
    hmac.finalizeHMAC(secret, 32, &out[4], 2);

    memcpy(&out[6], cipher, cipherLen);
    return 6 + cipherLen;
}

/// Verify the MAC of a datagram payload and decrypt it into plain (MAX_CIPHER_LEN bytes). False on any mismatch.
static bool openDatagram(const uint8_t secret[32], uint8_t payloadType, const uint8_t *frame, size_t len,
                         const MeshCoreRepeater::FrameInfo &info, uint8_t *plain, size_t *plainLen)
{
    if (!secret || !frame || info.payload_type != payloadType || info.header_len >= len)
        return false;
    if (info.payload_len < 1 + 1 + 2 + 16)
        return false;

    const uint8_t *payload = &frame[info.header_len];
    const size_t cipherLen = info.payload_len - 4;
    if (cipherLen % 16 != 0 || cipherLen > MeshCoreCompanion::MAX_CIPHER_LEN)
        return false;

    uint8_t mac[2];
    SHA256 hmac;
    hmac.resetHMAC(secret, 32);
    hmac.update(&payload[4], cipherLen);
    hmac.finalizeHMAC(secret, 32, mac, sizeof(mac));
    if (memcmp(mac, &payload[2], sizeof(mac)) != 0)
        return false;

    *plainLen = aesDecrypt(secret, &payload[4], cipherLen, plain);
    return *plainLen != 0;
}

size_t MeshCoreCompanion::encodeDirectText(const uint8_t secret[32], uint8_t destHash, uint8_t srcHash, uint32_t timestamp,
                                           uint8_t flags, const char *text, uint8_t *out, size_t outCap, bool *truncated)
{
    if (!secret || !out)
        return 0;

    const size_t textLen = utf8PrefixLen(text, MAX_TEXT_LEN);
    if (truncated)
        *truncated = text && textLen < strlen(text);

    uint8_t plain[5 + MAX_TEXT_LEN];
    writeLe32(plain, timestamp);
    plain[4] = flags;
    if (textLen)
        memcpy(&plain[5], text, textLen);

    return sealDatagram(secret, 0x02, destHash, srcHash, plain, 5 + textLen, out, outCap); // TXT_MSG
}

bool MeshCoreCompanion::decodeDirectText(const uint8_t secret[32], const uint8_t *frame, size_t len,
                                         const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                         uint32_t *timestampOut, uint8_t *flagsOut)
{
    uint8_t plain[MAX_DIRECT_CIPHER_LEN];
    size_t plainLen = 0;
    if (!openDatagram(secret, 0x02, frame, len, info, plain, &plainLen) || plainLen < 5 || (plain[4] >> 2) != TXT_FLAGS_PLAIN)
        return false;

    if (textOut && textOutCap) {
        size_t end = 0;
        while (end < plainLen - 5 && plain[5 + end] != 0)
            end++;
        size_t copyLen = end < textOutCap ? end : textOutCap - 1;
        memcpy(textOut, &plain[5], copyLen);
        textOut[copyLen] = 0;
        sanitizeUtf8(textOut, textOutCap);
    }
    if (timestampOut)
        *timestampOut = readLe32(plain);
    if (flagsOut)
        *flagsOut = plain[4];
    return true;
}

size_t MeshCoreCompanion::encodePathAck(const uint8_t secret[32], uint8_t destHash, uint8_t srcHash, uint32_t hash, uint8_t *out,
                                        size_t outCap)
{
    if (!secret || !out)
        return 0;
    // Mesh::createPathReturn: the path walked so far (none), then an extra block: its type and the ACK hash.
    uint8_t plain[6];
    plain[0] = 0x00; // path_len: 1-byte hashes, no hops
    plain[1] = 0x03; // extra type: ACK
    writeLe32(&plain[2], hash);
    return sealDatagram(secret, 0x08, destHash, srcHash, plain, sizeof(plain), out, outCap); // PATH
}

bool MeshCoreCompanion::decodePathAck(const uint8_t secret[32], const uint8_t *frame, size_t len,
                                      const MeshCoreRepeater::FrameInfo &info, uint32_t *hashOut)
{
    uint8_t plain[MAX_CIPHER_LEN];
    size_t plainLen = 0;
    if (!hashOut || !openDatagram(secret, 0x08, frame, len, info, plain, &plainLen))
        return false;

    // path_len (hash size in the top two bits, hop count in the rest), the path, then the extra block.
    size_t at = 0;
    const uint8_t pathLenByte = plain[at++];
    at += (size_t)((pathLenByte >> 6) + 1) * (pathLenByte & 0x3F);
    if (at >= plainLen || (plain[at++] & 0x0F) != 0x03 || plainLen - at < 4) // the extra must be an ACK
        return false;
    *hashOut = readLe32(&plain[at]);
    return true;
}

uint32_t MeshCoreCompanion::ackHash(uint32_t timestamp, uint8_t flags, const char *text, const uint8_t senderPub[32])
{
    uint8_t head[5];
    writeLe32(head, timestamp);
    head[4] = flags;

    SHA256 sha;
    uint8_t digest[32];
    sha.reset();
    sha.update(head, sizeof(head));
    if (text)
        sha.update(text, strlen(text));
    sha.update(senderPub, 32);
    sha.finalize(digest, sizeof(digest));
    return readLe32(digest);
}

size_t MeshCoreCompanion::encodeAck(uint32_t hash, uint8_t *out, size_t outCap)
{
    if (!out || outCap < 6)
        return 0;
    out[0] = (0x03 << 2) | 0x01; // ACK, ROUTE_FLOOD
    out[1] = MeshCoreRepeater::OWN_FLOOD_PATH_LEN_BYTE;
    writeLe32(&out[2], hash);
    return 6;
}

void MeshCoreCompanion::announceUser(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx)
{
    if (!router)
        return;
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;

    p->from = nodeNum;
    p->to = NODENUM_BROADCAST;
    p->id = generatePacketId();
    p->channel = 0;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_User_msg, &user);
    // Local news for the client, never to be repeated over Meshtastic: a next hop that is not us closes the relay gate. The
    // router drops a packet whose hop_start is missing, so it carries the hop fields of the frame it announces.
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    hopFields(rx.pathHashCount, &p->hop_start, &p->hop_limit);
    const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    p->next_hop = (uint8_t)(ourRelay + 1);
    if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
        p->next_hop = 1;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
    if (rx.pathHashCount == 0) {
        p->rx_snr = rx.snr;
        p->has_rx_rssi = true;
        p->rx_rssi = rx.rssi;
    }
    router->enqueueReceivedMessage(p);
}

void MeshCoreCompanion::announceIfNeeded(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx)
{
    if (nodeDB && needsAnnounce(nodeDB->getMeshNode(nodeNum), user, getValidTime(RTCQualityFromNet)))
        announceUser(nodeNum, user, rx);
}

void MeshCoreCompanion::announcePosition(uint32_t nodeNum, int32_t latitudeI, int32_t longitudeI)
{
    if (!router)
        return;
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;

    meshtastic_Position pos = meshtastic_Position_init_default;
    pos.has_latitude_i = true;
    pos.latitude_i = latitudeI;
    pos.has_longitude_i = true;
    pos.longitude_i = longitudeI;
    pos.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    pos.time = getValidTime(RTCQualityFromNet);

    p->from = nodeNum;
    p->to = NODENUM_BROADCAST;
    p->id = generatePacketId();
    p->channel = 0;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_POSITION_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Position_msg, &pos);
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    hopFields(1, &p->hop_start, &p->hop_limit);
    const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    p->next_hop = (uint8_t)(ourRelay + 1);
    if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
        p->next_hop = 1;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    router->enqueueReceivedMessage(p);
}

void MeshCoreCompanion::updatePosition(uint32_t nodeNum, int32_t latMicro, int32_t lonMicro)
{
    if (!nodeDB || (latMicro == 0 && lonMicro == 0))
        return;
    // MeshCore sends degrees x 1e6, Meshtastic keeps 1e-7.
    const int32_t latI = latMicro * 10, lonI = lonMicro * 10;
    meshtastic_PositionLite known;
    if (nodeDB->copyNodePosition(nodeNum, known) && known.latitude_i == latI && known.longitude_i == lonI)
        return; // adverts repeat every few minutes: only a move is news
    announcePosition(nodeNum, latI, lonI);
}

void MeshCoreCompanion::ensurePseudoNode()
{
    meshtastic_User user = {};
    strncpy(user.long_name, SYSTEM_BOT_LONG_NAME, sizeof(user.long_name) - 1);
    strncpy(user.short_name, SYSTEM_BOT_SHORT_NAME, sizeof(user.short_name) - 1);
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    user.has_is_unmessagable = true;
    user.is_unmessagable = true;
    announceIfNeeded(PSEUDO_NODE_NUM, user, MeshCoreRx());
}

bool MeshCoreCompanion::splitSender(const char *text, char *nameOut, size_t nameCap, const char **body)
{
    if (!text || !nameOut || nameCap == 0 || !body)
        return false;
    const char *sep = strstr(text, ": ");
    if (!sep || sep == text)
        return false;

    const size_t nameLen = sep - text;
    char name[MAX_TEXT_LEN + 1];
    memcpy(name, text, nameLen);
    name[nameLen] = 0;
    const size_t keep = utf8PrefixLen(name, nameCap - 1);
    memcpy(nameOut, name, keep);
    nameOut[keep] = 0;
    *body = sep + 2;
    return true;
}

uint32_t MeshCoreCompanion::senderNodeNum(const char *name)
{
    const uint32_t num = MeshCoreNodeStore<1>::nodeNumForName(name);
    return num == PSEUDO_NODE_NUM ? num + 1 : num;
}

meshtastic_User MeshCoreCompanion::senderUser(const char *name, bool messagable, const uint8_t *publicKey,
                                              meshtastic_Config_DeviceConfig_Role role)
{
    // A Meshtacore node shows up twice, as a Meshtastic node and as a MeshCore one with the same name: the suffix tells them
    // apart.
    constexpr char suffix[] = " (MC)";
    char display[sizeof(((meshtastic_User *)nullptr)->long_name)];
    const size_t nameLen = utf8PrefixLen(name, sizeof(((meshtastic_NodeInfoLite *)nullptr)->long_name) - 1 - (sizeof(suffix) - 1));
    memcpy(display, name, nameLen);
    memcpy(&display[nameLen], suffix, sizeof(suffix));

    meshtastic_User user = {};
    strncpy(user.long_name, display, sizeof(user.long_name) - 1);
    memcpy(user.short_name, name, utf8PrefixLen(name, 4));
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = role;
    user.has_is_unmessagable = true;
    user.is_unmessagable = !messagable;
    if (publicKey) {
        user.public_key.size = 32;
        memcpy(user.public_key.bytes, publicKey, 32);
    }
    return user;
}

bool MeshCoreCompanion::needsAnnounce(const meshtastic_NodeInfoLite *lite, const meshtastic_User &user, uint32_t now)
{
    // Only a packet makes a node heard, so one that is missing or was never heard over the radio gets that packet.
    if (!lite || !nodeInfoLiteHasUser(lite) || !nodeInfoLiteHasRfHear(lite))
        return true;
    if (strncmp(lite->long_name, user.long_name, sizeof(lite->long_name) - 1) != 0 ||
        nodeInfoLiteIsUnmessagable(lite) != user.is_unmessagable || lite->role != user.role ||
        lite->public_key.size != user.public_key.size ||
        memcmp(lite->public_key.bytes, user.public_key.bytes, user.public_key.size) != 0)
        return true;
    // Like any packet from a Meshtastic node, a frame moves its last-heard on; spaced out so repeated adverts do not flood the
    // client.
    return now > lite->last_heard && lite->last_heard != 0 && now - lite->last_heard >= HEARD_REFRESH_SECS;
}

void MeshCoreCompanion::ensureSenderNode(uint32_t nodeNum, const char *name, bool messagable, const uint8_t *publicKey,
                                         meshtastic_Config_DeviceConfig_Role role, const MeshCoreRx &rx)
{
    if (!nodeDB)
        return;
    meshtastic_NodeInfoLite *lite = nodeDB->getMeshNode(nodeNum);
    // A real Meshtastic node that happens to share this number reports a hardware model: never rename it. An ignored one is
    // dropped by the router, so announcing it would never get it heard.
    if (lite && ((nodeInfoLiteHasUser(lite) && lite->hw_model != meshtastic_HardwareModel_UNSET) || nodeInfoLiteIsIgnored(lite)))
        return;
    // A keyed peer lives in the node store too: its node DB copy may go first when the DB is full. The node is born from the
    // announce below, so a new one gets the mark on its next frame, or on the next save sweep.
    if (lite && publicKey)
        nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_IS_MESHCORE_MASK, true);

    announceIfNeeded(nodeNum, senderUser(name, messagable, publicKey, role), rx);
}

void MeshCoreCompanion::hopFields(uint8_t pathHashCount, uint8_t *hopStart, uint8_t *hopLimit)
{
    const uint8_t hops = pathHashCount < HOP_START ? pathHashCount : HOP_START;
    *hopStart = HOP_START; // nonzero: a zero hop_start makes clients show '?' instead of the hop count
    *hopLimit = HOP_START - hops;
}

#if MESHTASTIC_MESHCORE_PATH_COMMAND
void MeshCoreCompanion::noteHeardRoute(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel,
                                       const uint8_t *path, uint8_t hashSize, uint8_t hopCount, uint8_t routeType,
                                       float snr, int32_t rssi)
{
    const size_t bytes = (size_t)hopCount * hashSize;
    const size_t keep = bytes < sizeof(heard[0].path) ? bytes : sizeof(heard[0].path);

    HeardPath *owner = nullptr;
    for (HeardPath &h : heard) {
        if (!h.packetId || h.frameHash != frameHash)
            continue;
        if (!owner)
            owner = &h;
        // The same route heard twice only refreshes its signal: a flood arrives once per route, not once per copy.
        if (!h.own && h.hashSize == hashSize && h.hopCount == hopCount &&
            (keep == 0 || (path && memcmp(h.path, path, keep) == 0))) {
            h.snr = snr;
            h.rssi = rssi;
            return;
        }
    }
    // A later copy carries no id of its own: it belongs to the text the first copy delivered to the client.
    if (!packetId) {
        if (!owner)
            return;
        packetId = owner->packetId;
        fromNum = owner->fromNum;
        channel = owner->channel;
    }

    HeardPath &h = heard[heardNext];
    heardNext = (heardNext + 1) % PATH_MEMORY;
    h = HeardPath();
    h.packetId = packetId;
    h.frameHash = frameHash;
    h.fromNum = fromNum;
    h.channel = channel;
    h.routeType = routeType;
    h.hashSize = hashSize;
    h.hopCount = hopCount;
    h.snr = snr;
    h.rssi = rssi;
    h.heardMs = millis();
    if (keep && path)
        memcpy(h.path, path, keep);
}

void MeshCoreCompanion::noteSentText(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel)
{
    HeardPath &h = heard[heardNext];
    heardNext = (heardNext + 1) % PATH_MEMORY;
    h = HeardPath();
    h.packetId = packetId;
    h.frameHash = frameHash;
    h.fromNum = fromNum;
    h.channel = channel;
    h.heardMs = millis();
    h.own = true;
}

const MeshCoreCompanion::HeardPath *MeshCoreCompanion::heardFind(uint32_t packetId) const
{
    if (!packetId)
        return nullptr;
    for (const HeardPath &h : heard)
        if (h.packetId == packetId)
            return &h;
    return nullptr;
}

/// A node the companion announced for a MeshCore peer: our number range, a MeshCore key and no Meshtastic hardware.
static bool isMeshCoreNode(const meshtastic_NodeInfoLite *n)
{
    return n && (n->num >> 24) == 0x4D && n->public_key.size == 32 && n->hw_model == meshtastic_HardwareModel_UNSET;
}

size_t MeshCoreCompanion::pathHopNames(const uint8_t *hash, uint8_t size, const char **out, size_t max) const
{
    size_t total = 0;
    auto addName = [&](const char *name) {
        if (!name || !name[0])
            return;
        for (size_t k = 0; k < total && k < max; k++)
            if (strcmp(out[k], name) == 0)
                return; // the same node listed twice is not a second candidate
        if (total < max && out)
            out[total] = name;
        total++;
    };

    const MeshCoreNode *known[PATH_HOP_NAMES_MAX];
    const size_t knownCount = nodes.findByPrefix(hash, size, known, PATH_HOP_NAMES_MAX);
    for (size_t i = 0; i < knownCount && i < PATH_HOP_NAMES_MAX; i++)
        addName(known[i]->name);
    // MeshCore nodes announced before the store existed are only in the node DB. A Meshtastic node's key has nothing to
    // do with a MeshCore hash, so only MeshCore nodes are looked at.
    for (size_t i = 0; nodeDB && i < nodeDB->getNumMeshNodes(); i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!isMeshCoreNode(n) || n->long_name[0] == 0 || memcmp(n->public_key.bytes, hash, size) != 0)
            continue;
        if (!nodes.find(n->public_key.bytes))
            addName(n->long_name);
    }
    return total;
}

/// "A|B" for the names behind a hop, or "" when none. Duplicates collapse; beyond two names the rest become "+N".
static void formatHopLabel(const char *const *names, size_t count, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    char cleaned[PATH_HOP_NAMES_MAX][40];
    size_t have = 0;
    for (size_t i = 0; i < count && i < PATH_HOP_NAMES_MAX; i++) {
        if (cleanPathLabel(names[i], cleaned[have], sizeof(cleaned[have]), sizeof(cleaned[have]) - 1) == 0)
            continue;
        stripMeshCoreSuffix(cleaned[have]);
        if (cleaned[have][0])
            have++;
    }
    if (have == 0)
        return;

    const size_t limit = have > 2 ? 2 : have;
    size_t o = 0;
    for (size_t i = 0; i < limit; i++) {
        if (i && o + 1 < cap)
            out[o++] = '|';
        const size_t l = strlen(cleaned[i]);
        if (o + l + 1 >= cap)
            break;
        memcpy(&out[o], cleaned[i], l);
        o += l;
    }
    if (have > limit) {
        const int n = snprintf(&out[o], cap - o, "+%u", (unsigned)(have - limit));
        if (n > 0 && (size_t)n < cap - o)
            o += (size_t)n;
    }
    out[o] = 0;
}

static constexpr size_t REPORT_CHUNK_CAP = sizeof(((meshtastic_MeshPacket *)nullptr)->decoded.payload.bytes);

/**
 * Packs the report into client-sized messages. Text is added one atomic token at a time - a glyph, a hex hop, a bold
 * name, a " · SNR ..." group - and a token always opens and closes inside the same message. A line that does not fit
 * continues in the next message, so nothing is cut and clients never see unmatched markdown markers.
 */
struct ReportWriter {
    MeshCoreCompanion::ReportSink sink;
    void *ctx;
    char buf[REPORT_CHUNK_CAP + 1] = {};
    size_t len = 0;
    unsigned parts = 0;

    void flush()
    {
        if (!len)
            return;
        sink(ctx, buf, len);
        parts++;
        len = 0;
        buf[0] = 0;
    }

    void startPart()
    {
        if (len == 0 && parts > 0) { // a piece that continues a split report
            memcpy(buf, "↳ ", 4);
            len = 4;
            buf[len] = 0;
        }
    }

    /// Append one token; firstOfLine also consumes the newline owed before it. A token that cannot fit alone is the
    /// only thing that gets split, and that needs a name or hop far longer than the report allows.
    void token(const char *text, bool firstOfLine = false)
    {
        const size_t n = strlen(text);
        if (firstOfLine && len) {
            if (len + 1 + n > REPORT_CHUNK_CAP)
                flush();
            else {
                buf[len++] = '\n';
                buf[len] = 0;
            }
        }
        if (len + n > REPORT_CHUNK_CAP)
            flush();
        startPart();
        if (len + n > REPORT_CHUNK_CAP) {
            addSplit(text);
            return;
        }
        memcpy(&buf[len], text, n);
        len += n;
        buf[len] = 0;
    }

    void addSplit(const char *text)
    {
        size_t n = strlen(text);
        while (n) {
            startPart();
            const size_t room = REPORT_CHUNK_CAP - len;
            if (n <= room) {
                memcpy(&buf[len], text, n);
                len += n;
                buf[len] = 0;
                return;
            }
            size_t cut = room;
            while (cut > 0 && ((uint8_t)text[cut] & 0xC0) == 0x80)
                cut--; // never split a UTF-8 character
            if (cut == 0)
                cut = room;
            memcpy(&buf[len], text, cut);
            len += cut;
            buf[len] = 0;
            flush();
            text += cut;
            n -= cut;
        }
    }
};

size_t MeshCoreCompanion::formatPathReport(const HeardPath *const *routes, size_t count, const char *sender,
                                           ReportSink sink, void *ctx) const
{
    if (!routes || count == 0 || !sink)
        return 0;

    ReportWriter w{sink, ctx};

    char header[128];
    formatReportHeader(header, sizeof(header), sender, count);
    w.token(header, true);
    uint32_t firstHeardMs = routes[0]->heardMs;

    for (size_t r = 0; r < count; r++) {
        const HeardPath &route = *routes[r];
        char token[128];
        if ((int32_t)(route.heardMs - firstHeardMs) < 0)
            firstHeardMs = route.heardMs; // the text is as old as its first copy

        // The glyph is the hop count: no "N salto(s)" text to repeat it.
        if (route.hopCount == 0) {
            w.token("⚡ directo", true);
        } else {
            if (const char *glyph = hopGlyph(route.hopCount))
                snprintf(token, sizeof(token), "%s", glyph);
            else
                snprintf(token, sizeof(token), "%u saltos", (unsigned)route.hopCount);
            w.token(token, true);

            const size_t stored = route.hashSize ? sizeof(route.path) / route.hashSize : 0;
            const size_t total = route.hopCount < stored ? route.hopCount : stored;
            for (size_t hop = 0; hop < total; hop++) {
                const uint8_t *hash = &route.path[hop * route.hashSize];
                const char *names[PATH_HOP_NAMES_MAX];
                const size_t nameCount = pathHopNames(hash, route.hashSize, names, PATH_HOP_NAMES_MAX);
                char one[96];
                formatHopLabel(names, nameCount, one, sizeof(one));
                if (!one[0])
                    formatHopHex(hash, route.hashSize, one, sizeof(one));
                snprintf(token, sizeof(token), hop ? " › %s" : " %s", one);
                w.token(token);
            }
        }
        formatSignal(token, sizeof(token), route.snr, true, route.rssi);
        w.token(token);
    }
    const bool direct = routes[0]->channel == HeardPath::DIRECT_CHANNEL;
    char where[96];
    formatHeardWhere(where, sizeof(where), direct, direct ? nullptr : channels.getName(routes[0]->channel), routes[0]->channel,
                     false, (millis() - firstHeardMs) / 1000);
    w.token(where, true);
    w.flush();
    return w.parts;
}

void MeshCoreCompanion::injectReportChunk(void *ctx, const char *text, size_t len)
{
    auto *r = static_cast<ReportInject *>(ctx);
    r->self->injectText(r->fromNum, r->to, r->channel, text, r->replyId, r->direct, r->contact, r->hops, r->snr, r->rssi);
    LOG_INFO("MeshCore .path report piece: %u B", (unsigned)len);
}

bool MeshCoreCompanion::handlePathCommand(const meshtastic_MeshPacket *p, bool direct)
{
    if (!p || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;
    if (!direct && !channels.isMeshCoreChannel((ChannelIndex)p->channel))
        return false;

    char text[sizeof(p->decoded.payload.bytes) + 1];
    const size_t len = p->decoded.payload.size < sizeof(p->decoded.payload.bytes) ? p->decoded.payload.size
                                                                                  : sizeof(p->decoded.payload.bytes);
    memcpy(text, p->decoded.payload.bytes, len);
    text[len] = 0;
    trimInPlace(text);
    if (strcasecmp(text, ".path") != 0 && strcasecmp(text, ".ruta") != 0)
        return false;
    // Only a reply is answered: a plain ".path" is an ordinary text and goes out.
    if (!p->decoded.reply_id)
        return false;

    // The text being replied to: every copy heard of it is a route. Our own transmission is the entry they hang from, not one.
    const HeardPath *entry = heardFind(p->decoded.reply_id);

    // Every copy of that text is a separate route: the report lists them all, the shortest first.
    const HeardPath *routes[PATH_MEMORY];
    size_t routeCount = 0;
    if (entry) {
        for (const HeardPath &h : heard)
            if (h.packetId && !h.own && h.frameHash == entry->frameHash && routeCount < PATH_MEMORY)
                routes[routeCount++] = &h;
        for (size_t i = 1; i < routeCount; i++) { // insertion sort: tiny, and stable on equal hop counts
            const HeardPath *key = routes[i];
            size_t j = i;
            while (j > 0 && routes[j - 1]->hopCount > key->hopCount) {
                routes[j] = routes[j - 1];
                j--;
            }
            routes[j] = key;
        }
    }
    const HeardPath *best = routeCount ? routes[0] : nullptr;

    LOG_INFO("MeshCore .path: %u routes, %u MeshCore nodes known", (unsigned)routeCount, (unsigned)nodes.count());

    // Never on air: mark the command delivered and answer it here, in the same channel / chat. A long report goes out
    // as several messages, so the sender must be in place before the first piece is streamed.
    reportDelivery(direct ? p->to : PSEUDO_NODE_NUM, p->id, meshtastic_Routing_Error_NONE);
    const MeshCoreNode *c = direct ? contactByNodeNum(p->to) : nullptr;
    if (direct) {
        if (!c)
            return true; // the contact is gone; the command still must not go out
        ensureSenderNode(c->nodeNum, c->name, true, c->pub, meshtastic_Config_DeviceConfig_Role_CLIENT,
                         MeshCoreRx(best ? best->hopCount : 0, best ? best->snr : 0, best ? best->rssi : 0));
    } else {
        ensurePseudoNode();
    }

    const uint32_t fromNum = c ? c->nodeNum : PSEUDO_NODE_NUM;
    const uint32_t to = direct ? nodeDB->getNodeNum() : NODENUM_BROADCAST;
    const uint8_t channel = direct ? 0 : p->channel;
    const uint32_t replyId = entry ? entry->packetId : p->id;
    const uint8_t hops = best ? best->hopCount : 0;
    const float snr = best ? best->snr : 0;
    const int32_t rssi = best ? best->rssi : 0;

    // A text of ours nobody has re-flooded yet has an entry but no route: that is no data either.
    if (routeCount) {
        ReportInject inject = {this, fromNum, to, replyId, channel, direct, c, hops, snr, rssi};
        formatPathReport(routes, routeCount, pathSenderName(entry->fromNum), injectReportChunk, &inject);
    } else {
        char report[sizeof(p->decoded.payload.bytes) + 1];
        snprintf(report, sizeof(report), "🛰️ *Sin datos*: no he visto la ruta de ese mensaje.");
        sanitizeUtf8(report, sizeof(report));
        injectText(fromNum, to, channel, report, replyId, direct, c, hops, snr, rssi);
    }
    LOG_INFO("MeshCore path command answered locally (direct=%d, routes=%u)", direct, (unsigned)routeCount);
    return true;
}
#endif

void MeshCoreCompanion::noteRecentText(uint32_t packetId, const char *sender)
{
    RecentText &r = recent[recentNext];
    recentNext = (recentNext + 1) % RECENT_TEXTS;
    r.packetId = packetId;
    strncpy(r.sender, sender, sizeof(r.sender) - 1);
    r.sender[sizeof(r.sender) - 1] = 0;
}

const char *MeshCoreCompanion::recentSender(uint32_t packetId) const
{
    for (const RecentText &r : recent)
        if (r.packetId == packetId && r.sender[0])
            return r.sender;
    return nullptr;
}

uint32_t MeshCoreCompanion::replyTarget(const char *text, const char **rest) const
{
    if (rest)
        *rest = text;
    if (text[0] != '@' || text[1] != '[')
        return 0;
    const char *close = strchr(text + 2, ']');
    if (!close || close[1] != ' ' || close == text + 2 || close - text - 2 >= MeshCoreNode::NAME_LEN)
        return 0;

    char name[MeshCoreNode::NAME_LEN];
    memcpy(name, text + 2, close - text - 2);
    name[close - text - 2] = 0;
    // Newest first: the tag names a person, not a message, so the latest text from them is the one answered.
    for (size_t i = 1; i <= RECENT_TEXTS; i++) {
        const RecentText &r = recent[(recentNext + RECENT_TEXTS - i) % RECENT_TEXTS];
        if (r.sender[0] && strcmp(r.sender, name) == 0) {
            if (rest)
                *rest = close + 2;
            return r.packetId;
        }
    }
    return 0;
}

/// Length of name when text starts with it (ASCII case-insensitive) and the name ends there; 0 otherwise.
static size_t mentionMatchLen(const char *text, const char *name)
{
    const size_t n = strlen(name);
    if (n == 0 || strncasecmp(text, name, n) != 0)
        return 0;
    const uint8_t next = (uint8_t)text[n];
    const bool wordChar =
        (next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || next == '_';
    return wordChar ? 0 : n;
}

size_t MeshCoreCompanion::prepareGroupText(const char *text, uint32_t replyId, char *out, size_t cap)
{
    if (!text || !out || cap == 0)
        return 0;

    size_t o = 0;
    auto put = [&](const char *s, size_t n) {
        const size_t room = cap - 1 - o;
        const size_t k = n < room ? n : room;
        memcpy(&out[o], s, k);
        o += k;
    };

    // MeshCore shows a reply as "@[name] " in front of the text.
    const char *replied = replyId ? recentSender(replyId) : nullptr;
    if (replied) {
        put("@[", 2);
        put(replied, strlen(replied));
        put("] ", 2);
    }

    // MeshCore marks a mention as "@[name]"; a bare "@name" is just text there. Names we know are matched whole (longest
    // first, so "Bob Builder" beats "Bob"); an unknown one is taken as a single word.
    for (const char *p = text; *p;) {
        const bool wordStart = p == text || p[-1] == ' ' || p[-1] == '\n';
        if (*p == '@' && p[1] != '[' && p[1] != ' ' && p[1] != 0 && wordStart) {
            char best[MeshCoreNode::NAME_LEN] = {};
            size_t bestLen = 0;
            auto consider = [&](const char *name, size_t nameLen) {
                char known[MeshCoreNode::NAME_LEN];
                if (nameLen == 0 || nameLen >= sizeof(known))
                    return;
                memcpy(known, name, nameLen);
                known[nameLen] = 0;
                const size_t m = mentionMatchLen(p + 1, known);
                if (m > bestLen) {
                    bestLen = m;
                    memcpy(best, known, nameLen + 1);
                }
            };
            nodes.forEach([&](MeshCoreNode &n) {
                if (n.type == MeshCoreNode::CHAT)
                    consider(n.name, strlen(n.name));
            });
            for (const RecentText &r : recent)
                consider(r.sender, strlen(r.sender));
            // Nodes of MeshCore people kept in the node DB (shown as "name (MC)") outlive the recent-senders list.
            for (size_t n = 0; nodeDB && n < nodeDB->getNumMeshNodes(); n++) {
                const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNodeByIndex(n);
                if ((lite->num >> 24) != 0x4D)
                    continue;
                const size_t len = strlen(lite->long_name);
                if (len > 5 && strcmp(&lite->long_name[len - 5], " (MC)") == 0)
                    consider(lite->long_name, len - 5);
            }
            if (bestLen > 0) {
                put("@[", 2);
                put(best, strlen(best));
                put("]", 1);
                p += 1 + bestLen;
                continue;
            }
            // Unknown: one word, without the punctuation that ends a sentence.
            size_t word = 0;
            while (p[1 + word] && p[1 + word] != ' ' && p[1 + word] != '\n')
                word++;
            while (word > 0 && strchr(".,;:!?)", p[word]))
                word--;
            if (word > 0) {
                put("@[", 2);
                put(p + 1, word);
                put("]", 1);
                p += 1 + word;
                continue;
            }
        }
        put(p, 1);
        p++;
    }
    LOG_DEBUG("MeshCore text prepared: reply=%u known=%d", (unsigned)replyId, replied != nullptr);
    out[o] = 0;
    return o;
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
    if (!MeshCoreRepeater::parse(raw, len, &info))
        return;

    switch (info.payload_type) {
    case 0x05: // GRP_TXT
        handleGroupText(raw, len, info, snr, rssi);
        break;
    case 0x04: // ADVERT
        handleAdvert(raw, len, info, snr, rssi);
        break;
    case 0x02: // TXT_MSG
        handleDirectText(raw, len, info, snr, rssi);
        break;
    case 0x03: // ACK
        handleAck(raw, len, info);
        break;
    case 0x08: // PATH: how MeshCore answers a flood, with the ACK inside
        handlePathAck(raw, len, info);
        break;
    default:
        break;
    }
}

void MeshCoreCompanion::handleGroupText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                        int32_t rssi)
{
    rxSeen++;
    if (info.header_len >= len || info.payload_len < 1 + 2 + 16) {
        rxIgnored++;
        return;
    }
    const uint32_t hash = MeshCoreRepeater::hashFrame(raw, len, info);
    // A repeater re-flooding a text of ours (it carries at least its own hash in the path) is the proof it left the room.
    if (info.path_hash_count > 0)
        settleEcho(hash);
    if (wasRecentlySeen(hash)) {
        rxDup++;
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        // Another copy of the same text: record its route (or refresh it) for ".path".
        noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type, snr, rssi);
#endif
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

        // Texts read "name: body": the sender gets its own node and the client shows the body alone. Anything
        // without a prefix falls back to the generic "MeshCore" node.
        char sender[sizeof(((meshtastic_User *)nullptr)->long_name)];
        const char *body = text;
        uint32_t fromNum = PSEUDO_NODE_NUM;
        if (splitSender(text, sender, sizeof(sender), &body)) {
            // Someone we can message shows up as that one node in channels too, instead of a second, unmessagable one.
            // The sender is announced before the text, and both wait in the router queue in that order: the client has the
            // name when the text arrives, with or without an advert from them.
            const MeshCoreRx rx(info.path_hash_count, snr, rssi);
            MeshCoreNode *contact = nodes.findUniqueByName(sender, MeshCoreNode::CHAT);
            if (contact) {
                fromNum = contact->nodeNum;
                ensureSenderNode(fromNum, contact->name, true, contact->pub, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
            } else {
                fromNum = senderNodeNum(sender);
                // Two contacts sharing this name leave it ambiguous: the number may belong to one of them, who stays as is.
                if (!nodes.findByNodeNum(fromNum))
                    ensureSenderNode(fromNum, sender, false, nullptr, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
            }
        } else {
            ensurePseudoNode(); // a node-DB reset or eviction may have removed it since boot
        }

        // Looked up before this text is noted, so a text can never quote itself.
        const uint32_t replyId = replyTarget(body, nullptr);
        const uint32_t packetId =
            injectText(fromNum, NODENUM_BROADCAST, i, body, replyId, false, nullptr, info.path_hash_count, snr, rssi);
        if (!packetId)
            return;

        rememberFrame(hash);
        rxDecoded++;
        if (fromNum != PSEUDO_NODE_NUM)
            noteRecentText(packetId, sender);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        noteHeardRoute(packetId, hash, fromNum, i, pathOf(raw, info), info.path_hash_size, info.path_hash_count,
                       info.route_type, snr, rssi);
#endif
        LOG_INFO("MeshCore group text ch=%u ts=%u len=%u", (unsigned)i, timestamp, (unsigned)strlen(body));
        return;
    }

    rxIgnored++;
}

void MeshCoreCompanion::setIdentity(const uint8_t seed[32], const uint8_t pub[32])
{
    memcpy(identitySeed, seed, sizeof(identitySeed));
    memcpy(identityPub, pub, sizeof(identityPub));
    identityReady = true;
    aliasReady = false; // a new seed means a new alias; the radio installs the derived one on its next service tick
    secrets.clear(); // they were derived for another identity
}

void MeshCoreCompanion::setRelayAliasPub(const uint8_t pub[32])
{
    memcpy(aliasPub, pub, sizeof(aliasPub));
    aliasReady = true;
}

const uint8_t *MeshCoreCompanion::secretFor(const MeshCoreNode &n)
{
    if (!identityReady)
        return nullptr;
    if (const uint8_t *cached = secrets.find(n.pub))
        return cached;
    uint8_t secret[32];
    return sharedSecret(identitySeed, n.pub, secret) ? secrets.put(n.pub, secret) : nullptr;
}

MeshCoreNode *MeshCoreCompanion::contactByPub(const uint8_t pub[32])
{
    MeshCoreNode *n = nodes.find(pub);
    return n && n->type == MeshCoreNode::CHAT ? n : nullptr;
}

MeshCoreNode *MeshCoreCompanion::contactByNodeNum(uint32_t nodeNum)
{
    MeshCoreNode *n = nodes.findByNodeNum(nodeNum);
    return n && n->type == MeshCoreNode::CHAT ? n : nullptr;
}

MeshCoreNode *MeshCoreCompanion::learnContact(const uint8_t pub[32], const char *name)
{
    MeshCoreNode *n = nodes.upsert(pub);
    if (n->type != MeshCoreNode::CHAT)
        n->nodeNum = 0; // a repeater turned chat takes the number contacts get, from its name
    n->type = MeshCoreNode::CHAT;
    if (name)
        n->setName(name);
    if (n->nodeNum == 0) // a contact keeps its number across renames, as the client keeps its conversation
        nodes.assignNodeNum(n, MeshCoreNodeStore<1>::nodeNumForName(n->name), PSEUDO_NODE_NUM);
    return n;
}

bool MeshCoreCompanion::nextReplayNode(size_t &cursor, MeshCoreNode &out) const
{
    for (; cursor < nodes.capacity(); cursor++) {
        const MeshCoreNode *n = nodes.at(cursor);
        if (!n || n->nodeNum == 0 || (n->type != MeshCoreNode::CHAT && n->type != MeshCoreNode::REPEATER))
            continue;
        if (nodeDB && nodeDB->getMeshNode(n->nodeNum))
            continue; // the sync already sent it
        // Two equal copies: a key torn by an advert landing mid-copy would leave the client warning of a changed key.
        MeshCoreNode check;
        bool consistent = false;
        for (int attempt = 0; attempt < 3 && !consistent; attempt++) {
            out = *n;
            check = *n;
            consistent = memcmp(&out, &check, sizeof(out)) == 0 && out.lastMod != 0;
        }
        if (!consistent)
            continue;
        cursor++;
        return true;
    }
    return false;
}

MeshCoreNode *MeshCoreCompanion::learnRepeater(const uint8_t pub[32], const char *name)
{
    MeshCoreNode *n = nodes.upsert(pub);
    if (n->type != MeshCoreNode::REPEATER)
        n->nodeNum = 0; // a chat identity turned repeater takes the number repeaters get, from its key
    n->type = MeshCoreNode::REPEATER;
    n->setName(name);
    if (n->nodeNum == 0)
        nodes.assignNodeNum(n, MeshCoreNodeStore<1>::nodeNumForKey(pub), PSEUDO_NODE_NUM);
    return n;
}

void MeshCoreCompanion::noteAdvert(MeshCoreNode *n, uint32_t advertTime, int32_t latMicro, int32_t lonMicro)
{
    n->lastAdvert = advertTime;
    if (latMicro != 0 || lonMicro != 0) { // 0,0 is "no position", not the Gulf of Guinea
        n->latitudeMicro = latMicro;
        n->longitudeMicro = lonMicro;
    }
}

void MeshCoreCompanion::handleAdvert(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                     int32_t rssi)
{
    // ADVERT payload: public key (32), timestamp (4), signature (64), then app data: a flags byte (node type in the
    // low nibble, 0x10 position, 0x20/0x40 reserved features, 0x80 name) followed by those fields in that order.
    constexpr size_t appDataAt = 32 + 4 + 64;
    if (!identityReady || info.header_len >= len || info.payload_len <= appDataAt)
        return;

    const uint8_t *payload = &raw[info.header_len];
    if (memcmp(payload, identityPub, 32) == 0 || (aliasReady && memcmp(payload, aliasPub, 32) == 0))
        return; // our own advert coming back, either identity

    const uint8_t flags = payload[appDataAt];
    const uint8_t type = flags & 0x0F;
    const bool repeater = type == MeshCoreRepeater::ADV_TYPE_REPEATER;
    if ((type != MeshCoreRepeater::ADV_TYPE_CHAT && !repeater) || !(flags & 0x80))
        return; // only named chat nodes (to message) and repeaters (to list): rooms and sensors are not shown

    size_t at = appDataAt + 1;
    int32_t latMicro = 0, lonMicro = 0;
    if (flags & 0x10) {
        if (at + 8 > info.payload_len)
            return;
        latMicro = (int32_t)readLe32(&payload[at]);
        lonMicro = (int32_t)readLe32(&payload[at + 4]);
        at += 8;
    }
    if (flags & 0x20)
        at += 2;
    if (flags & 0x40)
        at += 2;
    if (at >= info.payload_len)
        return;

    char name[MeshCoreNode::NAME_LEN];
    size_t nameLen = info.payload_len - at;
    if (nameLen > sizeof(name) - 1)
        nameLen = sizeof(name) - 1;
    memcpy(name, &payload[at], nameLen);
    name[nameLen] = 0;
    sanitizeUtf8(name, sizeof(name));
    if (name[0] == 0)
        return;

    const MeshCoreRx rx(info.path_hash_count, snr, rssi);
    const uint32_t advertTime = readLe32(&payload[32]);
    if (repeater) {
        // A repeater is not someone to message: numbered from its key, and announced unmessagable.
        const MeshCoreNode *known = nodes.find(payload);
        const bool trusted =
            known && known->type == MeshCoreNode::REPEATER && strncmp(known->name, name, MeshCoreNode::NAME_LEN - 1) == 0;
        if (!trusted && !MeshCoreRepeater::verifyAdvert(payload, info.payload_len))
            return;
        MeshCoreNode *n = learnRepeater(payload, name);
        noteAdvert(n, advertTime, latMicro, lonMicro);
        ensureSenderNode(n->nodeNum, n->name, false, n->pub, meshtastic_Config_DeviceConfig_Role_REPEATER, rx);
        updatePosition(n->nodeNum, latMicro, lonMicro);
        return;
    }

    // A contact we already trust under the same name only needs its LRU stamp refreshed: skip the signature check.
    MeshCoreNode *known = contactByPub(payload);
    if (known && strncmp(known->name, name, MeshCoreNode::NAME_LEN - 1) == 0) {
        nodes.touch(known);
        noteAdvert(known, advertTime, latMicro, lonMicro);
        // Brings a node made before keys were set up along.
        ensureSenderNode(known->nodeNum, known->name, true, known->pub, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
        updatePosition(known->nodeNum, latMicro, lonMicro);
        return;
    }
    if (!MeshCoreRepeater::verifyAdvert(payload, info.payload_len))
        return;

    MeshCoreNode *c = learnContact(payload, name);
    noteAdvert(c, advertTime, latMicro, lonMicro);
    if (!known)
        contactsLearned++;
    ensureSenderNode(c->nodeNum, c->name, true, c->pub, meshtastic_Config_DeviceConfig_Role_CLIENT, rx);
    retireGroupNode(c->name);
    retireKeyedNode(c->pub, c->nodeNum);
    updatePosition(c->nodeNum, latMicro, lonMicro);
}

void MeshCoreCompanion::retireGroupNode(const char *name)
{
    // The node made for this name before it was a contact: ours (no hardware model, unmessagable), not a favourite.
    const uint32_t num = senderNodeNum(name);
    if (nodes.findByNodeNum(num))
        return; // the contact took over this very node: nothing left to retire
    const meshtastic_NodeInfoLite *lite = nodeDB ? nodeDB->getMeshNode(num) : nullptr;
    if (lite && nodeInfoLiteHasUser(lite) && lite->hw_model == meshtastic_HardwareModel_UNSET &&
        nodeInfoLiteIsUnmessagable(lite) && !nodeInfoLiteIsFavorite(lite))
        nodeDB->removeNodeByNum(num);
}

void MeshCoreCompanion::retireKeyedNode(const uint8_t pub[32], uint32_t keepNum)
{
    // Earlier builds numbered a contact's node from its public key; that node carries the same key and no hardware model.
    const uint32_t num = MeshCoreNodeStore<1>::nodeNumForKey(pub);
    if (num == keepNum || !nodeDB)
        return;
    const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNode(num);
    if (lite && nodeInfoLiteHasUser(lite) && lite->hw_model == meshtastic_HardwareModel_UNSET && !nodeInfoLiteIsFavorite(lite) &&
        lite->public_key.size == 32 && memcmp(lite->public_key.bytes, pub, 32) == 0)
        nodeDB->removeNodeByNum(num);
}

uint32_t MeshCoreCompanion::injectText(uint32_t fromNum, uint32_t to, uint8_t channel, const char *text, uint32_t replyId,
                                       bool direct, const MeshCoreNode *contact, uint8_t pathHashCount, float snr,
                                       int32_t rssi)
{
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return 0;

    p->from = fromNum;
    p->to = to;
    p->id = generatePacketId();
    p->channel = channel;
    if (direct && contact && !contact->usesPlainDmChat()) {
        // A client files a direct message under PKI or plain depending on how it reached it, and keeps one chat for each:
        // a reply marked unlike the chat the user writes from opens a second one with the same node.
        p->pki_encrypted = true;
        p->public_key.size = 32;
        memcpy(p->public_key.bytes, contact->pub, 32);
    }
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    // A quoted reply carries the answered packet id, and keeps its "@[name] " tag in the body the way MeshCore shows it.
    if (replyId)
        p->decoded.reply_id = replyId;
    const size_t textLen = strlen(text);
    p->decoded.payload.size = textLen <= sizeof(p->decoded.payload.bytes) ? textLen : sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, p->decoded.payload.size);
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    // Hops are the repeaters the frame crossed (its path). A hop_limit below hop_start also keeps this
    // synthetic packet's relay gate closed, and a next hop that is not us closes it for direct frames.
    hopFields(pathHashCount, &p->hop_start, &p->hop_limit);
    if (!direct && nodeDB) {
        const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
        p->next_hop = (uint8_t)(ourRelay + 1);
        if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
            p->next_hop = 1;
    }
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
    if (pathHashCount == 0) {
        p->rx_snr = snr;
        p->has_rx_rssi = true;
        p->rx_rssi = rssi;
    }
    const uint32_t id = p->id;
    router->enqueueReceivedMessage(p);
    return id;
}

uint32_t MeshCoreCompanion::injectDirectText(const MeshCoreNode &c, const char *text,
                                             const MeshCoreRepeater::FrameInfo &info, float snr, int32_t rssi)
{
    // A node-DB reset or eviction may have removed it.
    ensureSenderNode(c.nodeNum, c.name, true, c.pub, meshtastic_Config_DeviceConfig_Role_CLIENT,
                     MeshCoreRx(info.path_hash_count, snr, rssi));
    return injectText(c.nodeNum, nodeDB->getNodeNum(), 0, text, 0, true, &c, info.path_hash_count, snr, rssi);
}

void MeshCoreCompanion::queueAck(uint32_t hash)
{
    uint8_t frame[8];
    const size_t n = encodeAck(hash, frame, sizeof(frame));
    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (n == 0 || !iface || !iface->queueMeshCoreFrame(frame, n))
        LOG_WARN("MeshCore ACK could not be queued");
}

void MeshCoreCompanion::handleDirectText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr,
                                         int32_t rssi)
{
    // TXT_MSG payload: dest hash (1), source hash (1), MAC (2), ciphertext.
    if (!identityReady || info.header_len >= len || info.payload_len < 1 + 1 + 2 + 16)
        return;
    const uint32_t hash = MeshCoreRepeater::hashFrame(raw, len, info);
    // A repeater re-flooding our own direct message: the last hop on the path is the repeater that carried it.
    if (info.path_hash_count > 0)
        settleRelay(hash, raw[info.header_len - info.path_hash_size]);
    const uint8_t *payload = &raw[info.header_len];
    if (payload[0] != identityPub[0]) {
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        // A re-flood of a direct message we sent is still a route to it, even though it is not for us.
        if (info.path_hash_count > 0)
            noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type,
                           snr, rssi);
#endif
        return; // not for us: the repeater relays it, we cannot read it anyway
    }

    if (wasRecentlySeen(hash)) {
        rxDup++;
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        noteHeardRoute(0, hash, 0, 0, pathOf(raw, info), info.path_hash_size, info.path_hash_count, info.route_type, snr, rssi);
#endif
        return;
    }

    // The source hash is one byte, so several contacts can share it: the MAC tells which one wrote it.
    MeshCoreNode *candidates[HASH_CANDIDATES_MAX];
    const size_t n = nodes.findByPrefix(&payload[1], 1, candidates, HASH_CANDIDATES_MAX);
    for (size_t i = 0; i < n && i < HASH_CANDIDATES_MAX; i++) {
        MeshCoreNode *c = candidates[i];
        char text[MAX_TEXT_LEN + 1];
        uint32_t timestamp = 0;
        uint8_t flags = 0;
        const uint8_t *secret = c->type == MeshCoreNode::CHAT ? secretFor(*c) : nullptr;
        if (!secret || !decodeDirectText(secret, raw, len, info, text, sizeof(text), &timestamp, &flags))
            continue;

        rememberFrame(hash);
        nodes.touch(c);
        nodes.noteDirectMessage(c);
        dmRx++;
        LOG_INFO("MeshCore direct text from %s ts=%u len=%u", c->name, timestamp, (unsigned)strlen(text));
        const uint32_t packetId = injectDirectText(*c, text, info, snr, rssi);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
        if (packetId)
            noteHeardRoute(packetId, hash, c->nodeNum, HeardPath::DIRECT_CHANNEL, pathOf(raw, info), info.path_hash_size,
                           info.path_hash_count,
                           info.route_type, snr, rssi);
#endif
        queueAck(ackHash(timestamp, flags, text, c->pub));
        return;
    }
}

void MeshCoreCompanion::reportDelivery(uint32_t fromNum, uint32_t packetId, meshtastic_Routing_Error err, int relayNode)
{
    meshtastic_Routing r = meshtastic_Routing_init_default;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = err;

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;
    p->from = fromNum;
    p->to = nodeDB->getNodeNum();
    p->id = generatePacketId();
    p->channel = 0;
    p->priority = meshtastic_MeshPacket_Priority_ACK;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.request_id = packetId;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Routing_msg, &r);
    // The router drops a packet whose hop_start is missing, so the delivery report carries real hop fields (direct).
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    hopFields(0, &p->hop_start, &p->hop_limit);
    if (relayNode >= 0)
        p->relay_node = (uint8_t)relayNode;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    router->enqueueReceivedMessage(p);
}

MeshCoreCompanion::PendingAck *MeshCoreCompanion::addPending(uint32_t ackHash, uint32_t packetId, uint32_t contactNum, bool group)
{
    PendingAck *slot = nullptr;
    for (auto &e : pending) {
        if (!e.used) {
            slot = &e;
            break;
        }
        if (!slot || (uint32_t)(millis() - e.sentMs) > (uint32_t)(millis() - slot->sentMs))
            slot = &e; // the oldest one gives way
    }
    if (slot->used && !slot->relayed)
        reportDelivery(slot->contactNum, slot->packetId, meshtastic_Routing_Error_MAX_RETRANSMIT);

    slot->ackHash = ackHash;
    slot->frameHash = 0;
    slot->packetId = packetId;
    slot->contactNum = contactNum;
    slot->sentMs = millis();
    slot->group = group;
    slot->relayed = false;
    slot->used = true;
    return slot;
}

bool MeshCoreCompanion::trackGroupText(const uint8_t *frame, size_t len, uint32_t packetId, uint8_t channel)
{
    MeshCoreRepeater::FrameInfo info;
    if (!frame || !MeshCoreRepeater::parse(frame, len, &info))
        return false;
    const uint32_t hash = MeshCoreRepeater::hashFrame(frame, len, info);
    addPending(hash, packetId, PSEUDO_NODE_NUM, true);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    noteSentText(packetId, hash, nodeDB->getNodeNum(), channel);
#endif
    return true;
}

void MeshCoreCompanion::settleEcho(uint32_t hash)
{
    for (auto &e : pending) {
        if (e.used && e.group && e.ackHash == hash) {
            e.used = false;
            groupEchoed++;
            reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_NONE);
            return;
        }
    }
}

bool MeshCoreCompanion::sendDirectText(uint32_t nodeNum, uint32_t packetId, const char *text, uint32_t now, RadioInterface *iface,
                                       bool clientPki)
{
    MeshCoreNode *c = contactByNodeNum(nodeNum);
    const uint8_t *secret = c && iface && now != 0 && text ? secretFor(*c) : nullptr;
    if (!secret)
        return false;

    // The ACK hashes the text as the peer will read it, so cut it the way encodeDirectText does.
    char cut[MAX_TEXT_LEN + 1];
    const size_t cutLen = utf8PrefixLen(text, MAX_TEXT_LEN);
    memcpy(cut, text, cutLen);
    cut[cutLen] = 0;

    const uint32_t timestamp = nextTxTimestamp(now);
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t frameLen =
        encodeDirectText(secret, c->pub[0], identityPub[0], timestamp, TXT_FLAGS_PLAIN, cut, frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    if (frameLen == 0 || !MeshCoreRepeater::parse(frame, frameLen, &info) || !iface->queueMeshCoreFrame(frame, frameLen))
        return false;

    PendingAck *slot = addPending(ackHash(timestamp, TXT_FLAGS_PLAIN, cut, identityPub), packetId, nodeNum);
    slot->frameHash = MeshCoreRepeater::hashFrame(frame, frameLen, info);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    noteSentText(packetId, slot->frameHash, nodeDB->getNodeNum(), HeardPath::DIRECT_CHANNEL);
#endif
    nodes.touch(c);
    nodes.noteDirectMessage(c);
    c->flags = clientPki ? (c->flags & ~MeshCoreNode::PLAIN_DM_CHAT) : (c->flags | MeshCoreNode::PLAIN_DM_CHAT);
    dmTx++;
    return true;
}

void MeshCoreCompanion::settleAck(uint32_t hash)
{
    for (auto &e : pending) {
        if (e.used && !e.group && e.ackHash == hash) {
            e.used = false;
            dmAcked++;
            reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_NONE);
            return;
        }
    }
}

void MeshCoreCompanion::settleRelay(uint32_t hash, uint8_t relayHop)
{
    for (auto &e : pending) {
        if (e.used && !e.group && !e.relayed && e.frameHash == hash) {
            e.relayed = true;
            reportDelivery(nodeDB->getNodeNum(), e.packetId, meshtastic_Routing_Error_NONE, relayHop);
            return;
        }
    }
}

void MeshCoreCompanion::handlePathAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    // PATH payload: dest hash (1), source hash (1), MAC (2), ciphertext. Only ours, from a contact we hold a secret for.
    if (!identityReady || info.header_len >= len || info.payload_len < 1 + 1 + 2 + 16)
        return;
    const uint8_t *payload = &raw[info.header_len];
    if (payload[0] != identityPub[0])
        return;

    MeshCoreNode *candidates[HASH_CANDIDATES_MAX];
    const size_t n = nodes.findByPrefix(&payload[1], 1, candidates, HASH_CANDIDATES_MAX);
    for (size_t i = 0; i < n && i < HASH_CANDIDATES_MAX; i++) {
        uint32_t hash = 0;
        const uint8_t *secret = candidates[i]->type == MeshCoreNode::CHAT ? secretFor(*candidates[i]) : nullptr;
        if (secret && decodePathAck(secret, raw, len, info, &hash)) {
            settleAck(hash);
            return;
        }
    }
}

void MeshCoreCompanion::handleAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    if (info.payload_len != 4 || info.header_len + 4 > len)
        return;
    settleAck(readLe32(&raw[info.header_len]));
}

void MeshCoreCompanion::service()
{
    for (auto &e : pending) {
        if (e.used && !Throttle::isWithinTimespanMs(e.sentMs, ACK_TIMEOUT_MS)) {
            e.used = false;
            (e.group ? groupTimedOut : dmTimedOut)++;
            // A relayed message stops here without a failure, as a Meshtastic one does once its relay has ACKed it.
            if (!e.relayed)
                reportDelivery(e.contactNum, e.packetId, meshtastic_Routing_Error_MAX_RETRANSMIT);
        }
    }
}

void MeshCoreCompanion::syncWithNodeDB()
{
    nodes.forEach([](MeshCoreNode &n) {
        meshtastic_NodeInfoLite *lite = nodeDB ? nodeDB->getMeshNode(n.nodeNum) : nullptr;
        if (!lite || lite->hw_model != meshtastic_HardwareModel_UNSET)
            return; // evicted (the store keeps what it had), or a Meshtastic node sharing the number
        nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_IS_MESHCORE_MASK, true);
        // The client stars a node in the node DB; a contact evicted from there keeps the flag it had.
        if (n.type == MeshCoreNode::CHAT)
            n.flags = nodeInfoLiteIsFavorite(lite) ? (n.flags | MeshCoreNode::FAVORITE) : (n.flags & ~MeshCoreNode::FAVORITE);
    });
}

size_t MeshCoreCompanion::exportContacts(uint8_t *buf, size_t cap)
{
    syncWithNodeDB();
    return MeshCoreNodeFile::save(nodes, buf, cap);
}

size_t MeshCoreCompanion::importContacts(const uint8_t *buf, size_t len)
{
    const size_t loaded = MeshCoreNodeFile::load(nodes, buf, len);
    if (identityReady) {
        nodes.remove(identityPub);
        if (aliasReady)
            nodes.remove(aliasPub);
    }
    nodes.forEach([&](MeshCoreNode &n) {
        if (n.type != MeshCoreNode::CHAT)
            return;
        if (n.nodeNum == 0) // migrated from the contacts file, which kept no numbers
            nodes.assignNodeNum(&n, MeshCoreNodeStore<1>::nodeNumForName(n.name), PSEUDO_NODE_NUM);
        ensureSenderNode(n.nodeNum, n.name, true, n.pub);
        retireGroupNode(n.name);
        retireKeyedNode(n.pub, n.nodeNum);
    });
    return loaded;
}

uint32_t MeshCoreCompanion::contactsFingerprint()
{
    syncWithNodeDB();
    return MeshCoreNodeFile::fingerprint(nodes);
}

void MeshCoreCompanion::noteTransmitted(const uint8_t *raw, size_t len)
{
    MeshCoreRepeater::FrameInfo info;
    if (raw && MeshCoreRepeater::parse(raw, len, &info))
        rememberFrame(MeshCoreRepeater::hashFrame(raw, len, info));
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
