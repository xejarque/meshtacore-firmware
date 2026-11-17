#pragma once

#include "concurrency/OSThread.h"
#include <stdint.h>

/**
 * Detects a node that keeps rebooting and holds back its boot-time broadcasts.
 *
 * Every boot bumps a streak counter in flash; staying up STABLE_UPTIME_MS clears it. A streak of two or
 * more means the previous boot never got that far, whatever the cause (watchdog, lockup or a brownout,
 * which nRF52 cannot tell from a power-on). Periodic modules then wait an extra holdoff before their first
 * broadcast, so a node rebooting every few minutes stops re-announcing NodeInfo and telemetry each time.
 */
class BootLoopGuard : private concurrency::OSThread
{
  public:
    static constexpr uint32_t STABLE_UPTIME_MS = 30 * 60 * 1000;
    static constexpr uint32_t HOLDOFF_MS = 30 * 60 * 1000;
    static constexpr uint32_t HOLDOFF_JITTER_MS = 5 * 60 * 1000;
    static constexpr uint32_t STREAK_CAP = 1000;

    BootLoopGuard();

    /// Reads and bumps the streak. Call once, after fsInit() and before the modules are created.
    void begin();

    bool isLoopSuspected() const { return loopSuspected; }
    uint32_t getStreak() const { return streak; }

    /// Extra delay for a module's first broadcast: zero on a normal boot.
    uint32_t getBroadcastHoldoffMs() const { return holdoffMs; }

    /// True while this boot is still inside its broadcast holdoff.
    bool isHoldoffActive() const;

    static uint32_t nextStreak(uint32_t previousStreak);
    static bool isLoopStreak(uint32_t currentStreak);
    static uint32_t computeHoldoffMs(bool suspected, uint32_t jitterMs);

  protected:
    int32_t runOnce() override;

  private:
    uint32_t streak = 0;
    bool loopSuspected = false;
    uint32_t holdoffMs = 0;

    static uint32_t readStoredStreak();
    static bool writeStoredStreak(uint32_t value);
};

extern BootLoopGuard *bootLoopGuard;
