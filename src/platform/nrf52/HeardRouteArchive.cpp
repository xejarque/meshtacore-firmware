#include "configuration.h"

#include "mesh/HeardRouteArchive.h"

#ifdef HAS_HEARD_ROUTE_ARCHIVE

#include "DebugConfiguration.h"
#include "concurrency/OSThread.h"
#include "mesh/Throttle.h"

using namespace Adafruit_LittleFS_Namespace;
using namespace HeardRouteLog;

// A slot is rotated once it holds this much; the history kept is between one and two slots, so about 700 to 1400 routes.
static constexpr uint32_t SLOT_BYTES = 20 * 1024;
// Routes are held in RAM until a flash page worth has gathered. The file stays open while routes keep arriving, and is
// closed (committed) once none has for this long.
static constexpr size_t BATCH_BYTES = 256;
static constexpr uint32_t SETTLE_AFTER_MS = 30 * 1000;
static constexpr uint32_t SETTLE_CHECK_MS = 10 * 1000;

/// The two log files on the QSPI flash.
///
/// Opening, writing and closing a LittleFS file costs a metadata commit, a flash erase among it, so doing that per route
/// would stall the node for hundreds of milliseconds each time. Instead routes are gathered in a page-sized buffer, the
/// write handle stays open while routes keep coming, and is closed (which commits) once they have stopped for a while or
/// something needs to read. A read handle is likewise kept for the length of one scan, as every re-open costs a seek.
class LfsRouteStorage : public RouteStorage
{
  public:
    uint32_t size(uint8_t slot) override { return fileSize(slot) + (held && heldSlot == slot ? held : 0); }

    bool append(uint8_t slot, const uint8_t *data, size_t len) override
    {
        lastAppendMs = millis();
        if (held && heldSlot != slot && !drain())
            return false;
        if (held + len > sizeof(batch) && !drain())
            return false;
        if (len > sizeof(batch))
            return write(slot, data, len);
        if (!held)
            heldSlot = slot;
        memcpy(&batch[held], data, len);
        held += len;
        return true;
    }

    size_t read(uint8_t slot, uint32_t offset, uint8_t *out, size_t len) override
    {
        if (!settle() || !QspiFS.acquire())
            return 0;
        if (!readerOpen || readerSlot != slot) {
            closeReader();
            reader = QspiFS.open(path(slot), FILE_O_READ);
            readerOpen = reader;
            readerSlot = slot;
            readerPos = 0;
        }
        if (!readerOpen)
            return 0;
        if (offset != readerPos && !reader.seek(offset))
            return 0;
        const int n = reader.read(out, len);
        readerPos = n > 0 ? offset + n : offset;
        return n > 0 ? (size_t)n : 0;
    }

    bool reset(uint8_t slot) override
    {
        if (held && heldSlot == slot)
            held = 0; // about to be thrown away with the file
        closeReader();
        if (writerOpen && writerSlot == slot)
            closeWriter();
        if (!QspiFS.acquire())
            return false;
        if (QspiFS.exists(path(slot)) && !QspiFS.remove(path(slot)))
            return false;
        sizes[slot] = 0;
        known[slot] = true;
        return true;
    }

    void release() override { closeReader(); }

    /// Write out what is held and close the file, which commits it. Dropped on failure: a flash that refuses must not
    /// wedge every later route.
    bool settle()
    {
        const bool ok = drain();
        closeWriter();
        return ok;
    }

    /// True once routes have stopped arriving for long enough to commit what is pending.
    bool settleDue() const { return (held || writerOpen) && Throttle::hasElapsed(lastAppendMs, SETTLE_AFTER_MS); }

  private:
    static const char *path(uint8_t slot) { return slot == 0 ? "/routes.a" : "/routes.b"; }

    uint32_t fileSize(uint8_t slot)
    {
        if (!known[slot]) {
            sizes[slot] = 0;
            if (QspiFS.acquire()) {
                File f = QspiFS.open(path(slot), FILE_O_READ);
                if (f) {
                    sizes[slot] = f.size();
                    f.close();
                }
                known[slot] = true;
            }
        }
        return sizes[slot];
    }

    bool drain()
    {
        if (!held)
            return true;
        const size_t n = held;
        held = 0;
        return write(heldSlot, batch, n);
    }

    bool write(uint8_t slot, const uint8_t *data, size_t len)
    {
        if (!QspiFS.acquire())
            return false;
        if (!writerOpen || writerSlot != slot) {
            closeWriter();
            closeReader();  // it would see the file as it was
            fileSize(slot); // learn the size before the file grows
            writer = QspiFS.open(path(slot), FILE_O_WRITE);
            writerOpen = writer;
            writerSlot = slot;
        }
        if (!writerOpen || writer.write(data, len) != len)
            return false;
        sizes[slot] += len;
        return true;
    }

    void closeWriter()
    {
        if (writerOpen)
            writer.close();
        writerOpen = false;
    }

    void closeReader()
    {
        if (readerOpen)
            reader.close();
        readerOpen = false;
    }

    uint8_t batch[BATCH_BYTES];
    size_t held = 0;
    uint8_t heldSlot = 0;
    uint32_t lastAppendMs = 0;
    uint32_t sizes[2] = {};
    bool known[2] = {};
    File writer;
    File reader;
    bool writerOpen = false;
    bool readerOpen = false;
    uint8_t writerSlot = 0;
    uint8_t readerSlot = 0;
    uint32_t readerPos = 0;
};

static LfsRouteStorage storage;
static Log routeLog(storage, SLOT_BYTES);
static bool opened = false;

/// Commits the routes pending once they have stopped arriving.
class ArchiveFlushThread : public concurrency::OSThread
{
  public:
    ArchiveFlushThread() : OSThread("RouteFlush") {}

  protected:
    int32_t runOnce() override
    {
        if (storage.settleDue())
            storage.settle();
        return SETTLE_CHECK_MS;
    }
};

static bool open()
{
    if (opened)
        return true;
    if (!QspiFS.acquire() || !routeLog.begin())
        return false;
    opened = true;
    new ArchiveFlushThread(); // never allocated statically: threads must be created after setup() starts
    return true;
}

namespace HeardRouteArchive
{
void add(const Route &r)
{
    if (open() && !routeLog.append(r))
        LOG_WARN("Route log write failed");
}

size_t forEach(Visitor visit, void *ctx)
{
    if (!open() || !storage.settle())
        return 0;
    return routeLog.forEach([&](const Route &r) { return visit(r, ctx); });
}

#ifdef MESHTASTIC_QSPI_FS_SELFTEST
namespace
{
struct Count {
    uint32_t hash = 0;
    size_t total = 0;
    size_t matched = 0;
};

bool countVisit(const Route &r, void *ctx)
{
    auto *c = static_cast<Count *>(ctx);
    c->total++;
    if (r.frameHash == c->hash)
        c->matched++;
    return true;
}
} // namespace

void heardRouteArchiveSelfTest()
{
    static uint32_t run = 0;
    run++;
    Count before;
    forEach(countVisit, &before);

    // The first run also writes enough routes to fill a slot, so the log rotates once.
    const uint32_t burst = run == 1 ? 800 : 0;
    const uint32_t t0 = millis();
    for (uint32_t i = 0; i < burst; i++) {
        Route r;
        r.packetId = 0xB0000000u + i;
        r.frameHash = 0xB0000000u;
        r.hashSize = 1;
        r.hopCount = 3;
        add(r);
    }
    const uint32_t burstMs = millis() - t0;

    Count c;
    c.hash = 0x5E1F0000u + run;
    for (uint32_t i = 0; i < 5; i++) {
        Route r;
        r.packetId = c.hash + i;
        r.frameHash = c.hash;
        r.snr = -3.5f;
        r.rssi = -90;
        r.hashSize = 1;
        r.hopCount = 2;
        r.path[0] = 0xA1;
        r.path[1] = 0xB2;
        add(r);
    }
    const uint32_t t1 = millis();
    forEach(countVisit, &c);
    LOG_INFO("ROUTELOG run=%u before=%u burst=%u in %u ms; now total=%u matched=%u scan=%u ms", (unsigned)run,
             (unsigned)before.total, (unsigned)burst, (unsigned)burstMs, (unsigned)c.total, (unsigned)c.matched,
             (unsigned)(millis() - t1));
}
#endif
} // namespace HeardRouteArchive

#endif
