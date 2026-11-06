#include "configuration.h"

#include "QspiFlashFS.h"

#ifdef HAS_QSPI_FLASH_FS

#include "DebugConfiguration.h"
#include "concurrency/OSThread.h"
#include "mesh/HeardRouteArchive.h"
#include "mesh/Throttle.h"
// The core does not build the nrfx drivers it ships; main-nrf52.cpp pulls nrfx_wdt.c in the same way.
#ifndef NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY
#define NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY 6
#endif
#include <nrfx_qspi.c>
#include <nrfx_qspi.h>

using namespace Adafruit_LittleFS_Namespace;

// NOR flash geometry common to the parts boards fit (MX25R1635F, GD25Q16, W25Q16): 4 KB erase sector, 256 B page.
static constexpr uint32_t SECTOR_SIZE = 4096;
static constexpr uint32_t PAGE_SIZE = 256;

static constexpr uint8_t CMD_WRITE_ENABLE = 0x06;
static constexpr uint8_t CMD_READ_STATUS = 0x05;
static constexpr uint8_t CMD_SECTOR_ERASE = 0x20;
static constexpr uint8_t CMD_DEEP_POWER_DOWN = 0xB9;
static constexpr uint8_t CMD_RELEASE_POWER_DOWN = 0xAB;
static constexpr uint8_t CMD_READ_JEDEC_ID = 0x9F;
static constexpr uint8_t STATUS_BUSY = 0x01;

// JEDEC capacity byte is log2 of the size in bytes: 0x15 = 2 MB, 0x16 = 4 MB, 0x17 = 8 MB.
static constexpr uint8_t CAPACITY_LOG2_MIN = 0x10; // 64 KB
static constexpr uint8_t CAPACITY_LOG2_MAX = 0x19; // 32 MB

// Deep power-down exit takes 35 us on the parts we know (t-exit-dpd); 50 us covers them all.
static constexpr uint32_t EXIT_POWER_DOWN_US = 50;

// The QSPI EasyDMA wants word-aligned RAM and lengths in whole words; littlefs promises neither for its buffers.
static uint32_t bounce[PAGE_SIZE / sizeof(uint32_t)];

static bool peripheralUp = false;
static uint32_t lastUseMs = 0;

static nrf_qspi_cinstr_conf_t commandFor(uint8_t opcode, nrf_qspi_cinstr_len_t len)
{
    nrf_qspi_cinstr_conf_t cinstr = NRFX_QSPI_DEFAULT_CINSTR(opcode, len);
    // nrfx drives WP# and HOLD# low by default; a held chip ignores the bus and reads back as zeros.
    cinstr.io2_level = true;
    cinstr.io3_level = true;
    return cinstr;
}

static bool sendCommand(uint8_t opcode, nrf_qspi_cinstr_len_t len)
{
    nrf_qspi_cinstr_conf_t cinstr = commandFor(opcode, len);
    return nrfx_qspi_cinstr_xfer(&cinstr, NULL, NULL) == NRFX_SUCCESS;
}

static uint8_t readStatus()
{
    uint8_t sr = 0xFF;
    nrf_qspi_cinstr_conf_t cinstr = commandFor(CMD_READ_STATUS, NRF_QSPI_CINSTR_LEN_2B);
    nrfx_qspi_cinstr_xfer(&cinstr, &sr, &sr);
    return sr;
}

// nrfx's own busy check reads the status with HOLD# low, so it never sees the chip; poll ours, bounded.
static bool waitReady(uint32_t timeoutMs = 500)
{
    const uint32_t start = millis();
    while (readStatus() & STATUS_BUSY)
        if (!Throttle::isWithinTimespanMs(start, timeoutMs))
            return false;
    return true;
}

// Init the peripheral and wake the chip, which the bootloader or a previous sleep may have left in deep power-down.
static bool powerUp()
{
    lastUseMs = millis();
    if (peripheralUp)
        return true;
    // Single-line read and page program: no quad-enable bit to set, which differs from one flash vendor to the next.
    nrfx_qspi_config_t cfg =
        NRFX_QSPI_DEFAULT_CONFIG(PIN_QSPI_SCK, PIN_QSPI_CS, PIN_QSPI_IO0, PIN_QSPI_IO1, PIN_QSPI_IO2, PIN_QSPI_IO3);
    cfg.phy_if.sck_freq = NRF_QSPI_FREQ_DIV4; // 8 MHz
    if (nrfx_qspi_init(&cfg, NULL, NULL) != NRFX_SUCCESS)
        return false;
    peripheralUp = true;
    sendCommand(CMD_RELEASE_POWER_DOWN, NRF_QSPI_CINSTR_LEN_1B);
    delayMicroseconds(EXIT_POWER_DOWN_US);
    return true;
}

// Sleep the chip and stop the peripheral. nrfx releases the pins on uninit, and a floating CS# reads as low: the chip
// would stay selected for as long as the node runs, so CS# is driven high by hand.
static void powerDown()
{
    if (!peripheralUp)
        return;
    sendCommand(CMD_DEEP_POWER_DOWN, NRF_QSPI_CINSTR_LEN_1B);
    nrfx_qspi_uninit();
    peripheralUp = false;
    nrf_gpio_cfg_output(PIN_QSPI_CS);
    nrf_gpio_pin_set(PIN_QSPI_CS);
}

static int qspiRead(const struct lfs_config *, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    if (!powerUp())
        return LFS_ERR_IO;
    uint32_t addr = block * SECTOR_SIZE + off;
    uint8_t *out = (uint8_t *)buffer;
    while (size) {
        const lfs_size_t chunk = size < PAGE_SIZE ? size : PAGE_SIZE;
        if (nrfx_qspi_read(bounce, (chunk + 3) & ~3u, addr) != NRFX_SUCCESS)
            return LFS_ERR_IO;
        memcpy(out, bounce, chunk);
        out += chunk;
        addr += chunk;
        size -= chunk;
    }
    return 0;
}

static int qspiProg(const struct lfs_config *, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    if (!powerUp())
        return LFS_ERR_IO;
    uint32_t addr = block * SECTOR_SIZE + off;
    const uint8_t *in = (const uint8_t *)buffer;
    while (size) {
        const lfs_size_t chunk = size < PAGE_SIZE ? size : PAGE_SIZE;
        memcpy(bounce, in, chunk);
        if (nrfx_qspi_write(bounce, chunk, addr) != NRFX_SUCCESS || !waitReady())
            return LFS_ERR_IO;
        in += chunk;
        addr += chunk;
        size -= chunk;
    }
    return 0;
}

// nrfx_qspi_erase starts the ERASE task and never returns on this part, so the watchdog resets the node in a loop.
// Erase with the chip's own commands instead: write-enable, sector erase, then poll the busy bit.
static int qspiErase(const struct lfs_config *, lfs_block_t block)
{
    if (!powerUp())
        return LFS_ERR_IO;
    const uint32_t addr = block * SECTOR_SIZE;
    uint8_t a[3] = {(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    nrf_qspi_cinstr_conf_t erase = commandFor(CMD_SECTOR_ERASE, NRF_QSPI_CINSTR_LEN_4B);
    if (!sendCommand(CMD_WRITE_ENABLE, NRF_QSPI_CINSTR_LEN_1B) || nrfx_qspi_cinstr_xfer(&erase, a, NULL) != NRFX_SUCCESS)
        return LFS_ERR_IO;
    return waitReady() ? 0 : LFS_ERR_IO;
}

static int qspiSync(const struct lfs_config *)
{
    return 0;
}

static struct lfs_config qspiConfig = {
    .context = NULL,
    .read = qspiRead,
    .prog = qspiProg,
    .erase = qspiErase,
    .sync = qspiSync,
    .read_size = PAGE_SIZE,
    .prog_size = PAGE_SIZE,
    .block_size = SECTOR_SIZE,
    .block_count = 0, // set from the chip's JEDEC id before the first mount
    .lookahead = 128,
    .read_buffer = NULL,
    .prog_buffer = NULL,
    .lookahead_buffer = NULL,
    .file_buffer = NULL,
};

QspiFlashFS QspiFS;

/// Polls once a second and sleeps the chip after QspiFlashFS::IDLE_MS without use.
class QspiIdleThread : public concurrency::OSThread
{
  public:
    QspiIdleThread() : OSThread("QspiIdle") {}

  protected:
    int32_t runOnce() override
    {
        QspiFS.sleepIfIdle();
        return 1000;
    }
};

QspiFlashFS::QspiFlashFS() : Adafruit_LittleFS(&qspiConfig) {}

bool QspiFlashFS::isAwake() const
{
    return peripheralUp;
}

void QspiFlashFS::sleepIfIdle()
{
    if (peripheralUp && Throttle::hasElapsed(lastUseMs, IDLE_MS))
        powerDown();
}

bool QspiFlashFS::mountChip()
{
    if (!powerUp())
        return false;

    uint8_t id[3] = {};
    nrf_qspi_cinstr_conf_t cinstr = commandFor(CMD_READ_JEDEC_ID, NRF_QSPI_CINSTR_LEN_4B);
    if (nrfx_qspi_cinstr_xfer(&cinstr, NULL, id) != NRFX_SUCCESS)
        return false;
    // No chip on the bus reads back all zeros or all ones; an unknown capacity byte is not a chip we can size.
    if (id[2] < CAPACITY_LOG2_MIN || id[2] > CAPACITY_LOG2_MAX)
        return false;
    jedec = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
    capacity = 1u << id[2];
    qspiConfig.block_count = capacity / SECTOR_SIZE;

    if (Adafruit_LittleFS::begin())
        return true;
    return format() && Adafruit_LittleFS::begin();
}

bool QspiFlashFS::acquire()
{
    if (mounted) {
        lastUseMs = millis();
        return true;
    }
    if (failed && Throttle::isWithinTimespanMs(lastFailMs, RETRY_MS))
        return false;

    mounted = mountChip();
    if (!mounted) {
        jedec = 0;
        capacity = 0;
        failed = true;
        lastFailMs = millis();
        powerDown();
        LOG_WARN("QSPI flash did not mount");
        return false;
    }
    failed = false;
    LOG_INFO("QSPI flash 0x%06x, %u KB mounted", jedec, capacity / 1024);
    new QspiIdleThread(); // never allocated statically: threads must be created after setup() starts
    return true;
}

bool QspiFlashFS::eraseAll()
{
    if (!acquire())
        return false;
    // LittleFS keeps its state in RAM: unmount before the chip is formatted under it, then mount the empty one.
    end();
    mounted = false;
    if (!format())
        return false;
    mounted = Adafruit_LittleFS::begin();
    return mounted;
}

#ifdef MESHTASTIC_QSPI_FS_SELFTEST
static void runSelfTest()
{
    uint32_t t0 = millis();
    const bool mounted = QspiFS.acquire();
    LOG_INFO("QSPIFS mount=%d jedec=0x%06x size=%u KB in %u ms", mounted, QspiFS.jedecId(), QspiFS.capacityBytes() / 1024,
             millis() - t0);
    if (!mounted)
        return;

    static constexpr size_t RECORD_LEN = 128;
    static constexpr size_t RECORDS = 300;
    uint8_t rec[RECORD_LEN];

    File f = QspiFS.open("/selftest.bin", FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("QSPIFS open for write failed");
        return;
    }
    f.seek(0);
    for (size_t i = 0; i < RECORDS; i++) {
        memset(rec, (uint8_t)i, sizeof(rec));
        f.write(rec, sizeof(rec));
    }
    f.close();

    // Let the idle thread put the chip to sleep, then read through a fresh handle: the read must wake it by itself.
    const uint32_t sleepStart = millis();
    while (QspiFS.isAwake() && Throttle::isWithinTimespanMs(sleepStart, 3 * QspiFlashFS::IDLE_MS)) {
        QspiFS.sleepIfIdle();
        delay(100);
    }
    const bool sleptAfterIdle = !QspiFS.isAwake();

    size_t bad = 0;
    t0 = millis();
    f = QspiFS.open("/selftest.bin", FILE_O_READ);
    for (size_t i = 0; i < RECORDS; i++) {
        if (f.read(rec, sizeof(rec)) != (int)sizeof(rec)) {
            bad++;
            continue;
        }
        for (size_t k = 0; k < sizeof(rec); k++)
            if (rec[k] != (uint8_t)i) {
                bad++;
                break;
            }
    }
    f.close();
    LOG_INFO("QSPIFS slept=%d wokeByRead=%d read=%u ms bad=%u", sleptAfterIdle, QspiFS.isAwake(), millis() - t0, bad);
    HeardRouteArchive::heardRouteArchiveSelfTest();
}

/// The log API only attaches after boot, so a result printed from nrf52Setup() is never seen: run the test once the node is up,
/// and again every minute so a client that connects late still catches one.
class SelfTestThread : public concurrency::OSThread
{
  public:
    SelfTestThread() : OSThread("QspiSelfTest", 20000) {}

  protected:
    int32_t runOnce() override
    {
        runSelfTest();
        return 60000;
    }
};

void qspiFlashFsSelfTest()
{
    new SelfTestThread();
}
#endif

#endif
