#pragma once

#include "configuration.h"
#include <stddef.h>

#if HAS_NEXUS

/**
 * Keeps a copy of the settings a remote node cannot be recovered without (admin keys, channels, LoRa and role,
 * owner names and broadcast intervals) in /nexus_rescue.bin, outside /prefs, so a factory reset or a wiped /prefs
 * brings the node back on its mesh instead of on the defaults.
 *
 * The copy follows every successful save of config, module config, channels or device state. An empty admin
 * key slot never erases a stored key. Only RESCUE CLEAR removes the copy.
 */
namespace NexusRescue
{

/// Refreshes the copy from the live settings; a no-op when nothing changed or on a degraded boot.
void snapshot();

/// Called once at boot after the config load: re-applies and persists the copy when /prefs had no config.
/// Returns true when settings were restored.
bool restoreIfConfigWasMissing();

/// Deletes the copy.
bool clear();

/// One-line summary of the stored copy for /nexus RESCUE SHOW.
void describe(char *out, size_t outLen);

} // namespace NexusRescue

#endif
