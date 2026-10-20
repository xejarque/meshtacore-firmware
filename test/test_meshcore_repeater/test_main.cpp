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
                             uint8_t payloadFill)
{
    size_t i = 0;
    buf[i++] = (payloadType << 2) | 0x01; // ver 0, route FLOOD
    buf[i++] = (uint8_t)(((hashSize - 1) << 6) | hashCount);
    for (uint8_t h = 0; h < hashCount * hashSize; h++)
        buf[i++] = pathFill + h;
    for (uint8_t p = 0; p < 8; p++)
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
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA);

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
    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0xAA, 0xAA); // 12 bytes < 16
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

void test_advert_flood_hop_limit()
{
    MeshCoreRepeater repeater;
    uint8_t buf[160];

    size_t len = makeAdvertFlood(buf, 8, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);

    Time::advanceTestMillis(1000);
    len = makeAdvertFlood(buf, 7, 0x20);
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

// One relay slot: a second flood arriving while one is queued is dropped (flood redundancy).
void test_second_flood_dropped_while_pending()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    uint8_t first[64];
    size_t firstLen = makeFloodFrame(first, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(first, firstLen, 5, kAirtimeMs);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x20);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);

    uint8_t out[64];
    TEST_ASSERT_EQUAL_UINT32(firstLen, repeater.takePendingFrame(out, sizeof(out))); // first one still pending
    TEST_ASSERT_EQUAL_MEMORY(first, out, firstLen);
}

// The flood dropped for lack of a relay slot was never forwarded, so it must not be remembered as
// seen: a later copy of it (a neighbour re-flooding it) still gets relayed.
void test_flood_dropped_while_pending_is_relayed_later()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);

    len = makeFloodFrame(buf, kTxtMsg, 2, 1, 0x10, 0x20);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs); // dropped: slot taken

    repeater.markRelayed();
    repeater.clearPendingFrame();
    Time::advanceTestMillis(1000);

    len = makeFloodFrame(buf, kTxtMsg, 3, 1, 0x30, 0x20); // same payload, one hop further
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_EQUAL_UINT32(0, repeater.rxDup);
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());
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

// A pending relay that never got its turn (starved radio events) must not wedge the slot: once
// it is older than the dupe window it is dropped in favour of the frame just heard.
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
    TEST_ASSERT_TRUE(repeater.hasPendingFrame());    // new one took the slot
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
    TEST_ASSERT_EQUAL_HEX8(0x00, frame[1]); // no path yet
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
    RUN_TEST(test_dedupe_is_path_independent);
    RUN_TEST(test_direct_frames_are_never_relayed);
    RUN_TEST(test_flood_with_full_path_is_dropped);
    RUN_TEST(test_advert_flood_hop_limit);
    RUN_TEST(test_relay_respects_role_and_rebroadcast_mode);
    RUN_TEST(test_second_flood_dropped_while_pending);
    RUN_TEST(test_flood_dropped_while_pending_is_relayed_later);
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
    exit(UNITY_END());
}

void loop() {}
