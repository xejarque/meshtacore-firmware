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
    n->type = MeshCoreNode::REPEATER;
    n->outPathLen = 3;
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
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::REPEATER, r->type);
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
    addNode(store, 0xA0, "Pepe")->outPathLen = 2;
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
    exit(UNITY_END());
}

void loop() {}
