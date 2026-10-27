// Unit tests for MeshCoreConfig in src/mesh/MeshCoreConfig.h - the MeshCore settings Nexus changes at run time (relay
// mode and ADVERT period) and their image in /prefs/meshcore_config.bin.
//
// Why it matters: these settings let an operator switch the MeshCore relay off or change how often the node floods its
// ADVERT. Every node re-sends a flood, so a wrong period costs the whole mesh airtime. A corrupt or future file must
// therefore apply nothing at all, rather than half of itself, or the relay would run in a mode nobody chose.
//
// Regressions guarded: a round trip that drops the relay mode or the period; a zero period that does not mean the 12 h
// default; a period outside 1..168 h accepted; a relay mode outside the three known values accepted; a corrupt,
// truncated or future-version image partly applied; an encode into a buffer too small writing past its end.
#include "mesh/MeshCoreConfig.h"

// Declares setup() and loop() with C linkage, which the portduino main() calls. Without it this file's setup() gets C++
// linkage and the link fails. The other suites get it through configuration.h; this one does not include that.
#include "Arduino.h"
#include "TestUtil.h"
#include <unity.h>

static void test_round_trip_keeps_relay_and_period()
{
    MeshCoreConfig in;
    in.relay = MeshCoreConfig::RELAY_OFF;
    in.advertHours = 24;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    TEST_ASSERT_EQUAL_UINT32(MeshCoreConfig::IMAGE_LEN, MeshCoreConfig::encode(in, image, sizeof(image)));

    MeshCoreConfig out;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_OFF, out.relay);
    TEST_ASSERT_EQUAL_UINT8(24, out.advertHours);
}

static void test_default_config_is_the_built_in_behaviour()
{
    MeshCoreConfig defaults;
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_AUTO, defaults.relay);
    TEST_ASSERT_EQUAL_UINT8(0, defaults.advertHours);
    TEST_ASSERT_EQUAL_UINT8(12, defaults.effectiveAdvertHours());

    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    MeshCoreConfig::encode(defaults, image, sizeof(image));
    MeshCoreConfig out;
    out.relay = MeshCoreConfig::RELAY_ON;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_AUTO, out.relay);
    TEST_ASSERT_EQUAL_UINT8(12, out.effectiveAdvertHours());
}

static void test_zero_period_means_the_twelve_hour_default()
{
    MeshCoreConfig c;
    TEST_ASSERT_EQUAL_UINT8(12, c.effectiveAdvertHours());
    c.advertHours = 6;
    TEST_ASSERT_EQUAL_UINT8(6, c.effectiveAdvertHours());
}

static void test_period_bounds_are_one_to_one_hundred_sixty_eight_hours()
{
    TEST_ASSERT_FALSE(MeshCoreConfig::validAdvertHours(0));
    TEST_ASSERT_TRUE(MeshCoreConfig::validAdvertHours(1));
    TEST_ASSERT_TRUE(MeshCoreConfig::validAdvertHours(168));
    TEST_ASSERT_FALSE(MeshCoreConfig::validAdvertHours(169));
}

static void test_wrong_magic_or_version_loads_nothing()
{
    MeshCoreConfig in;
    in.relay = MeshCoreConfig::RELAY_OFF;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    MeshCoreConfig::encode(in, image, sizeof(image));

    MeshCoreConfig out;
    out.relay = MeshCoreConfig::RELAY_ON;
    uint8_t badMagic[MeshCoreConfig::IMAGE_LEN];
    memcpy(badMagic, image, sizeof(image));
    badMagic[0] = 'X';
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(badMagic, sizeof(badMagic), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_ON, out.relay);

    uint8_t futureVersion[MeshCoreConfig::IMAGE_LEN];
    memcpy(futureVersion, image, sizeof(image));
    futureVersion[3] = MeshCoreConfig::VERSION + 1;
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(futureVersion, sizeof(futureVersion), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_ON, out.relay);
}

static void test_flipped_byte_fails_the_checksum()
{
    MeshCoreConfig in;
    in.relay = MeshCoreConfig::RELAY_OFF;
    in.advertHours = 24;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    MeshCoreConfig::encode(in, image, sizeof(image));
    image[4] = MeshCoreConfig::RELAY_ON; // the relay byte changes, the stored checksum does not

    MeshCoreConfig out;
    out.advertHours = 5;
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(5, out.advertHours);
}

static void test_truncated_or_padded_image_loads_nothing()
{
    MeshCoreConfig in;
    in.advertHours = 3;
    uint8_t image[MeshCoreConfig::IMAGE_LEN + 1] = {};
    MeshCoreConfig::encode(in, image, MeshCoreConfig::IMAGE_LEN);

    MeshCoreConfig out;
    out.advertHours = 7;
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, MeshCoreConfig::IMAGE_LEN - 1, &out));
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, MeshCoreConfig::IMAGE_LEN + 1, &out));
    TEST_ASSERT_EQUAL_UINT8(7, out.advertHours);
}

static void test_out_of_range_values_load_nothing_even_with_a_valid_checksum()
{
    // encode() writes whatever it is given, so these images carry a correct checksum and only the value is wrong.
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    MeshCoreConfig badRelay;
    badRelay.relay = (MeshCoreConfig::RelayMode)3;
    MeshCoreConfig::encode(badRelay, image, sizeof(image));
    MeshCoreConfig out;
    out.advertHours = 7;
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, sizeof(image), &out));

    MeshCoreConfig badPeriod;
    badPeriod.advertHours = 200;
    MeshCoreConfig::encode(badPeriod, image, sizeof(image));
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(7, out.advertHours);
}

static void test_encode_refuses_a_small_buffer()
{
    MeshCoreConfig in;
    in.advertHours = 9;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    memset(image, 0xAA, sizeof(image));
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreConfig::encode(in, image, MeshCoreConfig::IMAGE_LEN - 1));
    for (size_t i = 0; i < sizeof(image); i++)
        TEST_ASSERT_EQUAL_UINT8(0xAA, image[i]);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_keeps_relay_and_period);
    RUN_TEST(test_default_config_is_the_built_in_behaviour);
    RUN_TEST(test_zero_period_means_the_twelve_hour_default);
    RUN_TEST(test_period_bounds_are_one_to_one_hundred_sixty_eight_hours);
    RUN_TEST(test_wrong_magic_or_version_loads_nothing);
    RUN_TEST(test_flipped_byte_fails_the_checksum);
    RUN_TEST(test_truncated_or_padded_image_loads_nothing);
    RUN_TEST(test_out_of_range_values_load_nothing_even_with_a_valid_checksum);
    RUN_TEST(test_encode_refuses_a_small_buffer);
    exit(UNITY_END());
}

void loop() {}
