// Unit tests for BootLoopGuard's streak/holdoff rules and the orphan tmp file name check in FSCommon.
// The flash-backed parts (reading and writing /prefs/bootStreak) are left to the device test.
#include "FSCommon.h"
#include "TestUtil.h"
#include "mesh/BootLoopGuard.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// A fresh node (no file) counts its first boot as streak 1, which must not look like a loop.
static void test_first_boot_is_not_a_loop()
{
    const uint32_t streak = BootLoopGuard::nextStreak(0);
    TEST_ASSERT_EQUAL_UINT32(1, streak);
    TEST_ASSERT_FALSE(BootLoopGuard::isLoopStreak(streak));
}

// A second boot before the streak was cleared means the previous boot died early: that is the loop.
static void test_second_boot_without_stable_uptime_is_a_loop()
{
    TEST_ASSERT_TRUE(BootLoopGuard::isLoopStreak(BootLoopGuard::nextStreak(1)));
}

// The counter saturates so a node stuck rebooting forever cannot wrap back to "first boot".
static void test_streak_saturates_at_cap()
{
    TEST_ASSERT_EQUAL_UINT32(BootLoopGuard::STREAK_CAP, BootLoopGuard::nextStreak(BootLoopGuard::STREAK_CAP));
    TEST_ASSERT_EQUAL_UINT32(BootLoopGuard::STREAK_CAP, BootLoopGuard::nextStreak(UINT32_MAX));
}

// A normal boot must not delay any broadcast.
static void test_no_holdoff_when_not_suspected()
{
    TEST_ASSERT_EQUAL_UINT32(0, BootLoopGuard::computeHoldoffMs(false, 12345));
}

// The holdoff is 30 min plus up to 5 min of jitter, for any jitter value the RNG can return.
static void test_holdoff_stays_within_base_plus_jitter()
{
    const uint32_t jitters[] = {0, 1, BootLoopGuard::HOLDOFF_JITTER_MS, BootLoopGuard::HOLDOFF_JITTER_MS + 1, UINT32_MAX};
    for (uint32_t jitter : jitters) {
        const uint32_t holdoff = BootLoopGuard::computeHoldoffMs(true, jitter);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BootLoopGuard::HOLDOFF_MS, holdoff);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(BootLoopGuard::HOLDOFF_MS + BootLoopGuard::HOLDOFF_JITTER_MS, holdoff);
    }
}

// The holdoff is added to an int32_t start delay, so it must stay far from INT32_MAX.
static void test_holdoff_fits_a_start_delay()
{
    TEST_ASSERT_LESS_THAN_UINT32(INT32_MAX / 2, BootLoopGuard::computeHoldoffMs(true, UINT32_MAX));
}

// Only names ending in ".tmp" with something before the suffix are SafeFile leftovers.
static void test_orphan_tmp_name_detection()
{
    TEST_ASSERT_TRUE(isOrphanTmpName("/prefs/nodes.proto.tmp"));
    TEST_ASSERT_TRUE(isOrphanTmpName("/nexus_rescue.bin.tmp"));
    TEST_ASSERT_FALSE(isOrphanTmpName("/prefs/nodes.proto"));
    TEST_ASSERT_FALSE(isOrphanTmpName(".tmp"));
    TEST_ASSERT_FALSE(isOrphanTmpName("/prefs/tmp"));
    TEST_ASSERT_FALSE(isOrphanTmpName("/prefs/a.tmp.bak"));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_first_boot_is_not_a_loop);
    RUN_TEST(test_second_boot_without_stable_uptime_is_a_loop);
    RUN_TEST(test_streak_saturates_at_cap);
    RUN_TEST(test_no_holdoff_when_not_suspected);
    RUN_TEST(test_holdoff_stays_within_base_plus_jitter);
    RUN_TEST(test_holdoff_fits_a_start_delay);
    RUN_TEST(test_orphan_tmp_name_detection);
    exit(UNITY_END());
}

void loop() {}
