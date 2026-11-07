#pragma once

#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"

#ifdef FSCom

/**
 * This class provides 'safe'/paranoid file writing.
 *
 * Some of our filesystems (in particular the nrf52) may have bugs beneath our layer.  Therefore we want to
 * be very careful about how we write files.  This class provides a restricted (Stream only) writing API for writing to files.
 *
 * Notably:
 * - we keep a CRC32 of all characters that were written.
 * - We do not allow seeking (because we want to maintain our hash)
 * - we provide an close() method which is similar to close but returns false if we were unable to successfully write the
 * file.  Also this method
 * - atomically replaces any old version of the file on the disk with our new file (after first rereading the file from the disk
 * to confirm the hash matches)
 * - Some files are super huge so we can't do the full atomic rename/copy (because of filesystem size limits).  If !fullAtomic
 * then we still do the readback to verify file is valid so higher level code can handle failures.
 */
class SafeFile : public Print
{
  public:
    explicit SafeFile(char const *filepath, bool fullAtomic = false);

    virtual size_t write(uint8_t);
    virtual size_t write(const uint8_t *buffer, size_t size);

    /**
     * Atomically close the file (deleting any old versions) and readback the contents to confirm the hash matches
     *
     * @return false for failure
     */
    bool close();

    /**
     * Give up on the file: drop the temp copy and leave the previous version untouched. Call it instead of close() when
     * the content being written is known to be incomplete (e.g. the encoder failed half way), because close() would
     * read back what was written, find the checksum consistent and rename a truncated file over the good one.
     */
    void abort();

  private:
    /// Read our (closed) tempfile back in and compare the hash
    bool testReadback();

    String filename;
    File f;
    bool fullAtomic;
    uint32_t hash = 0xFFFFFFFFu; // running CRC32, see crc32Update() in SafeFile.cpp
    bool shortWrite = false;     // the filesystem took fewer bytes than offered: close() must not replace the old file
};

#endif