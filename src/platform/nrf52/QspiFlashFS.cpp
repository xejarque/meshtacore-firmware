#include "configuration.h"

#include "QspiFlashFS.h"

#ifdef HAS_QSPI_FLASH_FS

#include "DebugConfiguration.h"
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
static constexpr uint32_t FS_SIZE = 2 * 1024 * 1024;

static constexpr uint8_t CMD_RELEASE_POWER_DOWN = 0xAB;
static constexpr uint8_t CMD_READ_JEDEC_ID = 0x9F;

// The QSPI EasyDMA wants word-aligned RAM and lengths in whole words; littlefs promises neither for its buffers.
static uint32_t bounce[PAGE_SIZE / sizeof(uint32_t)];

static nrf_qspi_cinstr_conf_t commandFor(uint8_t opcode, nrf_qspi_cinstr_len_t len)
{
    nrf_qspi_cinstr_conf_t cinstr = NRFX_QSPI_DEFAULT_CINSTR(opcode, len);
    // nrfx drives WP# and HOLD# low by default; a held chip ignores the bus and reads back as zeros.
    cinstr.io2_level = true;
    cinstr.io3_level = true;
    return cinstr;
}

static bool sendCommand(uint8_t opcode, nrf_qspi_cinstr_len_t len, void *rx)
{
    nrf_qspi_cinstr_conf_t cinstr = commandFor(opcode, len);
    return nrfx_qspi_cinstr_xfer(&cinstr, NULL, rx) == NRFX_SUCCESS;
}

static uint8_t readStatus()
{
    uint8_t sr = 0xFF;
    nrf_qspi_cinstr_conf_t cinstr = commandFor(0x05, NRF_QSPI_CINSTR_LEN_2B);
    nrfx_qspi_cinstr_xfer(&cinstr, &sr, &sr);
    return sr;
}

// nrfx's own busy check reads the status with HOLD# low, so it never sees the chip; poll ours, bounded.
static bool waitReady(uint32_t timeoutMs = 500)
{
    const uint32_t start = millis();
    while (readStatus() & 0x01)
        if (millis() - start > timeoutMs)
            return false;
    return true;
}

static int qspiRead(const struct lfs_config *, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
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
    uint32_t addr = block * SECTOR_SIZE + off;
    const uint8_t *in = (const uint8_t *)buffer;
    while (size) {
        const lfs_size_t chunk = size < PAGE_SIZE ? size : PAGE_SIZE;
        memcpy(bounce, in, chunk);
        static uint32_t progs = 0;
        if (progs < 3) {
            LOG_DEBUG("QSPIFS prog @%x len %u sr=%02x", addr, chunk, readStatus());
            Serial.flush();
        }
        const nrfx_err_t err = nrfx_qspi_write(bounce, chunk, addr);
        const bool ok = waitReady();
        if (progs++ < 3) {
            LOG_DEBUG("QSPIFS prog done err=0x%x ok=%d sr=%02x", err, ok, readStatus());
            Serial.flush();
        }
        if (err != NRFX_SUCCESS || !ok)
            return LFS_ERR_IO;
        in += chunk;
        addr += chunk;
        size -= chunk;
    }
    return 0;
}

static int qspiErase(const struct lfs_config *, lfs_block_t block)
{
    const uint32_t addr = block * SECTOR_SIZE;
    const uint8_t srBefore = readStatus();
    sendCommand(0x06, NRF_QSPI_CINSTR_LEN_1B, NULL); // WREN
    const uint8_t srWel = readStatus();
    uint8_t a[3] = {(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    nrf_qspi_cinstr_conf_t se = commandFor(0x20, NRF_QSPI_CINSTR_LEN_4B);
    const nrfx_err_t err = nrfx_qspi_cinstr_xfer(&se, a, NULL);
    const uint32_t t0 = millis();
    const bool ok = waitReady();
    LOG_DEBUG("QSPIFS erase %u sr=%02x wel=%02x err=0x%x ok=%d %u ms sr=%02x", block, srBefore, srWel, err, ok, millis() - t0,
              readStatus());
    Serial.flush();
    return ok && err == NRFX_SUCCESS ? 0 : LFS_ERR_IO;
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
    .block_count = FS_SIZE / SECTOR_SIZE,
    .lookahead = 128,
    .read_buffer = NULL,
    .prog_buffer = NULL,
    .lookahead_buffer = NULL,
    .file_buffer = NULL,
};

QspiFlashFS QspiFS;

QspiFlashFS::QspiFlashFS() : Adafruit_LittleFS(&qspiConfig) {}


bool QspiFlashFS::begin()
{
    // Single-line read and page program: no quad-enable bit to set, which differs from one flash vendor to the next.
    nrfx_qspi_config_t cfg = NRFX_QSPI_DEFAULT_CONFIG(PIN_QSPI_SCK, PIN_QSPI_CS, PIN_QSPI_IO0, PIN_QSPI_IO1, PIN_QSPI_IO2,
                                                      PIN_QSPI_IO3);
    cfg.phy_if.sck_freq = NRF_QSPI_FREQ_DIV4; // 8 MHz
    const nrfx_err_t initErr = nrfx_qspi_init(&cfg, NULL, NULL);
    LOG_DEBUG("QSPIFS init err=0x%x", initErr);
    if (initErr != NRFX_SUCCESS)
        return false;

    // The bootloader or a previous run may have left the chip in deep power-down, where it ignores everything else.
    sendCommand(CMD_RELEASE_POWER_DOWN, NRF_QSPI_CINSTR_LEN_1B, NULL);
    delayMicroseconds(50);

    uint8_t id[3] = {};
    nrf_qspi_cinstr_conf_t cinstr = commandFor(CMD_READ_JEDEC_ID, NRF_QSPI_CINSTR_LEN_4B);
    const nrfx_err_t idErr = nrfx_qspi_cinstr_xfer(&cinstr, NULL, id);
    LOG_DEBUG("QSPIFS jedec err=0x%x raw=%02x %02x %02x", idErr, id[0], id[1], id[2]);
    if (idErr != NRFX_SUCCESS)
        return false;
    jedec = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
    if (jedec == 0 || jedec == 0xFFFFFF) {
        jedec = 0;
        return false;
    }

    if (Adafruit_LittleFS::begin())
        return true;
    return format() && Adafruit_LittleFS::begin();
}

#ifdef MESHTASTIC_QSPI_FS_SELFTEST
void qspiFlashFsSelfTest()
{
    uint32_t t0 = millis();
    const bool mounted = QspiFS.begin();
    LOG_INFO("QSPIFS mount=%d jedec=0x%06x in %u ms", mounted, QspiFS.jedecId(), millis() - t0);
    if (!mounted)
        return;

    // One fixed-size record per slot, rewritten in place: the access pattern a MeshCore node store would have.
    static constexpr size_t RECORD_LEN = 128;
    static constexpr size_t RECORDS = 300;
    uint8_t rec[RECORD_LEN];

    t0 = millis();
    LOG_INFO("QSPIFS open");
    Serial.flush();
    File f = QspiFS.open("/selftest.bin", FILE_O_WRITE);
    if (!f) {
        LOG_ERROR("QSPIFS open for write failed");
        return;
    }
    f.seek(0);
    for (size_t i = 0; i < RECORDS; i++) {
        memset(rec, (uint8_t)i, sizeof(rec));
        f.write(rec, sizeof(rec));
        if (i % 50 == 0) {
            LOG_INFO("QSPIFS wrote %u at %u ms", i, millis() - t0);
            Serial.flush();
        }
    }
    LOG_INFO("QSPIFS closing");
    Serial.flush();
    f.close();
    const uint32_t writeMs = millis() - t0;

    t0 = millis();
    f = QspiFS.open("/selftest.bin", FILE_O_WRITE);
    f.seek(150 * RECORD_LEN);
    memset(rec, 0xA5, sizeof(rec));
    f.write(rec, sizeof(rec));
    f.close();
    const uint32_t patchMs = millis() - t0;

    t0 = millis();
    size_t bad = 0;
    f = QspiFS.open("/selftest.bin", FILE_O_READ);
    const uint32_t size = f.size();
    for (size_t i = 0; i < RECORDS; i++) {
        if (f.read(rec, sizeof(rec)) != (int)sizeof(rec)) {
            bad++;
            continue;
        }
        const uint8_t expect = i == 150 ? 0xA5 : (uint8_t)i;
        for (size_t k = 0; k < sizeof(rec); k++)
            if (rec[k] != expect) {
                bad++;
                break;
            }
    }
    f.close();
    LOG_INFO("QSPIFS %u records of %u B: size=%u write=%u ms patch=%u ms read=%u ms bad=%u", RECORDS, RECORD_LEN, size,
             writeMs, patchMs, millis() - t0, bad);
}
#endif

#endif
