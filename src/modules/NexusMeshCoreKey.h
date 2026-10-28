// SPDX-License-Identifier: AGPL-3.0-only
//
// MeshCore channel keys as Nexus reads and writes them. MeshCore shows a private channel's key as 32 hex digits; the
// Meshtastic app takes a channel key in base64 only, so the same key has to be converted to share a channel.

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace meshtastic
{

/// Parse a 32- or 64-digit hex key, "0x" prefix allowed, into buffer. False, with buffer untouched, if input is not one.
bool parseHexPsk(unsigned char *buffer, size_t maxLen, size_t *outLen, const char *input);

/// Write a MeshCore private channel key, given as MeshCore shows it (32 hex digits; "0x", blanks, '-' and ':' are
/// skipped), in base64 as the Meshtastic app takes it. False if hex is not a 16-byte key or out is too small (25 bytes).
bool meshCoreKeyHexToBase64(const char *hex, char *out, size_t outLen);

} // namespace meshtastic
