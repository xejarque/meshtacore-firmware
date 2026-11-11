// Unit tests for MeshCoreRegionMap in src/mesh/MeshCoreRegionMap.{h,cpp} - the MeshCore region scoping,
// hierarchy, HMAC transport code calculation, packet matching, and serialization.
//
// Why it matters: MeshCore repeaters use region maps to scope flood routing across wide areas. Unscoped
// floods must respect the wildcard deny flag, while scoped floods must match an allowed region's HMAC
// transport code. If transport codes miscalculate or the tree corrupts, packets are either leaked across
// boundaries or dropped entirely.
//
// Regressions guarded:
// - putRegion invalid characters, self-parenting cycles, and table capacity overflow.
// - removeRegion prevention of deleting wildcard or non-empty parent regions.
// - Case-insensitive and hash-prefix-agnostic name lookups.
// - Transport key derivation (SHA256 of #name) and HMAC transport code calculation (including 0000/FFFF clamp).
// - Packet matching against allowed vs denied regions.
// - Bulk definition (processDef) segment parsing and cursor jumps.
// - Tree export formatting with line boundaries and home/default indicators.
// - Binary serialization CRC integrity and round-trip fidelity.
#include "mesh/MeshCoreRegionMap.h"

#include "Arduino.h"
#include "TestUtil.h"
#include <SHA256.h>
#include <unity.h>

static void test_put_region_hierarchy_and_limits()
{
    MeshCoreRegionMap map;
    TEST_ASSERT_EQUAL_INT(0, map.getCount());

    RegionEntry *es = map.putRegion("ES", 0);
    TEST_ASSERT_NOT_NULL(es);
    TEST_ASSERT_EQUAL_UINT16(0, es->parent);
    TEST_ASSERT_EQUAL_STRING("ES", es->name);
    TEST_ASSERT_EQUAL_INT(1, map.getCount());

    RegionEntry *mad = map.putRegion("MADRID", es->id);
    TEST_ASSERT_NOT_NULL(mad);
    TEST_ASSERT_EQUAL_UINT16(es->id, mad->parent);
    TEST_ASSERT_EQUAL_INT(2, map.getCount());

    // Invalid characters rejected
    TEST_ASSERT_NULL(map.putRegion("BAD NAME!", 0));
    TEST_ASSERT_NULL(map.putRegion("SP@CE", 0));

    // Self-parenting rejected
    TEST_ASSERT_NULL(map.putRegion("ES", es->id));

    // Capacity bound check
    for (int i = map.getCount(); i < MAX_REGION_ENTRIES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "REG%d", i);
        TEST_ASSERT_NOT_NULL(map.putRegion(name, 0));
    }
    TEST_ASSERT_EQUAL_INT(MAX_REGION_ENTRIES, map.getCount());
    TEST_ASSERT_NULL(map.putRegion("OVERFLOW", 0));
}

// An explicit id used to skip the capacity check and write regions[MAX_REGION_ENTRIES].
static void test_put_region_explicit_id_respects_capacity()
{
    MeshCoreRegionMap map;
    for (int i = 0; i < MAX_REGION_ENTRIES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "REG%d", i);
        TEST_ASSERT_NOT_NULL(map.putRegion(name, 0));
    }
    TEST_ASSERT_NULL(map.putRegion("EXTRA", 0, 500));
    TEST_ASSERT_EQUAL_INT(MAX_REGION_ENTRIES, map.getCount());
}

// Re-parenting a region under its own descendant would detach both from the tree for good.
static void test_put_region_rejects_parent_cycle()
{
    MeshCoreRegionMap map;
    RegionEntry *a = map.putRegion("A", 0);
    RegionEntry *b = map.putRegion("B", a->id);
    RegionEntry *c = map.putRegion("C", b->id);

    TEST_ASSERT_NULL(map.putRegion("A", b->id));
    TEST_ASSERT_NULL(map.putRegion("A", c->id));
    TEST_ASSERT_EQUAL_UINT16(0, a->parent);

    // Moving to an unrelated parent is still allowed.
    RegionEntry *d = map.putRegion("D", 0);
    TEST_ASSERT_NOT_NULL(map.putRegion("C", d->id));
    TEST_ASSERT_EQUAL_UINT16(d->id, c->parent);
}

// A deep chain fills the 64-byte line with indent; the copy must never exceed it.
static void test_tree_export_deep_chain_stays_bounded()
{
    MeshCoreRegionMap map;
    uint16_t parent = 0;
    for (int i = 0; i < MAX_REGION_ENTRIES; i++) {
        char name[31];
        snprintf(name, sizeof(name), "R%02dXXXXXXXXXXXXXXXXXXXXXXXXX", i);
        RegionEntry *r = map.putRegion(name, parent);
        TEST_ASSERT_NOT_NULL(r);
        parent = r->id;
    }
    char tree[4096];
    size_t len = map.exportTo(tree, sizeof(tree));
    TEST_ASSERT_EQUAL_size_t(strlen(tree), len);
    const char *line = tree;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t lineLen = nl ? (size_t)(nl - line) : strlen(line);
        TEST_ASSERT_TRUE(lineLen < 64);
        line = nl ? nl + 1 : line + lineLen;
    }
}

// A map loaded from flash can hold a region that names itself as parent, or a deeper cycle: export must terminate
// instead of recursing until the stack runs out.
static void test_tree_export_terminates_on_a_cycle()
{
    MeshCoreRegionMap map;
    TEST_ASSERT_NOT_NULL(map.putRegion("SELF", 50, 50)); // its own parent, as a loaded file may say
    RegionEntry *a = map.putRegion("A", 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(map.putRegion("B", a->id, 60));
    a->parent = 60; // A under B under A: only a corrupt file builds this
    char tree[2048];
    const size_t len = map.exportTo(tree, sizeof(tree));
    TEST_ASSERT_EQUAL_size_t(strlen(tree), len);
}

static void test_remove_region_rules()
{
    MeshCoreRegionMap map;
    RegionEntry *es = map.putRegion("ES", 0);
    RegionEntry *mad = map.putRegion("MADRID", es->id);

    map.setHomeRegion(mad);
    map.setDefaultRegion(mad);
    TEST_ASSERT_EQUAL_PTR(mad, map.getHomeRegion());
    TEST_ASSERT_EQUAL_PTR(mad, map.getDefaultRegion());

    // Wildcard cannot be removed
    TEST_ASSERT_FALSE(map.removeRegion(map.getWildcard()));

    // Parent with children cannot be removed
    TEST_ASSERT_FALSE(map.removeRegion(*es));

    // Child can be removed, and clears home/default references
    TEST_ASSERT_TRUE(map.removeRegion(*mad));
    TEST_ASSERT_NULL(map.getHomeRegion());
    TEST_ASSERT_NULL(map.getDefaultRegion());
    TEST_ASSERT_EQUAL_INT(1, map.getCount());

    // Now parent can be removed
    TEST_ASSERT_TRUE(map.removeRegion(*es));
    TEST_ASSERT_EQUAL_INT(0, map.getCount());
}

static void test_find_by_name_and_prefix()
{
    MeshCoreRegionMap map;
    map.putRegion("CATALUNYA", 0);
    map.putRegion("#BARCELONA", 0);

    TEST_ASSERT_EQUAL_PTR(&map.getWildcard(), map.findByName("*"));
    TEST_ASSERT_EQUAL_PTR(&map.getWildcard(), map.findByNamePrefix("*"));

    // Case-insensitive and hash-prefix optional
    RegionEntry *cat = map.findByName("catalunya");
    TEST_ASSERT_NOT_NULL(cat);
    TEST_ASSERT_EQUAL_PTR(cat, map.findByName("#catalunya"));

    RegionEntry *bcn = map.findByName("barcelona");
    TEST_ASSERT_NOT_NULL(bcn);
    TEST_ASSERT_EQUAL_PTR(bcn, map.findByName("#barcelona"));

    // Prefix matching
    TEST_ASSERT_EQUAL_PTR(cat, map.findByNamePrefix("cat"));
    TEST_ASSERT_EQUAL_PTR(bcn, map.findByNamePrefix("#barc"));
}

static void test_transport_key_and_code_calculation()
{
    MeshCoreRegionMap map;
    RegionEntry *es = map.putRegion("ES", 0);
    TEST_ASSERT_NOT_NULL(es);

    MeshCoreTransportKey key;
    TEST_ASSERT_EQUAL_INT(1, map.getTransportKeysFor(*es, &key, 1));
    TEST_ASSERT_FALSE(key.isNull());

    // Hashtag key: first 16 bytes of SHA256("#ES")
    SHA256 sha;
    sha.update("#ES", 3);
    uint8_t expectedKey[16];
    sha.finalize(expectedKey, sizeof(expectedKey));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedKey, key.key, 16);

    // Transport code calculation
    uint8_t payloadType = 0x04; // ADVERT
    uint8_t dummyPayload[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    uint16_t code = key.calcTransportCode(payloadType, dummyPayload, sizeof(dummyPayload));
    TEST_ASSERT_NOT_EQUAL(0, code);
    TEST_ASSERT_NOT_EQUAL(0xFFFF, code);

    // Same inputs produce identical transport code
    uint16_t code2 = key.calcTransportCode(payloadType, dummyPayload, sizeof(dummyPayload));
    TEST_ASSERT_EQUAL_UINT16(code, code2);

    // Different payload changes the transport code
    dummyPayload[0] = 0x99;
    uint16_t code3 = key.calcTransportCode(payloadType, dummyPayload, sizeof(dummyPayload));
    TEST_ASSERT_NOT_EQUAL(code, code3);
}

static void test_find_match_scoped_packets()
{
    MeshCoreRegionMap map;
    RegionEntry *es = map.putRegion("ES", 0);
    RegionEntry *fr = map.putRegion("FR", 0);
    fr->flags |= REGION_DENY_FLOOD;

    MeshCoreTransportKey esKey;
    map.getTransportKeysFor(*es, &esKey, 1);
    MeshCoreTransportKey frKey;
    map.getTransportKeysFor(*fr, &frKey, 1);

    uint8_t pType = 0x02; // TXT_MSG
    uint8_t payload[] = "Hello MeshCore";
    uint16_t esCode = esKey.calcTransportCode(pType, payload, strlen((char *)payload));
    uint16_t frCode = frKey.calcTransportCode(pType, payload, strlen((char *)payload));

    // Matching allowed region succeeds
    RegionEntry *match = map.findMatch(esCode, pType, payload, strlen((char *)payload), REGION_DENY_FLOOD);
    TEST_ASSERT_EQUAL_PTR(es, match);

    // Matching denied region is rejected under REGION_DENY_FLOOD
    RegionEntry *matchDenied = map.findMatch(frCode, pType, payload, strlen((char *)payload), REGION_DENY_FLOOD);
    TEST_ASSERT_NULL(matchDenied);

    // Unknown code is rejected
    RegionEntry *matchUnknown = map.findMatch(0x1234, pType, payload, strlen((char *)payload), REGION_DENY_FLOOD);
    TEST_ASSERT_NULL(matchUnknown);
}

static void test_process_def_bulk_definition()
{
    MeshCoreRegionMap map;
    char reply[64] = {0};

    // Bulk define, ZephCore semantics: "|X" jumps the cursor to X after the segment,
    // so FR lands under ES and PARIS under FR.
    const char *def = "ES MADRID|ES BCN|ES FR PARIS|FR";
    TEST_ASSERT_TRUE(map.processDef(def, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(5, map.getCount());

    RegionEntry *es = map.findByName("ES");
    RegionEntry *mad = map.findByName("MADRID");
    RegionEntry *bcn = map.findByName("BCN");
    RegionEntry *fr = map.findByName("FR");
    RegionEntry *paris = map.findByName("PARIS");

    TEST_ASSERT_NOT_NULL(es);
    TEST_ASSERT_NOT_NULL(mad);
    TEST_ASSERT_NOT_NULL(bcn);
    TEST_ASSERT_NOT_NULL(fr);
    TEST_ASSERT_NOT_NULL(paris);

    TEST_ASSERT_EQUAL_UINT16(0, es->parent);
    TEST_ASSERT_EQUAL_UINT16(es->id, mad->parent);
    TEST_ASSERT_EQUAL_UINT16(es->id, bcn->parent);
    TEST_ASSERT_EQUAL_UINT16(es->id, fr->parent);
    TEST_ASSERT_EQUAL_UINT16(fr->id, paris->parent);

    // Empty definition returns error
    TEST_ASSERT_FALSE(map.processDef("", reply, sizeof(reply)));
    TEST_ASSERT_NOT_NULL(strstr(reply, "Err"));
}

static void test_tree_export_and_list_names()
{
    MeshCoreRegionMap map;
    RegionEntry *es = map.putRegion("ES", 0);
    RegionEntry *mad = map.putRegion("MADRID", es->id);
    mad->flags |= REGION_DENY_FLOOD;

    map.setHomeRegion(es);
    map.setDefaultRegion(es);

    char tree[128];
    size_t len = map.exportTo(tree, sizeof(tree));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(tree, "* F\n"));
    TEST_ASSERT_NOT_NULL(strstr(tree, " ES^* F\n"));
    TEST_ASSERT_NOT_NULL(strstr(tree, "  MADRID\n"));

    char allowed[64];
    int countAllowed = map.exportNamesTo(allowed, sizeof(allowed), REGION_DENY_FLOOD, false);
    TEST_ASSERT_EQUAL_INT(1, countAllowed);
    TEST_ASSERT_EQUAL_STRING("ES", allowed);

    char denied[64];
    int countDenied = map.exportNamesTo(denied, sizeof(denied), REGION_DENY_FLOOD, true);
    TEST_ASSERT_EQUAL_INT(1, countDenied);
    TEST_ASSERT_EQUAL_STRING("MADRID", denied);
}

static void test_binary_serialization_round_trip()
{
    MeshCoreRegionMap in;
    RegionEntry *es = in.putRegion("ES", 0);
    RegionEntry *mad = in.putRegion("MADRID", es->id);
    mad->flags = REGION_DENY_FLOOD;
    in.setHomeRegion(es);
    in.setDefaultRegion(mad);
    in.getWildcard().flags = REGION_DENY_FLOOD;

    uint8_t buffer[MeshCoreRegionMap::SERIALIZED_MAX_LEN];
    size_t encLen = in.encode(buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, encLen);

    MeshCoreRegionMap out;
    TEST_ASSERT_TRUE(out.decode(buffer, encLen));

    TEST_ASSERT_EQUAL_INT(2, out.getCount());
    TEST_ASSERT_EQUAL_UINT8(REGION_DENY_FLOOD, out.getWildcard().flags);
    TEST_ASSERT_NOT_NULL(out.getHomeRegion());
    TEST_ASSERT_EQUAL_STRING("ES", out.getHomeRegion()->name);
    TEST_ASSERT_NOT_NULL(out.getDefaultRegion());
    TEST_ASSERT_EQUAL_STRING("MADRID", out.getDefaultRegion()->name);

    RegionEntry *outMad = out.findByName("MADRID");
    TEST_ASSERT_NOT_NULL(outMad);
    TEST_ASSERT_EQUAL_UINT8(REGION_DENY_FLOOD, outMad->flags);

    // Tampered byte fails CRC
    buffer[encLen - 1] ^= 0xFF;
    TEST_ASSERT_FALSE(out.decode(buffer, encLen));

    // Buffer too small for encode
    TEST_ASSERT_EQUAL_UINT32(0, in.encode(buffer, encLen - 1));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_put_region_hierarchy_and_limits);
    RUN_TEST(test_put_region_explicit_id_respects_capacity);
    RUN_TEST(test_put_region_rejects_parent_cycle);
    RUN_TEST(test_tree_export_deep_chain_stays_bounded);
    RUN_TEST(test_tree_export_terminates_on_a_cycle);
    RUN_TEST(test_remove_region_rules);
    RUN_TEST(test_find_by_name_and_prefix);
    RUN_TEST(test_transport_key_and_code_calculation);
    RUN_TEST(test_find_match_scoped_packets);
    RUN_TEST(test_process_def_bulk_definition);
    RUN_TEST(test_tree_export_and_list_names);
    RUN_TEST(test_binary_serialization_round_trip);
    exit(UNITY_END());
}

void loop() {}
