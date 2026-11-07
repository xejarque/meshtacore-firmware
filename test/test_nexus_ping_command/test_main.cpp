// Unit tests for /mtc PPING (NexusModule::handlePpingCommand) and the settings it shares with /mtc MESHCORE PPING
// (NexusModule::applyPpingSetting).
//
// What is under test:
//   - PPING ON/OFF switch only the Meshtastic public ping (public_ping) and leave the per-channel flags alone, so
//     /mtc MESHCORE PPING can be turned on and off independently.
//   - SETHEX/GETHEX keep the mask layout of the first release: bit0 public_ping, 1 rssi, 2 snr, 3 freqerr.
//   - Anything that is not a 16-bit hex mask changes nothing.
//   - The reporting toggles (RSSI, SNR, FREQERR, FLAGS, OM...) reach the flags both handlers read.
#include "MeshTypes.h"
#include "TestUtil.h"
#include <unity.h>

#include "configuration.h"
#include "modules/NexusModule.h"

namespace
{

class TestNexusModule : public meshtastic::NexusModule
{
  public:
    char reply[256] = {0};

    void run(const char *args)
    {
        memset(reply, 0, sizeof(reply));
        handlePpingCommand(args, reply, sizeof(reply));
    }
};

void test_off_switches_only_the_meshtastic_ping()
{
    TestNexusModule m;
    m.public_ping = true;
    m.ping_channels[0] = true;
    m.ping_channels[3] = true;

    m.run("OFF");

    TEST_ASSERT_FALSE(m.public_ping);
    TEST_ASSERT_TRUE(m.ping_channels[0]);
    TEST_ASSERT_TRUE(m.ping_channels[3]); // MeshCore channels are /mtc MESHCORE PPING's business
    TEST_ASSERT_NOT_NULL(strstr(m.reply, "DISABLED"));

    m.run("ON");
    TEST_ASSERT_TRUE(m.public_ping);
}

void test_the_hex_mask_keeps_the_layout_of_the_first_release()
{
    TestNexusModule m;
    m.public_ping = false;
    m.ping_rssi = m.ping_snr = m.ping_freqerr = false;

    m.run("SETHEX 0x0001");
    TEST_ASSERT_TRUE(m.public_ping);
    TEST_ASSERT_FALSE(m.ping_rssi);

    m.run("SETHEX 0x000E");
    TEST_ASSERT_FALSE(m.public_ping);
    TEST_ASSERT_TRUE(m.ping_rssi);
    TEST_ASSERT_TRUE(m.ping_snr);
    TEST_ASSERT_TRUE(m.ping_freqerr);

    m.run("GETHEX");
    TEST_ASSERT_NOT_NULL(strstr(m.reply, "0x000E"));
}

void test_the_hex_mask_round_trips_channels_and_dm()
{
    TestNexusModule m;
    m.run("SETHEX 0x8081"); // public_ping, channel 0, allow DM
    TEST_ASSERT_TRUE(m.public_ping);
    TEST_ASSERT_TRUE(m.ping_channels[0]);
    TEST_ASSERT_FALSE(m.ping_channels[1]);
    TEST_ASSERT_TRUE(m.ping_allow_dm);

    m.run("GETHEX");
    TEST_ASSERT_NOT_NULL(strstr(m.reply, "0x8081"));
}

void test_a_bad_mask_changes_nothing()
{
    TestNexusModule m;
    m.public_ping = true;
    m.ping_rssi = true;

    m.run("SETHEX zz");
    TEST_ASSERT_TRUE(m.public_ping);
    TEST_ASSERT_TRUE(m.ping_rssi);

    m.run("SETHEX 0x10000"); // wider than 16 bits
    TEST_ASSERT_TRUE(m.public_ping);

    m.run("SETHEX 12 junk");
    TEST_ASSERT_TRUE(m.public_ping);
}

void test_reporting_toggles_reach_the_shared_flags()
{
    TestNexusModule m;
    m.ping_rssi = m.ping_snr = true;
    m.ping_freqerr = false;

    m.run("RSSI OFF");
    TEST_ASSERT_FALSE(m.ping_rssi);
    m.run("SNR OFF");
    TEST_ASSERT_FALSE(m.ping_snr);
    m.run("FREQERR ON");
    TEST_ASSERT_TRUE(m.ping_freqerr);
    m.run("FLAGS OFF");
    TEST_ASSERT_FALSE(m.ping_bitmask);
    m.run("OM ON");
    TEST_ASSERT_TRUE(m.ping_bitmask);
    m.run("IGNORE REMOTE ON");
    TEST_ASSERT_TRUE(m.ping_ignore_remote);
    m.run("DIRECT IGNORE ON");
    TEST_ASSERT_TRUE(m.ping_ignore_direct);
}

void test_a_toggle_without_on_or_off_is_not_applied()
{
    TestNexusModule m;
    m.ping_rssi = true;
    m.run("RSSI maybe");
    TEST_ASSERT_TRUE(m.ping_rssi);
    TEST_ASSERT_NOT_NULL(strstr(m.reply, "PPING:")); // usage text
}

void test_cmd_sets_the_trigger_up_to_the_first_space()
{
    TestNexusModule m;
    m.run("CMD !ping now");
    TEST_ASSERT_EQUAL_STRING("!ping", m.ping_trigger);
    m.run("CMD DEFAULT");
    TEST_ASSERT_EQUAL_STRING("/ping", m.ping_trigger);
}

// The header mode is per layer: acting on one leaves the other where it was, and the custom text is shared.
void test_header_modes_are_per_layer()
{
    TestNexusModule m;
    m.mt_ping_header_mode = meshtastic::PING_HEADER_DEFAULT;
    m.mc_ping_header_mode = meshtastic::PING_HEADER_NONE;

    m.run("HEADER NONE");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, m.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, m.mc_ping_header_mode); // untouched

    m.run("HEADER DEFAULT");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_DEFAULT, m.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, m.mc_ping_header_mode);

    // OFF is the spelling the first release documented for "back to the default header".
    m.run("HEADER OFF");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_DEFAULT, m.mt_ping_header_mode);
}

void test_a_custom_header_applies_to_the_layer_it_was_typed_on()
{
    TestNexusModule m;
    m.mt_ping_header_mode = meshtastic::PING_HEADER_DEFAULT;
    m.mc_ping_header_mode = meshtastic::PING_HEADER_NONE;

    m.run("HEADER Relay Under Test");
    TEST_ASSERT_EQUAL_STRING("Relay Under Test", m.pping_header);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, m.mt_ping_header_mode);
    TEST_ASSERT_TRUE(m.custom_pping_active); // the mirror older builds read
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, m.mc_ping_header_mode);

    // CUSTOM reuses the stored text without retyping it.
    m.run("HEADER DEFAULT");
    m.run("HEADER CUSTOM");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, m.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_STRING("Relay Under Test", m.pping_header);
}

// A keyword needs quotes to be a literal header; an empty text and CUSTOM with nothing stored are refused.
void test_header_keywords_can_be_quoted_and_empty_text_is_refused()
{
    TestNexusModule m;
    m.mt_ping_header_mode = meshtastic::PING_HEADER_DEFAULT;

    m.run("HEADER CUSTOM");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_DEFAULT, m.mt_ping_header_mode); // nothing stored to show
    m.run("HEADER ");
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_DEFAULT, m.mt_ping_header_mode);

    m.run("HEADER \"none\"");
    TEST_ASSERT_EQUAL_STRING("none", m.pping_header);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, m.mt_ping_header_mode);
}

// The vault mirror tracks the Meshtastic mode alone, so a MeshCore-only custom header does not light it up.
void test_the_vault_mirror_follows_the_meshtastic_mode()
{
    TestNexusModule m;
    m.custom_pping_active = false;
    strncpy(m.pping_header, "Shared Text", sizeof(m.pping_header) - 1); // CUSTOM needs a stored text to show
    m.mt_ping_header_mode = meshtastic::PING_HEADER_DEFAULT;
    m.mc_ping_header_mode = meshtastic::PING_HEADER_CUSTOM;

    m.run("HEADER NONE");
    TEST_ASSERT_FALSE(m.custom_pping_active);
    m.run("HEADER CUSTOM");
    TEST_ASSERT_TRUE(m.custom_pping_active);
    m.run("HEADER DEFAULT");
    TEST_ASSERT_FALSE(m.custom_pping_active);
}

} // namespace

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_off_switches_only_the_meshtastic_ping);
    RUN_TEST(test_the_hex_mask_keeps_the_layout_of_the_first_release);
    RUN_TEST(test_the_hex_mask_round_trips_channels_and_dm);
    RUN_TEST(test_a_bad_mask_changes_nothing);
    RUN_TEST(test_reporting_toggles_reach_the_shared_flags);
    RUN_TEST(test_a_toggle_without_on_or_off_is_not_applied);
    RUN_TEST(test_cmd_sets_the_trigger_up_to_the_first_space);
    RUN_TEST(test_header_modes_are_per_layer);
    RUN_TEST(test_a_custom_header_applies_to_the_layer_it_was_typed_on);
    RUN_TEST(test_header_keywords_can_be_quoted_and_empty_text_is_refused);
    RUN_TEST(test_the_vault_mirror_follows_the_meshtastic_mode);
    exit(UNITY_END());
}

void loop() {}
