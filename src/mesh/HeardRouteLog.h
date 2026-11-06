#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * The routes by which MeshCore texts were heard, kept on external flash so the ".path" command can answer about a text
 * long after the few routes that fit in RAM were overwritten.
 *
 * The log is two files used in turn. Records are only ever appended, which a LittleFS file does cheaply; rewriting one in
 * the middle would copy the rest of the file. When the current file is full the other one is emptied and becomes current,
 * so the oldest half of the history is the part dropped.
 *
 * Nothing here knows the flash: RouteStorage is the whole of the file system the log needs, so the format and the rotation
 * run in the native tests.
 */
namespace HeardRouteLog
{
/// One route by which a copy of a text was heard, as kept on flash.
struct Route {
    static constexpr size_t PATH_BYTES_MAX = 64; // MeshCore MAX_PATH_SIZE

    uint32_t packetId = 0;  // the id the client sees; a reply points at it
    uint32_t frameHash = 0; // the same text over another route carries the same hash
    uint32_t fromNum = 0;
    uint32_t epochSecs = 0; // wall clock when heard, 0 when the node had no time yet
    float snr = 0;
    int32_t rssi = 0;
    uint8_t channel = 0;
    uint8_t routeType = 0;
    uint8_t hashSize = 0;
    uint8_t hopCount = 0;
    bool own = false; // our own transmission, not a route
    uint8_t path[PATH_BYTES_MAX] = {};
};

static constexpr size_t FIXED_LEN = 25; // the record body without the path bytes: 4 x 4 + 1 + 2 + 5 + 1
static constexpr size_t RECORD_MAX = 1 + FIXED_LEN + Route::PATH_BYTES_MAX;
static constexpr float SNR_STEP = 0.25f; // SNR is kept in quarter dB, as the radio reports it

inline uint8_t *putU32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        *p++ = (uint8_t)(v >> (8 * i));
    return p;
}

inline const uint8_t *getU32(const uint8_t *p, uint32_t *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++)
        *v |= (uint32_t)*p++ << (8 * i);
    return p;
}

inline size_t pathBytes(const Route &r)
{
    const size_t bytes = (size_t)r.hopCount * r.hashSize;
    return bytes < Route::PATH_BYTES_MAX ? bytes : Route::PATH_BYTES_MAX;
}

/// Write r as [length][body] into out (RECORD_MAX bytes); returns the bytes written.
inline size_t encode(const Route &r, uint8_t *out)
{
    const size_t path = pathBytes(r);
    float steps = r.snr / SNR_STEP;
    steps = steps > 127 ? 127 : (steps < -128 ? -128 : steps);
    int rssi = r.rssi > 32767 ? 32767 : (r.rssi < -32768 ? -32768 : (int)r.rssi);

    uint8_t *p = out;
    *p++ = (uint8_t)(FIXED_LEN + path);
    p = putU32(p, r.packetId);
    p = putU32(p, r.frameHash);
    p = putU32(p, r.fromNum);
    p = putU32(p, r.epochSecs);
    *p++ = (uint8_t)(int8_t)(steps < 0 ? steps - 0.5f : steps + 0.5f);
    *p++ = (uint8_t)rssi;
    *p++ = (uint8_t)((uint16_t)rssi >> 8);
    *p++ = r.channel;
    *p++ = r.own ? 1 : 0;
    *p++ = r.routeType;
    *p++ = r.hashSize;
    *p++ = r.hopCount;
    *p++ = (uint8_t)path;
    memcpy(p, r.path, path);
    return 1 + FIXED_LEN + path;
}

/// Read a body of bodyLen bytes. False when it is shorter than the fixed part or claims more path than it holds.
inline bool decode(const uint8_t *body, size_t bodyLen, Route *r)
{
    if (bodyLen < FIXED_LEN)
        return false;
    const uint8_t *p = body;
    *r = Route();
    p = getU32(p, &r->packetId);
    p = getU32(p, &r->frameHash);
    p = getU32(p, &r->fromNum);
    p = getU32(p, &r->epochSecs);
    r->snr = (int8_t)*p++ * SNR_STEP;
    r->rssi = (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
    p += 2;
    r->channel = *p++;
    r->own = *p++ != 0;
    r->routeType = *p++;
    r->hashSize = *p++;
    r->hopCount = *p++;
    const size_t path = *p++;
    if (path > Route::PATH_BYTES_MAX || FIXED_LEN + path > bodyLen)
        return false;
    memcpy(r->path, p, path);
    return true;
}

/// The file system the log lives on: two slots, each an append-only file.
class RouteStorage
{
  public:
    virtual ~RouteStorage() = default;
    /// Bytes in the slot, 0 when it does not exist.
    virtual uint32_t size(uint8_t slot) = 0;
    virtual bool append(uint8_t slot, const uint8_t *data, size_t len) = 0;
    /// Read up to len bytes from offset; returns how many were read.
    virtual size_t read(uint8_t slot, uint32_t offset, uint8_t *out, size_t len) = 0;
    /// Empty the slot.
    virtual bool reset(uint8_t slot) = 0;
};

class Log
{
  public:
    static constexpr uint8_t SLOTS = 2;
    static constexpr size_t HEADER_LEN = 8; // 'R' 'T' 'L' version, then the sequence number
    static constexpr uint8_t VERSION = 1;

    /// maxSlotBytes: a slot is rotated once a record would take it past this.
    Log(RouteStorage &storage, uint32_t maxSlotBytes) : fs(storage), maxBytes(maxSlotBytes) {}

    /// Find the current slot, or start a fresh log. False when the storage refuses.
    bool begin()
    {
        uint32_t seq[SLOTS] = {};
        bool valid[SLOTS] = {};
        for (uint8_t s = 0; s < SLOTS; s++)
            valid[s] = readHeader(s, &seq[s]);

        if (!valid[0] && !valid[1])
            return startSlot(0, 1);
        current = (valid[0] && (!valid[1] || seq[0] > seq[1])) ? 0 : 1;
        currentSeq = seq[current];
        return true;
    }

    /// Add one route; the older half of the history goes when the slot is full.
    bool append(const Route &r)
    {
        uint8_t rec[RECORD_MAX];
        const size_t len = encode(r, rec);
        if (fs.size(current) + len > maxBytes && !startSlot(1 - current, currentSeq + 1))
            return false;
        return fs.append(current, rec, len);
    }

    /// Call visit for every record, the older slot first; it returns false to stop. Returns the records visited.
    template <typename Visitor> size_t forEach(Visitor visit)
    {
        size_t visited = 0;
        for (uint8_t i = 0; i < SLOTS; i++) {
            const uint8_t slot = i == 0 ? 1 - current : current;
            uint32_t seq;
            if (!readHeader(slot, &seq))
                continue;
            uint32_t offset = HEADER_LEN;
            const uint32_t end = fs.size(slot);
            uint8_t window[WINDOW];
            while (offset < end) {
                const size_t got = fs.read(slot, offset, window, WINDOW);
                size_t used = 0;
                while (used < got) {
                    const size_t bodyLen = window[used];
                    if (used + 1 + bodyLen > got)
                        break; // continues in the next window, or the write was cut short
                    Route r;
                    if (decode(&window[used + 1], bodyLen, &r)) {
                        visited++;
                        if (!visit(r))
                            return visited;
                    }
                    used += 1 + bodyLen;
                }
                if (!used)
                    break; // a torn record at the end of the slot
                offset += used;
            }
        }
        return visited;
    }

  private:
    static constexpr size_t WINDOW = RECORD_MAX + 64;

    bool readHeader(uint8_t slot, uint32_t *seq)
    {
        uint8_t h[HEADER_LEN];
        if (fs.size(slot) < HEADER_LEN || fs.read(slot, 0, h, HEADER_LEN) != HEADER_LEN)
            return false;
        if (h[0] != 'R' || h[1] != 'T' || h[2] != 'L' || h[3] != VERSION)
            return false;
        getU32(&h[4], seq);
        return true;
    }

    bool startSlot(uint8_t slot, uint32_t seq)
    {
        uint8_t h[HEADER_LEN] = {'R', 'T', 'L', VERSION};
        putU32(&h[4], seq);
        if (!fs.reset(slot) || !fs.append(slot, h, HEADER_LEN))
            return false;
        current = slot;
        currentSeq = seq;
        return true;
    }

    RouteStorage &fs;
    uint32_t maxBytes;
    uint8_t current = 0;
    uint32_t currentSeq = 0;
};
} // namespace HeardRouteLog
