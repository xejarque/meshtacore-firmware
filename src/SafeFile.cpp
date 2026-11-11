#include "SafeFile.h"

#ifdef FSCom

/// Bitwise CRC32 (IEEE, reflected). No table on purpose: 1 KB of flash is worth more than speed on a config-sized file.
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// Only way to work on both esp32 and nrf52
static File openFile(const char *filename, bool fullAtomic)
{
    concurrency::LockGuard g(spiLock);
    LOG_DEBUG("Opening %s, fullAtomic=%d", filename, fullAtomic);
    if (!fullAtomic) {
        FSCom.remove(filename); // Nuke the old file to make space (ignore if it !exists)
    }

    String filenameTmp = filename;
    filenameTmp += ".tmp";

    // FILE_O_WRITE appends on Adafruit_LittleFS (nRF52) and STM32 LittleFS, so a tmp left by an interrupted
    // write must go first. exists() guards it: a bare remove() of a missing file logs on Portduino.
    if (FSCom.exists(filenameTmp.c_str())) {
        LOG_DEBUG("Remove stale %s", filenameTmp.c_str());
        // Opening anyway would append the new data to the stale bytes and rename the mix over the good file.
        if (!FSCom.remove(filenameTmp.c_str())) {
            LOG_ERROR("Can't remove stale %s", filenameTmp.c_str());
            return File();
        }
    }

    // clear any previous LFS errors
    return FSCom.open(filenameTmp.c_str(), FILE_O_WRITE);
}

SafeFile::SafeFile(const char *_filename, bool fullAtomic)
    : filename(_filename), f(openFile(_filename, fullAtomic)), fullAtomic(fullAtomic)
{
}

size_t SafeFile::write(uint8_t ch)
{
    if (!f)
        return 0;

    // Hash only what the filesystem accepted, so a short write (full flash) cannot be hidden by the readback.
    size_t written = f.write(ch);
    if (written == 1)
        hash = crc32Update(hash, &ch, 1);
    else
        shortWrite = true;
    return written;
}

size_t SafeFile::write(const uint8_t *buffer, size_t size)
{
    if (!f)
        return 0;

    // This nasty cast is _IMPORTANT_ otherwise the correct adafruit method does not get used (they made a mistake in their
    // typing)
    size_t written = f.write((uint8_t const *)buffer, size);
    hash = crc32Update(hash, buffer, written);
    if (written != size)
        shortWrite = true;
    return written;
}

/**
 * Atomically close the file (overwriting any old version) and readback the contents to confirm the hash matches
 *
 * @return false for failure
 */
bool SafeFile::close()
{
    if (!f)
        return false;

    if (shortWrite) {
        LOG_ERROR("Short write on %s (filesystem full?)", filename.c_str());
        abort();
        return false;
    }

    spiLock->lock();
    f.close();
    spiLock->unlock();

    // A failed tmp is removed: on a small LittleFS its blocks would starve every other save until the next write of
    // this file.
    if (!testReadback()) {
        abort();
        return false;
    }

    // Rename or overwrite (atomic operation)
    String filenameTmp = filename;
    filenameTmp += ".tmp";
    if (!renameFile(filenameTmp.c_str(), filename.c_str())) {
        LOG_ERROR("Can't rename new pref file");
        abort();
        return false;
    }

    return true;
}

void SafeFile::abort()
{
    concurrency::LockGuard g(spiLock);
    if (f)
        f.close();
    String filenameTmp = filename;
    filenameTmp += ".tmp";
    if (FSCom.exists(filenameTmp.c_str()))
        FSCom.remove(filenameTmp.c_str());
    LOG_WARN("Aborted write of %s, previous version kept", filename.c_str());
}

/// Read our (closed) tempfile back in and compare the hash
bool SafeFile::testReadback()
{
    concurrency::LockGuard g(spiLock);

    String filenameTmp = filename;
    filenameTmp += ".tmp";
    auto f2 = FSCom.open(filenameTmp.c_str(), FILE_O_READ);
    if (!f2) {
        LOG_ERROR("Can't open tmp file for readback");
        return false;
    }

    int c = 0;
    uint32_t test_hash = 0xFFFFFFFFu;
    while ((c = f2.read()) >= 0) {
        const uint8_t byte = (uint8_t)c;
        test_hash = crc32Update(test_hash, &byte, 1);
    }
    f2.close();

    if (test_hash != hash) {
        LOG_ERROR("Readback failed hash mismatch");
        return false;
    }

    return true;
}

#endif