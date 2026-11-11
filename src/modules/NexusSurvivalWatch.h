// SPDX-License-Identifier: AGPL-3.0-only
//
// The decision half of NexusModule::serviceSurvivalWatch(): when a node nobody can reach should reboot itself. It reads the
// clock but touches no hardware, so a native test can drive it with a made-up heap and RX counter.
#pragma once

#include "Throttle.h"
#include "UptimeClock.h"
#include <stdint.h>

namespace meshtastic
{

class SurvivalWatch
{
  public:
    enum class Verdict : uint8_t { Healthy, HeapExhausted, RadioSilent };

    static constexpr uint32_t CHECK_EVERY_MS = 60 * 1000;
    static constexpr uint32_t HEAP_FLOOR_BYTES = 4096;
    static constexpr uint8_t HEAP_LOW_CHECKS = 3; // consecutive checks below the floor
    static constexpr uint32_t RX_SILENCE_MS = 12UL * 60 * 60 * 1000;

    /**
     * One look at the node. Runs its checks at most once per CHECK_EVERY_MS and answers Healthy in between.
     * freeHeap: UINT32_MAX or 0 mean "unknown" and never count as low. rxTotal: every frame the radio decoded or
     * rejected, so a corrupt frame still proves the receiver is alive. radioPresent: false skips the silence check.
     * A node that is already below the heap floor on its first check never trips the heap check: that is how it boots,
     * not a leak, and rebooting it would only loop.
     */
    Verdict check(uint32_t freeHeap, bool radioPresent, uint32_t rxTotal)
    {
        if (armed && Throttle::isWithinTimespanMs(lastCheckMs, CHECK_EVERY_MS))
            return Verdict::Healthy;
        const uint32_t now = Time::getMillis();
        lastCheckMs = now;
        if (!armed) { // first pass: start both clocks from here
            armed = true;
            rxChangeMs = now;
            rxSeen = rxTotal;
            heapStartedHealthy = !isHeapLow(freeHeap);
            return Verdict::Healthy;
        }

        if (heapStartedHealthy && isHeapLow(freeHeap)) {
            if (++heapLowChecks >= HEAP_LOW_CHECKS)
                return Verdict::HeapExhausted;
        } else {
            heapLowChecks = 0;
        }

        if (radioPresent) {
            if (rxTotal != rxSeen) {
                rxSeen = rxTotal;
                rxChangeMs = now;
            } else if (!Throttle::isWithinTimespanMs(rxChangeMs, RX_SILENCE_MS)) {
                return Verdict::RadioSilent;
            }
        }
        return Verdict::Healthy;
    }

  private:
    static bool isHeapLow(uint32_t freeHeap) { return freeHeap != UINT32_MAX && freeHeap != 0 && freeHeap < HEAP_FLOOR_BYTES; }

    bool armed = false;
    bool heapStartedHealthy = true;
    uint8_t heapLowChecks = 0;
    uint32_t lastCheckMs = 0;
    uint32_t rxChangeMs = 0;
    uint32_t rxSeen = 0;
};

} // namespace meshtastic
