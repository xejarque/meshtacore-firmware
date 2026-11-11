// Unit tests for nexus::writeVault in src/modules/NexusRuntime.h - the atomic save of the Nexus vault (/nexus.bin).
//
// Why it matters: the vault is written to /nexus.bin.tmp, read back and renamed over /nexus.bin. On a 28 KB LittleFS
// (nRF52) a tmp left behind by a failed save holds blocks that every other save (nodes, config) then lacks.
//
// Regressions guarded: a failed readback or rename that leaves /nexus.bin.tmp on disk; a successful save that leaves
// anything but /nexus.bin; a failed save that touches the previous /nexus.bin.
#include "modules/NexusRuntime.h"

#include "TestUtil.h"
#include <Arduino.h>
#include <map>
#include <string>
#include <unity.h>
#include <vector>

namespace
{
enum Mode { READ, WRITE };

struct FakeFs;

struct FakeFile {
    FakeFs *fs = nullptr;
    std::string path;
    size_t pos = 0;
    explicit operator bool() const { return fs != nullptr; }
    size_t write(const uint8_t *bytes, size_t size);
    int read();
    size_t size() const;
    void close() {}
};

// In-memory filesystem with switches to make the write short, the readback differ or the rename fail.
struct FakeFs {
    std::map<std::string, std::vector<uint8_t>> files;
    bool shortWrite = false;
    bool corruptReadback = false;
    bool failRename = false;

    bool exists(const char *path) const { return files.count(path) != 0; }
    bool remove(const char *path) { return files.erase(path) != 0; }
    FakeFile open(const char *path, Mode mode)
    {
        if (mode == WRITE)
            files[path].clear();
        else if (!exists(path))
            return FakeFile();
        FakeFile f;
        f.fs = this;
        f.path = path;
        return f;
    }
    bool rename(const char *from, const char *to)
    {
        if (failRename || !exists(from))
            return false;
        files[to] = files[from];
        files.erase(from);
        return true;
    }
};

size_t FakeFile::write(const uint8_t *bytes, size_t size)
{
    const size_t kept = fs->shortWrite ? size / 2 : size;
    fs->files[path].insert(fs->files[path].end(), bytes, bytes + kept);
    return kept;
}

int FakeFile::read()
{
    const std::vector<uint8_t> &data = fs->files[path];
    if (pos >= data.size())
        return -1;
    const uint8_t byte = data[pos++];
    return fs->corruptReadback ? (uint8_t)(byte ^ 0xFF) : byte;
}

size_t FakeFile::size() const
{
    return fs->files[path].size();
}

const uint8_t kImage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const std::vector<uint8_t> kPrevious = {0xAA, 0xBB};

bool save(FakeFs &fs)
{
    return meshtastic::nexus::writeVault(fs, kImage, sizeof(kImage), READ, WRITE);
}

void assertFailedCleanly(FakeFs &fs)
{
    TEST_ASSERT_FALSE(fs.exists("/nexus.bin.tmp"));
    TEST_ASSERT_TRUE(fs.files["/nexus.bin"] == kPrevious);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_a_good_save_leaves_only_the_vault()
{
    FakeFs fs;
    fs.files["/nexus.bin"] = kPrevious;
    TEST_ASSERT_TRUE(save(fs));
    TEST_ASSERT_FALSE(fs.exists("/nexus.bin.tmp"));
    TEST_ASSERT_EQUAL_MEMORY(kImage, fs.files["/nexus.bin"].data(), sizeof(kImage));
}

void test_a_short_write_removes_the_tmp()
{
    FakeFs fs;
    fs.files["/nexus.bin"] = kPrevious;
    fs.shortWrite = true;
    TEST_ASSERT_FALSE(save(fs));
    assertFailedCleanly(fs);
}

void test_a_failed_readback_removes_the_tmp()
{
    FakeFs fs;
    fs.files["/nexus.bin"] = kPrevious;
    fs.corruptReadback = true;
    TEST_ASSERT_FALSE(save(fs));
    assertFailedCleanly(fs);
}

void test_a_failed_rename_removes_the_tmp()
{
    FakeFs fs;
    fs.files["/nexus.bin"] = kPrevious;
    fs.failRename = true;
    TEST_ASSERT_FALSE(save(fs));
    assertFailedCleanly(fs);
}

void test_a_stale_tmp_is_replaced()
{
    FakeFs fs;
    fs.files["/nexus.bin.tmp"] = {0x01, 0x02, 0x03};
    TEST_ASSERT_TRUE(save(fs));
    TEST_ASSERT_FALSE(fs.exists("/nexus.bin.tmp"));
    TEST_ASSERT_EQUAL_UINT32(sizeof(kImage), fs.files["/nexus.bin"].size());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_a_good_save_leaves_only_the_vault);
    RUN_TEST(test_a_short_write_removes_the_tmp);
    RUN_TEST(test_a_failed_readback_removes_the_tmp);
    RUN_TEST(test_a_failed_rename_removes_the_tmp);
    RUN_TEST(test_a_stale_tmp_is_replaced);
    exit(UNITY_END());
}

void loop() {}
