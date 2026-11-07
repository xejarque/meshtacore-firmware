// Unit tests for MeshCoreConfig in src/mesh/MeshCoreConfig.h - the MeshCore settings Nexus changes at run time (relay
// mode, ADVERT period, path hash size and flood depth limits) and their image in /prefs/meshcore_config.bin.
//
// Why it matters: these settings let an operator switch the MeshCore relay off or change how often the node floods its
// ADVERT. Every node re-sends a flood, so a wrong period costs the whole mesh airtime. A corrupt or future file must
// therefore apply nothing at all, rather than half of itself, or the relay would run in a mode nobody chose.
//
// Regressions guarded: a round trip that drops the relay mode or the period; a zero period that does not mean the 12 h
// default; a period outside 1..168 h accepted; a relay mode outside the three known values accepted; a corrupt,
// truncated or future-version image partly applied; an encode into a buffer too small writing past its end; a version 1
// file from a node already in the field no longer loading; a path size or flood limit outside its range accepted.
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

static void test_path_size_and_flood_limits_round_trip()
{
    MeshCoreConfig in;
    in.pathHashSize = 3;
    in.floodMax = 12;
    in.floodMaxUnscoped = 6;
    in.floodMaxAdvert = 2;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    TEST_ASSERT_EQUAL_UINT32(MeshCoreConfig::IMAGE_LEN, MeshCoreConfig::encode(in, image, sizeof(image)));

    MeshCoreConfig out;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(3, out.effectivePathHashSize());
    TEST_ASSERT_EQUAL_UINT8(12, out.effectiveFloodMax());
    TEST_ASSERT_EQUAL_UINT8(6, out.effectiveFloodMaxUnscoped());
    TEST_ASSERT_EQUAL_UINT8(2, out.effectiveFloodMaxAdvert());
}

static void test_zero_path_size_and_flood_limits_mean_meshcore_defaults()
{
    // This fork has always originated floods with 2-byte path hashes, and MeshCore's own flood limits are 64/64/8:
    // a config that sets nothing must keep both.
    const MeshCoreConfig defaults;
    TEST_ASSERT_EQUAL_UINT8(2, defaults.effectivePathHashSize());
    TEST_ASSERT_EQUAL_UINT8(64, defaults.effectiveFloodMax());
    TEST_ASSERT_EQUAL_UINT8(64, defaults.effectiveFloodMaxUnscoped());
    TEST_ASSERT_EQUAL_UINT8(8, defaults.effectiveFloodMaxAdvert());
}

static void test_version_one_image_still_loads_with_default_path_size_and_limits()
{
    // Nodes already running have a version 1 file (relay and period only): it must keep working after the upgrade.
    uint8_t v1[MeshCoreConfig::IMAGE_LEN_V1];
    memcpy(v1, MeshCoreConfig::MAGIC, sizeof(MeshCoreConfig::MAGIC));
    v1[3] = 1;
    v1[4] = MeshCoreConfig::RELAY_OFF;
    v1[5] = 24;
    const uint32_t crc = crc32Buffer(v1, MeshCoreConfig::BODY_LEN_V1);
    for (size_t i = 0; i < 4; i++)
        v1[MeshCoreConfig::BODY_LEN_V1 + i] = (uint8_t)(crc >> (8 * i));

    MeshCoreConfig out;
    out.pathHashSize = 3;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(v1, sizeof(v1), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_OFF, out.relay);
    TEST_ASSERT_EQUAL_UINT8(24, out.advertHours);
    TEST_ASSERT_EQUAL_UINT8(2, out.effectivePathHashSize());
    TEST_ASSERT_EQUAL_UINT8(64, out.effectiveFloodMax());
}

static void test_firewall_settings_round_trip()
{
    MeshCoreConfig in;
    in.dropMask = MeshCoreConfig::DROP_ADVERT | MeshCoreConfig::DROP_SOURCE;
    in.advertGapMin = 30;
    in.hopRatePerMin = 12;
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    TEST_ASSERT_EQUAL_UINT32(MeshCoreConfig::IMAGE_LEN, MeshCoreConfig::encode(in, image, sizeof(image)));

    MeshCoreConfig out;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(in.dropMask, out.dropMask);
    TEST_ASSERT_EQUAL_UINT8(30, out.advertGapMin);
    TEST_ASSERT_EQUAL_UINT8(12, out.hopRatePerMin);
}

static void test_default_config_has_the_firewall_off()
{
    const MeshCoreConfig defaults;
    TEST_ASSERT_EQUAL_UINT8(0, defaults.dropMask);
    TEST_ASSERT_EQUAL_UINT8(0, defaults.advertGapMin);
    TEST_ASSERT_EQUAL_UINT8(0, defaults.hopRatePerMin);
}

static void test_version_two_image_still_loads_with_the_firewall_off()
{
    // Nodes already running have a version 2 file (no firewall): the upgrade must not switch any rule on.
    uint8_t v2[MeshCoreConfig::BODY_LEN_V2 + 4] = {};
    memcpy(v2, MeshCoreConfig::MAGIC, sizeof(MeshCoreConfig::MAGIC));
    v2[3] = 2;
    v2[4] = MeshCoreConfig::RELAY_ON;
    v2[5] = 6;
    v2[6] = 3;
    const uint32_t crc = crc32Buffer(v2, MeshCoreConfig::BODY_LEN_V2);
    for (size_t i = 0; i < 4; i++)
        v2[MeshCoreConfig::BODY_LEN_V2 + i] = (uint8_t)(crc >> (8 * i));

    MeshCoreConfig out;
    out.dropMask = 0xFF;
    TEST_ASSERT_TRUE(MeshCoreConfig::decode(v2, sizeof(v2), &out));
    TEST_ASSERT_EQUAL_UINT8(MeshCoreConfig::RELAY_ON, out.relay);
    TEST_ASSERT_EQUAL_UINT8(3, out.effectivePathHashSize());
    TEST_ASSERT_EQUAL_UINT8(0, out.dropMask);
    TEST_ASSERT_EQUAL_UINT8(0, out.advertGapMin);
}

static void test_every_drop_rule_has_its_own_bit_and_tag()
{
    uint8_t seen = 0;
    for (const MeshCoreConfig::DropRule &rule : MeshCoreConfig::DROP_RULES) {
        TEST_ASSERT_NOT_NULL(rule.tag);
        TEST_ASSERT_EQUAL_UINT8(0, seen & rule.bit);
        seen |= rule.bit;
    }
    TEST_ASSERT_EQUAL_UINT8(0xFF, seen);
}

static void test_out_of_range_path_size_or_flood_limit_loads_nothing()
{
    uint8_t image[MeshCoreConfig::IMAGE_LEN];
    MeshCoreConfig out;
    out.floodMax = 9;

    MeshCoreConfig badPath;
    badPath.pathHashSize = 4;
    MeshCoreConfig::encode(badPath, image, sizeof(image));
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, sizeof(image), &out));

    MeshCoreConfig badFlood;
    badFlood.floodMaxAdvert = 65;
    MeshCoreConfig::encode(badFlood, image, sizeof(image));
    TEST_ASSERT_FALSE(MeshCoreConfig::decode(image, sizeof(image), &out));
    TEST_ASSERT_EQUAL_UINT8(9, out.floodMax);

    TEST_ASSERT_FALSE(MeshCoreConfig::validPathHashSize(0));
    TEST_ASSERT_TRUE(MeshCoreConfig::validPathHashSize(1));
    TEST_ASSERT_TRUE(MeshCoreConfig::validPathHashSize(3));
    TEST_ASSERT_FALSE(MeshCoreConfig::validFloodMax(0));
    TEST_ASSERT_TRUE(MeshCoreConfig::validFloodMax(64));
    TEST_ASSERT_FALSE(MeshCoreConfig::validFloodMax(65));
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
    RUN_TEST(test_path_size_and_flood_limits_round_trip);
    RUN_TEST(test_zero_path_size_and_flood_limits_mean_meshcore_defaults);
    RUN_TEST(test_version_one_image_still_loads_with_default_path_size_and_limits);
    RUN_TEST(test_firewall_settings_round_trip);
    RUN_TEST(test_default_config_has_the_firewall_off);
    RUN_TEST(test_version_two_image_still_loads_with_the_firewall_off);
    RUN_TEST(test_every_drop_rule_has_its_own_bit_and_tag);
    RUN_TEST(test_out_of_range_path_size_or_flood_limit_loads_nothing);
    exit(UNITY_END());
}

void loop() {}
