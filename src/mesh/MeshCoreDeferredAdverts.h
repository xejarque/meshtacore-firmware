// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "MeshCoreNodeStore.h"
#include "MeshCoreRepeater.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER

/**
 * ADVERT frames whose signature nobody has checked yet. Ed25519 costs tens of ms on an nRF52, and on the receive path the
 * radio is deaf meanwhile, so the companion files such a frame here and checks one per service tick, as the relay does
 * for its neighbours. Copies of the same advert share one slot, keeping the one that crossed the fewest repeaters.
 */
class MeshCoreDeferredAdverts
{
  public:
#ifdef MESHCORE_HEAP_TIGHT_BOARD
    static constexpr size_t CAPACITY = 2; // 272 B a slot, and this board has no heap to spare
#else
    static constexpr size_t CAPACITY = 4;
#endif
    /** An advert that waited longer than this is stale: its hops and signal no longer describe the sender. */
    static constexpr uint32_t TTL_MS = 120000;

    struct Entry {
        uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
        uint8_t len = 0; // 0 marks a free slot
        uint8_t hops = 0;
        float snr = 0;
        int32_t rssi = 0;
        uint32_t atMs = 0;
    };

    /** File a frame. When every slot is taken, the oldest advert gives way. */
    void hold(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr, int32_t rssi)
    {
        if (len == 0 || len > MeshCoreRepeater::MAX_FRAME_LEN)
            return;
        Entry *slot = sameAdvert(&raw[info.header_len], info.payload_len);
        if (slot && slot->hops <= info.path_hash_count)
            return; // a copy no closer than the one held adds nothing
        if (!slot)
            slot = freeOrOldest();
        memcpy(slot->frame, raw, len);
        slot->len = (uint8_t)len;
        slot->hops = info.path_hash_count;
        slot->snr = snr;
        slot->rssi = rssi;
        slot->atMs = Time::getMillis();
    }

    /** Hand out the oldest advert still fresh, freeing its slot; stale ones are dropped on the way. False when none. */
    bool takeOldest(Entry &out)
    {
        Entry *oldest = nullptr;
        for (Entry &e : entries) {
            if (e.len == 0)
                continue;
            if (!Throttle::isWithinTimespanMs(e.atMs, TTL_MS)) {
                e.len = 0;
                continue;
            }
            if (!oldest || (int32_t)(e.atMs - oldest->atMs) < 0)
                oldest = &e;
        }
        if (!oldest)
            return false;
        out = *oldest;
        oldest->len = 0;
        return true;
    }

    bool empty() const
    {
        for (const Entry &e : entries) {
            if (e.len != 0)
                return false;
        }
        return true;
    }

    void clear()
    {
        for (Entry &e : entries)
            e.len = 0;
    }

  private:
    Entry entries[CAPACITY];

    Entry *sameAdvert(const uint8_t *payload, size_t payloadLen)
    {
        for (Entry &e : entries) {
            MeshCoreRepeater::FrameInfo held;
            if (e.len != 0 && MeshCoreRepeater::parse(e.frame, e.len, &held) && held.payload_len == payloadLen &&
                memcmp(&e.frame[held.header_len], payload, payloadLen) == 0)
                return &e;
        }
        return nullptr;
    }

    Entry *freeOrOldest()
    {
        Entry *oldest = &entries[0];
        for (Entry &e : entries) {
            if (e.len == 0)
                return &e;
            if ((int32_t)(e.atMs - oldest->atMs) < 0)
                oldest = &e;
        }
        return oldest;
    }
};

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
