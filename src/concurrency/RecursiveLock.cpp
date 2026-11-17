#include "RecursiveLock.h"
#include "configuration.h"
#include <cassert>
#include <stdlib.h>

namespace concurrency
{

#ifdef HAS_FREE_RTOS
RecursiveLock::RecursiveLock() : handle(xSemaphoreCreateRecursiveMutex())
{
    assert(handle);
    if (!handle)
        abort();
}

RecursiveLock::~RecursiveLock()
{
    vSemaphoreDelete(handle);
}

void RecursiveLock::lock()
{
    if (xSemaphoreTakeRecursive(handle, portMAX_DELAY) != pdTRUE)
        abort();
}

void RecursiveLock::unlock()
{
    xSemaphoreGiveRecursive(handle);
}
#elif defined(ARCH_PORTDUINO)
RecursiveLock::RecursiveLock()
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0 || pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutex, &attr) != 0)
        abort();
    pthread_mutexattr_destroy(&attr);
}

RecursiveLock::~RecursiveLock()
{
    pthread_mutex_destroy(&mutex);
}

void RecursiveLock::lock()
{
    pthread_mutex_lock(&mutex);
}

void RecursiveLock::unlock()
{
    pthread_mutex_unlock(&mutex);
}
#else
// Single-threaded targets (no FreeRTOS, no pthreads): nothing to exclude.
RecursiveLock::RecursiveLock() {}
RecursiveLock::~RecursiveLock() {}
void RecursiveLock::lock() {}
void RecursiveLock::unlock() {}
#endif

} // namespace concurrency
