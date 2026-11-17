#pragma once

#include "../freertosinc.h"

#if !defined(HAS_FREE_RTOS) && defined(ARCH_PORTDUINO)
#include <pthread.h>
#endif

namespace concurrency
{

/**
 * A mutex the task holding it may take again. For a data structure whose public entry points call one another, so each can
 * guard itself without the caller knowing which of them nest. Must not be used from an ISR.
 */
class RecursiveLock
{
  public:
    RecursiveLock();
    ~RecursiveLock();

    RecursiveLock(const RecursiveLock &) = delete;
    RecursiveLock &operator=(const RecursiveLock &) = delete;

    void lock();
    void unlock();

  private:
#ifdef HAS_FREE_RTOS
    SemaphoreHandle_t handle;
#elif defined(ARCH_PORTDUINO)
    pthread_mutex_t mutex;
#endif
};

/** RAII holder of a RecursiveLock for one scope. */
class RecursiveLockGuard
{
  public:
    explicit RecursiveLockGuard(RecursiveLock &lock) : held(lock) { held.lock(); }
    ~RecursiveLockGuard() { held.unlock(); }

    RecursiveLockGuard(const RecursiveLockGuard &) = delete;
    RecursiveLockGuard &operator=(const RecursiveLockGuard &) = delete;

  private:
    RecursiveLock &held;
};

} // namespace concurrency
