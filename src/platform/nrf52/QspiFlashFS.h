#pragma once

#if defined(NRF52840_XXAA) && defined(EXTERNAL_FLASH_USE_QSPI) && defined(PIN_QSPI_SCK)
#define HAS_QSPI_FLASH_FS 1

#include "Adafruit_LittleFS.h"

/** LittleFS on the board's external QSPI NOR flash, kept apart from the 28 KB internal one. */
class QspiFlashFS : public Adafruit_LittleFS
{
  public:
    QspiFlashFS();

    /// Bring up the QSPI peripheral and mount, formatting a blank or corrupt chip. False when no flash answers.
    bool begin();

    /// JEDEC id read during begin(), 0 when the chip did not answer.
    uint32_t jedecId() const { return jedec; }

  private:
    uint32_t jedec = 0;
};

extern QspiFlashFS QspiFS;

/// Mount, write, read back and time a test file, logging the result. Only built for the spike.
void qspiFlashFsSelfTest();

#endif
