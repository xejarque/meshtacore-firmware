// Unit tests for src/mesh/MeshCoreRepeater.{h,cpp} - MeshCore frame detection (sniffer) and the
// transparent flood relay.
//
// The code is compile-time gated and the native test build compiles src/ with the flags off, so
// its translation unit is empty there. This suite defines the flags and includes the .cpp
// directly; the flag-off copy contributes no symbols, so nothing collides.
//
// The false-positive half of the suite is the important one: with the sniffer enabled on a real
// node, a Meshtastic frame misread as MeshCore is swallowed instead of routed - and flood-relayed
// as garbage into the MeshCore network. The relay half covers the path-independent dedupe (one
// re-emission per packet, not per path variant) and the ZephCore-style adaptive relay delay.
#define MESHTASTIC_ENABLE_MESHCORE_SNIFFER 1
#define MESHTASTIC_ENABLE_MESHCORE_RELAY 1
#include "mesh/MeshCoreRepeater.cpp"

#include "Arduino.h"
#include "TestUtil.h"
#include <unity.h>

// Our NodeNum (crc32 of our public key in real life - arbitrary but fixed here).
static const uint32_t kOurNodeNum = 0x12345678;

// MeshCore payload types used below (mirrors of the MC_PAYLOAD_TYPE_* values in the .cpp).
static const uint8_t kTxtMsg = 0x02;
static const uint8_t kAdvert = 0x04;

// Airtime reported for test frames; the adaptive delay scales with it (see test expectations).
static const uint32_t kAirtimeMs = 100;

void setUp(void)
{
    Time::setTestMillis(1000);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
}

void tearDown(void)
{
    Time::useRealClock();
}

// --- frame builders ---

// MeshCore flood frame: 1 header byte, 1 path_len byte, hashCount x hashSize path bytes, payload.
// pathFill and payloadFill are independent: MeshCore rewrites the path at every hop, so path
// variants of one packet must hash equal (dedupe) while distinct packets must not.
static size_t makeFloodFrame(uint8_t *buf, uint8_t payloadType, uint8_t hashCount, uint8_t hashSize, uint8_t pathFill,
                             uint8_t payloadFill, uint8_t payloadLen = 8)
{
    size_t i = 0;
    buf[i++] = (payloadType << 2) | 0x01; // ver 0, route FLOOD
    buf[i++] = (uint8_t)(((hashSize - 1) << 6) | hashCount);
    for (uint8_t h = 0; h < hashCount * hashSize; h++)
        buf[i++] = pathFill + h;
    for (uint8_t p = 0; p < payloadLen; p++)
        buf[i++] = (uint8_t)(payloadFill ^ (0x40 + p));
    return i;
}

// MeshCore transport-flood frame: additionally carries two uint16 transport codes after the header.
static size_t makeTransportFloodFrame(uint8_t *buf, uint8_t payloadType, uint8_t hashCount)
{
    size_t i = 0;
    buf[i++] = (payloadType << 2) | 0x00; // ver 0, route TRANSPORT_FLOOD
    buf[i++] = 0x01;
    buf[i++] = 0x00; // transport code 1
    buf[i++] = 0x02;
    buf[i++] = 0x00;               // transport code 2
    buf[i++] = (uint8_t)hashCount; // path_len: mode 0, hash_size 1
    for (uint8_t h = 0; h < hashCount; h++)
        buf[i++] = (uint8_t)(0x80 + h);
    for (uint8_t p = 0; p < 4; p++)
        buf[i++] = (uint8_t)(0xC0 + p);
    return i;
}

// Meshtastic on-air frame: PacketHeader (to/from/id LE, flags, channel, next_hop, relay_node) + payload.
static size_t makeMeshtasticFrame(uint8_t *buf, uint32_t to, uint32_t from, uint32_t id, uint8_t payloadLen = 4)
{
    size_t i = 0;
    buf[i++] = to & 0xFF;
    buf[i++] = (to >> 8) & 0xFF;
    buf[i++] = (to >> 16) & 0xFF;
    buf[i++] = (to >> 24) & 0xFF;
    buf[i++] = from & 0xFF;
    buf[i++] = (from >> 8) & 0xFF;
    buf[i++] = (from >> 16) & 0xFF;
    buf[i++] = (from >> 24) & 0xFF;
    buf[i++] = id & 0xFF;
    buf[i++] = (id >> 8) & 0xFF;
    buf[i++] = (id >> 16) & 0xFF;
    buf[i++] = (id >> 24) & 0xFF;
    buf[i++] = 0x03; // flags: hop limit 3
    buf[i++] = 0x08; // channel hash
    buf[i++] = 0xFF; // next_hop: none
    buf[i++] = 0xFF; // relay_node: none
    for (uint8_t p = 0; p < payloadLen; p++)
        buf[i++] = (uint8_t)(0x50 + p);
    return i;
}

static void giveIdentity(MeshCoreRepeater &repeater); // defined with the identity tests below

// A TXT_MSG payload as MeshCore builds it: dest hash, src hash, 2-byte MAC, one 16-byte AES block.
static const uint8_t kTxtMsgPayloadLen = 1 + 1 + 2 + 16;

// --- parser / detector ---

void test_flood_frame_parses_with_expected_fields()
{
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA, kTxtMsgPayloadLen);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, &info));
    TEST_ASSERT_EQUAL_UINT8(0x01, info.route_type);
    TEST_ASSERT_EQUAL_UINT8(kTxtMsg, info.payload_type);
    TEST_ASSERT_EQUAL_UINT8(0, info.payload_ver);
    TEST_ASSERT_EQUAL_UINT8(1, info.path_hash_size);
    TEST_ASSERT_EQUAL_UINT8(2, info.path_hash_count);
    TEST_ASSERT_EQUAL_UINT8(4, info.header_len); // header + path_len + 2 path bytes
    TEST_ASSERT_EQUAL_UINT8(kTxtMsgPayloadLen, info.payload_len);
}

void test_transport_flood_frame_accounts_for_transport_codes()
{
    uint8_t buf[64];
    size_t len = makeTransportFloodFrame(buf, kAdvert, 1);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, &info));
    TEST_ASSERT_EQUAL_UINT8(0x00, info.route_type);
    TEST_ASSERT_EQUAL_UINT8(kAdvert, info.payload_type);
    TEST_ASSERT_EQUAL_UINT8(7, info.header_len); // header + 4 transport code bytes + path_len + 1 path byte
    TEST_ASSERT_EQUAL_UINT8(4, info.payload_len);
}

void test_malformed_frames_are_rejected()
{
    uint8_t buf[300];
    MeshCoreRepeater::FrameInfo info;

    // Too short / too long.
    buf[0] = (kTxtMsg << 2) | 0x01;
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 1, kOurNodeNum, &info));
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 256, kOurNodeNum, &info));

    // Payload version 1 is not MeshCore.
    buf[0] = (1 << 6) | (kTxtMsg << 2) | 0x01;
    buf[1] = 0x00;
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 8, kOurNodeNum, &info));

    // Unknown payload type (0x0C..0x0E are unused).
    buf[0] = (0x0C << 2) | 0x01;
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 8, kOurNodeNum, &info));

    // Path mode 3 is reserved.
    buf[0] = (kTxtMsg << 2) | 0x01;
    buf[1] = 0xC0;
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 8, kOurNodeNum, &info));

    // Path overruns the frame: count 10 x hash_size 3 = 30 bytes into an 8-byte frame.
    buf[1] = (uint8_t)(((3 - 1) << 6) | 10);
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 8, kOurNodeNum, &info));

    // Transport codes overrun the frame.
    buf[0] = (kAdvert << 2) | 0x00;
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, 4, kOurNodeNum, &info));
}

// --- Meshtastic false positives ---

void test_meshtastic_broadcast_is_never_misread()
{
    uint8_t buf[64];
    // to == 0xffffffff: byte 0 = 0xff fails the MeshCore payload version check outright.
    size_t len = makeMeshtasticFrame(buf, 0xFFFFFFFF, 0x0BADF00D, 0x55667788);
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
}

// A Meshtastic unicast whose to NodeNum happens to look like a MeshCore header (ver 0, type 1,
// flood route, empty path) would parse as MeshCore without the ourNodeNum veto - and would be
// flood-relayed as garbage. This is the damaging case the veto exists for.
void test_meshtastic_unicast_to_us_is_not_swallowed()
{
    uint8_t buf[64];
    const uint32_t to = 0x20000005; // LE bytes 05 00 00 20: parses as MeshCore flood
    // 6 payload bytes make the MeshCore payload (header 16 + 6 - 2 = 20 bytes) a whole cipher block after dest/src/MAC.
    size_t len = makeMeshtasticFrame(buf, to, 0x0BADF00D, 0x55667788, 6);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, 0, &info)); // hazard without the veto
    TEST_ASSERT_EQUAL_UINT8(0x01, info.route_type);                                 // ...and it is a flood: would relay

    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, to, &info)); // veto
}

void test_meshtastic_unicast_from_us_is_not_swallowed()
{
    uint8_t buf[64];
    const uint32_t to = 0x20000005; // parses as MeshCore flood
    size_t len = makeMeshtasticFrame(buf, to, kOurNodeNum, 0x55667788, 6);
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
}

// Short MeshCore frames have no room for a Meshtastic header and must survive the veto even when
// their first bytes collide with our NodeNum.
void test_short_meshcore_frame_survives_veto()
{
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kAdvert, 2, 1, 0xAA, 0xAA); // 12 bytes < 16
    TEST_ASSERT_LESS_THAN_UINT32(16, (uint32_t)len);
    // Construct the collision: raw[0..3] == ourNodeNum.
    uint32_t collision = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, collision, NULL));
}

// Every encrypted MeshCore payload is routing hashes + 2-byte MAC + whole 16-byte AES blocks. A Meshtastic frame that
// happens to parse as MeshCore almost never has that shape, so it stays with the Meshtastic path instead of being relayed.
void test_encrypted_payload_needs_mac_and_whole_cipher_blocks()
{
    uint8_t buf[MeshCoreRepeater::MAX_FRAME_LEN];
    struct Case {
        uint8_t type;
        uint8_t fixedLen; // bytes before the cipher text: routing hashes + MAC
    };
    static const uint8_t kReq = 0x00, kResponse = 0x01, kAnonReq = 0x07, kPath = 0x08, kGrpTxt = 0x05, kGrpData = 0x06;
    const Case cases[] = {{kReq, 4}, {kResponse, 4}, {kTxtMsg, 4}, {kPath, 4}, {kAnonReq, 35}, {kGrpTxt, 3}, {kGrpData, 3}};
    for (const Case &c : cases) {
        size_t len = makeFloodFrame(buf, c.type, 1, 1, 0x10, 0x10, c.fixedLen + 16);
        TEST_ASSERT_TRUE_MESSAGE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL), "one block");
        len = makeFloodFrame(buf, c.type, 1, 1, 0x10, 0x10, c.fixedLen + 48);
        TEST_ASSERT_TRUE_MESSAGE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL), "three blocks");
        len = makeFloodFrame(buf, c.type, 1, 1, 0x10, 0x10, c.fixedLen + 15);
        TEST_ASSERT_FALSE_MESSAGE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL), "partial block");
        len = makeFloodFrame(buf, c.type, 1, 1, 0x10, 0x10, c.fixedLen); // MAC and nothing to decrypt
        TEST_ASSERT_FALSE_MESSAGE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL), "no cipher text");
    }

    // ADVERT is signed rather than block-structured, and an ACK carries whatever length its sender chose.
    size_t len = makeFloodFrame(buf, kAdvert, 1, 1, 0x10, 0x10, 100);
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
    len = makeFloodFrame(buf, 0x03, 1, 1, 0x10, 0x10, 4);
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
}

// The ordinary case the shape check exists for: a Meshtastic unicast between two other nodes, on a channel we hold no key
// for, whose first bytes parse as MeshCore. Without a MeshCore-shaped payload it is left to the Meshtastic path.
void test_meshtastic_unicast_between_others_is_not_misread()
{
    uint8_t buf[64];
    const uint32_t to = 0x20000005;                                    // parses as a MeshCore RESPONSE flood
    size_t len = makeMeshtasticFrame(buf, to, 0x0BADF00D, 0x55667788); // 4-byte payload: 18-byte MeshCore payload
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
}

// --- relay policy (Fase B) ---

void test_flood_is_queued_for_relay_once_and_deduped()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA);

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed); // counted when it goes on air, not when queued

    repeater.markRelayed();
    repeater.clearPendingFrame();
    Time::advanceTestMillis(1000);

    // Same frame again: a MeshCore node re-flooded it to us; do not relay twice.
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txRelayed);
}

// The key amplification regression: MeshCore appends a hop hash to the path on every forward, so
// the same packet comes back to us as different bytes. The dedupe keys on type+payload, so path
// variants count as duplicates - one re-emission per packet, not one per variant.
void test_dedupe_is_path_independent()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x77);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    repeater.markRelayed();
    repeater.clearPendingFrame();

    // Same payload, longer path (a MeshCore node re-flooded it one hop further).
    len = makeFloodFrame(buf, kTxtMsg, 3, 1, 0x20, 0x77);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txRelayed);

    // A different payload is still a different packet.
    len = makeFloodFrame(buf, kTxtMsg, 3, 1, 0x20, 0x88);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
}

void test_direct_frames_are_never_relayed()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t i = 0;
    buf[i++] = (kTxtMsg << 2) | 0x02; // route DIRECT
    buf[i++] = 0x01;                  // mode 0, 1 hop
    buf[i++] = 0xAB;
    buf[i++] = 0x11;
    buf[i++] = 0x22;

    repeater.handleReceivedFrame(buf, i, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxSeen); // still sniffed
    TEST_ASSERT_EQUAL_UINT32(0, repeater.rxFlood);
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed);
}

void test_flood_with_full_path_is_dropped()
{
    MeshCoreRepeater repeater;
    uint8_t buf[200];
    // count 63 x hash_size 2: the next hop would need 128 path bytes, over MeshCore's 64-byte
    // MAX_PATH_SIZE, so real MeshCore nodes could not extend it either.
    size_t len = makeFloodFrame(buf, kTxtMsg, 63, 2, 0x10, 0x10);

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed);
}

// MeshCore flood_max_advert default: ADVERT floods are not forwarded past 8 hops.
// A flood ADVERT with the minimum MeshCore payload (key, timestamp, signature) and a given hop count.
static size_t makeAdvertFlood(uint8_t *buf, uint8_t hashCount, uint8_t fill)
{
    size_t i = 0;
    buf[i++] = (kAdvert << 2) | 0x01;
    buf[i++] = hashCount;
    for (uint8_t h = 0; h < hashCount; h++)
        buf[i++] = (uint8_t)(0x10 + h);
    for (int p = 0; p < 100; p++)
        buf[i++] = (uint8_t)(fill ^ p);
    return i;
}

static void giveIdentity(MeshCoreRepeater &repeater);
static size_t makeSignedAdvertFlood(uint8_t *buf, uint8_t hashCount);

void test_advert_flood_hop_limit()
{
    MeshCoreRepeater repeater;
    uint8_t buf[160];

    size_t len = makeAdvertFlood(buf, 8, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);

    Time::advanceTestMillis(1000);
    len = makeSignedAdvertFlood(buf, 7);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
}

void test_relay_respects_role_and_rebroadcast_mode()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len;

    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    Time::advanceTestMillis(1000);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_NONE;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x50, 0x50); // distinct frame: the dedup ring remembers the first
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    Time::advanceTestMillis(1000);
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x90, 0x90);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
}

// A flood arriving while another is queued gets its own slot: the old single relay slot dropped it.
void test_second_flood_is_queued_beside_the_first()
{
    MeshCoreRepeater repeater;
    uint8_t first[64], second[64];
    size_t firstLen = makeFloodFrame(first, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(first, firstLen, 5, kAirtimeMs);
    size_t secondLen = makeFloodFrame(second, kTxtMsg, 2, 1, 0x10, 0x20);
    repeater.handleReceivedFrame(second, secondLen, 5, kAirtimeMs);
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txDropped);

    Time::advanceTestMillis(3000); // both due
    uint8_t out[64];
    // Same priority: the one due first goes first, and both go.
    size_t n1 = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT32(0, n1);
    repeater.clearPendingFrame();
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    size_t n2 = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT32(0, n2);
    repeater.clearPendingFrame();
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

static size_t queueDistinctFlood(MeshCoreRepeater &repeater, uint8_t hops, uint8_t seed)
{
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, hops, 1, 0x10, seed);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    return len;
}

// A full queue keeps the floods with the fewest hops (ZephCore: the new one expels the worst only if it ranks above it).
void test_full_queue_evicts_the_flood_with_most_hops()
{
    MeshCoreRepeater repeater;
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++)
        queueDistinctFlood(repeater, 4, (uint8_t)(0x40 + n));
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txDropped);

    // Same hops as the worst queued: does not rank above it, dropped.
    queueDistinctFlood(repeater, 4, 0x70);
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txDropped);

    // Fewer hops: takes the place of one of the 4-hop floods.
    queueDistinctFlood(repeater, 1, 0x71);
    TEST_ASSERT_EQUAL_UINT32(2, repeater.txDropped);

    Time::advanceTestMillis(3000);
    uint8_t out[64];
    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]); // the 1-hop flood goes first
}

// A flood dropped (or evicted) for lack of room was never forwarded, so it must not stay remembered as seen:
// a later copy of it still gets relayed.
void test_flood_dropped_for_room_is_relayed_when_heard_again()
{
    MeshCoreRepeater repeater;
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++)
        queueDistinctFlood(repeater, 4, (uint8_t)(0x40 + n));
    queueDistinctFlood(repeater, 4, 0x70); // dropped: queue full of equal floods
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txDropped);

    // Make room, then hear the dropped one again (one hop further).
    Time::advanceTestMillis(3000);
    uint8_t out[64];
    repeater.takePendingFrame(out, sizeof(out));
    repeater.clearPendingFrame();

    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 5, 1, 0x30, 0x70);
    const uint32_t dupBefore = repeater.rxDup;
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_EQUAL_UINT32(dupBefore, repeater.rxDup);
}

// An evicted flood is forgotten too: it was remembered when queued, but never went on air.
void test_evicted_flood_is_forgotten()
{
    MeshCoreRepeater repeater;
    uint8_t victim[64];
    size_t victimLen = makeFloodFrame(victim, kTxtMsg, 4, 1, 0x10, 0x40);
    repeater.handleReceivedFrame(victim, victimLen, 5, kAirtimeMs);
    for (uint8_t n = 1; n < MeshCoreRepeater::QUEUE_SIZE; n++)
        queueDistinctFlood(repeater, 4, (uint8_t)(0x40 + n));

    // Every queued flood ties; the one due last is evicted. Push more 1-hop floods until the victim is gone.
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++)
        queueDistinctFlood(repeater, 1, (uint8_t)(0x80 + n));
    TEST_ASSERT_EQUAL_UINT32(MeshCoreRepeater::QUEUE_SIZE, repeater.txDropped);

    // All the 4-hop floods are gone, so a copy of any of them is a new packet again.
    const uint32_t dupBefore = repeater.rxDup;
    Time::advanceTestMillis(3000);
    uint8_t out[64];
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++) {
        TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
        repeater.clearPendingFrame();
    }
    repeater.handleReceivedFrame(victim, victimLen, 5, kAirtimeMs);
    TEST_ASSERT_EQUAL_UINT32(dupBefore, repeater.rxDup);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
}

// A radio event between taking a frame and clearing it (the channel scan dispatches a pending RX) can evict the frame
// and reuse its slot. Clearing must then leave the newcomer alone instead of dropping whatever sits at that index.
void test_clear_after_the_taken_frame_was_evicted_keeps_the_newcomer()
{
    MeshCoreRepeater repeater;
    queueDistinctFlood(repeater, 6, 0x30);
    Time::advanceTestMillis(3000); // the 6-hop flood is due
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE - 1; n++)
        queueDistinctFlood(repeater, 1, (uint8_t)(0x50 + n)); // better, but not due yet

    uint8_t out[64];
    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x06, out[1]);

    queueDistinctFlood(repeater, 1, 0x60); // queue full: the 6-hop flood is the worst, so it is evicted
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txDropped);
    repeater.clearPendingFrame();

    Time::advanceTestMillis(3000);
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++) {
        TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
        TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);
        repeater.clearPendingFrame();
    }
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

// Routed traffic goes ahead of our own frames, those ahead of floods; a due flood is not held back by a
// better frame that is not due yet.
void test_queue_order_is_direct_then_own_then_floods_by_hops()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    const uint8_t us = repeater.getIdentityPub()[0];

    queueDistinctFlood(repeater, 5, 0x41);
    queueDistinctFlood(repeater, 1, 0x42);

    uint8_t own[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t ownLen = repeater.buildAdvert(own, sizeof(own), "Xavi M1", 1792521507, false);
    TEST_ASSERT_TRUE(repeater.queueOwnFrame(own, ownLen));

    uint8_t direct[16];
    size_t i = 0;
    direct[i++] = (kTxtMsg << 2) | 0x02;
    direct[i++] = 0x01;
    direct[i++] = us;
    for (int p = 0; p < 6; p++)
        direct[i++] = (uint8_t)(0xA0 + p);
    repeater.handleReceivedFrame(direct, i, 5, kAirtimeMs);

    Time::advanceTestMillis(3000);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8((kTxtMsg << 2) | 0x02, out[0]); // routed
    repeater.clearPendingFrame();

    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_TRUE(repeater.pendingIsOwn());
    repeater.clearPendingFrame();

    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x02, out[1] & 0x3F); // the 1-hop flood we forwarded, our hash appended: 2 hops
    repeater.clearPendingFrame();

    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x06, out[1] & 0x3F);
    repeater.clearPendingFrame();
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

void test_due_flood_goes_before_a_better_frame_that_is_not_due()
{
    MeshCoreRepeater repeater;
    queueDistinctFlood(repeater, 5, 0x41); // worse priority
    Time::advanceTestMillis(3000);         // due
    queueDistinctFlood(repeater, 1, 0x42); // better priority, but its jitter has not elapsed

    uint8_t out[64];
    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x05, out[1]); // the 5-hop one
}

// Our own frame takes the place of the worst queued flood when the queue is full, but not of routed or own frames.
void test_own_frame_evicts_a_flood_when_the_queue_is_full()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++)
        queueDistinctFlood(repeater, 2, (uint8_t)(0x40 + n));

    uint8_t own[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t ownLen = repeater.buildAdvert(own, sizeof(own), "Xavi M1", 1792521507, false);
    TEST_ASSERT_TRUE(repeater.queueOwnFrame(own, ownLen));
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txDropped);
    TEST_ASSERT_TRUE(repeater.hasPendingOwnFrame());
}

void test_own_frames_are_never_evicted_by_each_other()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t own[MeshCoreRepeater::MAX_FRAME_LEN];
    for (uint8_t n = 0; n < MeshCoreRepeater::QUEUE_SIZE; n++) {
        size_t ownLen = repeater.buildAdvert(own, sizeof(own), "Xavi M1", 1792521507 + n, false);
        TEST_ASSERT_TRUE(repeater.queueOwnFrame(own, ownLen));
    }
    size_t ownLen = repeater.buildAdvert(own, sizeof(own), "Xavi M1", 1792521600, false);
    TEST_ASSERT_FALSE(repeater.queueOwnFrame(own, ownLen));
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txDropped);
}

// --- busy channel backoff and post-TX hold-off ---

// A busy channel is retried after a random number of slots whose range doubles with every consecutive deferral
// (2, 4, 8, 16 slots), so two repeaters that heard the same packet do not come back together.
void test_busy_backoff_window_doubles_up_to_sixteen_slots()
{
    MeshCoreRepeater repeater;
    const uint32_t slot = 28;
    const uint32_t maxSlots[] = {2, 4, 8, 16, 16, 16};
    for (uint32_t cap : maxSlots) {
        for (int trial = 0; trial < 20; trial++) {
            MeshCoreRepeater probe = repeater; // same attempt count each trial
            uint32_t wait = probe.busyBackoffMs(slot);
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(slot, wait);
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(cap * slot, wait);
            TEST_ASSERT_EQUAL_UINT32(0, wait % slot);
        }
        repeater.busyBackoffMs(slot);
    }
}

// Fast presets have a few-ms slot: the backoff never degenerates into a spin.
void test_busy_backoff_has_a_minimum_slot()
{
    MeshCoreRepeater repeater;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(20, repeater.busyBackoffMs(3));
}

// After a transmission the next frame waits a few slots, like Meshtastic's own post-TX delay; and the busy
// backoff starts over.
void test_after_transmit_holds_the_next_frame_back()
{
    MeshCoreRepeater repeater;
    queueDistinctFlood(repeater, 2, 0x41);
    Time::advanceTestMillis(3000);
    TEST_ASSERT_TRUE(repeater.isRelayDue());

    repeater.afterTransmit(28);
    TEST_ASSERT_FALSE(repeater.isRelayDue());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(28, repeater.relayDueInMs());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(3 * 28, repeater.relayDueInMs());

    Time::advanceTestMillis(3 * 28);
    TEST_ASSERT_TRUE(repeater.isRelayDue());
    TEST_ASSERT_EQUAL_UINT32(0, repeater.relayDueInMs());
}

void test_dedup_forget_makes_a_hash_new_again()
{
    MeshCoreDedup<4> dedup;
    dedup.remember(0xAAAA);
    dedup.remember(0xBBBB);
    dedup.forget(0xAAAA);
    TEST_ASSERT_FALSE(dedup.contains(0xAAAA));
    TEST_ASSERT_TRUE(dedup.contains(0xBBBB));
}

// --- adaptive relay delay (ZephCore-style) ---

// Jitter is 20ms floor + rand(0, 5 x airtime x factor), capped at 6 x airtime and 2000ms. With
// airtime 100 and the pre-warmup factor 0.5 the delay lands in [20, 270].
void test_adaptive_delay_is_bounded()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, repeater.getFloodDelayFactor()); // pre-warmup default

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    TEST_ASSERT_FALSE(repeater.isRelayDue());

    uint32_t delay = repeater.relayDueInMs();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(20, delay);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(270, delay); // 20 + 5 x 100 x 0.5

    Time::advanceTestMillis(delay);
    TEST_ASSERT_TRUE(repeater.isRelayDue());
}

// The contention estimate: dupes heard per relay fold into an EMA. Busy mesh -> more dupes ->
// bigger factor (longer jitter); quiet mesh -> back toward the minimum.
void test_delay_factor_reflects_observed_contention()
{
    uint8_t buf[64];
    uint8_t variant[64];

    // Quiet mesh: four relays, zero dupes -> factor settles at the minimum.
    MeshCoreRepeater quiet;
    for (int n = 0; n < 4; n++) {
        size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, (uint8_t)(0x40 + n));
        quiet.handleReceivedFrame(buf, len, 5, kAirtimeMs);
        quiet.clearPendingFrame();
        Time::advanceTestMillis(1000);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.40f, quiet.getFloodDelayFactor());

    // Busy mesh: four relays, each with 30 dupes (path variants of the same packet).
    MeshCoreRepeater busy;
    for (int n = 0; n < 4; n++) {
        size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, (uint8_t)(0x40 + n));
        busy.handleReceivedFrame(buf, len, 5, kAirtimeMs);
        for (int d = 0; d < 30; d++) {
            size_t vlen = makeFloodFrame(variant, kTxtMsg, 2, 1, (uint8_t)(0x30 + d), (uint8_t)(0x40 + n));
            busy.handleReceivedFrame(variant, vlen, 5, kAirtimeMs);
        }
        busy.clearPendingFrame();
        Time::advanceTestMillis(1000);
    }
    // factor = 0.40 + 0.60 x 30/(30+4) ~ 0.93
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.93f, busy.getFloodDelayFactor());
}

// Reactive extension: hearing another node retransmit the queued packet postpones ours, so the
// echo lands after theirs instead of colliding with it (ZephCore extendPendingRetransmit).
void test_dupe_postpones_pending_relay()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    uint32_t initialDelay = repeater.relayDueInMs();

    // A dupe (path variant of the same packet): +0.2 x airtime = 20ms, clock frozen.
    len = makeFloodFrame(buf, kTxtMsg, 3, 1, 0x20, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
    TEST_ASSERT_EQUAL_UINT32(initialDelay + 20, repeater.relayDueInMs());

    // Cumulative extension caps at 12 x airtime = 1200ms: 60 more dupes add nothing past that.
    for (int d = 0; d < 60; d++) {
        len = makeFloodFrame(buf, kTxtMsg, 2, 1, (uint8_t)(0x30 + d), 0x10);
        repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    }
    TEST_ASSERT_EQUAL_UINT32(initialDelay + 1200, repeater.relayDueInMs());
}

// The absolute deadline survives the 32-bit millis() wrap (Throttle::deadlinePassed semantics).
void test_relay_deadline_survives_millis_wrap()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    Time::setTestMillis(0xFFFFFF00u); // just before the wrap

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.isRelayDue()); // deadline wrapped to the other side

    Time::advanceTestMillis(300); // crosses 0; deadline is <= 270ms out
    TEST_ASSERT_TRUE(repeater.isRelayDue());
}

// A queued relay that never got its turn (starved radio events) must not wedge the queue: once
// it is older than the dupe window it is dropped when the next frame is heard.
void test_stale_pending_relay_is_dropped_for_new_frame()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());

    Time::advanceTestMillis(11000); // past DUPE_WINDOW_MS, still queued (nobody sent it)
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x20);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);

    TEST_ASSERT_EQUAL_UINT32(1, repeater.txDropped); // stale one discarded
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed); // neither went on air
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());    // the new one is queued
}

void test_take_pending_frame_copies_without_clearing()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);

    uint8_t out[64];
    TEST_ASSERT_EQUAL_UINT32(0, repeater.takePendingFrame(out, 4)); // too small
    TEST_ASSERT_EQUAL_UINT32(len, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, out, len);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame()); // still queued until clearPendingFrame()

    repeater.clearPendingFrame();
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
}

// --- ADVERT identity (MeshCore Mesh::createAdvert layout) ---

// The companion accepts our ADVERT only if the signature verifies over pub||timestamp||app_data, so check
// the bytes against the same Ed25519 implementation, and the header against what parse() accepts.
void test_advert_is_signed_and_parseable()
{
    MeshCoreRepeater repeater;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++)
        seed[i] = (uint8_t)(0x11 * (i + 1));
    while (!repeater.setIdentitySeed(seed))
        seed[0]++;
    TEST_ASSERT_TRUE(repeater.hasIdentity());

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    const uint32_t timestamp = 1792521507;
    size_t len = repeater.buildAdvert(frame, sizeof(frame), "Xavi M1", timestamp, true);
    // header, path_len, pub, ts, sig, app data (type + name)
    TEST_ASSERT_EQUAL_UINT32(2 + 32 + 4 + 64 + 1 + 7, len);

    TEST_ASSERT_EQUAL_HEX8(0x11, frame[0]); // ADVERT, flood, version 0
    TEST_ASSERT_EQUAL_HEX8(0x40, frame[1]); // no hops yet, 2-byte path hashes
    TEST_ASSERT_EQUAL_MEMORY(repeater.getIdentityPub(), &frame[2], 32);

    uint32_t ts;
    memcpy(&ts, &frame[34], 4);
    TEST_ASSERT_EQUAL_UINT32(timestamp, ts);

    const uint8_t *sig = &frame[38];
    const uint8_t *appData = &frame[102];
    TEST_ASSERT_EQUAL_HEX8(0x82, appData[0]); // repeater (2) with the name flag (0x80)
    TEST_ASSERT_EQUAL_MEMORY("Xavi M1", &appData[1], 7);

    uint8_t message[32 + 4 + 8];
    memcpy(message, &frame[2], 32);
    memcpy(message + 32, &frame[34], 4);
    memcpy(message + 36, appData, 8);
    TEST_ASSERT_TRUE(Ed25519::verify(sig, repeater.getIdentityPub(), message, sizeof(message)));

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(frame, len, &info));
    TEST_ASSERT_EQUAL_UINT8(0x04, info.payload_type);
}

// Our own ADVERT goes out on the relay slot without being counted as a relay.
void test_own_advert_is_queued_and_not_a_relay()
{
    MeshCoreRepeater repeater;
    uint8_t seed[32] = {0x42};
    while (!repeater.setIdentitySeed(seed))
        seed[0]++;

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = repeater.buildAdvert(frame, sizeof(frame), "Xavi M1", 1792521507, true);
    TEST_ASSERT_TRUE(repeater.queueOwnFrame(frame, len));
    TEST_ASSERT_TRUE(repeater.pendingIsOwn());
    TEST_ASSERT_TRUE(repeater.isRelayDue());

    repeater.clearPendingFrame(); // the radio path skips markRelayed() for own frames, so txRelayed stays 0
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

// --- repeater behaviour once we have an identity (MeshCore Mesh.cpp parity) ---

static void giveIdentity(MeshCoreRepeater &repeater)
{
    uint8_t seed[32];
    for (int i = 0; i < 32; i++)
        seed[i] = (uint8_t)(0x5A ^ i);
    while (!repeater.setIdentitySeed(seed)) // a reserved key: try the next seed
        seed[0]++;
}

// A zero-hop advert is route DIRECT with an empty path (Mesh::sendZeroHop): neighbours hear it, nobody relays it.
void test_zero_hop_advert_header()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = repeater.buildAdvert(frame, sizeof(frame), "Xavi M1", 1792521507, false);
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_EQUAL_HEX8((0x04 << 2) | 0x02, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, frame[1]);
}

// Flood forward appends our path hash (public key prefix), like Mesh::routeRecvPacket, so the sender's app can
// show the repeat as heard through us.
void test_flood_relay_appends_our_hash()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10); // 2 hops of 1 byte

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t outLen = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(len + 1, outLen);
    TEST_ASSERT_EQUAL_HEX8(buf[0], out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[1]);                         // 3 hops now
    TEST_ASSERT_EQUAL_MEMORY(&buf[2], &out[2], 2);                // previous hops kept in order
    TEST_ASSERT_EQUAL_HEX8(repeater.getIdentityPub()[0], out[4]); // ours appended
    TEST_ASSERT_EQUAL_MEMORY(&buf[4], &out[5], len - 4);          // payload untouched
}

// A direct frame is forwarded only when we are its next hop, with our hop removed (Mesh::removeSelfFromPath).
void test_direct_frame_forwarded_only_as_next_hop()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    const uint8_t us = repeater.getIdentityPub()[0];

    uint8_t buf[16];
    size_t i = 0;
    buf[i++] = (kTxtMsg << 2) | 0x02; // DIRECT
    buf[i++] = 0x02;                  // 2 hops of 1 byte
    buf[i++] = (uint8_t)(us ^ 0xFF);  // first hop is someone else
    buf[i++] = 0x77;
    for (int p = 0; p < 6; p++)
        buf[i++] = (uint8_t)(0xA0 + p);
    repeater.handleReceivedFrame(buf, i, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    buf[2] = us; // now we are the next hop
    repeater.handleReceivedFrame(buf, i, 5, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t outLen = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(i - 1, outLen);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]); // one hop left
    TEST_ASSERT_EQUAL_HEX8(0x77, out[2]); // and it is the next one
    TEST_ASSERT_EQUAL_MEMORY(&buf[4], &out[3], 6);
}

// Neighbour discovery: answer a zero-hop NODE_DISCOVER_REQ for repeaters with our key and the request tag.
void test_discover_request_gets_a_reply()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t req[2 + 10];
    req[0] = (0x0B << 2) | 0x02; // CONTROL, DIRECT
    req[1] = 0x00;               // zero hop
    req[2] = 0x80;               // NODE_DISCOVER_REQ, full key
    req[3] = 1 << 2;             // filter: repeaters
    req[4] = 0xDE;
    req[5] = 0xAD;
    req[6] = 0xBE;
    req[7] = 0xEF;
    memset(&req[8], 0, 4); // since = 0

    repeater.handleReceivedFrame(req, sizeof(req), 6.0f, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t outLen = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(2 + 6 + 32, outLen);
    TEST_ASSERT_EQUAL_HEX8((0x0B << 2) | 0x02, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x92, out[2]);       // DISCOVER_RESP | repeater
    TEST_ASSERT_EQUAL_INT8(24, (int8_t)out[3]); // SNR x4
    TEST_ASSERT_EQUAL_MEMORY(&req[4], &out[4], 4);
    TEST_ASSERT_EQUAL_MEMORY(repeater.getIdentityPub(), &out[8], 32);
    TEST_ASSERT_TRUE(repeater.pendingIsOwn());
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed);
}

// No identity: relay stays transparent, discovery is ignored.
void test_without_identity_relay_is_transparent()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_EQUAL_UINT32(len, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(buf, out, len);
}

// --- MeshCore parity fixes ---

// RFC 8032 test vector 1: the signature MeshCore verifies must be the standard Ed25519 one, not just one our library
// agrees with itself about.
void test_ed25519_matches_rfc8032()
{
    static const uint8_t seed[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                     0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                     0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    static const uint8_t pub[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                    0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                    0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    static const uint8_t sig[64] = {0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80,
                                    0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73,
                                    0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b,
                                    0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0,
                                    0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};
    uint8_t derived[32];
    Ed25519::derivePublicKey(derived, seed);
    TEST_ASSERT_EQUAL_MEMORY(pub, derived, 32);
    uint8_t out[64];
    Ed25519::sign(out, seed, derived, "", 0);
    TEST_ASSERT_EQUAL_MEMORY(sig, out, 64);
}

// MeshCore refuses a key whose 1-byte path hash would be reserved (0x00 / 0xFF).
void test_identity_with_reserved_hash_is_rejected()
{
    uint8_t seed[32] = {0};
    uint8_t pub[32];
    bool sawReserved = false, sawValid = false;
    for (int n = 0; n < 2000 && !(sawReserved && sawValid); n++) {
        seed[0] = (uint8_t)n;
        seed[1] = (uint8_t)(n >> 8);
        Ed25519::derivePublicKey(pub, seed);
        MeshCoreRepeater repeater;
        bool ok = repeater.setIdentitySeed(seed);
        if (pub[0] == 0x00 || pub[0] == 0xFF) {
            TEST_ASSERT_FALSE(ok);
            TEST_ASSERT_FALSE(repeater.hasIdentity());
            sawReserved = true;
        } else {
            TEST_ASSERT_TRUE(ok);
            sawValid = true;
        }
    }
    TEST_ASSERT_TRUE(sawReserved && sawValid);
}

// Types MeshCore never floods, or truncated packets, are not relayed.
void test_only_meshcore_routed_types_are_flooded()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, 0x0F, 2, 1, 0x10, 0x10); // RAW_CUSTOM
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    // A TXT_MSG with only 4 payload bytes (dest, src, 2-byte MAC, no data) is truncated.
    size_t i = 0;
    buf[i++] = (kTxtMsg << 2) | 0x01;
    buf[i++] = 0x00;
    for (int p = 0; p < 4; p++)
        buf[i++] = (uint8_t)(0x30 + p);
    repeater.handleReceivedFrame(buf, i, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

// Payloads over MeshCore's 184 bytes and paths over 64 bytes are not MeshCore packets.
void test_parse_enforces_meshcore_limits()
{
    uint8_t buf[MeshCoreRepeater::MAX_FRAME_LEN];
    buf[0] = (kTxtMsg << 2) | 0x01;
    buf[1] = 0x00;
    memset(&buf[2], 0x33, 185);
    TEST_ASSERT_FALSE(MeshCoreRepeater::parse(buf, 2 + 185, NULL));
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(buf, 2 + 184, NULL));

    buf[1] = (uint8_t)((1 << 6) | 33); // 33 hops x 2 bytes = 66 path bytes
    TEST_ASSERT_FALSE(MeshCoreRepeater::parse(buf, 2 + 66 + 10, NULL));
}

// A late copy (well past the old 15 s window) is still a duplicate, like MeshCore's time-unbounded table.
void test_dedupe_has_no_time_expiry()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    repeater.clearPendingFrame();
    Time::advanceTestMillis(10 * 60 * 1000);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
}

// Our own flood ADVERT echoed back by a neighbour is never relayed.
void test_own_advert_echo_is_not_relayed()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = repeater.buildAdvert(frame, sizeof(frame), "Xavi M1", 1792521507, true);
    repeater.handleReceivedFrame(frame, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

// A genuine ADVERT from another repeater (own identity, signed), with hashCount path bytes spliced in: the
// signature does not cover the path, which every forwarder rewrites.
static size_t makeSignedAdvertFlood(uint8_t *buf, uint8_t hashCount)
{
    MeshCoreRepeater remote;
    giveIdentity(remote);
    uint8_t advert[MeshCoreRepeater::MAX_FRAME_LEN];
    const size_t advertLen = remote.buildAdvert(advert, sizeof(advert), "Remote", 1792521507, true);
    size_t i = 0;
    buf[i++] = advert[0];
    buf[i++] = hashCount;
    for (uint8_t h = 0; h < hashCount; h++)
        buf[i++] = (uint8_t)(0x10 + h);
    memcpy(&buf[i], &advert[2], advertLen - 2);
    return i + advertLen - 2;
}

// Mesh::onRecvPacket drops an ADVERT whose signature does not verify; forwarding it would spread forgeries.
// The check is deferred to takePendingFrame(): it is slow enough to deafen the radio, so it runs with the radio
// listening, just before the frame would go on air.
void test_advert_with_bad_signature_is_not_relayed()
{
    MeshCoreRepeater repeater;
    uint8_t buf[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = makeSignedAdvertFlood(buf, 1);

    buf[len - 1] ^= 0x01; // tamper with the advertised name
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame()); // queued unverified...
    TEST_ASSERT_EQUAL_UINT32(0, repeater.rxDropped);

    Time::advanceTestMillis(3000);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_EQUAL_UINT32(0, repeater.takePendingFrame(out, sizeof(out))); // ...and refused when its turn comes
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);

    // The forged copy again: remembered, so it is not verified a second time.
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);

    buf[len - 1] ^= 0x01; // restored: the genuine advert is a different packet and is forwarded
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    Time::advanceTestMillis(3000);
    TEST_ASSERT_EQUAL_UINT32(len, repeater.takePendingFrame(out, sizeof(out)));
}

// A forged advert ahead of a genuine frame must not block it: the pick moves on to the next one.
void test_forged_advert_does_not_block_the_next_frame()
{
    MeshCoreRepeater repeater;
    uint8_t forged[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t forgedLen = makeSignedAdvertFlood(forged, 0); // 0 hops: ranks first
    forged[forgedLen - 1] ^= 0x01;
    repeater.handleReceivedFrame(forged, forgedLen, 5, kAirtimeMs);
    queueDistinctFlood(repeater, 3, 0x41);

    Time::advanceTestMillis(3000);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_GREATER_THAN_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8((kTxtMsg << 2) | 0x01, out[0]); // the text, not the advert
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);
}

void test_advert_with_valid_signature_is_relayed()
{
    MeshCoreRepeater repeater;
    uint8_t buf[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t len = makeSignedAdvertFlood(buf, 1);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    Time::advanceTestMillis(3000);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    TEST_ASSERT_EQUAL_UINT32(len, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0, repeater.rxDropped);
}

// --- TRACE ---

// Direct TRACE: path holds the SNRs gathered so far; payload = tag, auth, flags (hash size exponent), hashes to visit.
static size_t makeTrace(uint8_t *buf, uint8_t snrCount, const uint8_t *hashes, uint8_t hashCount, uint8_t flags)
{
    size_t i = 0;
    buf[i++] = (0x09 << 2) | 0x02;
    buf[i++] = snrCount;
    for (uint8_t k = 0; k < snrCount; k++)
        buf[i++] = (uint8_t)(0x20 + k);
    const uint8_t head[9] = {0x01, 0x02, 0x03, 0x04, 0xA1, 0xA2, 0xA3, 0xA4, flags};
    memcpy(&buf[i], head, sizeof(head));
    i += sizeof(head);
    memcpy(&buf[i], hashes, (size_t)hashCount << (flags & 3));
    return i + ((size_t)hashCount << (flags & 3));
}

void test_trace_is_forwarded_with_our_snr()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    const uint8_t us = repeater.getIdentityPub()[0];
    const uint8_t hashes[3] = {0x77, us, 0x99};
    uint8_t buf[64];
    size_t len = makeTrace(buf, 1, hashes, 3, 0); // one hop done, we are the second in the list

    repeater.handleReceivedFrame(buf, len, 5.5f, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t outLen = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(len + 1, outLen);
    TEST_ASSERT_EQUAL_HEX8(buf[0], out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[1]);                // two SNRs now
    TEST_ASSERT_EQUAL_HEX8(0x20, out[2]);                // the one already there
    TEST_ASSERT_EQUAL_INT8(22, (int8_t)out[3]);          // our SNR x4
    TEST_ASSERT_EQUAL_MEMORY(&buf[3], &out[4], len - 3); // payload untouched
}

void test_trace_is_ignored_unless_we_are_next()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    const uint8_t us = repeater.getIdentityPub()[0];
    uint8_t buf[64];

    const uint8_t elsewhere[3] = {0x77, (uint8_t)(us ^ 0xFF), 0x99};
    size_t len = makeTrace(buf, 1, elsewhere, 3, 0);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    const uint8_t done[2] = {0x77, us};
    len = makeTrace(buf, 2, done, 2, 0); // path as long as the list: the trace ended before us
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

void test_trace_needs_an_identity()
{
    MeshCoreRepeater repeater;
    const uint8_t hashes[1] = {0x42};
    uint8_t buf[64];
    size_t len = makeTrace(buf, 0, hashes, 1, 0);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

// A route may cross the same repeater twice (out and back): both passes are forwarded.
void test_trace_revisiting_us_is_forwarded_twice()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    const uint8_t us = repeater.getIdentityPub()[0];
    const uint8_t hashes[3] = {us, 0x55, us};
    uint8_t buf[64];

    size_t len = makeTrace(buf, 0, hashes, 3, 0);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    repeater.clearPendingFrame();

    len = makeTrace(buf, 2, hashes, 3, 0);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    repeater.clearPendingFrame();

    len = makeTrace(buf, 2, hashes, 3, 0); // the very same frame again is a duplicate
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

void test_trace_with_wider_hashes()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t hashes[4 * 2] = {0xEE, 0xEE, 0xEE, 0xEE};
    memcpy(&hashes[4], repeater.getIdentityPub(), 4);
    uint8_t buf[64];
    size_t len = makeTrace(buf, 1, hashes, 2, 2); // flags&3 = 2: 4-byte hashes, we are the second

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
}

// --- MULTIPART ---

// Direct MULTIPART ACK: [remaining<<4 | ACK] then the ACK crc. We are the first hop.
static size_t makeMultipart(uint8_t *buf, uint8_t us, uint8_t innerType)
{
    size_t i = 0;
    buf[i++] = (0x0A << 2) | 0x02;
    buf[i++] = 0x02; // two hops
    buf[i++] = us;
    buf[i++] = 0x66;
    buf[i++] = (uint8_t)((2 << 4) | innerType);
    buf[i++] = 0xC1;
    buf[i++] = 0xC2;
    buf[i++] = 0xC3;
    buf[i++] = 0xC4;
    return i;
}

void test_multipart_ack_is_forwarded_as_plain_ack()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t buf[32];
    size_t len = makeMultipart(buf, repeater.getIdentityPub()[0], 0x03);

    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    uint8_t out[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t outLen = repeater.takePendingFrame(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(2 + 1 + 4, outLen);
    TEST_ASSERT_EQUAL_HEX8((0x03 << 2) | 0x02, out[0]); // ACK, direct
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);               // we are off the path
    TEST_ASSERT_EQUAL_HEX8(0x66, out[2]);
    const uint8_t crc[4] = {0xC1, 0xC2, 0xC3, 0xC4};
    TEST_ASSERT_EQUAL_MEMORY(crc, &out[3], 4);
}

void test_multipart_other_types_are_not_forwarded()
{
    MeshCoreRepeater repeater;
    giveIdentity(repeater);
    uint8_t buf[32];
    size_t len = makeMultipart(buf, repeater.getIdentityPub()[0], 0x02);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    len = makeMultipart(buf, (uint8_t)(repeater.getIdentityPub()[0] ^ 0xFF), 0x03); // not our hop
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_flood_frame_parses_with_expected_fields);
    RUN_TEST(test_transport_flood_frame_accounts_for_transport_codes);
    RUN_TEST(test_malformed_frames_are_rejected);
    RUN_TEST(test_meshtastic_broadcast_is_never_misread);
    RUN_TEST(test_meshtastic_unicast_to_us_is_not_swallowed);
    RUN_TEST(test_meshtastic_unicast_from_us_is_not_swallowed);
    RUN_TEST(test_short_meshcore_frame_survives_veto);
    RUN_TEST(test_encrypted_payload_needs_mac_and_whole_cipher_blocks);
    RUN_TEST(test_meshtastic_unicast_between_others_is_not_misread);
    RUN_TEST(test_flood_is_queued_for_relay_once_and_deduped);
    RUN_TEST(test_dedupe_is_path_independent);
    RUN_TEST(test_direct_frames_are_never_relayed);
    RUN_TEST(test_flood_with_full_path_is_dropped);
    RUN_TEST(test_advert_flood_hop_limit);
    RUN_TEST(test_relay_respects_role_and_rebroadcast_mode);
    RUN_TEST(test_second_flood_is_queued_beside_the_first);
    RUN_TEST(test_full_queue_evicts_the_flood_with_most_hops);
    RUN_TEST(test_flood_dropped_for_room_is_relayed_when_heard_again);
    RUN_TEST(test_evicted_flood_is_forgotten);
    RUN_TEST(test_clear_after_the_taken_frame_was_evicted_keeps_the_newcomer);
    RUN_TEST(test_queue_order_is_direct_then_own_then_floods_by_hops);
    RUN_TEST(test_due_flood_goes_before_a_better_frame_that_is_not_due);
    RUN_TEST(test_own_frame_evicts_a_flood_when_the_queue_is_full);
    RUN_TEST(test_own_frames_are_never_evicted_by_each_other);
    RUN_TEST(test_busy_backoff_window_doubles_up_to_sixteen_slots);
    RUN_TEST(test_busy_backoff_has_a_minimum_slot);
    RUN_TEST(test_after_transmit_holds_the_next_frame_back);
    RUN_TEST(test_dedup_forget_makes_a_hash_new_again);
    RUN_TEST(test_adaptive_delay_is_bounded);
    RUN_TEST(test_delay_factor_reflects_observed_contention);
    RUN_TEST(test_dupe_postpones_pending_relay);
    RUN_TEST(test_relay_deadline_survives_millis_wrap);
    RUN_TEST(test_stale_pending_relay_is_dropped_for_new_frame);
    RUN_TEST(test_take_pending_frame_copies_without_clearing);
    RUN_TEST(test_advert_is_signed_and_parseable);
    RUN_TEST(test_own_advert_is_queued_and_not_a_relay);
    RUN_TEST(test_zero_hop_advert_header);
    RUN_TEST(test_flood_relay_appends_our_hash);
    RUN_TEST(test_direct_frame_forwarded_only_as_next_hop);
    RUN_TEST(test_discover_request_gets_a_reply);
    RUN_TEST(test_without_identity_relay_is_transparent);
    RUN_TEST(test_ed25519_matches_rfc8032);
    RUN_TEST(test_identity_with_reserved_hash_is_rejected);
    RUN_TEST(test_only_meshcore_routed_types_are_flooded);
    RUN_TEST(test_parse_enforces_meshcore_limits);
    RUN_TEST(test_dedupe_has_no_time_expiry);
    RUN_TEST(test_own_advert_echo_is_not_relayed);
    RUN_TEST(test_advert_with_bad_signature_is_not_relayed);
    RUN_TEST(test_forged_advert_does_not_block_the_next_frame);
    RUN_TEST(test_advert_with_valid_signature_is_relayed);
    RUN_TEST(test_trace_is_forwarded_with_our_snr);
    RUN_TEST(test_trace_is_ignored_unless_we_are_next);
    RUN_TEST(test_trace_needs_an_identity);
    RUN_TEST(test_trace_revisiting_us_is_forwarded_twice);
    RUN_TEST(test_trace_with_wider_hashes);
    RUN_TEST(test_multipart_ack_is_forwarded_as_plain_ack);
    RUN_TEST(test_multipart_other_types_are_not_forwarded);
    exit(UNITY_END());
}

void loop() {}
