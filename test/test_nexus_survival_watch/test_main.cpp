// Unit tests for SurvivalWatch (src/modules/NexusSurvivalWatch.h): when an unattended node should reboot itself.
//
// What is under test:
//   - A heap below the floor for three consecutive checks asks for a reboot; one good reading resets the count.
//   - A radio whose RX counter (good or bad frames) does not move for 12 h asks for a reboot; any movement restarts the clock.
//   - Checks run at most once a minute, the first one only arms the clocks, and unknown heap or no radio never trip it.
//   - A node already below the heap floor on its first check never trips the heap check (no reboot loop).
//
// Why that behavior is required:
//   - A summit node cannot be power-cycled by hand: a leaked heap or a radio that went deaf must end in a clean restart,
//     and a quiet but healthy node (or one on the bench with no radio) must never be restarted by mistake.
//
// The regression guarded:
//   - A check that counted a single low-heap reading would reboot on a transient dip; one that ignored corrupt frames
//     would reboot a node that is hearing a busy, noisy channel.
#include "Arduino.h"
#include "TestUtil.h"
#include "UptimeClock.h"
#include "modules/NexusSurvivalWatch.h"
#include <unity.h>

using meshtastic::SurvivalWatch;
using Verdict = SurvivalWatch::Verdict;

namespace
{
constexpr uint32_t kMinute = SurvivalWatch::CHECK_EVERY_MS;
constexpr uint32_t kHealthyHeap = 60000;
constexpr uint32_t kLowHeap = 1000;

// One minute on, one look: the cadence the module runs at.
Verdict nextCheck(SurvivalWatch &w, uint32_t heap, bool radio, uint32_t rx)
{
    Time::advanceTestMillis(kMinute);
    return w.check(heap, radio, rx);
}
} // namespace

void setUp(void)
{
    Time::setTestMillis(1000);
}

void tearDown(void)
{
    Time::useRealClock();
}

void test_first_check_only_arms_and_never_trips()
{
    SurvivalWatch w;
    TEST_ASSERT_EQUAL(Verdict::Healthy, w.check(kLowHeap, true, 0));
}

// A build that boots below the floor (a slim nRF52 image) would reboot every three minutes forever: only a heap that
// falls after a healthy start is a leak worth a restart.
void test_a_node_that_boots_below_the_floor_never_trips()
{
    SurvivalWatch w;
    w.check(kLowHeap, true, 0);
    for (uint32_t i = 1; i <= 10; i++)
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kLowHeap, true, i));
}

void test_low_heap_trips_on_the_third_consecutive_check()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 0);
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kLowHeap, true, 1));
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kLowHeap, true, 2));
    TEST_ASSERT_EQUAL(Verdict::HeapExhausted, nextCheck(w, kLowHeap, true, 3));
}

void test_one_healthy_reading_resets_the_low_heap_count()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 0);
    nextCheck(w, kLowHeap, true, 1);
    nextCheck(w, kLowHeap, true, 2);
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, true, 3));
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kLowHeap, true, 4));
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kLowHeap, true, 5));
    TEST_ASSERT_EQUAL(Verdict::HeapExhausted, nextCheck(w, kLowHeap, true, 6));
}

void test_unknown_heap_never_counts_as_low()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 0);
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, UINT32_MAX, true, (uint32_t)i + 1));
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, 0, true, (uint32_t)i + 100));
    }
}

void test_checks_run_at_most_once_a_minute()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 0);
    // Three low readings inside the same minute are one look, not three.
    Time::advanceTestMillis(kMinute);
    TEST_ASSERT_EQUAL(Verdict::Healthy, w.check(kLowHeap, true, 1));
    TEST_ASSERT_EQUAL(Verdict::Healthy, w.check(kLowHeap, true, 2));
    TEST_ASSERT_EQUAL(Verdict::Healthy, w.check(kLowHeap, true, 3));
}

void test_a_radio_silent_for_12_hours_trips()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 5);
    const uint32_t checks = SurvivalWatch::RX_SILENCE_MS / kMinute;
    for (uint32_t i = 1; i < checks; i++)
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, true, 5));
    TEST_ASSERT_EQUAL(Verdict::RadioSilent, nextCheck(w, kHealthyHeap, true, 5));
}

void test_any_received_frame_restarts_the_silence_clock()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 5);
    const uint32_t checks = SurvivalWatch::RX_SILENCE_MS / kMinute;
    for (uint32_t i = 1; i < checks; i++)
        nextCheck(w, kHealthyHeap, true, 5);
    // A corrupt frame counts too: it is still proof the receiver works.
    TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, true, 6));
    for (uint32_t i = 1; i < checks; i++)
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, true, 6));
    TEST_ASSERT_EQUAL(Verdict::RadioSilent, nextCheck(w, kHealthyHeap, true, 6));
}

void test_no_radio_never_trips_the_silence_check()
{
    SurvivalWatch w;
    w.check(kHealthyHeap, false, 0);
    const uint32_t checks = 2 * SurvivalWatch::RX_SILENCE_MS / kMinute;
    for (uint32_t i = 0; i < checks; i++)
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, false, 0));
}

void test_silence_clock_survives_the_millis_wrap()
{
    Time::setTestMillis(UINT32_MAX - 3 * kMinute);
    SurvivalWatch w;
    w.check(kHealthyHeap, true, 9);
    const uint32_t checks = SurvivalWatch::RX_SILENCE_MS / kMinute;
    for (uint32_t i = 1; i < checks; i++)
        TEST_ASSERT_EQUAL(Verdict::Healthy, nextCheck(w, kHealthyHeap, true, 9));
    TEST_ASSERT_EQUAL(Verdict::RadioSilent, nextCheck(w, kHealthyHeap, true, 9));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_first_check_only_arms_and_never_trips);
    RUN_TEST(test_low_heap_trips_on_the_third_consecutive_check);
    RUN_TEST(test_a_node_that_boots_below_the_floor_never_trips);
    RUN_TEST(test_one_healthy_reading_resets_the_low_heap_count);
    RUN_TEST(test_unknown_heap_never_counts_as_low);
    RUN_TEST(test_checks_run_at_most_once_a_minute);
    RUN_TEST(test_a_radio_silent_for_12_hours_trips);
    RUN_TEST(test_any_received_frame_restarts_the_silence_clock);
    RUN_TEST(test_no_radio_never_trips_the_silence_check);
    RUN_TEST(test_silence_clock_survives_the_millis_wrap);
    exit(UNITY_END());
}

void loop() {}
