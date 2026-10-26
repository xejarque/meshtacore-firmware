// Unit tests for MeshCoreNodeStore in src/mesh/MeshCoreNodeStore.h - the RAM table of MeshCore nodes (chat contacts
// and repeaters) kept apart from the Meshtastic node database.
//
// Why it matters: MeshCore nodes used to live in the Meshtastic NodeDB, where every advert took one of its 120 slots
// from a Meshtastic node. This table is where they go instead, so its eviction and save order decide what the user
// keeps: a full table must give up an old repeater before a favourite or a peer the user has messaged, and the few
// nodes saved to flash must be the favourites first, then the conversations, then the rest. Saving the wrong ones
// means a direct message from a friend cannot be decrypted after a reboot (its header carries only a 1-byte hash of
// the sender, so the key must already be known).
//
// Regressions guarded: plain LRU eviction that drops a favourite or a DM peer while a stranger is still in the table;
// a save order by advert recency that pushes a conversation out for a chatty repeater; prefix lookups that stop at
// the first match although MeshCore hashes collide; names cut in the middle of a UTF-8 character; the record growing
// past the size the 350-entry table was budgeted for.
#include "mesh/MeshCoreNodeStore.h"

#include "TestUtil.h"
#include <unity.h>

static void makeKey(uint8_t pub[32], uint8_t first, uint8_t second = 0)
{
    memset(pub, 0x11, 32);
    pub[0] = first;
    pub[1] = second;
}

static MeshCoreNode *addNode(MeshCoreNodeStore<4> &store, uint8_t first, uint8_t second = 0)
{
    uint8_t pub[32];
    makeKey(pub, first, second);
    return store.upsert(pub);
}

void setUp() {}
void tearDown() {}

void test_upsert_creates_once_and_find_returns_it()
{
    MeshCoreNodeStore<4> store;
    MeshCoreNode *a = addNode(store, 0xA0);
    a->nodeNum = 0x4D000001;

    uint8_t pub[32];
    makeKey(pub, 0xA0);
    TEST_ASSERT_EQUAL_PTR(a, store.upsert(pub));
    TEST_ASSERT_EQUAL_PTR(a, store.find(pub));
    TEST_ASSERT_EQUAL_PTR(a, store.findByNodeNum(0x4D000001));
    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    // A fresh node knows no route yet: direct messages to it must flood until one is learned.
    TEST_ASSERT_FALSE(a->hasOutPath());
}

void test_full_table_evicts_the_oldest_node()
{
    MeshCoreNodeStore<4> store;
    for (uint8_t i = 0; i < 4; i++)
        addNode(store, 0xA0 + i);
    addNode(store, 0xA0); // refreshed: 0xA1 is now the oldest

    addNode(store, 0xB0);

    uint8_t pub[32];
    makeKey(pub, 0xA1);
    TEST_ASSERT_NULL(store.find(pub));
    makeKey(pub, 0xA0);
    TEST_ASSERT_NOT_NULL(store.find(pub));
    TEST_ASSERT_EQUAL_UINT32(4, store.count());
}

void test_full_table_spares_favourites_and_dm_peers()
{
    MeshCoreNodeStore<4> store;
    store.setFavorite(addNode(store, 0xA0), true);
    store.noteDirectMessage(addNode(store, 0xA1));
    addNode(store, 0xA2);
    addNode(store, 0xA3);

    addNode(store, 0xB0);
    addNode(store, 0xB1);

    // The two oldest are kept; the strangers after them went instead.
    uint8_t pub[32];
    makeKey(pub, 0xA0);
    TEST_ASSERT_NOT_NULL(store.find(pub));
    makeKey(pub, 0xA1);
    TEST_ASSERT_NOT_NULL(store.find(pub));
    makeKey(pub, 0xA2);
    TEST_ASSERT_NULL(store.find(pub));
    makeKey(pub, 0xA3);
    TEST_ASSERT_NULL(store.find(pub));
}

void test_table_of_kept_nodes_evicts_the_oldest_of_them()
{
    MeshCoreNodeStore<4> store;
    for (uint8_t i = 0; i < 4; i++)
        store.setFavorite(addNode(store, 0xA0 + i), true);

    addNode(store, 0xB0);

    // A new node is never refused: with nothing else to give up, the oldest favourite goes.
    uint8_t pub[32];
    makeKey(pub, 0xA0);
    TEST_ASSERT_NULL(store.find(pub));
    makeKey(pub, 0xB0);
    TEST_ASSERT_NOT_NULL(store.find(pub));
}

void test_prefix_lookup_returns_every_collision()
{
    MeshCoreNodeStore<4> store;
    addNode(store, 0xA0, 0x01);
    addNode(store, 0xA0, 0x02);
    addNode(store, 0xB0, 0x01);

    MeshCoreNode *out[1];
    const uint8_t oneByte[] = {0xA0};
    TEST_ASSERT_EQUAL_UINT32(2, store.findByPrefix(oneByte, 1, out, 1)); // total, though only one fits
    TEST_ASSERT_EQUAL_UINT8(0xA0, out[0]->pub[0]);

    const uint8_t twoBytes[] = {0xA0, 0x02};
    TEST_ASSERT_EQUAL_UINT32(1, store.findByPrefix(twoBytes, 2, out, 1));
    TEST_ASSERT_EQUAL_UINT8(0x02, out[0]->pub[1]);
}

void test_remove_frees_the_slot()
{
    MeshCoreNodeStore<4> store;
    addNode(store, 0xA0);
    uint8_t pub[32];
    makeKey(pub, 0xA0);

    TEST_ASSERT_TRUE(store.remove(pub));
    TEST_ASSERT_FALSE(store.remove(pub));
    TEST_ASSERT_NULL(store.find(pub));
    TEST_ASSERT_EQUAL_UINT32(0, store.count());
}

void test_saving_order_is_favourites_then_conversations_then_the_rest()
{
    MeshCoreNodeStore<4> store;
    MeshCoreNode *stranger = addNode(store, 0xA0);
    MeshCoreNode *olderChat = addNode(store, 0xA1);
    MeshCoreNode *newerChat = addNode(store, 0xA2);
    MeshCoreNode *favourite = addNode(store, 0xA3);
    store.noteDirectMessage(olderChat);
    store.noteDirectMessage(newerChat);
    store.setFavorite(favourite, true);
    // A later advert must not move a conversation ahead of a more recent one: DM peers rank by the last DM.
    store.touch(olderChat);
    store.touch(stranger);

    const MeshCoreNode *picked[4];
    TEST_ASSERT_EQUAL_UINT32(4, store.pickForSaving(picked, 4));
    TEST_ASSERT_EQUAL_PTR(favourite, picked[0]);
    TEST_ASSERT_EQUAL_PTR(newerChat, picked[1]);
    TEST_ASSERT_EQUAL_PTR(olderChat, picked[2]);
    TEST_ASSERT_EQUAL_PTR(stranger, picked[3]);
}

void test_saving_stops_at_the_limit_with_the_best_nodes()
{
    MeshCoreNodeStore<4> store;
    addNode(store, 0xA0);
    MeshCoreNode *chat = addNode(store, 0xA1);
    addNode(store, 0xA2);
    store.noteDirectMessage(chat);

    const MeshCoreNode *picked[1];
    TEST_ASSERT_EQUAL_UINT32(1, store.pickForSaving(picked, 1));
    TEST_ASSERT_EQUAL_PTR(chat, picked[0]);
}

void test_name_is_cut_on_a_utf8_boundary()
{
    MeshCoreNode n;
    // 30 ASCII bytes and a 3-byte character: 33 bytes do not fit in 31, and the character must not be split.
    n.setName("abcdefghijklmnopqrstuvwxyz0123\xE2\x82\xAC");
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz0123", n.name);

    n.setName("Pepe");
    TEST_ASSERT_EQUAL_STRING("Pepe", n.name);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)n.name[MeshCoreNode::NAME_LEN - 1]);
}

void test_record_fits_the_ram_budget()
{
    // 350 records were budgeted at about 36 KB of RAM; a field added here costs 350 times its size.
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(108, sizeof(MeshCoreNode));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_creates_once_and_find_returns_it);
    RUN_TEST(test_full_table_evicts_the_oldest_node);
    RUN_TEST(test_full_table_spares_favourites_and_dm_peers);
    RUN_TEST(test_table_of_kept_nodes_evicts_the_oldest_of_them);
    RUN_TEST(test_prefix_lookup_returns_every_collision);
    RUN_TEST(test_remove_frees_the_slot);
    RUN_TEST(test_saving_order_is_favourites_then_conversations_then_the_rest);
    RUN_TEST(test_saving_stops_at_the_limit_with_the_best_nodes);
    RUN_TEST(test_name_is_cut_on_a_utf8_boundary);
    RUN_TEST(test_record_fits_the_ram_budget);
    exit(UNITY_END());
}

void loop() {}
