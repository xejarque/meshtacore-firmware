// SPDX-License-Identifier: AGPL-3.0-only
//
// Byte-level helpers the companion's source files share: where a parsed frame's path lies, and the little-endian u32
// MeshCore writes timestamps and hashes in.
#pragma once

#include "MeshCoreRepeater.h"
#include <stddef.h>
#include <stdint.h>

#if MESHTASTIC_ENABLE_MESHCORE_SNIFFER

/// Where the path lives in a parsed frame: it sits between the header and the payload.
inline const uint8_t *pathOf(const uint8_t *raw, const MeshCoreRepeater::FrameInfo &info)
{
    return &raw[info.header_len - (size_t)info.path_hash_count * info.path_hash_size];
}

inline uint32_t readLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline void writeLe32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_SNIFFER
