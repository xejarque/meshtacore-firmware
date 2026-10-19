// Unit tests for src/mesh/MeshCoreRepeater.{h,cpp} - MeshCore frame detection (sniffer) and the
// transparent flood relay.
//
// The code is compile-time gated and the native test build compiles src/ with the flags off, so
// its translation unit is empty there. This suite defines the flags and includes the .cpp
// directly; the flag-off copy contributes no symbols, so nothing collides.
//
// The false-positive half of the suite is the important one: with the sniffer enabled on a real
// node, a Meshtastic frame misread as MeshCore is swallowed instead of routed - and flood-relayed
// as garbage into the MeshCore network.
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

void setUp(void)
{
    Time::setTestMillis(1000); // t == 0 is the "never relayed" sentinel for the cooldown
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
}

void tearDown(void)
{
    Time::useRealClock();
}

// --- frame builders ---

// MeshCore flood frame: 1 header byte, 1 path_len byte, hashCount x hashSize path bytes, payload.
static size_t makeFloodFrame(uint8_t *buf, uint8_t payloadType, uint8_t hashCount, uint8_t hashSize, uint8_t fill)
{
    size_t i = 0;
    buf[i++] = (payloadType << 2) | 0x01; // ver 0, route FLOOD
    buf[i++] = (uint8_t)(((hashSize - 1) << 6) | hashCount);
    for (uint8_t h = 0; h < hashCount * hashSize; h++)
        buf[i++] = fill + h;
    for (uint8_t p = 0; p < 8; p++)
        buf[i++] = (uint8_t)(fill ^ (0x40 + p));
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
static size_t makeMeshtasticFrame(uint8_t *buf, uint32_t to, uint32_t from, uint32_t id)
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
    for (uint8_t p = 0; p < 4; p++)
        buf[i++] = (uint8_t)(0x50 + p);
    return i;
}

// --- parser / detector ---

void test_flood_frame_parses_with_expected_fields()
{
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, &info));
    TEST_ASSERT_EQUAL_UINT8(0x01, info.route_type);
    TEST_ASSERT_EQUAL_UINT8(kTxtMsg, info.payload_type);
    TEST_ASSERT_EQUAL_UINT8(0, info.payload_ver);
    TEST_ASSERT_EQUAL_UINT8(1, info.path_hash_size);
    TEST_ASSERT_EQUAL_UINT8(2, info.path_hash_count);
    TEST_ASSERT_EQUAL_UINT8(4, info.header_len); // header + path_len + 2 path bytes
    TEST_ASSERT_EQUAL_UINT8(8, info.payload_len);
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
    size_t len = makeMeshtasticFrame(buf, to, 0x0BADF00D, 0x55667788);

    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, 0, &info)); // hazard without the veto
    TEST_ASSERT_EQUAL_UINT8(0x01, info.route_type);                                 // ...and it is a flood: would relay

    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, to, &info)); // veto
}

void test_meshtastic_unicast_from_us_is_not_swallowed()
{
    uint8_t buf[64];
    const uint32_t to = 0x20000005; // parses as MeshCore flood
    size_t len = makeMeshtasticFrame(buf, to, kOurNodeNum, 0x55667788);
    TEST_ASSERT_FALSE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, kOurNodeNum, NULL));
}

// Short MeshCore frames have no room for a Meshtastic header and must survive the veto even when
// their first bytes collide with our NodeNum.
void test_short_meshcore_frame_survives_veto()
{
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA); // 12 bytes < 16
    TEST_ASSERT_LESS_THAN_UINT32(16, (uint32_t)len);
    // Construct the collision: raw[0..3] == ourNodeNum.
    uint32_t collision = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
    TEST_ASSERT_TRUE(MeshCoreRepeater::looksLikeMeshCoreFrame(buf, len, collision, NULL));
}

// --- relay policy (Fase B) ---

void test_flood_is_queued_for_relay_once_and_deduped()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA);

    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txRelayed);

    repeater.clearPendingFrame();
    Time::advanceTestMillis(1000);

    // Same frame again: a MeshCore node re-flooded it to us; do not relay twice.
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDup);
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txRelayed);
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

    repeater.handleReceivedFrame(buf, i, 5);
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
    size_t len = makeFloodFrame(buf, kTxtMsg, 63, 2, 0x10);

    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, repeater.txRelayed);
}

void test_relay_respects_role_and_rebroadcast_mode()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len;

    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA);
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    Time::advanceTestMillis(1000);
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_NONE;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x50); // distinct frame: the dedup ring remembers the first
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());

    Time::advanceTestMillis(1000);
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x90);
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
}

void test_relay_cooldown_spaces_retransmissions()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA);
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    repeater.clearPendingFrame();

    // A different frame 100ms later: not a dup, but inside the 250ms relay cooldown.
    Time::advanceTestMillis(100);
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x50);
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.txRelayed);

    // Past the cooldown it goes out.
    Time::advanceTestMillis(200);
    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x90);
    repeater.handleReceivedFrame(buf, len, 5);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(2, repeater.txRelayed);
}

void test_take_pending_frame_copies_without_clearing()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA);
    repeater.handleReceivedFrame(buf, len, 5);

    uint8_t out[64];
    TEST_ASSERT_EQUAL_UINT32(0, repeater.takePendingFrame(out, 4)); // too small
    TEST_ASSERT_EQUAL_UINT32(len, repeater.takePendingFrame(out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, out, len);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame()); // still queued until clearPendingFrame()

    repeater.clearPendingFrame();
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(0, repeater.takePendingFrame(out, sizeof(out)));
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
    RUN_TEST(test_flood_is_queued_for_relay_once_and_deduped);
    RUN_TEST(test_direct_frames_are_never_relayed);
    RUN_TEST(test_flood_with_full_path_is_dropped);
    RUN_TEST(test_relay_respects_role_and_rebroadcast_mode);
    RUN_TEST(test_relay_cooldown_spaces_retransmissions);
    RUN_TEST(test_take_pending_frame_copies_without_clearing);
    exit(UNITY_END());
}

void loop() {}
