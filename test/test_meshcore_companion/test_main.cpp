// Unit tests for src/mesh/MeshCoreCompanion.{h,cpp} - MeshCore GRP_TXT encode/decode.
//
// The native test build compiles src/ with the MeshCore flags off, so those translation units are
// empty there. This suite defines the flags and includes the .cpp files directly, mirroring
// test_meshcore_repeater.
#define MESHTASTIC_ENABLE_MESHCORE_SNIFFER 1
#define MESHTASTIC_ENABLE_MESHCORE_RELAY 0
#define MESHTASTIC_ENABLE_MESHCORE_COMPANION 1
#define MESHTASTIC_MESHCORE_PATH_COMMAND 1
#include "mesh/MeshCoreCompanion.cpp"
#include "mesh/MeshCoreRepeater.cpp"

#include "Arduino.h"
#include "TestUtil.h"
#include "TypeConversions.h"
#include <unity.h>

// The three seeded MeshCore channel keys (16-byte PSKs).
static const uint8_t kPublicKey[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
static const uint8_t kTestKey[16] = {0x9c, 0xd8, 0xfc, 0xf2, 0x2a, 0x47, 0x33, 0x3b,
                                     0x59, 0x1d, 0x96, 0xa2, 0xb8, 0x48, 0xb7, 0x3f};
static const uint8_t kBotKey[16] = {0xeb, 0x50, 0xa1, 0xbc, 0xb3, 0xe4, 0xe5, 0xd7,
                                    0xbf, 0x69, 0xa5, 0x7c, 0x9d, 0xad, 0xa2, 0x11};

// GRP_TXT for Public, timestamp 0x01020304, sender "Alice", text "hello".
static const uint8_t kExpectedFrame[] = {
    0x15, 0x40, 0x11, 0x80, 0x15, 0x2b, 0xe0, 0x79, 0x9d, 0x52, 0xfb, 0xeb, 0x96, 0xb1, 0x5f, 0xcc, 0xf0, 0xe9, 0xfa,
    0x0d, 0x91, 0xa1, 0x0f, 0x37, 0xa2, 0x24, 0xf5, 0x06, 0x4d, 0x77, 0x23, 0x54, 0x63, 0x3c, 0x4c, 0x1f, 0x22,
};

void setUp(void)
{
    Time::setTestMillis(1000);
}

void tearDown(void)
{
    Time::useRealClock();
}

void test_channel_hashes(void)
{
    uint8_t hash = 0;
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(kPublicKey, sizeof(kPublicKey), &hash));
    TEST_ASSERT_EQUAL_HEX8(0x11, hash);
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(kTestKey, sizeof(kTestKey), &hash));
    TEST_ASSERT_EQUAL_HEX8(0xd9, hash);
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(kBotKey, sizeof(kBotKey), &hash));
    TEST_ASSERT_EQUAL_HEX8(0xca, hash);
}

// Pins dynamic MeshCore PSK derivation when using Meshtastic default PSK (AQ== or empty):
// MC:Public yields the well-known key, MC:#test / MC:#bot derive from their hashtag room names,
// and custom 16-byte raw keys or 32-char hex ASCII keys are preserved.
void test_meshcore_channel_key_derivation(void)
{
    meshtastic_Channel prev = channels.getByIndex(1);
    meshtastic_Channel &ch = channels.getByIndex(1);
    ch = meshtastic_Channel();
    ch.has_settings = true;
    ch.role = meshtastic_Channel_Role_SECONDARY;

    // Default PSK (AQ==, size=1, byte=1) on MC:Public -> returns kPublicKey
    strncpy(ch.settings.name, "MC:Public", sizeof(ch.settings.name) - 1);
    ch.settings.psk.size = 1;
    ch.settings.psk.bytes[0] = 1;
    TEST_ASSERT_TRUE(channels.isMeshCoreChannel(1));
    uint8_t key[32] = {};
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kPublicKey, key, 16);

    // Default PSK (AQ==) on MC:#test -> derives kTestKey
    strncpy(ch.settings.name, "MC:#test", sizeof(ch.settings.name) - 1);
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kTestKey, key, 16);

    // Default PSK (AQ==) on MC:#bot -> derives kBotKey
    strncpy(ch.settings.name, "MC:#bot", sizeof(ch.settings.name) - 1);
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kBotKey, key, 16);

    // Empty PSK (size=0) also derives from room name
    ch.settings.psk.size = 0;
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kBotKey, key, 16);

    // A private room (no '#') without its MeshCore key has no key: MeshCore never derives one from such a name
    strncpy(ch.settings.name, "MC:Family", sizeof(ch.settings.name) - 1);
    ch.settings.psk.size = 1;
    ch.settings.psk.bytes[0] = 1;
    TEST_ASSERT_TRUE(channels.isMeshCoreChannel(1));
    TEST_ASSERT_EQUAL_UINT32(0, channels.getMeshCoreKey(1, key));
    ch.settings.psk.size = 0;
    TEST_ASSERT_EQUAL_UINT32(0, channels.getMeshCoreKey(1, key));

    // The same name as a hashtag room derives SHA-256("#Family")
    static const uint8_t kFamilyHashtagKey[16] = {0x2a, 0x4d, 0xf0, 0xb7, 0x4b, 0x66, 0xc9, 0x78,
                                                  0xee, 0x48, 0xc6, 0x17, 0x45, 0xd7, 0xf6, 0x17};
    strncpy(ch.settings.name, "MC:#Family", sizeof(ch.settings.name) - 1);
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFamilyHashtagKey, key, 16);

    // Custom 16-byte raw key
    uint8_t customKey[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    strncpy(ch.settings.name, "MC:Privado", sizeof(ch.settings.name) - 1);
    ch.settings.psk.size = 16;
    memcpy(ch.settings.psk.bytes, customKey, 16);
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(customKey, key, 16);

    // 32-character hex ASCII string in psk
    const char *hexStr = "8b3387e9c5cdea6ac9e5edbaa115cd72";
    ch.settings.psk.size = 32;
    memcpy(ch.settings.psk.bytes, hexStr, 32);
    memset(key, 0, sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(16, channels.getMeshCoreKey(1, key));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kPublicKey, key, 16);

    channels.getByIndex(1) = prev;
}

void test_encode_matches_meshcore_vector(void)
{
    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 0x01020304, "hello", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT(sizeof(kExpectedFrame), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpectedFrame, frame, len);
}

void test_frame_uses_two_byte_path_hashes(void)
{
    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 0x01020304, "hello", frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));
    TEST_ASSERT_EQUAL_UINT8(2, info.path_hash_size);
    TEST_ASSERT_EQUAL_UINT8(0, info.path_hash_count);
}

void test_hop_fields_report_the_path_length(void)
{
    uint8_t start = 0, limit = 0;
    MeshCoreCompanion::hopFields(0, &start, &limit);
    TEST_ASSERT_EQUAL_UINT8(7, start);
    TEST_ASSERT_EQUAL_UINT8(7, limit); // direct: 0 hops, signal is shown

    MeshCoreCompanion::hopFields(3, &start, &limit);
    TEST_ASSERT_EQUAL_UINT8(3, start - limit);

    MeshCoreCompanion::hopFields(40, &start, &limit); // capped at what hop_start can express
    TEST_ASSERT_EQUAL_UINT8(7, start - limit);
}

void test_round_trip(void)
{
    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 0x01020304, "hello", frame, sizeof(frame));
    TEST_ASSERT_GREATER_THAN(0, len);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    char text[161];
    uint32_t timestamp = 0;
    TEST_ASSERT_TRUE(
        MeshCoreCompanion::decodeGroupText(kPublicKey, sizeof(kPublicKey), frame, len, info, text, sizeof(text), &timestamp));
    TEST_ASSERT_EQUAL_UINT32(0x01020304, timestamp);
    TEST_ASSERT_EQUAL_STRING("Alice: hello", text);
}

void test_rejects_wrong_channel(void)
{
    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 0x01020304, "hello", frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    char text[161];
    TEST_ASSERT_FALSE(
        MeshCoreCompanion::decodeGroupText(kTestKey, sizeof(kTestKey), frame, len, info, text, sizeof(text), nullptr));
}

void test_rejects_tampered_mac(void)
{
    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 0x01020304, "hello", frame, sizeof(frame));
    frame[3] ^= 0x01; // first MAC byte

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));
    char text[161];
    TEST_ASSERT_FALSE(
        MeshCoreCompanion::decodeGroupText(kPublicKey, sizeof(kPublicKey), frame, len, info, text, sizeof(text), nullptr));
}

void test_utf8_truncation_keeps_boundary(void)
{
    char body[200];
    memset(body, 'a', 154);
    body[154] = 0;
    strcat(body, "\xC3\xA9"); // é, two bytes
    strcat(body, "z");

    uint8_t frame[255];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Ana", 0x01020304, body, frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    char text[161];
    TEST_ASSERT_TRUE(
        MeshCoreCompanion::decodeGroupText(kPublicKey, sizeof(kPublicKey), frame, len, info, text, sizeof(text), nullptr));
    TEST_ASSERT_EQUAL_UINT(159, strlen(text)); // "Ana: " + 154 ASCII bytes
    TEST_ASSERT_EQUAL_MEMORY("Ana: ", text, 5);
    TEST_ASSERT_EQUAL_CHAR('a', text[158]);
}

void test_zero_padded_32_byte_key_matches_16(void)
{
    uint8_t padded[32] = {};
    memcpy(padded, kPublicKey, sizeof(kPublicKey));

    uint8_t a[255], b[255];
    const size_t aLen = MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 7, "hi", a, sizeof(a));
    const size_t bLen = MeshCoreCompanion::encodeGroupText(padded, sizeof(padded), "Alice", 7, "hi", b, sizeof(b));
    TEST_ASSERT_EQUAL_UINT(aLen, bLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, aLen);

    padded[16] = 1; // a genuine 256-bit secret must hash differently
    uint8_t hash16 = 0, hash32 = 0;
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(kPublicKey, sizeof(kPublicKey), &hash16));
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(padded, sizeof(padded), &hash32));
    TEST_ASSERT_NOT_EQUAL(hash16, hash32);
}

void test_rejects_oversized_ciphertext_with_valid_mac(void)
{
    // 240 bytes of ciphertext with a correct MAC, as a peer holding the well-known Public key could build.
    const size_t cipherLen = 240;
    uint8_t frame[255] = {};
    uint8_t secret[32] = {};
    memcpy(secret, kPublicKey, sizeof(kPublicKey));
    frame[0] = (0x05 << 2) | 0x01;
    frame[1] = 0x00;
    TEST_ASSERT_TRUE(MeshCoreCompanion::channelHash(kPublicKey, sizeof(kPublicKey), &frame[2]));
    SHA256 hmac;
    hmac.resetHMAC(secret, sizeof(secret));
    hmac.update(&frame[5], cipherLen);
    hmac.finalizeHMAC(secret, sizeof(secret), &frame[3], 2);

    // parse() caps the payload at MeshCore's 184 bytes, so hand-build the info decodeGroupText would
    // otherwise never see: the function must defend itself, not rely on its caller.
    const size_t len = 5 + cipherLen;
    MeshCoreRepeater::FrameInfo info;
    info.payload_type = 0x05;
    info.header_len = 2;
    info.payload_len = (uint8_t)(3 + cipherLen);
    char text[161];
    TEST_ASSERT_FALSE(
        MeshCoreCompanion::decodeGroupText(kPublicKey, sizeof(kPublicKey), frame, len, info, text, sizeof(text), nullptr));
}

void test_encode_reports_truncation(void)
{
    char body[200];
    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = 0;

    uint8_t frame[255];
    bool truncated = false;
    TEST_ASSERT_GREATER_THAN(
        0, MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Ana", 1, body, frame, sizeof(frame), &truncated));
    TEST_ASSERT_TRUE(truncated);

    TEST_ASSERT_GREATER_THAN(
        0, MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Ana", 1, "hi", frame, sizeof(frame), &truncated));
    TEST_ASSERT_FALSE(truncated);
}

void test_tx_timestamp_is_strictly_increasing(void)
{
    MeshCoreCompanion companion;
    TEST_ASSERT_EQUAL_UINT32(100, companion.nextTxTimestamp(100));
    TEST_ASSERT_EQUAL_UINT32(101, companion.nextTxTimestamp(100)); // same second
    TEST_ASSERT_EQUAL_UINT32(102, companion.nextTxTimestamp(90));  // clock stepped back
    TEST_ASSERT_EQUAL_UINT32(200, companion.nextTxTimestamp(200));
}

void test_split_sender_separates_name_and_body(void)
{
    char name[40];
    const char *body = nullptr;
    TEST_ASSERT_TRUE(MeshCoreCompanion::splitSender("Xavi: hola: mundo", name, sizeof(name), &body));
    TEST_ASSERT_EQUAL_STRING("Xavi", name);
    TEST_ASSERT_EQUAL_STRING("hola: mundo", body);
}

void test_split_sender_rejects_missing_or_empty_name(void)
{
    char name[40];
    const char *body = nullptr;
    TEST_ASSERT_FALSE(MeshCoreCompanion::splitSender("sin separador", name, sizeof(name), &body));
    TEST_ASSERT_FALSE(MeshCoreCompanion::splitSender(": vacio", name, sizeof(name), &body));
}

void test_split_sender_cuts_long_name_on_utf8_boundary(void)
{
    char name[5];
    const char *body = nullptr;
    TEST_ASSERT_TRUE(MeshCoreCompanion::splitSender("ab\xC3\xA9\xC3\xA9: x", name, sizeof(name), &body)); // abéé
    TEST_ASSERT_EQUAL_STRING("ab\xC3\xA9", name);
}

void test_sender_node_num_is_stable_and_distinct(void)
{
    const uint32_t a = MeshCoreCompanion::senderNodeNum("Alice");
    TEST_ASSERT_EQUAL_HEX32(a, MeshCoreCompanion::senderNodeNum("Alice"));
    TEST_ASSERT_NOT_EQUAL(a, MeshCoreCompanion::senderNodeNum("Bob"));
    TEST_ASSERT_EQUAL_HEX8(0x4D, a >> 24);
    TEST_ASSERT_NOT_EQUAL(MeshCoreCompanion::PSEUDO_NODE_NUM, a);
}

// Two identities: the seed is the Ed25519 private key, the public key is derived from it.
static const uint8_t kSeedA[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static const uint8_t kSeedB[32] = {32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                   16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};

void test_shared_secret_is_symmetric(void)
{
    uint8_t pubA[32], pubB[32], ab[32], ba[32];
    Ed25519::derivePublicKey(pubA, kSeedA);
    Ed25519::derivePublicKey(pubB, kSeedB);

    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, pubB, ab));
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, pubA, ba));
    TEST_ASSERT_EQUAL_MEMORY(ab, ba, 32);

    uint8_t other[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, pubA, other));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(ab, other, 32));
}

void test_direct_text_round_trip(void)
{
    uint8_t pubA[32], pubB[32], secret[32];
    Ed25519::derivePublicKey(pubA, kSeedA);
    Ed25519::derivePublicKey(pubB, kSeedB);
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, pubB, secret));

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len = MeshCoreCompanion::encodeDirectText(secret, pubB[0], pubA[0], 0x01020304, 0, "hola", frame, sizeof(frame));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_HEX8(0x09, frame[0]); // TXT_MSG, flood
    TEST_ASSERT_EQUAL_HEX8(pubB[0], frame[2]);
    TEST_ASSERT_EQUAL_HEX8(pubA[0], frame[3]);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    // The receiver derives the same secret from its own seed and our public key.
    uint8_t rxSecret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, pubA, rxSecret));
    char text[MeshCoreCompanion::MAX_TEXT_LEN + 1];
    uint32_t ts = 0;
    uint8_t flags = 0xFF;
    TEST_ASSERT_TRUE(MeshCoreCompanion::decodeDirectText(rxSecret, frame, len, info, text, sizeof(text), &ts, &flags));
    TEST_ASSERT_EQUAL_STRING("hola", text);
    TEST_ASSERT_EQUAL_HEX32(0x01020304, ts);
    TEST_ASSERT_EQUAL_HEX8(0, flags);
}

void test_direct_text_rejects_wrong_secret_and_tampering(void)
{
    uint8_t pubA[32], pubB[32], secret[32];
    Ed25519::derivePublicKey(pubA, kSeedA);
    Ed25519::derivePublicKey(pubB, kSeedB);
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, pubB, secret));

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len = MeshCoreCompanion::encodeDirectText(secret, pubB[0], pubA[0], 1, 0, "hola", frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    char text[MeshCoreCompanion::MAX_TEXT_LEN + 1];
    uint8_t wrong[32];
    memcpy(wrong, secret, 32);
    wrong[0] ^= 1;
    TEST_ASSERT_FALSE(MeshCoreCompanion::decodeDirectText(wrong, frame, len, info, text, sizeof(text), nullptr, nullptr));

    frame[len - 1] ^= 1;
    TEST_ASSERT_FALSE(MeshCoreCompanion::decodeDirectText(secret, frame, len, info, text, sizeof(text), nullptr, nullptr));
}

void test_ack_hash_depends_on_text_and_sender(void)
{
    uint8_t pubA[32], pubB[32];
    Ed25519::derivePublicKey(pubA, kSeedA);
    Ed25519::derivePublicKey(pubB, kSeedB);

    const uint32_t h = MeshCoreCompanion::ackHash(7, 0, "hola", pubA);
    TEST_ASSERT_EQUAL_HEX32(h, MeshCoreCompanion::ackHash(7, 0, "hola", pubA));
    TEST_ASSERT_NOT_EQUAL(h, MeshCoreCompanion::ackHash(7, 0, "hola!", pubA));
    TEST_ASSERT_NOT_EQUAL(h, MeshCoreCompanion::ackHash(8, 0, "hola", pubA));
    TEST_ASSERT_NOT_EQUAL(h, MeshCoreCompanion::ackHash(7, 0, "hola", pubB));
}

void test_ack_frame_layout(void)
{
    uint8_t frame[8];
    TEST_ASSERT_EQUAL(6, MeshCoreCompanion::encodeAck(0x44332211, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_HEX8(0x0D, frame[0]); // ACK, flood
    TEST_ASSERT_EQUAL_HEX8(MeshCoreRepeater::OWN_FLOOD_PATH_LEN_BYTE, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x11, frame[2]);
    TEST_ASSERT_EQUAL_HEX8(0x44, frame[5]);
    TEST_ASSERT_EQUAL(0, MeshCoreCompanion::encodeAck(1, frame, 5));
}

static void fillPub(uint8_t pub[32], uint8_t first, uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        pub[i] = seed + i;
    pub[0] = first;
}

void test_contacts_keep_their_node_num_across_a_rename(void)
{
    MeshCoreCompanion c;
    uint8_t pub[32];
    fillPub(pub, 0x10, 1);

    MeshCoreNode *a = c.learnContact(pub, "Alice");
    const uint32_t num = a->nodeNum;
    TEST_ASSERT_EQUAL_HEX8(0x4D, num >> 24);

    // The client files the conversation under the node number: a rename must not open a second one.
    MeshCoreNode *again = c.learnContact(pub, "Alicia");
    TEST_ASSERT_EQUAL_PTR(a, again);
    TEST_ASSERT_EQUAL_STRING("Alicia", again->name);
    TEST_ASSERT_EQUAL_HEX32(num, again->nodeNum);
    TEST_ASSERT_TRUE(c.hasContact(num));
}

void test_contacts_never_reuse_the_pseudo_node_num(void)
{
    MeshCoreCompanion c;
    for (int i = 0; i < 32; i++) {
        uint8_t pub[32];
        fillPub(pub, i, i * 7);
        MeshCoreNode *n = c.learnContact(pub, "x"); // one name: every contact after the first must move on
        TEST_ASSERT_NOT_EQUAL(MeshCoreCompanion::PSEUDO_NODE_NUM, n->nodeNum);
        TEST_ASSERT_EQUAL_PTR(n, c.nodes.findByNodeNum(n->nodeNum));
    }
}

void test_contacts_image_round_trip_keeps_keys_names_and_numbers(void)
{
    MeshCoreCompanion writer;
    uint8_t a[32], b[32];
    fillPub(a, 1, 10);
    fillPub(b, 2, 20);
    writer.learnContact(a, "Alice");
    writer.learnContact(b, "Bob");

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t n = writer.exportContacts(image, sizeof(image));
    TEST_ASSERT_EQUAL(MeshCoreNodeFile::HEADER_LEN + 2 * MeshCoreNodeFile::RECORD_LEN, n);

    MeshCoreCompanion reader;
    TEST_ASSERT_EQUAL(2, reader.importContacts(image, n));
    MeshCoreNode *ra = reader.nodes.find(a);
    MeshCoreNode *rb = reader.nodes.find(b);
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL_STRING("Alice", ra->name);
    TEST_ASSERT_EQUAL_STRING("Bob", rb->name);
    TEST_ASSERT_EQUAL_HEX32(writer.nodes.find(a)->nodeNum, ra->nodeNum);
    TEST_ASSERT_TRUE(reader.hasContact(ra->nodeNum));
}

// A flag an earlier build saved and this one dropped (the plain-DM-chat bit) must not survive a load, or it would be written
// back to flash for good and read as a different flag the day its bit is reused.
void test_contacts_file_drops_flag_bits_no_longer_in_use(void)
{
    MeshCoreCompanion writer;
    uint8_t pub[32];
    fillPub(pub, 5, 50);
    writer.learnContact(pub, "Alice")->flags |= MeshCoreNode::FAVORITE;

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t n = writer.exportContacts(image, sizeof(image));
    constexpr size_t FLAGS_AT = MeshCoreNodeFile::HEADER_LEN + 32 + MeshCoreNode::NAME_LEN + 1;
    image[FLAGS_AT] |= 1 << 3;

    MeshCoreCompanion reader;
    TEST_ASSERT_EQUAL(1, reader.importContacts(image, n));
    TEST_ASSERT_EQUAL_HEX8(MeshCoreNode::FAVORITE, reader.nodes.find(pub)->flags);
}

void test_contacts_file_of_earlier_builds_is_migrated(void)
{
    // The version 1 image: magic, version, count, then public key and a 40-byte name per contact, newest first.
    uint8_t image[5 + 72] = {'M', 'C', 'C', 1, 1};
    uint8_t pub[32];
    fillPub(pub, 0x33, 3);
    memcpy(&image[5], pub, 32);
    strcpy((char *)&image[5 + 32], "Alice");

    MeshCoreCompanion c;
    TEST_ASSERT_EQUAL(1, c.importContacts(image, sizeof(image)));
    const MeshCoreNode *n = c.nodes.find(pub);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("Alice", n->name);
    // The number the contact had before the upgrade, so the client keeps the conversation it already shows.
    TEST_ASSERT_EQUAL_HEX32(MeshCoreCompanion::senderNodeNum("Alice"), n->nodeNum);
    TEST_ASSERT_TRUE(c.hasContact(n->nodeNum));
}

void test_contacts_image_rejects_garbage(void)
{
    MeshCoreCompanion c;
    uint8_t junk[64] = {};
    TEST_ASSERT_EQUAL(0, c.importContacts(junk, sizeof(junk)));

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    uint8_t a[32];
    fillPub(a, 1, 10);
    c.learnContact(a, "Alice");
    const size_t n = c.exportContacts(image, sizeof(image));

    MeshCoreCompanion other;
    TEST_ASSERT_EQUAL(0, other.importContacts(image, n - 1)); // truncated
    image[3] = 99;                                            // unknown version
    TEST_ASSERT_EQUAL(0, other.importContacts(image, n));
}

void test_contacts_fingerprint_follows_the_set_not_the_recency(void)
{
    MeshCoreCompanion c;
    uint8_t a[32], b[32];
    fillPub(a, 1, 10);
    fillPub(b, 2, 20);
    c.learnContact(a, "Alice");
    const uint32_t one = c.contactsFingerprint();

    c.nodes.touch(c.nodes.find(a));
    TEST_ASSERT_EQUAL_HEX32(one, c.contactsFingerprint());

    c.learnContact(b, "Bob");
    TEST_ASSERT_NOT_EQUAL(one, c.contactsFingerprint());
    const uint32_t two = c.contactsFingerprint();

    c.learnContact(a, "Alicia");
    TEST_ASSERT_NOT_EQUAL(two, c.contactsFingerprint());
}

void test_image_keeps_conversations_over_newer_contacts(void)
{
    MeshCoreCompanion c;
    uint8_t chat[32];
    fillPub(chat, 0x01, 0);
    c.nodes.noteDirectMessage(c.learnContact(chat, "friend"));
    for (int i = 0; i < 20; i++) { // all newer than the conversation, and more than the image holds
        uint8_t pub[32];
        fillPub(pub, i + 2, i * 3);
        c.learnContact(pub, "n");
    }
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t n = c.exportContacts(image, sizeof(image));
    TEST_ASSERT_EQUAL(MeshCoreNodeFile::SAVED_MAX, image[4]);

    MeshCoreCompanion reader;
    reader.importContacts(image, n);
    // Saved by recency alone, the friend would be gone and their next DM undecryptable after a reboot.
    TEST_ASSERT_NOT_NULL(reader.nodes.find(chat));
    uint8_t oldestStranger[32];
    fillPub(oldestStranger, 2, 0);
    TEST_ASSERT_NULL(reader.nodes.find(oldestStranger));
}

void test_direct_send_needs_identity_and_contact(void)
{
    MeshCoreCompanion c;
    uint8_t pub[32];
    fillPub(pub, 1, 10);
    const uint32_t num = c.learnContact(pub, "Alice")->nodeNum;
    TEST_ASSERT_FALSE(c.sendDirectText(num, 1, "hola", 1000, nullptr)); // no identity, no radio
    TEST_ASSERT_FALSE(c.sendDirectText(0x12345678, 1, "hola", 1000, nullptr));
    TEST_ASSERT_TRUE(c.hasContact(num));
    TEST_ASSERT_FALSE(c.hasContact(0x12345678));
}

void test_contact_user_carries_the_meshcore_key_and_is_messagable(void)
{
    uint8_t pub[32];
    fillPub(pub, 0x21, 77);

    const meshtastic_User keyed = MeshCoreCompanion::senderUser("Keyed", true, pub, meshtastic_Config_DeviceConfig_Role_CLIENT);
    TEST_ASSERT_EQUAL_STRING("Keyed (MC)", keyed.long_name);
    TEST_ASSERT_EQUAL(32, keyed.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(pub, keyed.public_key.bytes, 32);
    TEST_ASSERT_FALSE(keyed.is_unmessagable);

    // A group sender has no key and stays unmessagable.
    const meshtastic_User group =
        MeshCoreCompanion::senderUser("GroupOnly", false, nullptr, meshtastic_Config_DeviceConfig_Role_CLIENT);
    TEST_ASSERT_EQUAL(0, group.public_key.size);
    TEST_ASSERT_TRUE(group.is_unmessagable);
}

void test_a_node_is_announced_until_heard_and_again_when_it_changes_or_goes_stale(void)
{
    uint8_t pub[32];
    fillPub(pub, 0x22, 5);
    const meshtastic_User user = MeshCoreCompanion::senderUser("Rpt", false, pub, meshtastic_Config_DeviceConfig_Role_REPEATER);
    const uint32_t now = 1000000;

    TEST_ASSERT_TRUE(MeshCoreCompanion::needsAnnounce(nullptr, user, now)); // not in the list yet

    meshtastic_NodeInfoLite lite = meshtastic_NodeInfoLite_init_zero;
    TypeConversions::CopyUserToNodeInfoLite(&lite, user);
    lite.last_heard = now;
    TEST_ASSERT_TRUE(MeshCoreCompanion::needsAnnounce(&lite, user, now)); // in the list, never heard over the radio

    nodeInfoLiteSetHeardSlot(&lite, 1);
    TEST_ASSERT_FALSE(MeshCoreCompanion::needsAnnounce(&lite, user, now + 60));

    const meshtastic_User renamed =
        MeshCoreCompanion::senderUser("Rpt2", false, pub, meshtastic_Config_DeviceConfig_Role_REPEATER);
    TEST_ASSERT_TRUE(MeshCoreCompanion::needsAnnounce(&lite, renamed, now + 60));
    TEST_ASSERT_TRUE(MeshCoreCompanion::needsAnnounce(&lite, user, now + MeshCoreCompanion::HEARD_REFRESH_SECS));
    TEST_ASSERT_FALSE(MeshCoreCompanion::needsAnnounce(&lite, user, 0)); // clock untrusted: nothing to compare
}

void test_contacts_find_a_name_only_when_unique(void)
{
    MeshCoreCompanion comp;
    uint8_t a[32], b[32], c[32];
    fillPub(a, 1, 10);
    fillPub(b, 2, 20);
    fillPub(c, 3, 30);
    comp.learnContact(a, "Alice");
    comp.learnContact(b, "Alice");
    comp.learnContact(c, "Carol");

    TEST_ASSERT_NULL(comp.nodes.findUniqueByName("Alice", MeshCoreNode::CHAT)); // two people share it: it cannot pick one
    TEST_ASSERT_EQUAL_PTR(comp.nodes.find(c), comp.nodes.findUniqueByName("Carol", MeshCoreNode::CHAT));
    TEST_ASSERT_NULL(comp.nodes.findUniqueByName("Nobody", MeshCoreNode::CHAT));
}

static void sharedBetweenTestSeeds(uint8_t secret[32], uint8_t pubA[32], uint8_t pubB[32])
{
    Ed25519::derivePublicKey(pubA, kSeedA);
    Ed25519::derivePublicKey(pubB, kSeedB);
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, pubB, secret));
}

void test_path_ack_round_trip(void)
{
    uint8_t secret[32], pubA[32], pubB[32];
    sharedBetweenTestSeeds(secret, pubA, pubB);

    uint8_t frame[64];
    const size_t len = MeshCoreCompanion::encodePathAck(secret, pubA[0], pubB[0], 0xA1B2C3D4, frame, sizeof(frame));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_HEX8(0x21, frame[0]); // PATH, flood

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));
    uint32_t hash = 0;
    TEST_ASSERT_TRUE(MeshCoreCompanion::decodePathAck(secret, frame, len, info, &hash));
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4, hash);

    uint8_t wrong[32];
    memcpy(wrong, secret, 32);
    wrong[5] ^= 1;
    TEST_ASSERT_FALSE(MeshCoreCompanion::decodePathAck(wrong, frame, len, info, &hash));
}

void test_path_ack_skips_the_walked_path(void)
{
    uint8_t secret[32], pubA[32], pubB[32];
    sharedBetweenTestSeeds(secret, pubA, pubB);

    // path_len 0x42: 2-byte hashes, 2 hops (4 bytes of path), then extra type ACK and the hash.
    const uint8_t plain[] = {0x42, 0x11, 0x22, 0x33, 0x44, 0x03, 0x78, 0x56, 0x34, 0x12};
    uint8_t frame[80];
    const size_t len = sealDatagram(secret, 0x08, pubA[0], pubB[0], plain, sizeof(plain), frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    uint32_t hash = 0;
    TEST_ASSERT_TRUE(MeshCoreCompanion::decodePathAck(secret, frame, len, info, &hash));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, hash);
}

void test_path_with_another_extra_is_not_an_ack(void)
{
    uint8_t secret[32], pubA[32], pubB[32];
    sharedBetweenTestSeeds(secret, pubA, pubB);

    const uint8_t plain[] = {0x00, 0x0F, 1, 2, 3, 4}; // RAW_CUSTOM extra, what MeshCore sends with no ACK to carry
    uint8_t frame[64];
    const size_t len = sealDatagram(secret, 0x08, pubA[0], pubB[0], plain, sizeof(plain), frame, sizeof(frame));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));

    uint32_t hash = 0;
    TEST_ASSERT_FALSE(MeshCoreCompanion::decodePathAck(secret, frame, len, info, &hash));
}

void test_contact_and_channel_sender_share_one_node(void)
{
    MeshCoreCompanion c;
    uint8_t pub[32];
    fillPub(pub, 0x31, 5);
    // The same name gives the same node whether it first spoke in a channel or advertised: one entry in the client.
    TEST_ASSERT_EQUAL_HEX32(MeshCoreCompanion::senderNodeNum("Alice"), c.learnContact(pub, "Alice")->nodeNum);
}

void test_group_text_marks_replies_and_known_mentions(void)
{
    MeshCoreCompanion c;
    uint8_t pub[32];
    fillPub(pub, 0x41, 9);
    c.learnContact(pub, "Carol");
    c.noteRecentText(1234, "Bob Builder");

    char out[200];
    c.prepareGroupText("hi", 1234, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("@[Bob Builder] hi", out);

    c.prepareGroupText("hey @carol and @Bob Builder, @nobody.", 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hey @[Carol] and @[Bob Builder], @[nobody].", out);

    c.prepareGroupText("@Carolina stays", 0, out, sizeof(out)); // "Carol" is not the whole word: one unknown word
    TEST_ASSERT_EQUAL_STRING("@[Carolina] stays", out);

    c.prepareGroupText("mail a@b.com or @ alone", 0, out, sizeof(out)); // not at a word start / no name
    TEST_ASSERT_EQUAL_STRING("mail a@b.com or @ alone", out);

    c.prepareGroupText("already @[Carol]", 99, out, sizeof(out)); // unknown reply id, existing mention
    TEST_ASSERT_EQUAL_STRING("already @[Carol]", out);
}

void test_reply_tag_cites_the_newest_text_by_that_name(void)
{
    MeshCoreCompanion c;
    c.noteRecentText(1234, "Bob Builder");
    c.noteRecentText(1235, "Carol");
    c.noteRecentText(1236, "Bob Builder");

    const char *rest = nullptr;
    TEST_ASSERT_EQUAL_UINT32(1236, c.replyTarget("@[Bob Builder] hola", &rest));
    TEST_ASSERT_EQUAL_STRING("hola", rest);

    TEST_ASSERT_EQUAL_UINT32(1235, c.replyTarget("@[Carol] ok", &rest));
    TEST_ASSERT_EQUAL_STRING("ok", rest);
}

void test_reply_tag_leaves_text_without_a_recent_match(void)
{
    MeshCoreCompanion c;
    c.noteRecentText(1234, "Bob");

    const char *rest = nullptr;
    TEST_ASSERT_EQUAL_UINT32(0, c.replyTarget("@[Nobody] hola", &rest)); // no recent text by that name
    TEST_ASSERT_EQUAL_STRING("@[Nobody] hola", rest);

    TEST_ASSERT_EQUAL_UINT32(0, c.replyTarget("@[Bob]hola", &rest)); // no space after the tag: not a reply
    TEST_ASSERT_EQUAL_STRING("@[Bob]hola", rest);

    TEST_ASSERT_EQUAL_UINT32(0, c.replyTarget("hola @[Bob] que tal", &rest)); // a mention in the middle is not a reply
    TEST_ASSERT_EQUAL_STRING("hola @[Bob] que tal", rest);
}

// Our TXT_MSG as a repeater re-floods it: same payload, one more two-byte hop on the path.
static size_t repeaterEcho(const uint8_t *frame, size_t len, uint8_t *echo)
{
    echo[0] = frame[0];
    echo[1] = frame[1] + 1;
    echo[2] = 0xAB; // the repeater's hop hash, the relay the client is told about
    echo[3] = 0xCD;
    memcpy(&echo[4], &frame[2], len - 2);
    return len + 2;
}

static meshtastic_Routing_Error routingError(const meshtastic_MeshPacket &p)
{
    meshtastic_Routing r = meshtastic_Routing_init_default;
    TEST_ASSERT_TRUE(pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_Routing_msg, &r));
    return r.error_reason;
}

class CaptureRouter : public Router
{
  public:
    ~CaptureRouter()
    {
        iface.release();
        delete cryptLock;
        cryptLock = nullptr;
    }

    void setRadio(RadioInterface *r)
    {
        iface.release();
        iface.reset(r);
    }

    void enqueueReceivedMessage(meshtastic_MeshPacket *p) override
    {
        received.push_back(*p);
        packetPool.release(p);
    }

    std::vector<meshtastic_MeshPacket> received;
};

class CaptureRadio : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t totalPacketLen, bool received = false) override { return 100; }
    bool queueMeshCoreFrame(const uint8_t *bytes, size_t len) override
    {
        memcpy(frame, bytes, len);
        frameLen = len;
        return true;
    }

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t frameLen = 0;
};

// Our identity (kSeedA) with one contact, Bob (kSeedB), who has a key. Swaps in a node DB and a capturing router for the test.
struct DirectSetup {
    // A RadioInterface sizes its slot time from the region in force, which nothing earlier in this suite sets.
    struct RegionReady {
        RegionReady() { initRegion(); }
    } regionReady;
    NodeDB *previousNodeDB = nodeDB;
    Router *previousRouter = router;
    CaptureRouter capture;
    MeshCoreCompanion c;
    CaptureRadio radio;
    uint8_t pubA[32] = {};
    uint8_t pubB[32] = {};
    uint32_t bobNum = 0;

    DirectSetup()
    {
        nodeDB = new NodeDB();
        router = &capture;
        Ed25519::derivePublicKey(pubA, kSeedA);
        Ed25519::derivePublicKey(pubB, kSeedB);
        c.setIdentity(kSeedA, pubA);
        bobNum = c.learnContact(pubB, "Bob")->nodeNum;
    }

    ~DirectSetup()
    {
        delete nodeDB;
        nodeDB = previousNodeDB;
        router = previousRouter;
    }
};

// A direct message to a MeshCore contact must reach the client as relayed when a repeater re-floods it, then as delivered when
// the contact's ACK arrives. Regression guard: without the relay report the client stays on "Sending..." until the ACK.
void test_direct_relay_echo_reports_relayed_then_delivered(void)
{
    DirectSetup s;
    TEST_ASSERT_TRUE(s.c.sendDirectText(s.bobNum, 42, "hola", 1700000000, &s.radio));

    uint8_t echo[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t echoLen = repeaterEcho(s.radio.frame, s.radio.frameLen, echo);
    s.c.handleFrame(echo, echoLen, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, s.capture.received.size());
    const meshtastic_MeshPacket &relayed = s.capture.received[0];
    TEST_ASSERT_EQUAL_UINT32(nodeDB->getNodeNum(), relayed.from);
    TEST_ASSERT_EQUAL_UINT32(42, relayed.decoded.request_id);
    TEST_ASSERT_EQUAL_HEX8(0xAB, relayed.relay_node);
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_NONE, routingError(relayed));

    s.c.handleFrame(echo, echoLen, 0, 0); // another repeater re-flooding it
    TEST_ASSERT_EQUAL_UINT(1, s.capture.received.size());

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(s.radio.frame, s.radio.frameLen, &info));
    uint8_t rxSecret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, s.pubA, rxSecret));
    char text[MeshCoreCompanion::MAX_TEXT_LEN + 1];
    uint32_t ts = 0;
    uint8_t flags = 0;
    TEST_ASSERT_TRUE(
        MeshCoreCompanion::decodeDirectText(rxSecret, s.radio.frame, s.radio.frameLen, info, text, sizeof(text), &ts, &flags));

    uint8_t ack[6];
    const size_t ackLen = MeshCoreCompanion::encodeAck(MeshCoreCompanion::ackHash(ts, flags, text, s.pubA), ack, sizeof(ack));
    s.c.handleFrame(ack, ackLen, 0, 0);
    TEST_ASSERT_EQUAL_UINT(2, s.capture.received.size());
    TEST_ASSERT_EQUAL_UINT32(s.bobNum, s.capture.received[1].from);
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_NONE, routingError(s.capture.received[1]));
}

// A relayed direct message whose ACK never comes stops quietly at the relay, as a Meshtastic DM does once its relay has ACKed it.
// Regression guard: reporting a failure after "relayed" turns a delivered-to-mesh message into a failed one.
void test_relayed_direct_message_times_out_silently(void)
{
    DirectSetup s;
    TEST_ASSERT_TRUE(s.c.sendDirectText(s.bobNum, 42, "hola", 1700000000, &s.radio));
    uint8_t echo[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t echoLen = repeaterEcho(s.radio.frame, s.radio.frameLen, echo);
    s.c.handleFrame(echo, echoLen, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, s.capture.received.size());

    Time::setTestMillis(1000 + MeshCoreCompanion::ACK_TIMEOUT_MS + 1);
    s.c.service();
    TEST_ASSERT_EQUAL_UINT(1, s.capture.received.size());
}

// A direct message that nothing relays or acknowledges is reported as failed once ACK_TIMEOUT_MS has passed.
// Regression guard: the relayed exemption must not swallow the failure for a message that never left the air.
void test_unrelayed_direct_message_times_out_as_failed(void)
{
    DirectSetup s;
    TEST_ASSERT_TRUE(s.c.sendDirectText(s.bobNum, 42, "hola", 1700000000, &s.radio));

    Time::setTestMillis(1000 + MeshCoreCompanion::ACK_TIMEOUT_MS + 1);
    s.c.service();
    TEST_ASSERT_EQUAL_UINT(1, s.capture.received.size());
    TEST_ASSERT_EQUAL_UINT32(s.bobNum, s.capture.received[0].from);
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_MAX_RETRANSMIT, routingError(s.capture.received[0]));
}

#if MESHTASTIC_MESHCORE_PATH_COMMAND
// A ".path" reply is answered from RAM: it never reaches the radio, and the report describes the text it answers.
struct GroupSetup {
    struct RegionReady {
        RegionReady() { initRegion(); }
    } regionReady;
    NodeDB *previousNodeDB = nodeDB;
    Router *previousRouter = router;
    meshtastic_Channel previousChannel = {};
    CaptureRouter capture;
    CaptureRadio radio;
    MeshCoreCompanion c;

    GroupSetup()
    {
        capture.setRadio(&radio);
        previousChannel = channels.getByIndex(0);
        nodeDB = new NodeDB();
        router = &capture;
        meshtastic_Channel &ch = channels.getByIndex(0);
        ch = meshtastic_Channel();
        ch.has_settings = true;
        ch.role = meshtastic_Channel_Role_PRIMARY;
        ch.settings.psk.size = sizeof(kPublicKey);
        memcpy(ch.settings.psk.bytes, kPublicKey, sizeof(kPublicKey));
        strncpy(ch.settings.name, "MC:Public", sizeof(ch.settings.name) - 1);
    }

    ~GroupSetup()
    {
        capture.setRadio(nullptr);
        channels.getByIndex(0) = previousChannel;
        delete nodeDB;
        nodeDB = previousNodeDB;
        router = previousRouter;
    }
};

// A repeater the node DB knows: name plus the public key its path hash is the prefix of.
static void addRepeater(const uint8_t pub[32], const char *name)
{
    meshtastic_NodeInfoLite *n = nodeDB->getOrCreateMeshNode(MeshCoreNodeStore<1>::nodeNumForKey(pub));
    TEST_ASSERT_NOT_NULL(n);
    strncpy(n->long_name, name, sizeof(n->long_name) - 1);
    n->public_key.size = 32;
    memcpy(n->public_key.bytes, pub, 32);
}

// The same frame with a path spliced in: header, path_len, path, payload (see repeaterEcho).
static size_t withPath(const uint8_t *frame, size_t len, uint8_t *out, uint8_t hashSize, const uint8_t *hops, uint8_t count)
{
    out[0] = frame[0];
    out[1] = (uint8_t)(((hashSize - 1) << 6) | count);
    memcpy(&out[2], hops, (size_t)hashSize * count);
    memcpy(&out[2 + (size_t)hashSize * count], &frame[2], len - 2);
    return len + (size_t)hashSize * count;
}

static meshtastic_MeshPacket textPacket(uint8_t channel, uint32_t replyId, const char *text)
{
    meshtastic_MeshPacket p = {};
    p.id = 0x1234;
    p.channel = channel;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.reply_id = replyId;
    p.decoded.payload.size = strlen(text);
    memcpy(p.decoded.payload.bytes, text, p.decoded.payload.size);
    return p;
}

static const char *reportText(const meshtastic_MeshPacket &p, char *buf, size_t cap)
{
    const size_t n = p.decoded.payload.size < cap - 1 ? p.decoded.payload.size : cap - 1;
    memcpy(buf, p.decoded.payload.bytes, n);
    buf[n] = 0;
    return buf;
}

struct ReportCapture {
    char text[4096] = {};
    size_t len = 0;
    unsigned chunks = 0;
};

static void captureChunk(void *ctx, const char *text, size_t len)
{
    auto *c = static_cast<ReportCapture *>(ctx);
    TEST_ASSERT_TRUE(len <= sizeof(((meshtastic_MeshPacket *)nullptr)->decoded.payload.bytes));
    if (c->chunks++ && c->len + 1 < sizeof(c->text))
        c->text[c->len++] = '\n'; // separate messages in the app
    for (size_t i = 0; i < len && c->len + 1 < sizeof(c->text); i++)
        c->text[c->len++] = text[i];
    c->text[c->len] = 0;
}

static size_t formatOneRoute(MeshCoreCompanion &c, const MeshCoreCompanion::HeardPath &entry, const char *sender, char *out,
                             size_t cap)
{
    const MeshCoreCompanion::HeardPath *routes[] = {&entry};
    ReportCapture capture;
    c.formatPathReport(routes, 1, sender, captureChunk, &capture);
    const size_t n = strlen(capture.text) < cap - 1 ? strlen(capture.text) : cap - 1;
    memcpy(out, capture.text, n);
    out[n] = 0;
    return n;
}

/// Every text injected from index `from` on, i.e. all the pieces of one report.
static void collectReports(const CaptureRouter &capture, size_t from, char *buf, size_t cap)
{
    size_t o = 0;
    for (size_t i = from; i < capture.received.size(); i++) {
        const meshtastic_MeshPacket &p = capture.received[i];
        if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
            p.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
            continue;
        if (o && o + 1 < cap)
            buf[o++] = '\n';
        for (size_t k = 0; k < p.decoded.payload.size && o + 1 < cap; k++)
            buf[o++] = (char)p.decoded.payload.bytes[k];
    }
    buf[o] = 0;
}

// A channel text with two resolvable hops: the reply names them and carries the measured signal.
void test_path_command_answers_locally(void)
{
    GroupSetup s;
    const uint8_t pubA[32] = {0x11, 0x22};
    const uint8_t pubB[32] = {0x33, 0x44};
    addRepeater(pubA, "Alpha");
    addRepeater(pubB, "Bravo");

    const uint32_t packetId = 0x00ABC123;
    const uint8_t hops[] = {0x11, 0x22, 0x33, 0x44};
    const uint32_t fromNum = MeshCoreCompanion::senderNodeNum("Alice");
    s.c.noteHeardRoute(packetId, 0x00A1B2C3, fromNum, 0, hops, 2, 2, 1, -9.0f, -98);

    // The node the report names: the client had it announced with the " (MC)" suffix, which the report drops.
    meshtastic_NodeInfoLite *alice = nodeDB->getOrCreateMeshNode(fromNum);
    alice->bitfield |= NODEINFO_BITFIELD_HAS_USER_MASK;
    strncpy(alice->long_name, "Alice (MC)", sizeof(alice->long_name) - 1);

    const size_t before = s.capture.received.size();
    meshtastic_MeshPacket command = textPacket(0, packetId, ".path");
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, false));

    bool delivery = false;
    for (size_t i = before; i < s.capture.received.size(); i++) {
        const meshtastic_MeshPacket &q = s.capture.received[i];
        if (q.which_payload_variant == meshtastic_MeshPacket_decoded_tag && q.decoded.portnum == meshtastic_PortNum_ROUTING_APP &&
            q.decoded.request_id == command.id)
            delivery = true;
    }
    TEST_ASSERT_TRUE(delivery);

    const meshtastic_MeshPacket &report = s.capture.received.back();
    TEST_ASSERT_EQUAL_UINT32(MeshCoreCompanion::PSEUDO_NODE_NUM, report.from);
    TEST_ASSERT_EQUAL_UINT32(NODENUM_BROADCAST, report.to);
    TEST_ASSERT_EQUAL_UINT32(packetId, report.decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT8(0, report.channel);

    char body[256];
    reportText(report, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️"));
    TEST_ASSERT_NOT_NULL(strstr(body, "de **Alice**"));
    TEST_ASSERT_NULL(strstr(body, "(MC)"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Alpha"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Bravo"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -9.0 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️ Ruta de **Alice**"));
    TEST_ASSERT_NULL(strstr(body, "rutas")); // a count only when there is more than one route
    // Where and when, as the Meshtastic report says it.
    TEST_ASSERT_NOT_NULL(strstr(body, "Canal MC:Public · hace "));
    // Bold is for the sender only: one pair of markers.
    TEST_ASSERT_NULL(strstr(body, "**Alpha**"));
    TEST_ASSERT_NULL(strstr(strstr(body, "**Alice**") + 9, "**"));
}

// Only a reply is answered: without a reply target the ".path" is an ordinary text, not intercepted or injected.
void test_path_command_plain_path_is_not_intercepted(void)
{
    GroupSetup s;
    const uint8_t hops[] = {0xAB, 0xCD};
    const uint32_t packetId = 0x00ABC456;
    s.c.noteHeardRoute(packetId, 0x00A1B2C4, MeshCoreCompanion::senderNodeNum("Alice"), 0, hops, 2, 1, 1, -5.0f, -80);

    meshtastic_MeshPacket command = textPacket(0, 0, ".path");
    TEST_ASSERT_FALSE(s.c.handlePathCommand(&command, false));
    TEST_ASSERT_TRUE(s.capture.received.empty());
}

// Nothing heard yet: a short local notice, and the command still does not go out.
void test_path_command_without_data_is_a_hint(void)
{
    GroupSetup s;
    meshtastic_MeshPacket command = textPacket(0, 0x00ABC999, ".path");
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, false));

    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Sin datos"));
}

// Anything that is not exactly the command is left to the normal send path.
void test_path_command_ignores_other_text(void)
{
    GroupSetup s;
    meshtastic_MeshPacket dotted = textPacket(0, 0, ".paths");
    TEST_ASSERT_FALSE(s.c.handlePathCommand(&dotted, false));
    meshtastic_MeshPacket bare = textPacket(0, 0, "path");
    TEST_ASSERT_FALSE(s.c.handlePathCommand(&bare, false));
    meshtastic_MeshPacket withArgs = textPacket(0, 0, ".path foo");
    TEST_ASSERT_FALSE(s.c.handlePathCommand(&withArgs, false));
    meshtastic_MeshPacket otherChannel = textPacket(1, 0, ".path"); // not a MeshCore channel
    TEST_ASSERT_FALSE(s.c.handlePathCommand(&otherChannel, false));
    TEST_ASSERT_TRUE(s.capture.received.empty());
}

// An unknown hop, or one several nodes claim, is shown as hex rather than as a guess.
void test_path_report_uses_hex_for_unknown_and_ambiguous_hops(void)
{
    GroupSetup s;
    const uint8_t twinA[32] = {0x77, 0x01};
    const uint8_t twinB[32] = {0x77, 0x02};
    const uint8_t twinA2[32] = {0x77, 0x03}; // same name as twinA: the same node listed twice must collapse
    addRepeater(twinA, "Twin A");
    addRepeater(twinB, "Twin B");
    addRepeater(twinA2, "Twin A");

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 2;
    entry.hashSize = 1;
    entry.routeType = 1;
    entry.snr = -5.0f;
    entry.rssi = -80;
    const uint8_t hops[] = {0x99, 0x77};
    memcpy(entry.path, hops, sizeof(hops));

    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, nullptr, body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "`99`"));          // no node claims it
    TEST_ASSERT_NOT_NULL(strstr(body, "Twin A|Twin B")); // ambiguous: both candidates, no duplicate
    TEST_ASSERT_NULL(strstr(body, "`77`"));
}

// A hop the node DB no longer holds is still named by the DM contact whose key prefix it is.
void test_path_report_falls_back_to_a_contact_key(void)
{
    GroupSetup s;
    const uint8_t pub[32] = {0xC7, 0xC8};
    s.c.learnContact(pub, "Contact RPT");

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 1;
    entry.hashSize = 2;
    entry.routeType = 1;
    entry.snr = -5.0f;
    entry.rssi = -80;
    memcpy(entry.path, pub, 2);

    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, nullptr, body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "Contact RPT"));
}

// A report longer than one message is split into several: nothing is cut off.
void test_path_report_splits_without_cutting(void)
{
    GroupSetup s;
    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 32; // the full 64-byte MeshCore path, two bytes per hop
    entry.hashSize = 2;
    entry.routeType = 1;
    entry.snr = -5.0f;
    entry.rssi = -80;
    for (size_t i = 0; i < sizeof(entry.path); i++)
        entry.path[i] = (uint8_t)(0x40 + i);

    const MeshCoreCompanion::HeardPath *routes[] = {&entry};
    ReportCapture capture;
    TEST_ASSERT_TRUE(s.c.formatPathReport(routes, 1, nullptr, captureChunk, &capture) >= 2);

    TEST_ASSERT_NOT_NULL(strstr(capture.text, "`4041`")); // the first hop
    TEST_ASSERT_NOT_NULL(strstr(capture.text, "`7E7F`")); // and the last one: nothing was dropped
    TEST_ASSERT_NOT_NULL(strstr(capture.text, "SNR -5.0 dB"));
    TEST_ASSERT_NULL(strstr(capture.text, "…"));
}

static const meshtastic_MeshPacket &receiveFromBob(DirectSetup &s, uint32_t timestamp)
{
    uint8_t secret[32], frame[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, s.pubA, secret));
    const size_t len =
        MeshCoreCompanion::encodeDirectText(secret, s.pubA[0], s.pubB[0], timestamp, 0, "hola", frame, sizeof(frame));
    const size_t before = s.capture.received.size();
    s.c.handleFrame(frame, len, 0, 0);
    TEST_ASSERT_TRUE(s.capture.received.size() > before);
    return s.capture.received.back();
}

// The client keeps two chats per node, one for PKI messages and one for plain ones, and files an incoming message by the
// PKI mark of the packet that carried it. A contact is always announced with the key its ADVERT signed, so the client
// opens its PKI chat and the user writes from there: the reply must be marked the same way or it lands in a second chat
// with the same name the user cannot answer from. Regression guard: a reply marked plain because the firmware only knew
// which chat the user wrote from last, or no mark at all.
void test_direct_reply_lands_in_the_pki_chat_of_the_contact(void)
{
    DirectSetup s;
    TEST_ASSERT_TRUE(receiveFromBob(s, 1700000001).pki_encrypted);
    TEST_ASSERT_TRUE(receiveFromBob(s, 1700000002).pki_encrypted); // and stays so on the next one
    TEST_ASSERT_TRUE(s.c.sendDirectText(s.bobNum, 42, "hola", 1700000000, &s.radio));
    TEST_ASSERT_TRUE(receiveFromBob(s, 1700000003).pki_encrypted);
}

// The client opens the PKI chat from the key the node was announced with, and files the reply by the key the reply carries:
// both must be the contact's own, or the two chats part ways.
void test_direct_reply_carries_the_key_the_contact_was_announced_with(void)
{
    DirectSetup s;
    const meshtastic_MeshPacket reply = receiveFromBob(s, 1700000001);

    bool announced = false;
    for (const meshtastic_MeshPacket &p : s.capture.received) {
        if (p.decoded.portnum != meshtastic_PortNum_NODEINFO_APP || p.from != s.bobNum)
            continue;
        meshtastic_User user = meshtastic_User_init_zero;
        TEST_ASSERT_TRUE(pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_User_msg, &user));
        TEST_ASSERT_EQUAL(32, user.public_key.size);
        TEST_ASSERT_EQUAL_MEMORY(s.pubB, user.public_key.bytes, 32);
        announced = true;
    }
    TEST_ASSERT_TRUE(announced);
    TEST_ASSERT_EQUAL(32, reply.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(s.pubB, reply.public_key.bytes, 32);
}

// A direct reply from Bob must survive Router's passesRoutingAuthGate and applyRoutingAuthCache
// without stripping pki_encrypted or public_key. Regression guard: Router's decoded ingress gate
// unconditionally cleared pki_encrypted/public_key on decoded packets, which demoted direct
// replies to channel 0 and opened a duplicate chat on the client.
void test_direct_reply_preserves_pki_through_router_auth_gate(void)
{
    DirectSetup s;
    meshtastic_MeshPacket reply = receiveFromBob(s, 1700000001);
    TEST_ASSERT_TRUE(reply.pki_encrypted);
    TEST_ASSERT_EQUAL(32, reply.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(s.pubB, reply.public_key.bytes, 32);

    TEST_ASSERT_EQUAL(RoutingAuthVerdict::ACCEPT, passesRoutingAuthGate(&reply));
    TEST_ASSERT_TRUE(applyRoutingAuthCache(&reply));
    TEST_ASSERT_TRUE(reply.pki_encrypted);
    TEST_ASSERT_EQUAL(32, reply.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(s.pubB, reply.public_key.bytes, 32);

    // Also verify untrusted decoded ingress from an unknown node still has PKI stripped
    meshtastic_MeshPacket rogue = reply;
    rogue.id++;
    rogue.from = 0x12345678;
    TEST_ASSERT_EQUAL(RoutingAuthVerdict::ACCEPT, passesRoutingAuthGate(&rogue));
    TEST_ASSERT_TRUE(applyRoutingAuthCache(&rogue));
    TEST_ASSERT_FALSE(rogue.pki_encrypted);
    TEST_ASSERT_EQUAL(0, rogue.public_key.size);

    // Bob's number and key are public: a decoded packet claiming them (e.g. over MQTT) that the bridge did not
    // inject must still have PKI stripped
    meshtastic_MeshPacket forged = reply;
    forged.id += 2;
    TEST_ASSERT_EQUAL(RoutingAuthVerdict::ACCEPT, passesRoutingAuthGate(&forged));
    TEST_ASSERT_TRUE(applyRoutingAuthCache(&forged));
    TEST_ASSERT_FALSE(forged.pki_encrypted);
    TEST_ASSERT_EQUAL(0, forged.public_key.size);
}

// A direct message is answered in the same chat, from the contact and with its key, as a PKI reply.
void test_path_command_answers_a_direct_message(void)
{
    DirectSetup s;
    uint8_t secret[32], frame[MeshCoreRepeater::MAX_FRAME_LEN], path[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, s.pubA, secret)); // Bob writing to us
    const size_t len = MeshCoreCompanion::encodeDirectText(secret, s.pubA[0], s.pubB[0], 1, 0, "hola", frame, sizeof(frame));
    const uint8_t hops[] = {0xAB, 0xCD};
    const size_t pathLen = withPath(frame, len, path, 2, hops, 1);
    s.c.handleFrame(path, pathLen, -7.5f, -95);

    TEST_ASSERT_TRUE(!s.capture.received.empty());
    const meshtastic_MeshPacket original = s.capture.received.back();
    TEST_ASSERT_EQUAL(meshtastic_PortNum_TEXT_MESSAGE_APP, original.decoded.portnum);

    meshtastic_MeshPacket command = textPacket(0, original.id, ".path");
    command.to = s.bobNum;
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, true));

    const meshtastic_MeshPacket &report = s.capture.received.back();
    TEST_ASSERT_EQUAL_UINT32(s.bobNum, report.from);
    TEST_ASSERT_EQUAL_UINT32(nodeDB->getNodeNum(), report.to);
    TEST_ASSERT_TRUE(report.pki_encrypted);
    TEST_ASSERT_EQUAL_UINT32(original.id, report.decoded.reply_id);
    TEST_ASSERT_EQUAL_MEMORY(s.pubB, report.public_key.bytes, 32);

    char body[256];
    reportText(report, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -7.5 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Mensaje directo · hace ")); // a DM, not MeshCore channel 0
}

// A channel text we sent, re-flooded by a repeater, is answered with that repeater as its route. Regression guard: our own
// transmission had no entry, so every ".path" on it answered "Sin datos" whatever the repeaters did.
void test_path_command_answers_our_channel_text_with_its_echo(void)
{
    GroupSetup s;
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN], echo[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 1700000000, "hola", frame, sizeof(frame));
    TEST_ASSERT_GREATER_THAN(0, len);
    const uint32_t packetId = 0x00ABC777;
    s.c.noteTransmitted(frame, len);
    TEST_ASSERT_TRUE(s.c.trackGroupText(frame, len, packetId, 0));

    const size_t echoLen = repeaterEcho(frame, len, echo);
    s.c.handleFrame(echo, echoLen, -4.0f, -90);
    char texts[256];
    collectReports(s.capture, 0, texts, sizeof(texts));
    TEST_ASSERT_EQUAL_STRING("", texts); // the echo is no new text, only a route

    meshtastic_MeshPacket command = textPacket(0, packetId, ".path");
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, false));
    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️ Ruta"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -4.0 dB"));
    TEST_ASSERT_NULL(strstr(body, "Sin datos"));
    TEST_ASSERT_NULL(strstr(body, "rutas")); // one echo, one route
}

// Our channel text that no repeater has re-flooded yet has no route: ".path" says so, it does not stay silent.
void test_path_command_own_text_without_echo_is_no_data(void)
{
    GroupSetup s;
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len =
        MeshCoreCompanion::encodeGroupText(kPublicKey, sizeof(kPublicKey), "Alice", 1700000000, "hola", frame, sizeof(frame));
    const uint32_t packetId = 0x00ABC778;
    s.c.noteTransmitted(frame, len);
    TEST_ASSERT_TRUE(s.c.trackGroupText(frame, len, packetId, 0));

    meshtastic_MeshPacket command = textPacket(0, packetId, ".path");
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, false));
    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Sin datos"));
}

// A direct message we sent, re-flooded by a repeater, is answered with that route, as for any text we heard.
void test_path_command_answers_our_direct_message_with_its_echo(void)
{
    DirectSetup s;
    TEST_ASSERT_TRUE(s.c.sendDirectText(s.bobNum, 42, "hola", 1700000000, &s.radio));
    uint8_t echo[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t echoLen = repeaterEcho(s.radio.frame, s.radio.frameLen, echo);
    s.c.handleFrame(echo, echoLen, -4.0f, -90);

    meshtastic_MeshPacket command = textPacket(0, 42, ".path");
    command.to = s.bobNum;
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, true));
    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️ Ruta"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Mensaje directo"));
    TEST_ASSERT_NULL(strstr(body, "Sin datos"));
}

// The same text can reach us by several routes: ".path" lists them all, the shortest first.
void test_path_command_lists_every_route(void)
{
    DirectSetup s;
    uint8_t secret[32], frame[MeshCoreRepeater::MAX_FRAME_LEN], longPath[MeshCoreRepeater::MAX_FRAME_LEN],
        shortPath[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedB, s.pubA, secret)); // Bob writing to us
    const size_t len = MeshCoreCompanion::encodeDirectText(secret, s.pubA[0], s.pubB[0], 1, 0, "hola", frame, sizeof(frame));

    const uint8_t longHops[] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t shortHops[] = {0x55, 0x66};
    const size_t longLen = withPath(frame, len, longPath, 2, longHops, 2);
    const size_t shortLen = withPath(frame, len, shortPath, 2, shortHops, 1);

    s.c.handleFrame(longPath, longLen, -7.5f, -95); // two repeaters
    TEST_ASSERT_TRUE(!s.capture.received.empty());
    const meshtastic_MeshPacket original = s.capture.received.back();
    const size_t injected = s.capture.received.size();

    s.c.handleFrame(shortPath, shortLen, -4.0f, -70);            // same text, one repeater
    TEST_ASSERT_EQUAL_UINT(injected, s.capture.received.size()); // a route, not a second message

    meshtastic_MeshPacket command = textPacket(0, original.id, ".path");
    command.to = s.bobNum;
    const size_t beforeReport = s.capture.received.size();
    TEST_ASSERT_TRUE(s.c.handlePathCommand(&command, true));

    char body[1024];
    collectReports(s.capture, beforeReport, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "2 rutas"));
    TEST_ASSERT_NOT_NULL(strstr(body, "2️⃣")); // the glyph is the hop count of a route
    TEST_ASSERT_NOT_NULL(strstr(body, "1️⃣"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -4.0 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -7.5 dB"));
    const char *shorter = strstr(body, "`5566`");
    const char *longer = strstr(body, "`1122`");
    TEST_ASSERT_NOT_NULL(shorter);
    TEST_ASSERT_NOT_NULL(longer);
    TEST_ASSERT_TRUE(shorter < longer); // the shortest route comes first
}

// A long name is reported whole and the " (MC)" suffix is dropped, so the bold markers always close inside the message.
void test_path_report_keeps_whole_names(void)
{
    GroupSetup s;
    const uint8_t pub[32] = {0x5A, 0x5B};
    addRepeater(pub, "Repetidor Largo (MC)");

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 1;
    entry.hashSize = 2;
    entry.routeType = 1;
    entry.snr = -5.0f;
    entry.rssi = -80;
    memcpy(entry.path, pub, 2);

    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, "Sender Largo Sin Sufijo", body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "de **Sender Largo Sin Sufijo**"));
    TEST_ASSERT_NOT_NULL(strstr(body, " Repetidor Largo"));
    TEST_ASSERT_NULL(strstr(body, "**Repetidor Largo**"));
    TEST_ASSERT_NULL(strstr(body, "(MC)"));
    // The glyph carries the hop count; match the two forms the text would take, not a bare " salto" that a node name
    // could contain.
    TEST_ASSERT_NULL(strstr(body, "1 salto"));
    TEST_ASSERT_NULL(strstr(body, " saltos"));
}

// A name carrying markdown delimiters or control bytes cannot break the report's formatting.
void test_path_report_sanitizes_node_names(void)
{
    GroupSetup s;
    const uint8_t pub[32] = {0x42, 0x43};
    addRepeater(pub, "Bad*Name\n[with] `tricks`");

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 1;
    entry.hashSize = 2;
    entry.routeType = 1;
    entry.snr = -5.0f;
    entry.rssi = -80;
    const uint8_t hops[] = {0x42, 0x43};
    memcpy(entry.path, hops, sizeof(hops));

    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, nullptr, body, sizeof(body)) > 0);
    TEST_ASSERT_NULL(strstr(body, "Bad*"));
    TEST_ASSERT_NULL(strstr(body, "[with]"));
    TEST_ASSERT_NOT_NULL(strstr(body, "BadName"));
}

// A repeater's ADVERT lands in the MeshCore node store, numbered from its key as repeater nodes always were (so the client
// keeps the node it shows), and ".path" names its hop from there even when the node DB does not hold it. Regression guard:
// repeaters kept only in the node DB lose their name the moment a busy Meshtastic mesh evicts them.
void test_repeater_advert_is_stored_and_names_its_hop()
{
    GroupSetup s;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++)
        seed[i] = (uint8_t)(0x5A ^ i);
    uint8_t ourPub[32];
    Ed25519::derivePublicKey(ourPub, seed);
    s.c.setIdentity(seed, ourPub);

    MeshCoreRepeater other;
    uint8_t otherSeed[32];
    for (int i = 0; i < 32; i++)
        otherSeed[i] = (uint8_t)(0xC3 ^ i);
    while (!other.setIdentitySeed(otherSeed))
        otherSeed[0]++;
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len = other.buildAdvert(frame, sizeof(frame), "Montjuic RPT", 1792521507, true, true);
    TEST_ASSERT_GREATER_THAN(0, len);
    s.c.handleFrame(frame, len, 5, -60);

    const MeshCoreNode *n = s.c.nodes.find(other.getAliasPub());
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::REPEATER, n->type);
    TEST_ASSERT_EQUAL_STRING("Montjuic RPT", n->name);
    TEST_ASSERT_EQUAL_HEX32(MeshCoreNodeStore<1>::nodeNumForKey(other.getAliasPub()), n->nodeNum);
    TEST_ASSERT_EQUAL_UINT32(1792521507, n->lastAdvert);
    TEST_ASSERT_FALSE(s.c.hasContact(n->nodeNum));     // a repeater is listed, not messaged
    TEST_ASSERT_NULL(nodeDB->getMeshNode(n->nodeNum)); // the capturing router never filed the announce

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 1;
    entry.hashSize = 2;
    entry.routeType = 1;
    memcpy(entry.path, other.getAliasPub(), 2);
    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, nullptr, body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "Montjuic RPT"));
}

// The companion marks the node DB copies of its MeshCore peers so a full DB gives them up first (NodeDB eviction is tested in
// test_nodedb_blocked). Only keyed peers: a channel sender has no key and is evicted early anyway, and a Meshtastic node
// sharing the number reports hardware and is left alone. Regression guard: unmarked copies competing with Meshtastic nodes.
void test_meshcore_copies_in_the_node_db_are_marked()
{
    GroupSetup s;
    const uint8_t pub[32] = {0x61, 0x62};
    const uint32_t num = s.c.learnContact(pub, "Dana")->nodeNum;
    const uint32_t eveNum = MeshCoreCompanion::senderNodeNum("Eve");
    const uint32_t otherNum = 0x4D777777;
    // As the router would have filed the announces. Each creation may move the DB, so nodes are looked up again below.
    nodeDB->getOrCreateMeshNode(num);
    nodeDB->getOrCreateMeshNode(eveNum);
    nodeDB->getOrCreateMeshNode(otherNum)->hw_model = meshtastic_HardwareModel_HELTEC_V3;

    s.c.ensureSenderNode(num, "Dana", true, pub);
    s.c.ensureSenderNode(eveNum, "Eve");
    TEST_ASSERT_TRUE(nodeInfoLiteIsMeshCore(nodeDB->getMeshNode(num)));
    TEST_ASSERT_FALSE(nodeInfoLiteIsMeshCore(nodeDB->getMeshNode(eveNum)));

    nodeInfoLiteSetBit(nodeDB->getMeshNode(num), NODEINFO_BITFIELD_IS_MESHCORE_MASK, false);
    s.c.contactsFingerprint(); // the save sweep marks whatever the announce created since
    TEST_ASSERT_TRUE(nodeInfoLiteIsMeshCore(nodeDB->getMeshNode(num)));
    TEST_ASSERT_FALSE(nodeInfoLiteIsMeshCore(nodeDB->getMeshNode(otherNum)));
}

// A client that syncs only gets the node DB, which gives MeshCore nodes up first. nextReplayNode() walks the store for the
// ones the sync left out, so the client keeps listing and mapping them (PhoneAPI's MeshCore replay phase sends them).
// Regression guard: replaying nodes the sync already sent, or stopping at the first one, or skipping repeaters.
void test_replay_walks_the_nodes_the_node_db_left_out()
{
    GroupSetup s;
    const uint8_t inDb[32] = {0x71}, gone[32] = {0x72}, rpt[32] = {0x73};
    const uint32_t inDbNum = s.c.learnContact(inDb, "Kept")->nodeNum;
    s.c.learnContact(gone, "Evicted");
    s.c.learnRepeater(rpt, "Tower RPT");
    nodeDB->getOrCreateMeshNode(inDbNum);

    size_t cursor = 0;
    MeshCoreNode n;
    size_t found = 0;
    bool sawContact = false, sawRepeater = false;
    while (s.c.nextReplayNode(cursor, n)) {
        found++;
        TEST_ASSERT_NOT_EQUAL(inDbNum, n.nodeNum);
        sawContact |= strcmp(n.name, "Evicted") == 0;
        sawRepeater |= strcmp(n.name, "Tower RPT") == 0;
    }
    TEST_ASSERT_EQUAL_UINT32(2, found);
    TEST_ASSERT_TRUE(sawContact);
    TEST_ASSERT_TRUE(sawRepeater);
    TEST_ASSERT_FALSE(s.c.nextReplayNode(cursor, n)); // a finished walk stays finished
}

// A Meshtastic node whose key happens to start with a MeshCore hop hash is not that hop: its key is a Curve25519 key of
// another network. Regression guard: naming MeshCore hops from every keyed node in the node DB.
void test_path_report_ignores_meshtastic_keys()
{
    GroupSetup s;
    const uint8_t pub[32] = {0x5D, 0x6E};
    meshtastic_NodeInfoLite *n = nodeDB->getOrCreateMeshNode(0x12345678);
    strncpy(n->long_name, "Meshtastic node", sizeof(n->long_name) - 1);
    n->hw_model = meshtastic_HardwareModel_HELTEC_V3;
    n->public_key.size = 32;
    memcpy(n->public_key.bytes, pub, 32);

    MeshCoreCompanion::HeardPath entry;
    entry.hopCount = 1;
    entry.hashSize = 2;
    entry.routeType = 1;
    memcpy(entry.path, pub, 2);
    char body[256];
    TEST_ASSERT_TRUE(formatOneRoute(s.c, entry, nullptr, body, sizeof(body)) > 0);
    TEST_ASSERT_NULL(strstr(body, "Meshtastic node"));
    TEST_ASSERT_NOT_NULL(strstr(body, "`5D6E`"));
}
#endif

// The relay alias is this node, not another repeater: hearing its own repeater-typed ADVERT back must not add a node.
void test_own_alias_advert_is_not_learned_as_a_contact()
{
    struct AliasSetup {
        NodeDB *previousNodeDB = nodeDB;
        Router *previousRouter = router;
        CaptureRouter capture;
        MeshCoreCompanion c;
        MeshCoreRepeater repeater;

        AliasSetup()
        {
            nodeDB = new NodeDB();
            router = &capture;
            uint8_t seed[32];
            for (int i = 0; i < 32; i++)
                seed[i] = (uint8_t)(0x33 ^ i);
            while (!repeater.setIdentitySeed(seed))
                seed[0]++;
            c.setIdentity(seed, repeater.getIdentityPub());
            c.setRelayAliasPub(repeater.getAliasPub());
        }

        ~AliasSetup()
        {
            delete nodeDB;
            nodeDB = previousNodeDB;
            router = previousRouter;
        }
    } s;

    TEST_ASSERT_TRUE(s.repeater.hasAliasIdentity());
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t len = s.repeater.buildAdvert(frame, sizeof(frame), "Xavi (relay)", 1792521507, true, true);
    TEST_ASSERT_GREATER_THAN(0, len);

    const size_t nodesBefore = nodeDB->getNumMeshNodes();
    s.c.handleFrame(frame, len, 5, -60);
    TEST_ASSERT_EQUAL_UINT32(nodesBefore, nodeDB->getNumMeshNodes());
    TEST_ASSERT_NULL(s.c.nodes.find(s.repeater.getAliasPub()));
}

void test_trace_command_syntax_help(void)
{
    GroupSetup s;
    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    TEST_ASSERT_FALSE(s.capture.received.empty());
    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Uso"));
    TEST_ASSERT_NOT_NULL(strstr(body, "..trace @nodo1"));
}

void test_trace_command_unknown_node_reports_error(void)
{
    GroupSetup s;
    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace @NoExiste");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    TEST_ASSERT_FALSE(s.capture.received.empty());
    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Nodo no encontrado"));
    TEST_ASSERT_NOT_NULL(strstr(body, "NoExiste"));
}

void test_trace_command_parses_nodes_and_symmetric_return(void)
{
    GroupSetup s;
    uint8_t pubA[32] = {0xAA};
    uint8_t pubB[32] = {0xBB};
    uint8_t selfPub[32] = {0x11};
    s.c.setIdentity(pubA, selfPub);
    s.c.learnContact(pubA, "Alpha");
    s.c.learnContact(pubB, "Bravo");

    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace @Alpha @Bravo ...");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    TEST_ASSERT_TRUE(s.c.activeTrace.active);
    TEST_ASSERT_TRUE(s.c.activeTrace.symmetric);
    TEST_ASSERT_EQUAL_UINT8(4, s.c.activeTrace.totalHops);
    TEST_ASSERT_EQUAL_HEX8(0xAA, s.c.activeTrace.hopHashes[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, s.c.activeTrace.hopHashes[1]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, s.c.activeTrace.hopHashes[2]);
    TEST_ASSERT_EQUAL_HEX8(0x11, s.c.activeTrace.hopHashes[3]);

    TEST_ASSERT_GREATER_THAN(0, s.radio.frameLen);
    TEST_ASSERT_EQUAL_HEX8((0x09 << 2) | 0x02, s.radio.frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0, s.radio.frame[1]);
}

void test_trace_command_without_symmetric_dots(void)
{
    GroupSetup s;
    uint8_t pubA[32] = {0xAA};
    uint8_t pubB[32] = {0xBB};
    uint8_t selfPub[32] = {0x11};
    s.c.setIdentity(pubA, selfPub);
    s.c.learnContact(pubA, "Alpha");
    s.c.learnContact(pubB, "Bravo");

    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace @Alpha @Bravo");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    TEST_ASSERT_TRUE(s.c.activeTrace.active);
    TEST_ASSERT_FALSE(s.c.activeTrace.symmetric);
    TEST_ASSERT_EQUAL_UINT8(2, s.c.activeTrace.totalHops);
    TEST_ASSERT_EQUAL_HEX8(0xAA, s.c.activeTrace.hopHashes[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, s.c.activeTrace.hopHashes[1]);
}

void test_trace_response_formats_snr_report(void)
{
    GroupSetup s;
    uint8_t pubA[32] = {0xAA};
    uint8_t pubB[32] = {0xBB};
    uint8_t selfPub[32] = {0x11};
    s.c.setIdentity(pubA, selfPub);
    s.c.learnContact(pubA, "Alpha");
    s.c.learnContact(pubB, "Bravo");

    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace @Alpha @Bravo ...");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    const uint32_t tag = s.c.activeTrace.tag;

    uint8_t rxFrame[64];
    size_t i = 0;
    rxFrame[i++] = (0x09 << 2) | 0x02;
    rxFrame[i++] = 3;
    rxFrame[i++] = (int8_t)(8.5f * 4);
    rxFrame[i++] = (int8_t)(5.0f * 4);
    rxFrame[i++] = (int8_t)(-2.0f * 4);
    memcpy(&rxFrame[i], &tag, 4);
    i += 4;
    uint32_t auth = 0;
    memcpy(&rxFrame[i], &auth, 4);
    i += 4;
    rxFrame[i++] = 0;
    rxFrame[i++] = 0xAA;
    rxFrame[i++] = 0xBB;
    rxFrame[i++] = 0xAA;
    rxFrame[i++] = 0x11;

    s.c.handleFrame(rxFrame, i, 9.25f, -70);
    TEST_ASSERT_FALSE(s.c.activeTrace.active);

    char body[512];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Trace completado"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Nosotros › Alpha: SNR 8.5 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Alpha › Bravo: SNR 5.0 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Bravo › Alpha: SNR -2.0 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Alpha › Nosotros: SNR 9.2 dB"));
}

void test_trace_timeout_handling(void)
{
    GroupSetup s;
    uint8_t pubA[32] = {0xAA};
    uint8_t selfPub[32] = {0x11};
    s.c.setIdentity(pubA, selfPub);
    s.c.learnContact(pubA, "Alpha");

    meshtastic_MeshPacket command = textPacket(0, 0x1234, "..trace @Alpha ...");
    TEST_ASSERT_TRUE(s.c.handleTraceCommand(&command, false));
    TEST_ASSERT_TRUE(s.c.activeTrace.active);

    Time::setTestMillis(s.c.activeTrace.deadlineMs + 100);
    s.c.service();
    TEST_ASSERT_FALSE(s.c.activeTrace.active);

    char body[256];
    reportText(s.capture.received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Trace timeout"));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_channel_hashes);
    RUN_TEST(test_meshcore_channel_key_derivation);
    RUN_TEST(test_encode_matches_meshcore_vector);
    RUN_TEST(test_frame_uses_two_byte_path_hashes);
    RUN_TEST(test_hop_fields_report_the_path_length);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_rejects_wrong_channel);
    RUN_TEST(test_rejects_tampered_mac);
    RUN_TEST(test_utf8_truncation_keeps_boundary);
    RUN_TEST(test_zero_padded_32_byte_key_matches_16);
    RUN_TEST(test_rejects_oversized_ciphertext_with_valid_mac);
    RUN_TEST(test_encode_reports_truncation);
    RUN_TEST(test_tx_timestamp_is_strictly_increasing);
    RUN_TEST(test_reply_tag_cites_the_newest_text_by_that_name);
    RUN_TEST(test_reply_tag_leaves_text_without_a_recent_match);
    RUN_TEST(test_split_sender_separates_name_and_body);
    RUN_TEST(test_split_sender_rejects_missing_or_empty_name);
    RUN_TEST(test_split_sender_cuts_long_name_on_utf8_boundary);
    RUN_TEST(test_sender_node_num_is_stable_and_distinct);
    RUN_TEST(test_shared_secret_is_symmetric);
    RUN_TEST(test_direct_text_round_trip);
    RUN_TEST(test_direct_text_rejects_wrong_secret_and_tampering);
    RUN_TEST(test_ack_hash_depends_on_text_and_sender);
    RUN_TEST(test_ack_frame_layout);
    RUN_TEST(test_contacts_keep_their_node_num_across_a_rename);
    RUN_TEST(test_contacts_never_reuse_the_pseudo_node_num);
    RUN_TEST(test_contacts_image_round_trip_keeps_keys_names_and_numbers);
    RUN_TEST(test_contacts_file_drops_flag_bits_no_longer_in_use);
    RUN_TEST(test_contacts_file_of_earlier_builds_is_migrated);
    RUN_TEST(test_contacts_image_rejects_garbage);
    RUN_TEST(test_contacts_fingerprint_follows_the_set_not_the_recency);
    RUN_TEST(test_own_alias_advert_is_not_learned_as_a_contact);
    RUN_TEST(test_image_keeps_conversations_over_newer_contacts);
    RUN_TEST(test_direct_send_needs_identity_and_contact);
    RUN_TEST(test_contact_user_carries_the_meshcore_key_and_is_messagable);
    RUN_TEST(test_a_node_is_announced_until_heard_and_again_when_it_changes_or_goes_stale);
    RUN_TEST(test_contacts_find_a_name_only_when_unique);
    RUN_TEST(test_path_ack_round_trip);
    RUN_TEST(test_path_ack_skips_the_walked_path);
    RUN_TEST(test_path_with_another_extra_is_not_an_ack);
    RUN_TEST(test_contact_and_channel_sender_share_one_node);
    RUN_TEST(test_group_text_marks_replies_and_known_mentions);
    RUN_TEST(test_direct_relay_echo_reports_relayed_then_delivered);
    RUN_TEST(test_relayed_direct_message_times_out_silently);
    RUN_TEST(test_unrelayed_direct_message_times_out_as_failed);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    RUN_TEST(test_path_command_answers_locally);
    RUN_TEST(test_path_command_plain_path_is_not_intercepted);
    RUN_TEST(test_path_command_without_data_is_a_hint);
    RUN_TEST(test_path_command_ignores_other_text);
    RUN_TEST(test_path_report_uses_hex_for_unknown_and_ambiguous_hops);
    RUN_TEST(test_path_report_falls_back_to_a_contact_key);
    RUN_TEST(test_path_report_splits_without_cutting);
    RUN_TEST(test_direct_reply_lands_in_the_pki_chat_of_the_contact);
    RUN_TEST(test_direct_reply_carries_the_key_the_contact_was_announced_with);
    RUN_TEST(test_direct_reply_preserves_pki_through_router_auth_gate);
    RUN_TEST(test_path_command_answers_a_direct_message);
    RUN_TEST(test_path_command_answers_our_channel_text_with_its_echo);
    RUN_TEST(test_path_command_own_text_without_echo_is_no_data);
    RUN_TEST(test_path_command_answers_our_direct_message_with_its_echo);
    RUN_TEST(test_path_command_lists_every_route);
    RUN_TEST(test_path_report_keeps_whole_names);
    RUN_TEST(test_path_report_sanitizes_node_names);
    RUN_TEST(test_repeater_advert_is_stored_and_names_its_hop);
    RUN_TEST(test_path_report_ignores_meshtastic_keys);
    RUN_TEST(test_meshcore_copies_in_the_node_db_are_marked);
    RUN_TEST(test_replay_walks_the_nodes_the_node_db_left_out);
    RUN_TEST(test_trace_command_syntax_help);
    RUN_TEST(test_trace_command_unknown_node_reports_error);
    RUN_TEST(test_trace_command_parses_nodes_and_symmetric_return);
    RUN_TEST(test_trace_command_without_symmetric_dots);
    RUN_TEST(test_trace_response_formats_snr_report);
    RUN_TEST(test_trace_timeout_handling);
#endif
    exit(UNITY_END());
}

void loop() {}
