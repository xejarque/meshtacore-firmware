// SPDX-License-Identifier: AGPL-3.0-only
//
// The check half of NexusModule::configureVoltRule(): a hibernation rule set by mistake leaves a node asleep with the
// radio off, where no remote command can reach it to undo the mistake. It touches no hardware, so a native test can
// drive it with made-up readings.
#pragma once

#include <stdint.h>

namespace meshtastic
{

struct VoltRuleProposal {
    bool hibernate = false;  // HIB action
    bool wake = false;       // WAKE action
    bool fallingEdge = false; // LOW direction (fires at or below mv)
    uint16_t mv = 0;
    uint16_t currentMv = 0;        // present reading on the rule's channel; 0 when unknown
    uint16_t highestOtherHibMv = 0; // other HIB rules; 0 when none
    uint16_t lowestOtherWakeMv = 0; // other WAKE rules; 0 when none
};

/// Why this rule would strand the node, or nullptr if it is safe to set.
inline const char *voltRuleStrandsNode(const VoltRuleProposal &p)
{
    if (p.hibernate && !p.fallingEdge)
        return "HIB needs LOW: on HIGH it would hibernate on a healthy battery";
    if (p.wake && p.fallingEdge)
        return "WAKE needs HIGH: on LOW it would only wake on a flat battery";
    if (p.hibernate && p.currentMv != 0 && p.currentMv <= p.mv)
        return "the reading is already at or below it: the node would hibernate now";
    if (p.hibernate && p.lowestOtherWakeMv != 0 && p.lowestOtherWakeMv <= p.mv)
        return "a WAKE rule sits at or below it: the node would wake and hibernate in a loop";
    if (p.wake && p.highestOtherHibMv != 0 && p.mv <= p.highestOtherHibMv)
        return "it sits at or below a HIB rule: the node would wake and hibernate in a loop";
    return nullptr;
}

} // namespace meshtastic
