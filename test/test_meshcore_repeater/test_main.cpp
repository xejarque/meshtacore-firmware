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
void test_advert_flood_hop_limit()
{
    MeshCoreRepeater repeater;
    uint8_t buf[64];

    size_t len = makeFloodFrame(buf, kAdvert, 8, 1, 0x10, 0x10);
    repeater.handleReceivedFrame(buf, len, 5, kAirtimeMs);
    TEST_ASSERT_FALSE(repeater.hasPendingFrame());
    TEST_ASSERT_EQUAL_UINT32(1, repeater.rxDropped);

    Time::advanceTestMillis(1000);
    len = makeFloodFrame(buf, kAdvert, 7, 1, 0x10, 0x20);
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
    exit(UNITY_END());
}

void loop() {}
