// Unit tests for MeshCoreNodeFile in src/mesh/MeshCoreNodeFile.h - the flash image of the MeshCore nodes kept across
// a reboot, and the migration from the contacts file (meshcore_contacts.bin, version 1) written before it.
//
// Why it matters: only 16 nodes survive a reboot, and they are what lets the node decrypt and acknowledge a direct
// message the moment it boots (the message header names the sender by a 1-byte hash, so the key must be known).
// The image must therefore hold the right 16 - favourites, then conversations, then the rest - and bring them back
// with their priority intact, or the next save after a reboot quietly drops a conversation for a stranger. Nodes
// that only had a contact entry before the store existed must not be lost on the first boot of the new firmware.
//
// Regressions guarded: a save that keeps the most recent adverts instead of the favourites and conversations; a load
// that forgets which nodes were favourites or DM peers, or reverses their order; a learned route, position or node
// number that does not survive the round trip; a corrupt length or a route longer than the record read past the
// buffer; the version 1 contacts dropped on upgrade; a fingerprint that changes on a mere reorder, which would turn
// every advert into a flash write.
#include "mesh/MeshCoreNodeFile.h"

#include "TestUtil.h"
#include <unity.h>

using Store = MeshCoreNodeStore<32>;

static void makeKey(uint8_t pub[32], uint8_t first)
{
    memset(pub, 0x22, 32);
    pub[0] = first;
}

static MeshCoreNode *addNode(Store &store, uint8_t first, const char *name)
{
    uint8_t pub[32];
    makeKey(pub, first);
    MeshCoreNode *n = store.upsert(pub);
    n->type = MeshCoreNode::CHAT;
    n->setName(name);
    return n;
}

static MeshCoreNode *findNode(Store &store, uint8_t first)
{
    uint8_t pub[32];
    makeKey(pub, first);
    return store.find(pub);
}

void setUp() {}
void tearDown() {}

void test_round_trip_keeps_every_field()
{
    Store store;
    MeshCoreNode *n = addNode(store, 0xA0, "Pepe");
    n->type = MeshCoreNode::ROOM; // any type but a repeater, which no longer lives in this table (see the test below)
    n->outPathLen = 3;
    n->flags |= MeshCoreNode::HAS_OUT_PATH;
    n->outPath[0] = 0x11;
    n->outPath[2] = 0x33;
    n->lastAdvert = 1761480000;
    n->latitudeMicro = 41385064;
    n->longitudeMicro = -2173403; // negative: the sign must survive the unsigned encoding
    n->nodeNum = 0x4D123456;
    store.setFavorite(n, true);

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));
    TEST_ASSERT_EQUAL_UINT32(MeshCoreNodeFile::HEADER_LEN + MeshCoreNodeFile::RECORD_LEN, len);

    Store loaded;
    TEST_ASSERT_EQUAL_UINT32(1, MeshCoreNodeFile::load(loaded, image, len));
    const MeshCoreNode *r = findNode(loaded, 0xA0);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("Pepe", r->name);
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::ROOM, r->type);
    TEST_ASSERT_TRUE(r->isFavorite());
    TEST_ASSERT_EQUAL_UINT8(3, r->outPathLen);
    TEST_ASSERT_EQUAL_UINT8(0x33, r->outPath[2]);
    TEST_ASSERT_EQUAL_UINT32(1761480000, r->lastAdvert);
    TEST_ASSERT_EQUAL_INT32(41385064, r->latitudeMicro);
    TEST_ASSERT_EQUAL_INT32(-2173403, r->longitudeMicro);
    TEST_ASSERT_EQUAL_HEX32(0x4D123456, r->nodeNum);
}

void test_save_keeps_the_sixteen_that_matter()
{
    Store store;
    MeshCoreNode *favourite = addNode(store, 0x01, "fav");
    store.setFavorite(favourite, true);
    MeshCoreNode *chat = addNode(store, 0x02, "chat");
    store.noteDirectMessage(chat);
    for (uint8_t i = 0; i < 20; i++) // newer than both, and more than the image holds
        addNode(store, 0x40 + i, "stranger");

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));
    Store loaded;
    TEST_ASSERT_EQUAL_UINT32(MeshCoreNodeFile::SAVED_MAX, MeshCoreNodeFile::load(loaded, image, len));
    TEST_ASSERT_NOT_NULL(findNode(loaded, 0x01));
    TEST_ASSERT_NOT_NULL(findNode(loaded, 0x02));
    TEST_ASSERT_NOT_NULL(findNode(loaded, 0x40 + 19)); // the newest strangers fill the rest
    TEST_ASSERT_NULL(findNode(loaded, 0x40));
}

void test_load_keeps_the_priority_for_the_next_save()
{
    Store store;
    MeshCoreNode *olderChat = addNode(store, 0x01, "older");
    MeshCoreNode *newerChat = addNode(store, 0x02, "newer");
    store.noteDirectMessage(olderChat);
    store.noteDirectMessage(newerChat);
    addNode(store, 0x03, "stranger");

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));
    Store loaded;
    MeshCoreNodeFile::load(loaded, image, len);

    TEST_ASSERT_TRUE(findNode(loaded, 0x01)->hadDm());
    TEST_ASSERT_FALSE(findNode(loaded, 0x03)->hadDm());
    const MeshCoreNode *picked[3];
    TEST_ASSERT_EQUAL_UINT32(3, loaded.pickForSaving(picked, 3));
    TEST_ASSERT_EQUAL_UINT8(0x02, picked[0]->pub[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, picked[1]->pub[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, picked[2]->pub[0]);
}

void test_legacy_contacts_are_migrated_as_chat_nodes()
{
    uint8_t image[MeshCoreNodeFile::HEADER_LEN + 2 * MeshCoreNodeFile::LEGACY_RECORD_LEN] = {'M', 'C', 'C', 1, 2};
    uint8_t *rec = &image[MeshCoreNodeFile::HEADER_LEN];
    makeKey(rec, 0xA0); // saved newest first
    strcpy((char *)&rec[32], "Newest");
    rec += MeshCoreNodeFile::LEGACY_RECORD_LEN;
    makeKey(rec, 0xA1);
    strcpy((char *)&rec[32], "Oldest");

    Store store;
    TEST_ASSERT_EQUAL_UINT32(2, MeshCoreNodeFile::load(store, image, sizeof(image)));
    const MeshCoreNode *newest = findNode(store, 0xA0);
    TEST_ASSERT_NOT_NULL(newest);
    TEST_ASSERT_EQUAL_STRING("Newest", newest->name);
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::CHAT, newest->type);
    TEST_ASSERT_EQUAL_UINT32(0, newest->nodeNum); // assigned by the companion, which owns the numbering
    TEST_ASSERT_TRUE(newest->lastMod > findNode(store, 0xA1)->lastMod);
}

void test_malformed_images_load_nothing()
{
    Store store;
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    addNode(store, 0xA0, "Pepe");
    const size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));

    Store loaded;
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::load(loaded, image, len - 1)); // truncated
    image[4] = MeshCoreNodeFile::SAVED_MAX + 1;
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::load(loaded, image, len)); // count past the limit
    image[4] = 1;
    image[3] = 9;
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::load(loaded, image, len)); // unknown version
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::load(loaded, nullptr, len));
    TEST_ASSERT_EQUAL_UINT32(0, loaded.count());
}

void test_route_longer_than_the_record_is_dropped()
{
    Store store;
    MeshCoreNode *n = addNode(store, 0xA0, "Pepe");
    n->outPathLen = 2;
    n->flags |= MeshCoreNode::HAS_OUT_PATH;
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));
    image[MeshCoreNodeFile::HEADER_LEN + 32 + MeshCoreNode::NAME_LEN + 2] = MeshCoreNode::MAX_PATH_HOPS + 1;

    Store loaded;
    MeshCoreNodeFile::load(loaded, image, len);
    // Trusting it would send a direct message along bytes that are not a route.
    TEST_ASSERT_FALSE(findNode(loaded, 0xA0)->hasOutPath());
}

void test_fingerprint_ignores_order_but_not_content()
{
    Store store;
    MeshCoreNode *a = addNode(store, 0xA0, "A");
    addNode(store, 0xA1, "B");
    const uint32_t before = MeshCoreNodeFile::fingerprint(store);

    store.touch(a); // an advert reorders the set without changing it
    TEST_ASSERT_EQUAL_HEX32(before, MeshCoreNodeFile::fingerprint(store));

    a->setName("A2");
    TEST_ASSERT_NOT_EQUAL(before, MeshCoreNodeFile::fingerprint(store));
}

void test_save_refuses_a_small_buffer()
{
    Store store;
    addNode(store, 0xA0, "Pepe");
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX - 1];
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::save(store, image, sizeof(image)));
}

// --- repeater names: the tail of the file, written and read a record at a time ---

using Repeaters = MeshCoreRepeaterNames<8>;

static void learnRepeater(Repeaters &table, uint8_t first, const char *name, float snr, uint8_t hops)
{
    uint8_t pub[32];
    makeKey(pub, first);
    table.learn(pub, name, snr, hops);
}

// A repeater record keeps the key prefix, the short name, the signal and the hops through the round trip.
void test_repeater_record_round_trips()
{
    Repeaters table;
    learnRepeater(table, 0xA1, "Tetuan Repeater", -6.5f, 2);
    uint8_t record[MeshCoreNodeFile::REPEATER_RECORD_LEN];
    MeshCoreNodeFile::writeRepeaterRecord(record, *table.nthNewest(0));

    Repeaters loaded;
    TEST_ASSERT_TRUE(MeshCoreNodeFile::readRepeaterRecord(loaded, record));
    const MeshCoreRepeaterName *r = loaded.nthNewest(0);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX8(0xA1, r->prefix[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, r->prefix[1]);
    char name[MeshCoreRepeaterName::NAME_LEN + 1];
    r->copyName(name);
    TEST_ASSERT_EQUAL_STRING("Tetuan R", name);
    TEST_ASSERT_EQUAL_FLOAT(-6.5f, r->snr());
    TEST_ASSERT_EQUAL_UINT8(2, r->hops());
}

// The file lists the repeaters oldest first and reads them in that order, so the one heard last is still the newest after a
// reboot and the next one to be evicted is still the oldest.
void test_repeaters_written_oldest_first_keep_their_order()
{
    Repeaters table;
    learnRepeater(table, 0x01, "A", 1.0f, 1);
    learnRepeater(table, 0x02, "B", 1.0f, 1);
    learnRepeater(table, 0x03, "C", 1.0f, 1);
    learnRepeater(table, 0x01, "A", 1.0f, 1); // A is now the newest

    Repeaters loaded;
    for (size_t rank = table.count(); rank-- > 0;) {
        uint8_t record[MeshCoreNodeFile::REPEATER_RECORD_LEN];
        MeshCoreNodeFile::writeRepeaterRecord(record, *table.nthNewest(rank));
        TEST_ASSERT_TRUE(MeshCoreNodeFile::readRepeaterRecord(loaded, record));
    }
    for (size_t rank = 0; rank < 3; rank++)
        TEST_ASSERT_EQUAL_HEX8(table.nthNewest(rank)->prefix[0], loaded.nthNewest(rank)->prefix[0]);
}

// A record with no key or no name names nothing, so a corrupt or erased tail adds nothing to the table.
void test_a_repeater_record_that_names_nothing_is_dropped()
{
    Repeaters loaded;
    uint8_t blank[MeshCoreNodeFile::REPEATER_RECORD_LEN] = {};
    TEST_ASSERT_FALSE(MeshCoreNodeFile::readRepeaterRecord(loaded, blank));
    uint8_t noName[MeshCoreNodeFile::REPEATER_RECORD_LEN] = {0xA1, 0x22, 0x22};
    TEST_ASSERT_FALSE(MeshCoreNodeFile::readRepeaterRecord(loaded, noName));
    TEST_ASSERT_EQUAL_UINT32(0, loaded.count());
}

// Earlier builds kept repeaters among the contacts. They are not brought back into the contact table, where they would take
// the slots of people: they are learned again from the next advert.
void test_repeaters_of_an_earlier_image_are_ignored()
{
    Store store;
    addNode(store, 0xA0, "Friend");
    MeshCoreNode *rpt = addNode(store, 0xA1, "Tower");
    rpt->type = MeshCoreNode::REPEATER;

    uint8_t image[MeshCoreNodeFile::IMAGE_MAX];
    const size_t len = MeshCoreNodeFile::save(store, image, sizeof(image));
    Store loaded;
    TEST_ASSERT_EQUAL_UINT32(1, MeshCoreNodeFile::load(loaded, image, len));
    TEST_ASSERT_NOT_NULL(findNode(loaded, 0xA0));
    TEST_ASSERT_NULL(findNode(loaded, 0xA1));
}

// The contacts image is all the file's head takes: what follows it is the repeater tail, and an image of the previous version
// (no tail) still loads.
void test_image_length_stops_before_the_repeater_tail()
{
    Store store;
    addNode(store, 0xA0, "Friend");
    uint8_t image[MeshCoreNodeFile::IMAGE_MAX + 64];
    const size_t len = MeshCoreNodeFile::save(store, image, MeshCoreNodeFile::IMAGE_MAX);
    memset(&image[len], 0x7F, 64); // a tail
    TEST_ASSERT_EQUAL_UINT32(len, MeshCoreNodeFile::imageLength(image, len + 64));

    image[3] = MeshCoreNodeFile::PREVIOUS_VERSION;
    Store loaded;
    TEST_ASSERT_EQUAL_UINT32(1, MeshCoreNodeFile::load(loaded, image, len));
    TEST_ASSERT_EQUAL_UINT32(len, MeshCoreNodeFile::imageLength(image, len));

    image[3] = 9; // a version nobody wrote
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::imageLength(image, len));
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::imageLength(image, 3));
    TEST_ASSERT_EQUAL_UINT32(0, MeshCoreNodeFile::imageLength(nullptr, 100));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_keeps_every_field);
    RUN_TEST(test_save_keeps_the_sixteen_that_matter);
    RUN_TEST(test_load_keeps_the_priority_for_the_next_save);
    RUN_TEST(test_legacy_contacts_are_migrated_as_chat_nodes);
    RUN_TEST(test_malformed_images_load_nothing);
    RUN_TEST(test_route_longer_than_the_record_is_dropped);
    RUN_TEST(test_fingerprint_ignores_order_but_not_content);
    RUN_TEST(test_save_refuses_a_small_buffer);
    RUN_TEST(test_repeater_record_round_trips);
    RUN_TEST(test_repeaters_written_oldest_first_keep_their_order);
    RUN_TEST(test_a_repeater_record_that_names_nothing_is_dropped);
    RUN_TEST(test_repeaters_of_an_earlier_image_are_ignored);
    RUN_TEST(test_image_length_stops_before_the_repeater_tail);
    exit(UNITY_END());
}

void loop() {}
