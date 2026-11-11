// Unit tests for voltRuleStrandsNode() in src/modules/NexusVoltRuleCheck.h: which VOLT rules are refused when set.
//
// Why it matters: a HIB rule puts the node in deep sleep with the radio off, waking only for a voltage sniff. A rule set
// by mistake (HIB on HIGH, a threshold above the battery, a WAKE below the HIB) leaves a summit node asleep for good or
// cycling, and no remote command can reach it to undo the mistake.
//
// Regressions guarded: HIB on a rising edge; WAKE on a falling edge; a HIB threshold the reading already meets; WAKE at
// or below HIB in either order of configuration; and refusing the ordinary, safe pair (HIB LOW 3.3 V, WAKE HIGH 3.7 V).
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/NexusVoltRuleCheck.h"
#include <unity.h>

using meshtastic::VoltRuleProposal;
using meshtastic::voltRuleStrandsNode;

namespace
{
VoltRuleProposal hibLow(uint16_t mv, uint16_t currentMv)
{
    VoltRuleProposal p;
    p.hibernate = true;
    p.fallingEdge = true;
    p.mv = mv;
    p.currentMv = currentMv;
    return p;
}

VoltRuleProposal wakeHigh(uint16_t mv)
{
    VoltRuleProposal p;
    p.wake = true;
    p.mv = mv;
    return p;
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_the_usual_hib_and_wake_pair_is_accepted()
{
    VoltRuleProposal hib = hibLow(3300, 4000);
    hib.lowestOtherWakeMv = 3700;
    TEST_ASSERT_NULL(voltRuleStrandsNode(hib));

    VoltRuleProposal wake = wakeHigh(3700);
    wake.highestOtherHibMv = 3300;
    TEST_ASSERT_NULL(voltRuleStrandsNode(wake));
}

void test_hib_on_a_rising_edge_is_refused()
{
    VoltRuleProposal p = hibLow(3300, 4000);
    p.fallingEdge = false;
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(p));
}

void test_wake_on_a_falling_edge_is_refused()
{
    VoltRuleProposal p = wakeHigh(3700);
    p.fallingEdge = true;
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(p));
}

void test_hib_the_reading_already_meets_is_refused()
{
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(hibLow(5000, 4100))); // above a full LiPo
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(hibLow(4100, 4100)));
    TEST_ASSERT_NULL(voltRuleStrandsNode(hibLow(3300, 0))); // no reading: nothing to judge by
}

void test_wake_at_or_below_hib_is_refused_in_either_order()
{
    VoltRuleProposal hib = hibLow(3500, 4000);
    hib.lowestOtherWakeMv = 3500;
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(hib));

    VoltRuleProposal wake = wakeHigh(3400);
    wake.highestOtherHibMv = 3500;
    TEST_ASSERT_NOT_NULL(voltRuleStrandsNode(wake));
}

void test_command_rules_are_never_refused()
{
    VoltRuleProposal p;
    p.mv = 5000;
    p.currentMv = 3000;
    TEST_ASSERT_NULL(voltRuleStrandsNode(p));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_the_usual_hib_and_wake_pair_is_accepted);
    RUN_TEST(test_hib_on_a_rising_edge_is_refused);
    RUN_TEST(test_wake_on_a_falling_edge_is_refused);
    RUN_TEST(test_hib_the_reading_already_meets_is_refused);
    RUN_TEST(test_wake_at_or_below_hib_is_refused_in_either_order);
    RUN_TEST(test_command_rules_are_never_refused);
    exit(UNITY_END());
}

void loop() {}
