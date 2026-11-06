#pragma once

#include "HeardRouteLog.h"
#include "configuration.h"

#ifdef ARCH_NRF52
#include "platform/nrf52/QspiFlashFS.h"
#endif

/**
 * Where the routes the companion no longer keeps in RAM go: a log on the external flash, on the boards that have one.
 * Boards without it define nothing here and keep all their routes in RAM, as before.
 */
#ifdef HAS_QSPI_FLASH_FS
#define HAS_HEARD_ROUTE_ARCHIVE 1

namespace HeardRouteArchive
{
/// Keep r. The write is held back and batched with the next ones, so the flash is not woken for every route heard.
void add(const HeardRouteLog::Route &r);

/// Return false to stop the search.
using Visitor = bool (*)(const HeardRouteLog::Route &r, void *ctx);

/// Call visit for every route kept, oldest first. Returns how many were visited; 0 when the flash is absent.
size_t forEach(Visitor visit, void *ctx);

#ifdef MESHTASTIC_QSPI_FS_SELFTEST
/// Write, find and rotate routes, logging the counts. Only built for the spike.
void heardRouteArchiveSelfTest();
#endif
} // namespace HeardRouteArchive
#endif
