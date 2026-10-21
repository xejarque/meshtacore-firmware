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
    0x15, 0x00, 0x11, 0x80, 0x15, 0x2b, 0xe0, 0x79, 0x9d, 0x52, 0xfb, 0xeb, 0x96, 0xb1, 0x5f, 0xcc, 0xf0, 0xe9, 0xfa,
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_channel_hashes);
    RUN_TEST(test_encode_matches_meshcore_vector);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_rejects_wrong_channel);
    RUN_TEST(test_rejects_tampered_mac);
    RUN_TEST(test_utf8_truncation_keeps_boundary);
    RUN_TEST(test_zero_padded_32_byte_key_matches_16);
    exit(UNITY_END());
}

void loop() {}
