// Unit tests for src/mesh/MeshCoreCompanion.{h,cpp} - MeshCore GRP_TXT encode/decode.
//
// The native test build compiles src/ with the MeshCore flags off, so those translation units are
// empty there. This suite defines the flags and includes the .cpp files directly, mirroring
// test_meshcore_repeater.
#define MESHTASTIC_ENABLE_MESHCORE_SNIFFER 1
#define MESHTASTIC_ENABLE_MESHCORE_RELAY 0
#define MESHTASTIC_ENABLE_MESHCORE_COMPANION 1
#include "mesh/MeshCoreCompanion.cpp"
#include "mesh/MeshCoreRepeater.cpp"

#include "Arduino.h"
#include "TestUtil.h"
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

void test_contacts_upsert_refreshes_name_and_keeps_node_num(void)
{
    MeshCoreContacts<4> table;
    uint8_t pub[32];
    fillPub(pub, 0x10, 1);

    MeshCoreContact *a = table.upsert(pub, "Alice");
    const uint32_t num = a->nodeNum;
    TEST_ASSERT_EQUAL_HEX8(0x4D, num >> 24);

    MeshCoreContact *again = table.upsert(pub, "Alicia");
    TEST_ASSERT_EQUAL_PTR(a, again);
    TEST_ASSERT_EQUAL_STRING("Alicia", again->name);
    TEST_ASSERT_EQUAL_HEX32(num, again->nodeNum);
    TEST_ASSERT_EQUAL_PTR(a, table.findByNodeNum(num));
}

void test_contacts_evict_least_recently_used(void)
{
    MeshCoreContacts<2> table;
    uint8_t a[32], b[32], c[32];
    fillPub(a, 1, 10);
    fillPub(b, 2, 20);
    fillPub(c, 3, 30);

    table.upsert(a, "A");
    table.upsert(b, "B");
    table.touch(table.findByPub(a)); // B is now the oldest
    table.upsert(c, "C");

    TEST_ASSERT_NOT_NULL(table.findByPub(a));
    TEST_ASSERT_NULL(table.findByPub(b));
    TEST_ASSERT_NOT_NULL(table.findByPub(c));
}

void test_contacts_share_a_one_byte_hash(void)
{
    MeshCoreContacts<4> table;
    uint8_t a[32], b[32], c[32];
    fillPub(a, 0x42, 1);
    fillPub(b, 0x42, 50);
    fillPub(c, 0x43, 90);
    table.upsert(a, "A");
    table.upsert(b, "B");
    table.upsert(c, "C");

    MeshCoreContact *found[4];
    TEST_ASSERT_EQUAL(2, table.findByHash(0x42, found, 4));
    TEST_ASSERT_EQUAL(1, table.findByHash(0x43, found, 4));
    TEST_ASSERT_EQUAL(0, table.findByHash(0x44, found, 4));
}

void test_contacts_never_reuse_the_pseudo_node_num(void)
{
    MeshCoreContacts<32> table;
    for (int i = 0; i < 32; i++) {
        uint8_t pub[32];
        fillPub(pub, i, i * 7);
        MeshCoreContact *c = table.upsert(pub, "x");
        TEST_ASSERT_NOT_EQUAL(MeshCoreCompanion::PSEUDO_NODE_NUM, c->nodeNum);
        TEST_ASSERT_EQUAL_PTR(c, table.findByNodeNum(c->nodeNum));
    }
}

void test_contacts_image_round_trip_keeps_keys_and_names(void)
{
    MeshCoreCompanion writer;
    uint8_t a[32], b[32];
    fillPub(a, 1, 10);
    fillPub(b, 2, 20);
    writer.contacts.upsert(a, "Alice");
    writer.contacts.upsert(b, "Bob");

    uint8_t image[MeshCoreCompanion::SAVED_IMAGE_MAX];
    const size_t n = writer.exportContacts(image, sizeof(image));
    TEST_ASSERT_EQUAL(5 + 2 * MeshCoreCompanion::SAVED_RECORD_LEN, n);

    MeshCoreCompanion reader;
    TEST_ASSERT_EQUAL(2, reader.importContacts(image, n));
    MeshCoreContact *ra = reader.contacts.findByPub(a);
    MeshCoreContact *rb = reader.contacts.findByPub(b);
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL_STRING("Alice", ra->name);
    TEST_ASSERT_EQUAL_STRING("Bob", rb->name);
    TEST_ASSERT_EQUAL_HEX32(writer.contacts.findByPub(a)->nodeNum, ra->nodeNum);
}

void test_contacts_image_rejects_garbage(void)
{
    MeshCoreCompanion c;
    uint8_t junk[64] = {};
    TEST_ASSERT_EQUAL(0, c.importContacts(junk, sizeof(junk)));

    uint8_t image[MeshCoreCompanion::SAVED_IMAGE_MAX];
    uint8_t a[32];
    fillPub(a, 1, 10);
    c.contacts.upsert(a, "Alice");
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
    c.contacts.upsert(a, "Alice");
    const uint32_t one = c.contactsFingerprint();

    c.contacts.touch(c.contacts.findByPub(a));
    TEST_ASSERT_EQUAL_HEX32(one, c.contactsFingerprint());

    c.contacts.upsert(b, "Bob");
    TEST_ASSERT_NOT_EQUAL(one, c.contactsFingerprint());
    const uint32_t two = c.contactsFingerprint();

    c.contacts.upsert(a, "Alicia");
    TEST_ASSERT_NOT_EQUAL(two, c.contactsFingerprint());
}

void test_image_is_capped_to_the_most_recent_contacts(void)
{
    MeshCoreCompanion c;
    for (int i = 0; i < 20; i++) {
        uint8_t pub[32];
        fillPub(pub, i + 1, i * 3);
        c.contacts.upsert(pub, "n");
    }
    uint8_t image[MeshCoreCompanion::SAVED_IMAGE_MAX];
    const size_t n = c.exportContacts(image, sizeof(image));
    TEST_ASSERT_EQUAL(5 + MeshCoreCompanion::SAVED_MAX * MeshCoreCompanion::SAVED_RECORD_LEN, n);
    TEST_ASSERT_EQUAL(MeshCoreCompanion::SAVED_MAX, image[4]);

    // The newest contact (first byte 20) is first in the image; the oldest four did not make it.
    TEST_ASSERT_EQUAL_HEX8(20, image[5]);
    uint8_t oldest[32];
    fillPub(oldest, 1, 0);
    MeshCoreCompanion reader;
    reader.importContacts(image, n);
    TEST_ASSERT_NULL(reader.contacts.findByPub(oldest));
}

void test_direct_send_needs_identity_and_contact(void)
{
    MeshCoreCompanion c;
    uint8_t pub[32];
    fillPub(pub, 1, 10);
    const uint32_t num = c.contacts.upsert(pub, "Alice")->nodeNum;
    TEST_ASSERT_FALSE(c.sendDirectText(num, 1, "hola", 1000, nullptr)); // no identity, no radio
    TEST_ASSERT_FALSE(c.sendDirectText(0x12345678, 1, "hola", 1000, nullptr));
    TEST_ASSERT_TRUE(c.hasContact(num));
    TEST_ASSERT_FALSE(c.hasContact(0x12345678));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_channel_hashes);
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
    RUN_TEST(test_split_sender_separates_name_and_body);
    RUN_TEST(test_split_sender_rejects_missing_or_empty_name);
    RUN_TEST(test_split_sender_cuts_long_name_on_utf8_boundary);
    RUN_TEST(test_sender_node_num_is_stable_and_distinct);
    RUN_TEST(test_shared_secret_is_symmetric);
    RUN_TEST(test_direct_text_round_trip);
    RUN_TEST(test_direct_text_rejects_wrong_secret_and_tampering);
    RUN_TEST(test_ack_hash_depends_on_text_and_sender);
    RUN_TEST(test_ack_frame_layout);
    RUN_TEST(test_contacts_upsert_refreshes_name_and_keeps_node_num);
    RUN_TEST(test_contacts_evict_least_recently_used);
    RUN_TEST(test_contacts_share_a_one_byte_hash);
    RUN_TEST(test_contacts_never_reuse_the_pseudo_node_num);
    RUN_TEST(test_contacts_image_round_trip_keeps_keys_and_names);
    RUN_TEST(test_contacts_image_rejects_garbage);
    RUN_TEST(test_contacts_fingerprint_follows_the_set_not_the_recency);
    RUN_TEST(test_image_is_capped_to_the_most_recent_contacts);
    RUN_TEST(test_direct_send_needs_identity_and_contact);
    exit(UNITY_END());
}

void loop() {}
