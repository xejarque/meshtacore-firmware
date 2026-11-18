// SPDX-License-Identifier: AGPL-3.0-only
//
// The MeshCore wire codec: channel and direct texts, path ACKs and plain ACKs, sealed with AES-128 and an HMAC
// exactly as MeshCore does, and the ECDH secret a direct conversation is sealed with.

#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#ifdef CRYPTO_AES_NO_DECRYPT
#error "MeshCore needs AES decryption: add -UCRYPTO_AES_NO_DECRYPT to this env (the nRF52 base defines it)"
#endif

#include "Channels.h"
#include "MeshCoreProtocol.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "PingReport.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <AES.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <SHA512.h>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "MeshCoreCompanionWire.h"

#if !(MESHTASTIC_EXCLUDE_XEDDSA)
#include "XEdDSA.h" // fe_* field arithmetic for the Ed25519 -> X25519 public key map
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

    out[0] = MC_HEADER(MC_PAYLOAD_TYPE_GRP_TXT, MC_ROUTE_FLOOD);
    out[1] = MeshCoreRepeater::ownFloodPathLenByte(); // zero hops, hashes of the configured size
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
    if (keySize == 0 || !frame || info.payload_type != MC_PAYLOAD_TYPE_GRP_TXT || info.header_len >= len)
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

    out[0] = MC_HEADER(payloadType, MC_ROUTE_FLOOD);
    out[1] = MeshCoreRepeater::ownFloodPathLenByte(); // zero hops, hashes of the configured size
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

    return sealDatagram(secret, MC_PAYLOAD_TYPE_TXT_MSG, destHash, srcHash, plain, 5 + textLen, out, outCap);
}

bool MeshCoreCompanion::decodeDirectText(const uint8_t secret[32], const uint8_t *frame, size_t len,
                                         const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                         uint32_t *timestampOut, uint8_t *flagsOut)
{
    uint8_t plain[MAX_DIRECT_CIPHER_LEN];
    size_t plainLen = 0;
    if (!openDatagram(secret, MC_PAYLOAD_TYPE_TXT_MSG, frame, len, info, plain, &plainLen) || plainLen < 5 || (plain[4] >> 2) != TXT_FLAGS_PLAIN)
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
    plain[1] = MC_PATH_EXTRA_ACK;
    writeLe32(&plain[2], hash);
    return sealDatagram(secret, MC_PAYLOAD_TYPE_PATH, destHash, srcHash, plain, sizeof(plain), out, outCap);
}

bool MeshCoreCompanion::decodePathAck(const uint8_t secret[32], const uint8_t *frame, size_t len,
                                      const MeshCoreRepeater::FrameInfo &info, uint32_t *hashOut)
{
    uint8_t plain[MAX_CIPHER_LEN];
    size_t plainLen = 0;
    if (!hashOut || !openDatagram(secret, MC_PAYLOAD_TYPE_PATH, frame, len, info, plain, &plainLen))
        return false;

    // path_len (hash size in the top two bits, hop count in the rest), the path, then the extra block.
    size_t at = 0;
    const uint8_t pathLenByte = plain[at++];
    at += (size_t)((pathLenByte >> 6) + 1) * (pathLenByte & 0x3F);
    if (at >= plainLen || (plain[at++] & 0x0F) != MC_PATH_EXTRA_ACK || plainLen - at < 4) // the extra must be an ACK
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
    out[0] = MC_HEADER(MC_PAYLOAD_TYPE_ACK, MC_ROUTE_FLOOD);
    out[1] = MeshCoreRepeater::ownFloodPathLenByte();
    writeLe32(&out[2], hash);
    return 6;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
