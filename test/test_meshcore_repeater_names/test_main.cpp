// Unit tests for MeshCoreRepeaterNames in src/mesh/MeshCoreRepeaterNames.h - the small RAM table that remembers the
// repeaters a node has heard, just enough to name them on a path.
//
// Why it matters: a MeshCore path carries only 1 to 3 bytes of each repeater's key, so a ping or a ".path" report can
// name a hop only if the node still remembers that repeater. The contact table is too small for that on a board such as
// the ThinkNode M1 (a full mountain of repeaters pushed the user's contacts out, and the oldest repeaters out with them),
// so repeaters now live in their own table of compact entries: the start of the key, the node number, a few bytes of name
// (12 on an nRF52, MeshCore's 32 elsewhere; these tests pin 8 so the label cases stay short), how and when it was heard.
// A relay without a companion lists its direct neighbours from the same table. What it keeps, what it drops when full and
// how it finds a hop decide whether the route lines show names or bare hex.
//
// Regressions guarded: a name that spends its 8 bytes on a pictogram ("🏔️RoquesBlanques" becoming just a mountain); a
// name cut in the middle of a UTF-8 character; a hop hash longer than the kept prefix finding nothing; a short hash
// naming only the first of several repeaters that share it; a typed name that cannot find the short form it was stored
// under; the 16-bit recency clock wrapping and making the newest repeater look the oldest; a flash write for every
// advert heard again; the entry growing past 16 bytes plus its name or getting a non-zero default that moves the table into
// flash; a restored entry passing for one heard this boot (a neighbour listing would date it "just now").
#define MESHCORE_REPEATER_NAME_LEN 8 // the label cases below are written for 8 bytes
#include "mesh/MeshCoreRepeaterNames.h"

#include "TestUtil.h"
#include <unity.h>

static void makeKey(uint8_t pub[32], uint8_t a, uint8_t b = 0, uint8_t c = 0)
{
    memset(pub, 0x11, 32);
    pub[0] = a;
    pub[1] = b;
    pub[2] = c;
}

static MeshCoreHeard heardWith(float snr, uint8_t hops, uint32_t uptimeSecs = 0)
{
    MeshCoreHeard how;
    how.snr = snr;
    how.hops = hops;
    how.uptimeSecs = uptimeSecs;
    return how;
}

static MeshCoreRepeaterName *hear(MeshCoreRepeaterNames<4> &t, uint8_t a, const char *name, uint8_t b = 0, uint8_t c = 0)
{
    uint8_t pub[32];
    makeKey(pub, a, b, c);
    return t.learn(pub, name, heardWith(5.0f, 1));
}

static MeshCoreRepeaterName savedEntry(uint8_t first, const char *name, int8_t snrQuarter, uint8_t hopsPlusOne)
{
    MeshCoreRepeaterName e;
    e.prefix[0] = first;
    e.type = MeshCoreRepeaterName::TYPE_REPEATER;
    e.nodeNum = 0x4D123456;
    strncpy(e.name, name, sizeof(e.name));
    e.snrQuarter = snrQuarter;
    e.hopsPlusOne = hopsPlusOne;
    return e;
}

static bool nameIs(const MeshCoreRepeaterName *e, const char *expected)
{
    char got[MeshCoreRepeaterName::NAME_LEN + 1];
    e->copyName(got);
    return strcmp(got, expected) == 0;
}

void setUp() {}
void tearDown() {}

// --- learn: one entry per key, refreshed in place ---

// Hearing the same repeater again refreshes its entry instead of taking a second slot, and a renamed repeater is renamed.
void test_learn_creates_once_and_refreshes_in_place()
{
    MeshCoreRepeaterNames<4> t;
    MeshCoreRepeaterName *first = hear(t, 0xA1, "Tetuan");
    MeshCoreRepeaterName *again = hear(t, 0xA1, "Tetuan Nord");
    TEST_ASSERT_EQUAL_PTR(first, again);
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
    TEST_ASSERT_TRUE(nameIs(again, "Tetuan N"));
}

// The signal a repeater was last heard with replaces the one before it, in quarter dB, and a value out of range is
// clamped rather than wrapped.
void test_signal_and_hops_follow_the_last_advert()
{
    MeshCoreRepeaterNames<4> t;
    uint8_t pub[32];
    makeKey(pub, 0xA1);
    MeshCoreRepeaterName *e = t.learn(pub, "R1", heardWith(6.5f, 3));
    TEST_ASSERT_EQUAL_FLOAT(6.5f, e->snr());
    TEST_ASSERT_EQUAL_UINT8(3, e->hops());

    e = t.learn(pub, "R1", heardWith(-7.25f, 0));
    TEST_ASSERT_EQUAL_FLOAT(-7.25f, e->snr());
    TEST_ASSERT_EQUAL_UINT8(0, e->hops());

    e = t.learn(pub, "R1", heardWith(500.0f, MeshCoreRepeaterName::HOPS_UNKNOWN));
    TEST_ASSERT_EQUAL_FLOAT(31.75f, e->snr());
    TEST_ASSERT_EQUAL_UINT8(MeshCoreRepeaterName::HOPS_UNKNOWN, e->hops());

    e = t.learn(pub, "R1", heardWith(-500.0f, 1));
    TEST_ASSERT_EQUAL_FLOAT(-32.0f, e->snr());
}

// --- eviction: the repeater heard longest ago goes first ---

// A full table gives up the oldest heard, and a repeater heard again counts as new, so the busy ones stay.
void test_full_table_evicts_the_repeater_heard_longest_ago()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "A");
    hear(t, 0x02, "B");
    hear(t, 0x03, "C");
    hear(t, 0x04, "D");
    hear(t, 0x01, "A"); // A is heard again: B is now the oldest
    hear(t, 0x05, "E");

    const MeshCoreRepeaterName *out[4];
    uint8_t key[3] = {0x02, 0x11, 0x11};
    TEST_ASSERT_EQUAL_UINT32(0, t.findByPrefix(key, 1, out, 4));
    key[0] = 0x01;
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 1, out, 4));
    key[0] = 0x05;
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 1, out, 4));
    TEST_ASSERT_EQUAL_UINT32(4, t.count());
}

// The recency stamp is 16 bits: after 65535 hearings it renumbers instead of wrapping, so a repeater heard just now is
// never taken for the oldest.
void test_recency_clock_wrap_keeps_the_order()
{
    MeshCoreRepeaterNames<3> t;
    uint8_t pub[32];
    makeKey(pub, 0x01);
    t.learn(pub, "A", heardWith(0, 1));
    makeKey(pub, 0x02);
    t.learn(pub, "B", heardWith(0, 1));
    makeKey(pub, 0x03);
    t.learn(pub, "C", heardWith(0, 1));
    for (int i = 0; i < 70000; i++) { // past the wrap, hearing only C and then B
        makeKey(pub, i < 69990 ? 0x03 : 0x02);
        t.learn(pub, i < 69990 ? "C" : "B", heardWith(0, 1));
    }
    makeKey(pub, 0x04);
    t.learn(pub, "D", heardWith(0, 1)); // evicts A, the one nobody has heard since the start

    const MeshCoreRepeaterName *out[3];
    uint8_t key[1] = {0x01};
    TEST_ASSERT_EQUAL_UINT32(0, t.findByPrefix(key, 1, out, 3));
    key[0] = 0x02;
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 1, out, 3));
    key[0] = 0x03;
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 1, out, 3));
}

// --- findByPrefix: a path hash is 1 to 3 bytes of the key ---

// One, two and three bytes of the key all find the repeater, and a hash that does not match finds nothing.
void test_prefix_lookup_accepts_every_hash_size()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0xAB, "R1", 0xCD, 0xEF);

    const MeshCoreRepeaterName *out[4];
    const uint8_t key[3] = {0xAB, 0xCD, 0xEF};
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 1, out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 2, out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 3, out, 4));
    TEST_ASSERT_TRUE(nameIs(out[0], "R1"));

    const uint8_t other[3] = {0xAB, 0xCD, 0x00};
    TEST_ASSERT_EQUAL_UINT32(0, t.findByPrefix(other, 3, out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(other, 2, out, 4));
}

// A hash longer than the kept prefix is compared on the 3 bytes that are kept, not rejected.
void test_prefix_lookup_clamps_a_longer_hash()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0xAB, "R1", 0xCD, 0xEF);
    const MeshCoreRepeaterName *out[4];
    const uint8_t key[8] = {0xAB, 0xCD, 0xEF, 0x11, 0x11, 0x11, 0x11, 0x11};
    TEST_ASSERT_EQUAL_UINT32(1, t.findByPrefix(key, 8, out, 4));
}

// MeshCore hashes collide: a one-byte hash shared by two repeaters returns both, counted past the room given.
void test_prefix_lookup_returns_every_collision()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0xAB, "R1", 0x01);
    hear(t, 0xAB, "R2", 0x02);
    hear(t, 0xAC, "R3");

    const MeshCoreRepeaterName *out[4];
    const uint8_t key[1] = {0xAB};
    TEST_ASSERT_EQUAL_UINT32(2, t.findByPrefix(key, 1, out, 4));
    const MeshCoreRepeaterName *one[1];
    TEST_ASSERT_EQUAL_UINT32(2, t.findByPrefix(key, 1, one, 1)); // total, though only one fits
    TEST_ASSERT_EQUAL_UINT32(2, t.findByPrefix(key, 1, nullptr, 0));
}

// --- labels: what 8 bytes keep of a name ---

// A name that opens with a pictogram keeps the letters after it, not the pictogram.
void test_label_skips_leading_pictograms()
{
    char label[MeshCoreRepeaterName::NAME_LEN + 1];
    TEST_ASSERT_EQUAL_UINT32(8, MeshCoreRepeaterLabel::make("🏔️RoquesBlanques", label));
    TEST_ASSERT_EQUAL_STRING("RoquesBl", label);
    MeshCoreRepeaterLabel::make("📡 Tetuan Repeater", label);
    TEST_ASSERT_EQUAL_STRING("Tetuan R", label);
}

// Markdown delimiters and the blanks at the ends are not worth a byte.
void test_label_drops_markdown_and_edge_blanks()
{
    char label[MeshCoreRepeaterName::NAME_LEN + 1];
    MeshCoreRepeaterLabel::make("  *[Nord_1]*  ", label);
    TEST_ASSERT_EQUAL_STRING("Nord1", label);
}

// The cut never leaves half of a multi-byte character, and accented letters count for what they weigh.
void test_label_is_cut_on_a_utf8_boundary()
{
    char label[MeshCoreRepeaterName::NAME_LEN + 1];
    const size_t len = MeshCoreRepeaterLabel::make("Cañón-1234", label);
    TEST_ASSERT_EQUAL_STRING("Cañón-", label); // C a ñ(2) ó(2) n - = 8 bytes; the 1 would be the ninth
    TEST_ASSERT_EQUAL_UINT32(8, len);
}

// A name made only of pictograms is kept as it comes rather than as nothing.
void test_label_of_only_pictograms_is_not_empty()
{
    char label[MeshCoreRepeaterName::NAME_LEN + 1];
    TEST_ASSERT_GREATER_THAN_UINT32(0, MeshCoreRepeaterLabel::make("🏔️", label));
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreRepeaterLabel::make("", label));
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreRepeaterLabel::make(nullptr, label));
}

// --- findByName: the short form a name was stored under ---

// The full name typed finds the 8-byte form, whatever the case and whatever pictogram opened it.
void test_name_lookup_matches_the_stored_short_form()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "🏔️RoquesBlanques");
    const MeshCoreRepeaterName *out[4];
    TEST_ASSERT_EQUAL_UINT32(1, t.findByName("RoquesBlanques", out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByName("roquesblanques", out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByName("🏔️RoquesBlanques", out, 4));
    TEST_ASSERT_TRUE(nameIs(out[0], "RoquesBl"));
}

// The first letters are enough, and a name that several repeaters share is reported as ambiguous by its count.
void test_name_lookup_by_prefix_reports_ambiguity()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "Montseny Nord");
    hear(t, 0x02, "Montseny Sud");
    hear(t, 0x03, "Tibidabo");

    const MeshCoreRepeaterName *out[4];
    TEST_ASSERT_EQUAL_UINT32(1, t.findByName("Tib", out, 4));
    TEST_ASSERT_EQUAL_UINT32(2, t.findByName("Mont", out, 4));
    TEST_ASSERT_EQUAL_UINT32(2, t.findByName("Montseny", out, 4));
}

// A typed name longer than what a shorter stored name holds does not match it, and an unknown name finds nothing.
void test_name_lookup_does_not_match_a_shorter_name()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "Roques");
    const MeshCoreRepeaterName *out[4];
    TEST_ASSERT_EQUAL_UINT32(0, t.findByName("RoquesBlanques", out, 4));
    TEST_ASSERT_EQUAL_UINT32(0, t.findByName("Zzz", out, 4));
    TEST_ASSERT_EQUAL_UINT32(0, t.findByName("", out, 4));
    TEST_ASSERT_EQUAL_UINT32(1, t.findByName("Roq", out, 4));
}

// --- newestFirst: the order a listing and a save use ---

// Repeaters come back most recently heard first, and only as many as asked for.
void test_newest_first_orders_by_recency_and_cuts_at_the_limit()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "A");
    hear(t, 0x02, "B");
    hear(t, 0x03, "C");
    hear(t, 0x01, "A"); // A is now the newest

    const MeshCoreRepeaterName *out[4];
    TEST_ASSERT_EQUAL_UINT32(3, t.newestFirst(out, 4));
    TEST_ASSERT_TRUE(nameIs(out[0], "A"));
    TEST_ASSERT_TRUE(nameIs(out[1], "C"));
    TEST_ASSERT_TRUE(nameIs(out[2], "B"));

    TEST_ASSERT_EQUAL_UINT32(2, t.newestFirst(out, 2));
    TEST_ASSERT_TRUE(nameIs(out[0], "A"));
    TEST_ASSERT_TRUE(nameIs(out[1], "C"));
}

// Walking by recency one step at a time visits every repeater once, in the same order as the ranks.
void test_recency_walk_matches_the_ranks()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "A");
    hear(t, 0x02, "B");
    hear(t, 0x03, "C");
    hear(t, 0x01, "A"); // A is now the newest

    const MeshCoreRepeaterName *r = t.newest();
    for (size_t rank = 0; rank < 3; rank++, r = t.nextOlder(r->heard))
        TEST_ASSERT_EQUAL_PTR(t.nthNewest(rank), r);
    TEST_ASSERT_NULL(r);

    r = t.nthNewest(2);
    TEST_ASSERT_TRUE(nameIs(r, "B"));
    r = t.nextNewer(r->heard);
    TEST_ASSERT_TRUE(nameIs(r, "C"));
    r = t.nextNewer(r->heard);
    TEST_ASSERT_TRUE(nameIs(r, "A"));
    TEST_ASSERT_NULL(t.nextNewer(r->heard));
}

// --- restore: loading a saved table ---

// Restored oldest first, the entries keep the saved order: the last restored is the newest. A repeater restored twice is
// one entry.
void test_restore_keeps_the_order_it_is_given_and_does_not_duplicate()
{
    MeshCoreRepeaterNames<4> t;
    t.restore(savedEntry(0x01, "A", 8, 2));
    t.restore(savedEntry(0x02, "B", -4, 0));
    t.restore(savedEntry(0x01, "A", 8, 2));
    TEST_ASSERT_EQUAL_UINT32(2, t.count());

    const MeshCoreRepeaterName *out[4];
    TEST_ASSERT_EQUAL_UINT32(2, t.newestFirst(out, 4));
    TEST_ASSERT_TRUE(nameIs(out[0], "A"));
    TEST_ASSERT_EQUAL_FLOAT(2.0f, out[0]->snr());
    TEST_ASSERT_EQUAL_UINT8(1, out[0]->hops());
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, out[1]->snr());
    TEST_ASSERT_EQUAL_UINT8(MeshCoreRepeaterName::HOPS_UNKNOWN, out[1]->hops());
}

// A restored entry keeps its type and node number but was not heard this boot, whatever the saved copy said: a neighbour
// listing must not show a node from the last boot as heard just now.
void test_restore_is_not_heard_this_boot()
{
    MeshCoreRepeaterNames<4> t;
    MeshCoreRepeaterName saved = savedEntry(0x01, "A", 8, 1);
    saved.heardAt = 77;
    const MeshCoreRepeaterName *e = t.restore(saved);
    TEST_ASSERT_FALSE(e->heardThisBoot());
    TEST_ASSERT_EQUAL_UINT8(MeshCoreRepeaterName::TYPE_REPEATER, e->type);
    TEST_ASSERT_EQUAL_HEX32(0x4D123456, e->nodeNum);
}

// --- what learn records besides the name ---

// The node number is the one the key is announced under, the type is the one the advert gave, and the age counts from the
// uptime it was heard at.
void test_learn_keeps_node_number_type_and_when()
{
    MeshCoreRepeaterNames<4> t;
    uint8_t pub[32];
    makeKey(pub, 0xA1);
    MeshCoreHeard how = heardWith(-3.0f, 0, 100);
    how.type = 1; // a chat node, as a relay without a companion keeps its neighbours
    const MeshCoreRepeaterName *e = t.learn(pub, "Cima", how);
    TEST_ASSERT_EQUAL_HEX32(MeshCoreNodeStore<1>::nodeNumForKey(pub), e->nodeNum);
    TEST_ASSERT_EQUAL_UINT8(1, e->type);
    TEST_ASSERT_TRUE(e->heardThisBoot());
    TEST_ASSERT_EQUAL_UINT32(0, e->ageSecs(100));
    TEST_ASSERT_EQUAL_UINT32(25, e->ageSecs(125));
}

// Heard in the very first second of uptime is still heard: 0 is kept for "only restored".
void test_heard_at_uptime_zero_is_still_heard()
{
    MeshCoreRepeaterNames<4> t;
    uint8_t pub[32];
    makeKey(pub, 0xA2);
    const MeshCoreRepeaterName *e = t.learn(pub, "Cima", heardWith(0, 0, 0));
    TEST_ASSERT_TRUE(e->heardThisBoot());
    TEST_ASSERT_EQUAL_UINT32(4, e->ageSecs(4));
}

// --- fingerprint: only a change worth a flash write moves it ---

// Hearing a repeater again, with another signal or hop count, does not change the fingerprint: a flash write for that
// alone would wear the flash for nothing.
void test_fingerprint_ignores_signal_and_recency()
{
    MeshCoreRepeaterNames<4> t;
    uint8_t pub[32];
    makeKey(pub, 0x01);
    t.learn(pub, "A", heardWith(1.0f, 1));
    makeKey(pub, 0x02);
    t.learn(pub, "B", heardWith(1.0f, 1));
    const uint32_t before = t.fingerprint();

    makeKey(pub, 0x01);
    t.learn(pub, "A", heardWith(-9.0f, 4));
    TEST_ASSERT_EQUAL_UINT32(before, t.fingerprint());
}

// A new repeater, a renamed one or an evicted one moves it, and so does a different set of the same size.
void test_fingerprint_follows_the_set_and_the_names()
{
    MeshCoreRepeaterNames<4> t;
    hear(t, 0x01, "A");
    const uint32_t one = t.fingerprint();
    hear(t, 0x02, "B");
    const uint32_t two = t.fingerprint();
    TEST_ASSERT_NOT_EQUAL_UINT32(one, two);

    hear(t, 0x02, "B2");
    TEST_ASSERT_NOT_EQUAL_UINT32(two, t.fingerprint());

    MeshCoreRepeaterNames<4> other;
    hear(other, 0x01, "A");
    hear(other, 0x03, "B");
    TEST_ASSERT_NOT_EQUAL_UINT32(two, other.fingerprint());
}

// --- budget ---

// The entry stays at 16 bytes plus its name: the table is sized for it, and a bigger entry costs the heap on a board that has
// little (an nRF52 keeps 12 bytes of name, 28 B an entry).
void test_entry_fits_the_ram_budget()
{
    TEST_ASSERT_EQUAL_UINT32(16 + 8, sizeof(MeshCoreRepeaterName));
}

// All-zero defaults keep the table in .bss; a non-zero default would store the whole table in flash as initialised data.
void test_a_new_entry_is_all_zeros()
{
    const MeshCoreRepeaterName fresh;
    uint8_t zeros[sizeof(MeshCoreRepeaterName)] = {};
    TEST_ASSERT_EQUAL_MEMORY(zeros, &fresh, sizeof(zeros));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_learn_creates_once_and_refreshes_in_place);
    RUN_TEST(test_signal_and_hops_follow_the_last_advert);
    RUN_TEST(test_full_table_evicts_the_repeater_heard_longest_ago);
    RUN_TEST(test_recency_clock_wrap_keeps_the_order);
    RUN_TEST(test_prefix_lookup_accepts_every_hash_size);
    RUN_TEST(test_prefix_lookup_clamps_a_longer_hash);
    RUN_TEST(test_prefix_lookup_returns_every_collision);
    RUN_TEST(test_label_skips_leading_pictograms);
    RUN_TEST(test_label_drops_markdown_and_edge_blanks);
    RUN_TEST(test_label_is_cut_on_a_utf8_boundary);
    RUN_TEST(test_label_of_only_pictograms_is_not_empty);
    RUN_TEST(test_name_lookup_matches_the_stored_short_form);
    RUN_TEST(test_name_lookup_by_prefix_reports_ambiguity);
    RUN_TEST(test_name_lookup_does_not_match_a_shorter_name);
    RUN_TEST(test_newest_first_orders_by_recency_and_cuts_at_the_limit);
    RUN_TEST(test_recency_walk_matches_the_ranks);
    RUN_TEST(test_restore_keeps_the_order_it_is_given_and_does_not_duplicate);
    RUN_TEST(test_restore_is_not_heard_this_boot);
    RUN_TEST(test_learn_keeps_node_number_type_and_when);
    RUN_TEST(test_heard_at_uptime_zero_is_still_heard);
    RUN_TEST(test_fingerprint_ignores_signal_and_recency);
    RUN_TEST(test_fingerprint_follows_the_set_and_the_names);
    RUN_TEST(test_entry_fits_the_ram_budget);
    RUN_TEST(test_a_new_entry_is_all_zeros);
    exit(UNITY_END());
}

void loop() {}
