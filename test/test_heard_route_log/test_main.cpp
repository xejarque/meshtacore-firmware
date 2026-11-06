// Unit tests for HeardRouteLog in src/mesh/HeardRouteLog.h - the routes by which MeshCore texts were heard, kept on
// external flash so the ".path" command can still answer long after the few routes held in RAM were overwritten.
//
// Why it matters: ".path" tells the user which repeaters carried a text. It reads this log for any text that has left the
// small RAM window, so a route that does not come back whole - a shortened path, a lost signal, the wrong text - makes
// the report lie, and a log that cannot rotate would fill the flash and stop recording for good.
//
// Regressions guarded: a path or signal that changes across the round trip (including the longest path and a negative
// SNR and RSSI); a record that reads past its own length; a full slot that is not rotated, or that drops the newest half
// instead of the oldest; a restart that loses the log or continues in the wrong file; a write cut short by a power loss
// that hides the records before it; a search that does not stop where the caller asks.
#include "mesh/HeardRouteLog.h"

#include <Arduino.h> // declares setup() and loop() with the C linkage the host entry point links against

#include "TestUtil.h"
#include <unity.h>
#include <vector>

using namespace HeardRouteLog;

/// Two RAM files standing in for the flash ones.
class RamStorage : public RouteStorage
{
  public:
    std::vector<uint8_t> files[2];
    bool failAppends = false;

    uint32_t size(uint8_t slot) override { return files[slot].size(); }
    bool append(uint8_t slot, const uint8_t *data, size_t len) override
    {
        if (failAppends)
            return false;
        files[slot].insert(files[slot].end(), data, data + len);
        return true;
    }
    size_t read(uint8_t slot, uint32_t offset, uint8_t *out, size_t len) override
    {
        if (offset >= files[slot].size())
            return 0;
        const size_t n = std::min<size_t>(len, files[slot].size() - offset);
        memcpy(out, &files[slot][offset], n);
        return n;
    }
    bool reset(uint8_t slot) override
    {
        files[slot].clear();
        return true;
    }
};

static Route makeRoute(uint32_t id, uint8_t hops = 2, uint8_t hashSize = 1)
{
    Route r;
    r.packetId = id;
    r.frameHash = 0xF00D0000u + id;
    r.fromNum = 0x4D000000u + id;
    r.epochSecs = 1761480000u + id;
    r.snr = -7.25f;
    r.rssi = -103;
    r.channel = 3;
    r.routeType = 1;
    r.hashSize = hashSize;
    r.hopCount = hops;
    for (size_t i = 0; i < Route::PATH_BYTES_MAX; i++)
        r.path[i] = (uint8_t)(0xA0 + i);
    return r;
}

static std::vector<Route> readAll(Log &log)
{
    std::vector<Route> all;
    log.forEach([&](const Route &r) {
        all.push_back(r);
        return true;
    });
    return all;
}

void setUp() {}
void tearDown() {}

void test_round_trip_keeps_every_field()
{
    const Route in = makeRoute(7, 3, 2);
    uint8_t buf[RECORD_MAX];
    const size_t len = encode(in, buf);
    TEST_ASSERT_EQUAL_UINT32(1 + FIXED_LEN + 6, len);
    TEST_ASSERT_EQUAL_UINT8(len - 1, buf[0]);

    Route out;
    TEST_ASSERT_TRUE(decode(&buf[1], buf[0], &out));
    TEST_ASSERT_EQUAL_UINT32(in.packetId, out.packetId);
    TEST_ASSERT_EQUAL_UINT32(in.frameHash, out.frameHash);
    TEST_ASSERT_EQUAL_UINT32(in.fromNum, out.fromNum);
    TEST_ASSERT_EQUAL_UINT32(in.epochSecs, out.epochSecs);
    TEST_ASSERT_EQUAL_FLOAT(-7.25f, out.snr);
    TEST_ASSERT_EQUAL_INT32(-103, out.rssi);
    TEST_ASSERT_EQUAL_UINT8(3, out.channel);
    TEST_ASSERT_EQUAL_UINT8(1, out.routeType);
    TEST_ASSERT_EQUAL_UINT8(2, out.hashSize);
    TEST_ASSERT_EQUAL_UINT8(3, out.hopCount);
    TEST_ASSERT_FALSE(out.own);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.path, out.path, 6);
}

void test_longest_path_and_extreme_signal_survive()
{
    Route in = makeRoute(1, 64, 1);
    in.snr = -40.0f; // beyond what the quarter-dB byte holds: clamped, never wrapped
    in.rssi = -140;
    in.own = true;
    uint8_t buf[RECORD_MAX];
    const size_t len = encode(in, buf);
    TEST_ASSERT_EQUAL_UINT32(RECORD_MAX, len);

    Route out;
    TEST_ASSERT_TRUE(decode(&buf[1], buf[0], &out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.path, out.path, Route::PATH_BYTES_MAX);
    TEST_ASSERT_EQUAL_FLOAT(-32.0f, out.snr);
    TEST_ASSERT_EQUAL_INT32(-140, out.rssi);
    TEST_ASSERT_TRUE(out.own);
}

void test_path_longer_than_the_record_is_refused()
{
    uint8_t buf[RECORD_MAX];
    encode(makeRoute(1, 2, 1), buf);
    Route out;
    TEST_ASSERT_FALSE_MESSAGE(decode(&buf[1], FIXED_LEN - 1, &out), "a body shorter than the fixed part");
    TEST_ASSERT_FALSE_MESSAGE(decode(&buf[1], FIXED_LEN + 1, &out), "claims 2 path bytes, holds 1");
    buf[1 + FIXED_LEN - 1] = 200; // path length beyond MeshCore's 64
    TEST_ASSERT_FALSE(decode(&buf[1], buf[0], &out));
}

void test_records_come_back_in_order()
{
    RamStorage fs;
    Log log(fs, 4096);
    TEST_ASSERT_TRUE(log.begin());
    for (uint32_t i = 1; i <= 5; i++)
        TEST_ASSERT_TRUE(log.append(makeRoute(i)));

    const std::vector<Route> all = readAll(log);
    TEST_ASSERT_EQUAL_UINT32(5, all.size());
    for (uint32_t i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_UINT32(i + 1, all[i].packetId);
}

void test_search_stops_when_asked()
{
    RamStorage fs;
    Log log(fs, 4096);
    log.begin();
    for (uint32_t i = 1; i <= 10; i++)
        log.append(makeRoute(i));

    size_t seen = 0;
    const size_t visited = log.forEach([&](const Route &) { return ++seen < 3; });
    TEST_ASSERT_EQUAL_UINT32(3, visited);
}

void test_full_slot_rotates_and_drops_the_oldest()
{
    RamStorage fs;
    const size_t recordLen = 1 + FIXED_LEN + 2;
    Log log(fs, Log::HEADER_LEN + 4 * recordLen); // four records a slot
    log.begin();
    for (uint32_t i = 1; i <= 6; i++)
        TEST_ASSERT_TRUE(log.append(makeRoute(i)));

    const std::vector<Route> all = readAll(log);
    TEST_ASSERT_EQUAL_UINT32(6, all.size()); // 1-4 in the first slot, 5-6 in the second: nothing lost yet
    TEST_ASSERT_EQUAL_UINT32(1, all.front().packetId);
    TEST_ASSERT_EQUAL_UINT32(6, all.back().packetId);

    for (uint32_t i = 7; i <= 9; i++)
        log.append(makeRoute(i)); // fills the second slot (5-8), then rotates and empties the first for 9

    const std::vector<Route> after = readAll(log);
    TEST_ASSERT_EQUAL_UINT32(5, after.size());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, after.front().packetId, "the oldest slot is the part dropped");
    TEST_ASSERT_EQUAL_UINT32(9, after.back().packetId);
}

void test_restart_continues_in_the_newest_slot()
{
    RamStorage fs;
    const size_t recordLen = 1 + FIXED_LEN + 2;
    {
        Log log(fs, Log::HEADER_LEN + 4 * recordLen);
        log.begin();
        for (uint32_t i = 1; i <= 6; i++)
            log.append(makeRoute(i));
    }
    Log again(fs, Log::HEADER_LEN + 4 * recordLen);
    TEST_ASSERT_TRUE(again.begin());
    again.append(makeRoute(7));

    const std::vector<Route> all = readAll(again);
    TEST_ASSERT_EQUAL_UINT32(7, all.size());
    TEST_ASSERT_EQUAL_UINT32(1, all.front().packetId);
    TEST_ASSERT_EQUAL_UINT32(7, all.back().packetId);
}

void test_torn_record_keeps_the_ones_before_it()
{
    RamStorage fs;
    Log log(fs, 4096);
    log.begin();
    log.append(makeRoute(1));
    log.append(makeRoute(2));
    fs.files[0].resize(fs.files[0].size() - 5); // power cut in the middle of the second record

    const std::vector<Route> all = readAll(log);
    TEST_ASSERT_EQUAL_UINT32(1, all.size());
    TEST_ASSERT_EQUAL_UINT32(1, all[0].packetId);
}

void test_unreadable_header_starts_a_fresh_log()
{
    RamStorage fs;
    fs.files[0] = {'x', 'x', 'x', 'x', 0, 0, 0, 0, 1, 2, 3};
    Log log(fs, 4096);
    TEST_ASSERT_TRUE(log.begin());
    log.append(makeRoute(1));
    TEST_ASSERT_EQUAL_UINT32(1, readAll(log).size());
}

void test_failed_write_is_reported()
{
    RamStorage fs;
    Log log(fs, 4096);
    log.begin();
    fs.failAppends = true;
    TEST_ASSERT_FALSE(log.append(makeRoute(1)));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_keeps_every_field);
    RUN_TEST(test_longest_path_and_extreme_signal_survive);
    RUN_TEST(test_path_longer_than_the_record_is_refused);
    RUN_TEST(test_records_come_back_in_order);
    RUN_TEST(test_search_stops_when_asked);
    RUN_TEST(test_full_slot_rotates_and_drops_the_oldest);
    RUN_TEST(test_restart_continues_in_the_newest_slot);
    RUN_TEST(test_torn_record_keeps_the_ones_before_it);
    RUN_TEST(test_unreadable_header_starts_a_fresh_log);
    RUN_TEST(test_failed_write_is_reported);
    exit(UNITY_END());
}

void loop() {}
