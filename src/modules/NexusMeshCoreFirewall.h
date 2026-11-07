// SPDX-License-Identifier: AGPL-3.0-only
//
// The MeshCore half of the Nexus firewall. It is driven by the same commands (DROP, UNDROP, DROPS, FW, FWS) with MeshCore tags
// (MCAD, MCRQ, MCTX, MCGR, MCAK, MCPA, MCTR, MCSRC) so one vocabulary covers both networks. The rules and limits live in the
// MeshCore config (/prefs/meshcore_config.bin), so /mtc SAVE keeps them; ignored nodes come from the NIGN list.
#pragma once

#include "configuration.h"
#include <stddef.h>

namespace meshtastic
{

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER
/// DROP / UNDROP of a MeshCore tag. False when tag is not one of them.
bool meshCoreFirewallSetRule(const char *tag, bool on);

/// MCGAP [minutes] and MCRATE [frames]: show or set the advert gap and the hop rate (0 = off). False when cmd is neither.
bool meshCoreFirewallCommand(const char *cmd, char *reply, size_t replyLen);

/// "MCAD MCTX " - the tags of the rules that are on, for DROPS and FW.
size_t meshCoreFirewallActiveTags(char *out, size_t outLen);

/// One line each: the limits, and what the firewall has dropped so far.
size_t meshCoreFirewallLimits(char *out, size_t outLen);
size_t meshCoreFirewallCounters(char *out, size_t outLen);
#else
inline bool meshCoreFirewallSetRule(const char *, bool)
{
    return false;
}
inline bool meshCoreFirewallCommand(const char *, char *, size_t)
{
    return false;
}
inline size_t meshCoreFirewallActiveTags(char *, size_t)
{
    return 0;
}
inline size_t meshCoreFirewallLimits(char *, size_t)
{
    return 0;
}
inline size_t meshCoreFirewallCounters(char *, size_t)
{
    return 0;
}
#endif

} // namespace meshtastic
