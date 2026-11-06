#pragma once

#if defined(NRF52840_XXAA) && defined(EXTERNAL_FLASH_USE_QSPI) && defined(PIN_QSPI_SCK)
#define HAS_QSPI_FLASH_FS 1

#include "Adafruit_LittleFS.h"

/**
 * LittleFS on the board's external QSPI NOR flash, kept apart from the 28 KB internal one.
 *
 * The chip is never touched at boot: acquire() brings it up on first use, because a probe at power-on can beat a cold
 * supply rail (the first boot after a flash) and would silently leave the node without it. Between uses it sleeps in
 * deep power-down with CS# held high, and wakes by itself on the next read, program or erase, so a file handle kept
 * open stays valid.
 *
 * Boards without the chip do not define HAS_QSPI_FLASH_FS: callers guard on it and keep their data in RAM or InternalFS.
 */
class QspiFlashFS : public Adafruit_LittleFS
{
  public:
    QspiFlashFS();

    /// Power the chip and mount, formatting a blank or corrupt one. True when files can be opened. A failure is not
    /// retried for RETRY_MS, so a caller polling a missing chip does not hammer the bus.
    bool acquire();

    /// Wipe the whole chip and mount it empty again. For a factory reset. False when the chip is absent.
    bool eraseAll();

    bool isMounted() const { return mounted; }

    /// Size of the chip in bytes, from its JEDEC id; 0 until acquire() succeeded.
    uint32_t capacityBytes() const { return capacity; }

    /// JEDEC id read by acquire(), 0 when the chip did not answer.
    uint32_t jedecId() const { return jedec; }

    /// Put the chip to sleep once nothing has used it for IDLE_MS. Called by the idle thread; harmless to call anywhere.
    void sleepIfIdle();

    /// True while the chip is awake. For tests and logs.
    bool isAwake() const;

    static constexpr uint32_t IDLE_MS = 3000;
    static constexpr uint32_t RETRY_MS = 10000;

  private:
    bool mountChip();

    uint32_t jedec = 0;
    uint32_t capacity = 0;
    uint32_t lastFailMs = 0;
    bool failed = false;
    bool mounted = false;
};

extern QspiFlashFS QspiFS;

/// Exercise wake, write, sleep, wake and read-back, logging the result. Only built for the spike.
void qspiFlashFsSelfTest();

#endif
