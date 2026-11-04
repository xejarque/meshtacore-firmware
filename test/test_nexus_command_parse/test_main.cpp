// Pure-function suite: it deliberately skips TestUtil.h, like test_utf8.
// Unit tests for isCommandWord(), isRelayIdArg(), parseOnOff(), parseUInt(), parseFloat(), parseHexByte(), takeUInt(),
// tokenInt(), tokenFloat(), parseUIntAuto(), parseHex64() and isAllHex() in src/modules/NexusCommandParse.h - how /nexus decides
// that a line is SAVE/LOAD and that a RELAYS argument names one relay instead of asking for a top-N ranking.
//
// Why it matters: both run on text typed by a remote operator. A prefix match turns SAVEX or LOADFOO into a flash write or
// a vault restore; a loose relay test turns a count such as `RELAYS 10` into a lookup of relay 0x10, or a typo into relay 0.
//
// Regressions guarded:
// - SAVE and LOAD accept any whitespace after the word (a trailing newline or tab), and nothing glued to it.
// - A bare two-digit decimal is a top-N count; a byte holding a hex letter (5A, a5, FF) is a relay id.
// - An ON/OFF argument that is neither (DISABLE, FALSE, a typo, empty) changes nothing, so a typo never silences the radio.
// - A numeric or hex argument that is not entirely a number (abc, 12x, empty, out of range) is refused instead of read as 0,
//   so `SDS abc` or `ZH ADD zz` cannot write 0 into a setting or touch node 00.
// - SETHEX zz must not read as mask 0 (it would switch every drop filter off) and a 64-char key with non-hex digits is refused.
// - `*` and `0x` need a hex digit behind them, and non-hex text such as `zz` is never read as relay 00.
#include "Arduino.h"
#include "modules/NexusCommandParse.h"
#include <climits>
#include <cmath>
#include <cstdlib>
#include <unity.h>

static void test_command_word_accepts_the_word_alone_or_before_whitespace()
{
    TEST_ASSERT_TRUE(isCommandWord("SAVE", "SAVE"));
    TEST_ASSERT_TRUE(isCommandWord("save", "SAVE"));
    TEST_ASSERT_TRUE(isCommandWord("SAVE BACKUP", "SAVE"));
    TEST_ASSERT_TRUE(isCommandWord("SAVE\n", "SAVE"));
    TEST_ASSERT_TRUE(isCommandWord("LOAD\tBACKUP", "LOAD"));
}

static void test_command_word_rejects_prefix_only_matches()
{
    TEST_ASSERT_FALSE(isCommandWord("SAVEX", "SAVE"));
    TEST_ASSERT_FALSE(isCommandWord("LOADFOO", "LOAD"));
    TEST_ASSERT_FALSE(isCommandWord("SAV", "SAVE"));
    TEST_ASSERT_FALSE(isCommandWord("", "SAVE"));
}

static void test_on_off_reads_only_the_four_valid_words()
{
    bool v = false;
    TEST_ASSERT_TRUE(parseOnOff("ON", v));
    TEST_ASSERT_TRUE(v);
    TEST_ASSERT_TRUE(parseOnOff("off", v));
    TEST_ASSERT_FALSE(v);
    TEST_ASSERT_TRUE(parseOnOff("1", v));
    TEST_ASSERT_TRUE(v);
    TEST_ASSERT_TRUE(parseOnOff("0", v));
    TEST_ASSERT_FALSE(v);
}

static void test_on_off_rejects_everything_else_and_leaves_the_value()
{
    bool v = true;
    TEST_ASSERT_FALSE(parseOnOff("DISABLE", v));
    TEST_ASSERT_FALSE(parseOnOff("FALSE", v));
    TEST_ASSERT_FALSE(parseOnOff("OF", v));
    TEST_ASSERT_FALSE(parseOnOff("2", v));
    TEST_ASSERT_FALSE(parseOnOff("", v));
    TEST_ASSERT_TRUE(v);
    v = false;
    TEST_ASSERT_FALSE(parseOnOff("yes", v));
    TEST_ASSERT_FALSE(v);
}

static void test_uint_takes_a_whole_in_range_number()
{
    uint32_t v = 99;
    TEST_ASSERT_TRUE(parseUInt("0", v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(parseUInt("  42  ", v));
    TEST_ASSERT_EQUAL_UINT32(42, v);
    TEST_ASSERT_TRUE(parseUInt("2", v, 2));
    TEST_ASSERT_EQUAL_UINT32(2, v);
}

static void test_uint_rejects_garbage_overflow_and_leaves_the_value()
{
    uint32_t v = 99;
    TEST_ASSERT_FALSE(parseUInt("abc", v));
    TEST_ASSERT_FALSE(parseUInt("12x", v));
    TEST_ASSERT_FALSE(parseUInt("-5", v));
    TEST_ASSERT_FALSE(parseUInt("", v));
    TEST_ASSERT_FALSE(parseUInt("3", v, 2));
    TEST_ASSERT_FALSE(parseUInt("99999999999999999999", v));
    TEST_ASSERT_FALSE(parseUInt("4294967296", v));
    TEST_ASSERT_EQUAL_UINT32(99, v);
}

static void test_float_takes_a_whole_in_range_number()
{
    float v = 0;
    TEST_ASSERT_TRUE(parseFloat("41.38", v, -90.0f, 90.0f));
    TEST_ASSERT_EQUAL_FLOAT(41.38f, v);
    TEST_ASSERT_TRUE(parseFloat(" -2.17 ", v, -180.0f, 180.0f));
    TEST_ASSERT_EQUAL_FLOAT(-2.17f, v);
    v = 7;
    TEST_ASSERT_FALSE(parseFloat("91", v, -90.0f, 90.0f));
    TEST_ASSERT_FALSE(parseFloat("abc", v, -90.0f, 90.0f));
    TEST_ASSERT_FALSE(parseFloat("1.5x", v, -90.0f, 90.0f));
    TEST_ASSERT_FALSE(parseFloat("nan", v, -90.0f, 90.0f));
    TEST_ASSERT_FALSE(parseFloat("", v, -90.0f, 90.0f));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, v);
}

static void test_hex_byte_accepts_plain_star_and_0x_forms()
{
    uint8_t v = 0;
    TEST_ASSERT_TRUE(parseHexByte("A4", v));
    TEST_ASSERT_EQUAL_UINT8(0xA4, v);
    TEST_ASSERT_TRUE(parseHexByte("*a4", v));
    TEST_ASSERT_EQUAL_UINT8(0xA4, v);
    TEST_ASSERT_TRUE(parseHexByte("0x0F", v));
    TEST_ASSERT_EQUAL_UINT8(0x0F, v);
    TEST_ASSERT_TRUE(parseHexByte("7", v));
    TEST_ASSERT_EQUAL_UINT8(0x07, v);
    TEST_ASSERT_TRUE(parseHexByte("00", v));
    TEST_ASSERT_EQUAL_UINT8(0x00, v);
}

static void test_hex_byte_rejects_garbage_and_oversize_leaving_the_value()
{
    uint8_t v = 0x55;
    TEST_ASSERT_FALSE(parseHexByte("zz", v));
    TEST_ASSERT_FALSE(parseHexByte("", v));
    TEST_ASSERT_FALSE(parseHexByte("*", v));
    TEST_ASSERT_FALSE(parseHexByte("0x", v));
    TEST_ASSERT_FALSE(parseHexByte("1FF", v));
    TEST_ASSERT_FALSE(parseHexByte("A4G", v));
    TEST_ASSERT_EQUAL_UINT8(0x55, v);
}

static void test_take_uint_walks_a_multi_argument_line()
{
    uint32_t a = 0, b = 0;
    const char *p = "3  5";
    TEST_ASSERT_TRUE(takeUInt(p, a, 7));
    TEST_ASSERT_TRUE(takeUInt(p, b, 7));
    TEST_ASSERT_EQUAL_UINT32(3, a);
    TEST_ASSERT_EQUAL_UINT32(5, b);
    TEST_ASSERT_EQUAL_CHAR('\0', *p);
    p = "3 9";
    TEST_ASSERT_TRUE(takeUInt(p, a, 7));
    TEST_ASSERT_FALSE(takeUInt(p, b, 7));
    p = "3 x";
    TEST_ASSERT_TRUE(takeUInt(p, a, 7));
    TEST_ASSERT_FALSE(takeUInt(p, b, 7));
    p = "3";
    TEST_ASSERT_TRUE(takeUInt(p, a, 7));
    TEST_ASSERT_FALSE(takeUInt(p, b, 7));
}

static void test_token_readers_give_a_sentinel_for_garbage()
{
    TEST_ASSERT_EQUAL_INT(12, tokenInt("12 rest"));
    TEST_ASSERT_EQUAL_INT(-9, tokenInt("-9"));
    TEST_ASSERT_EQUAL_INT(-1, tokenInt("abc"));
    TEST_ASSERT_EQUAL_INT(-1, tokenInt("5x"));
    TEST_ASSERT_EQUAL_INT(-1, tokenInt(""));
    TEST_ASSERT_EQUAL_INT(INT_MIN, tokenInt("--", INT_MIN));
    TEST_ASSERT_EQUAL_FLOAT(868.1f, tokenFloat("868.1 125"));
    TEST_ASSERT_TRUE(isnan(tokenFloat("abc")));
    TEST_ASSERT_TRUE(isnan(tokenFloat("")));
    TEST_ASSERT_TRUE(isnan(tokenFloat("8x")));
}

static void test_auto_base_and_hex_masks_need_the_whole_argument()
{
    uint32_t u = 0;
    TEST_ASSERT_TRUE(parseUIntAuto("10", u));
    TEST_ASSERT_EQUAL_UINT32(10, u);
    TEST_ASSERT_TRUE(parseUIntAuto("0x1F", u));
    TEST_ASSERT_EQUAL_UINT32(31, u);
    TEST_ASSERT_FALSE(parseUIntAuto("zz", u));
    TEST_ASSERT_FALSE(parseUIntAuto("0xZZ", u));
    TEST_ASSERT_FALSE(parseUIntAuto("12x", u));

    uint64_t h = 7;
    TEST_ASSERT_TRUE(parseHex64("FFFFFFFFFFFFFFFF", h));
    TEST_ASSERT_TRUE(h == 0xFFFFFFFFFFFFFFFFULL);
    TEST_ASSERT_TRUE(parseHex64("0x20", h));
    TEST_ASSERT_TRUE(h == 0x20ULL);
    h = 7;
    TEST_ASSERT_FALSE(parseHex64("zz", h));
    TEST_ASSERT_FALSE(parseHex64("", h));
    TEST_ASSERT_FALSE(parseHex64("1FFFFFFFFFFFFFFFF", h));
    TEST_ASSERT_TRUE(h == 7ULL);

    TEST_ASSERT_TRUE(isAllHex("00aaFF", 6));
    TEST_ASSERT_FALSE(isAllHex("00aaFG", 6));
}

static void test_relay_id_accepts_star_0x_and_hex_bytes()
{
    TEST_ASSERT_TRUE(isRelayIdArg("*AB"));
    TEST_ASSERT_TRUE(isRelayIdArg("*7"));
    TEST_ASSERT_TRUE(isRelayIdArg("0xAB"));
    TEST_ASSERT_TRUE(isRelayIdArg("0Xff"));
    TEST_ASSERT_TRUE(isRelayIdArg("5A"));
    TEST_ASSERT_TRUE(isRelayIdArg("a5"));
    TEST_ASSERT_TRUE(isRelayIdArg("FF"));
}

static void test_relay_id_leaves_counts_and_text_to_the_stats_handler()
{
    TEST_ASSERT_FALSE(isRelayIdArg("10"));
    TEST_ASSERT_FALSE(isRelayIdArg("5"));
    TEST_ASSERT_FALSE(isRelayIdArg("20"));
    TEST_ASSERT_FALSE(isRelayIdArg("zz"));
    TEST_ASSERT_FALSE(isRelayIdArg("up"));
    TEST_ASSERT_FALSE(isRelayIdArg("*"));
    TEST_ASSERT_FALSE(isRelayIdArg("*zz"));
    TEST_ASSERT_FALSE(isRelayIdArg("0x"));
    TEST_ASSERT_FALSE(isRelayIdArg("0xzz"));
    TEST_ASSERT_FALSE(isRelayIdArg("ABC"));
    TEST_ASSERT_FALSE(isRelayIdArg(""));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_command_word_accepts_the_word_alone_or_before_whitespace);
    RUN_TEST(test_command_word_rejects_prefix_only_matches);
    RUN_TEST(test_on_off_reads_only_the_four_valid_words);
    RUN_TEST(test_on_off_rejects_everything_else_and_leaves_the_value);
    RUN_TEST(test_uint_takes_a_whole_in_range_number);
    RUN_TEST(test_uint_rejects_garbage_overflow_and_leaves_the_value);
    RUN_TEST(test_float_takes_a_whole_in_range_number);
    RUN_TEST(test_hex_byte_accepts_plain_star_and_0x_forms);
    RUN_TEST(test_hex_byte_rejects_garbage_and_oversize_leaving_the_value);
    RUN_TEST(test_take_uint_walks_a_multi_argument_line);
    RUN_TEST(test_token_readers_give_a_sentinel_for_garbage);
    RUN_TEST(test_auto_base_and_hex_masks_need_the_whole_argument);
    RUN_TEST(test_relay_id_accepts_star_0x_and_hex_bytes);
    RUN_TEST(test_relay_id_leaves_counts_and_text_to_the_stats_handler);
    exit(UNITY_END());
}

void loop() {}
