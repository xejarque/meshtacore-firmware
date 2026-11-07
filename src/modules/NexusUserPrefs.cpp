// Build-time starting values for the Nexus settings that are otherwise runtime-only (PPING bots, firewall, Zero-Hop,
// features, security, rate limits). Each USERPREFS_NEXUS_* key mirrors one /mtc command, see docs/mtc-commands.md.
//
// They apply only to a device with no vault (first boot, factory reset), through NexusModule::applyFactoryUserPrefs(). A
// saved vault is the operator's choice and is never overwritten. Values outside the range the command accepts are ignored.
#include "Channels.h"
#include "configuration.h"
#include "modules/NexusModule.h"

#include <string.h>

using namespace meshtastic;

#if defined(USERPREFS_NEXUS_PPING_TRIGGER) || defined(USERPREFS_NEXUS_PPING_HEADER)
// Copies a build-time string into a fixed buffer, always terminated.
static void copyUserPrefString(char *dst, size_t dstLen, const char *src)
{
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}
#endif

#if defined(USERPREFS_NEXUS_PPING_MT_HEADER_MODE) || defined(USERPREFS_NEXUS_PPING_MC_HEADER_MODE)
static bool validPingHeaderMode(long mode)
{
    return mode >= PING_HEADER_DEFAULT && mode <= PING_HEADER_NONE;
}
#endif

void NexusModule::applyFactoryUserPrefs()
{
    // ---- PPING bots (/mt PPING, /mc PPING) ----
#ifdef USERPREFS_NEXUS_PPING_ENABLED
    public_ping = USERPREFS_NEXUS_PPING_ENABLED;
#endif
#ifdef USERPREFS_NEXUS_PPING_CHANNELS
    // Bit n of the mask is channel n.
    for (int i = 0; i < MAX_NUM_CHANNELS; i++)
        ping_channels[i] = (((uint32_t)(USERPREFS_NEXUS_PPING_CHANNELS) >> i) & 1u) != 0;
#endif
#ifdef USERPREFS_NEXUS_PPING_TRIGGER
    if (((const char *)USERPREFS_NEXUS_PPING_TRIGGER)[0] != '\0')
        copyUserPrefString(ping_trigger, sizeof(ping_trigger), USERPREFS_NEXUS_PPING_TRIGGER);
#endif
#ifdef USERPREFS_NEXUS_PPING_RSSI
    ping_rssi = USERPREFS_NEXUS_PPING_RSSI;
#endif
#ifdef USERPREFS_NEXUS_PPING_SNR
    ping_snr = USERPREFS_NEXUS_PPING_SNR;
#endif
#ifdef USERPREFS_NEXUS_PPING_FREQERR
    ping_freqerr = USERPREFS_NEXUS_PPING_FREQERR;
#endif
#ifdef USERPREFS_NEXUS_PPING_FLAGS
    ping_bitmask = USERPREFS_NEXUS_PPING_FLAGS;
#endif
#ifdef USERPREFS_NEXUS_PPING_IGNORE_DIRECT
    ping_ignore_direct = USERPREFS_NEXUS_PPING_IGNORE_DIRECT;
#endif
#ifdef USERPREFS_NEXUS_PPING_IGNORE_REMOTE
    ping_ignore_remote = USERPREFS_NEXUS_PPING_IGNORE_REMOTE;
#endif
#ifdef USERPREFS_NEXUS_PPING_ALLOW_DM
    ping_allow_dm = USERPREFS_NEXUS_PPING_ALLOW_DM;
#endif
#ifdef USERPREFS_NEXUS_PPING_EMOJI_MODE
    if (USERPREFS_NEXUS_PPING_EMOJI_MODE >= 0 && USERPREFS_NEXUS_PPING_EMOJI_MODE <= 2)
        ping_emoji_mode = USERPREFS_NEXUS_PPING_EMOJI_MODE;
#endif
#ifdef USERPREFS_NEXUS_PPING_HEADER
    copyUserPrefString(pping_header, sizeof(pping_header), USERPREFS_NEXUS_PPING_HEADER);
#endif
#ifdef USERPREFS_NEXUS_PPING_MT_HEADER_MODE
    if (validPingHeaderMode(USERPREFS_NEXUS_PPING_MT_HEADER_MODE))
        mt_ping_header_mode = USERPREFS_NEXUS_PPING_MT_HEADER_MODE;
#endif
#ifdef USERPREFS_NEXUS_PPING_MC_HEADER_MODE
    if (validPingHeaderMode(USERPREFS_NEXUS_PPING_MC_HEADER_MODE))
        mc_ping_header_mode = USERPREFS_NEXUS_PPING_MC_HEADER_MODE;
#endif
    // The vault mirror older builds read: "the Meshtastic header is CUSTOM".
    custom_pping_active = (mt_ping_header_mode == PING_HEADER_CUSTOM);

    // ---- Firewall (/mtc DROP, OUTDROP, INVALID, RATELIMIT, BD) ----
#ifdef USERPREFS_NEXUS_DROP_MASK
    setNexusDropHex((uint64_t)(USERPREFS_NEXUS_DROP_MASK));
#endif
#ifdef USERPREFS_NEXUS_OUT_DROP_MASK
    setNexusOutDropHex((uint64_t)(USERPREFS_NEXUS_OUT_DROP_MASK));
#endif
#ifdef USERPREFS_NEXUS_INVALID_MASK
    setNexusInvalidHex((uint32_t)(USERPREFS_NEXUS_INVALID_MASK));
#endif
#ifdef USERPREFS_NEXUS_RL_TEXT_LIMIT
    rl_text_limit = USERPREFS_NEXUS_RL_TEXT_LIMIT;
    rl_tokens[0] = (float)rl_text_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_TRACE_LIMIT
    rl_trace_limit = USERPREFS_NEXUS_RL_TRACE_LIMIT;
    rl_tokens[1] = (float)rl_trace_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_OTHER_LIMIT
    rl_other_limit = USERPREFS_NEXUS_RL_OTHER_LIMIT;
    rl_tokens[2] = (float)rl_other_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_NODEINFO_LIMIT
    rl_nodeinfo_limit = USERPREFS_NEXUS_RL_NODEINFO_LIMIT;
    rl_tokens[3] = (float)rl_nodeinfo_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_POSITION_LIMIT
    rl_position_limit = USERPREFS_NEXUS_RL_POSITION_LIMIT;
    rl_tokens[4] = (float)rl_position_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_TELEMETRY_LIMIT
    rl_telemetry_limit = USERPREFS_NEXUS_RL_TELEMETRY_LIMIT;
    rl_tokens[5] = (float)rl_telemetry_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_RL_PRIVATE_LIMIT
    rl_private_limit = USERPREFS_NEXUS_RL_PRIVATE_LIMIT;
    rl_tokens[6] = (float)rl_private_limit; // like /mtc RL: the bucket starts full
#endif
#ifdef USERPREFS_NEXUS_BROADCAST_DELAY_MAX_S
    if (USERPREFS_NEXUS_BROADCAST_DELAY_MAX_S >= 1 && USERPREFS_NEXUS_BROADCAST_DELAY_MAX_S <= 300)
        broadcast_delay_max_s = USERPREFS_NEXUS_BROADCAST_DELAY_MAX_S;
#endif

    // ---- Zero-Hop, features, security (/mtc ZH, FEATURES, SECURITY) ----
#ifdef USERPREFS_NEXUS_ZERO_HOP_CONFIG
    zero_hop_config = (uint8_t)(USERPREFS_NEXUS_ZERO_HOP_CONFIG);
#endif
#ifdef USERPREFS_NEXUS_FEATURES_MASK
    setNexusFeaturesHex((uint64_t)(USERPREFS_NEXUS_FEATURES_MASK));
#endif
#ifdef USERPREFS_NEXUS_SECURITY_MASK
    nexus_security_bitmask = (uint32_t)(USERPREFS_NEXUS_SECURITY_MASK);
#endif

    // ---- Radio (/mtc PRE) ----
#ifdef USERPREFS_NEXUS_PREAMBLE
    // Valid LoRa range is 6..65535, 0 keeps the default (same rule loadVault applies).
    if (USERPREFS_NEXUS_PREAMBLE >= 6 && USERPREFS_NEXUS_PREAMBLE <= 65535)
        preamble_override = USERPREFS_NEXUS_PREAMBLE;
#endif
}
