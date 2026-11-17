#include "BootLoopGuard.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "UptimeClock.h"
#include <Arduino.h>

BootLoopGuard *bootLoopGuard = nullptr;

static constexpr const char *STREAK_FILE = "/prefs/bootStreak";

BootLoopGuard::BootLoopGuard() : concurrency::OSThread("BootLoopGuard", STABLE_UPTIME_MS) {}

uint32_t BootLoopGuard::nextStreak(uint32_t previousStreak)
{
    return previousStreak < STREAK_CAP ? previousStreak + 1 : STREAK_CAP;
}

bool BootLoopGuard::isLoopStreak(uint32_t currentStreak)
{
    return currentStreak >= 2;
}

uint32_t BootLoopGuard::computeHoldoffMs(bool suspected, uint32_t jitterMs)
{
    if (!suspected)
        return 0;
    return HOLDOFF_MS + (jitterMs % (HOLDOFF_JITTER_MS + 1));
}

void BootLoopGuard::begin()
{
    streak = nextStreak(readStoredStreak());
    loopSuspected = isLoopStreak(streak);
    holdoffMs = computeHoldoffMs(loopSuspected, static_cast<uint32_t>(random(HOLDOFF_JITTER_MS + 1)));
    if (!writeStoredStreak(streak))
        LOG_WARN("BootLoopGuard: can't persist boot streak %u", (unsigned)streak);
    if (loopSuspected)
        LOG_WARN("BootLoopGuard: %u boots without %us uptime, holding broadcasts %us", (unsigned)streak,
                 (unsigned)(STABLE_UPTIME_MS / 1000), (unsigned)(holdoffMs / 1000));
}

bool BootLoopGuard::isHoldoffActive() const
{
    return holdoffMs > 0 && Time::getUptimeSecs() < holdoffMs / 1000;
}

int32_t BootLoopGuard::runOnce()
{
    // OSThread's first run comes after the constructor's period, i.e. once the node has stayed up long enough.
    if (streak > 0 && writeStoredStreak(0))
        LOG_INFO("BootLoopGuard: stable for %us, boot streak cleared", (unsigned)(STABLE_UPTIME_MS / 1000));
    return disable();
}

uint32_t BootLoopGuard::readStoredStreak()
{
    uint32_t value = 0;
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(STREAK_FILE, FILE_O_READ);
    if (f) {
        if (f.read(reinterpret_cast<uint8_t *>(&value), sizeof(value)) != sizeof(value))
            value = 0;
        f.close();
    }
#endif
    return value;
}

bool BootLoopGuard::writeStoredStreak(uint32_t value)
{
#ifdef FSCom
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir("/prefs");
    }
    // A bare FILE_O_WRITE appends on nRF52 LittleFS, so go through SafeFile to replace the value.
    SafeFile f(STREAK_FILE, true);
    f.write(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
    return f.close();
#else
    (void)value;
    return false;
#endif
}
