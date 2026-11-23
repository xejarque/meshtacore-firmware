// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE for the full license text.
#pragma once

#include <stdint.h>

/// Node numbers the MeshCore companion gives its peers: 'M' (0x4D) in the top byte, 24 bits hashed from a name or a key.
/// The range alone does not make a node MeshCore: one Meshtastic node in 256 draws a number in it too. Tell them apart
/// with nodeInfoLiteIsMeshCorePeer() (NodeDB.h), which also asks for the MeshCore mark or a missing hardware model.
static constexpr uint32_t MESHCORE_NODENUM_PREFIX = 0x4D000000u;
static constexpr uint32_t MESHCORE_NODENUM_HASH_MASK = 0x00FFFFFFu;

/// A number in the companion's range, built from a 24-bit hash.
constexpr uint32_t meshCoreNodeNum(uint32_t hash)
{
    return MESHCORE_NODENUM_PREFIX | (hash & MESHCORE_NODENUM_HASH_MASK);
}

/// True when num lies in the companion's range. Necessary, not sufficient, for a MeshCore peer.
constexpr bool inMeshCoreNodeNumRange(uint32_t num)
{
    return (num & ~MESHCORE_NODENUM_HASH_MASK) == MESHCORE_NODENUM_PREFIX;
}
