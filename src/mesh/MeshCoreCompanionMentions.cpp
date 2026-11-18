// SPDX-License-Identifier: AGPL-3.0-only
//
// Mentions and replies on MeshCore channels: who sent a recent text, which contact a reply answers, and the
// "@[name]" form MeshCore apps expect on what we send.

#include "MeshCoreCompanion.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "MeshCoreProtocol.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "PingReport.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <AES.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <SHA512.h>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "MeshCoreCompanionWire.h"

void MeshCoreCompanion::noteRecentText(uint32_t packetId, const char *sender)
{
    RecentText &r = recent[recentNext];
    recentNext = (recentNext + 1) % RECENT_TEXTS;
    r.packetId = packetId;
    strncpy(r.sender, sender, sizeof(r.sender) - 1);
    r.sender[sizeof(r.sender) - 1] = 0;
}

const char *MeshCoreCompanion::recentSender(uint32_t packetId) const
{
    for (const RecentText &r : recent)
        if (r.packetId == packetId && r.sender[0])
            return r.sender;
    return nullptr;
}

uint32_t MeshCoreCompanion::replyTarget(const char *text, const char **rest) const
{
    if (rest)
        *rest = text;
    if (text[0] != '@' || text[1] != '[')
        return 0;
    const char *close = strchr(text + 2, ']');
    if (!close || close[1] != ' ' || close == text + 2 || close - text - 2 >= MeshCoreNode::NAME_LEN)
        return 0;

    char name[MeshCoreNode::NAME_LEN];
    memcpy(name, text + 2, close - text - 2);
    name[close - text - 2] = 0;
    // Newest first: the tag names a person, not a message, so the latest text from them is the one answered.
    for (size_t i = 1; i <= RECENT_TEXTS; i++) {
        const RecentText &r = recent[(recentNext + RECENT_TEXTS - i) % RECENT_TEXTS];
        if (r.sender[0] && strcmp(r.sender, name) == 0) {
            if (rest)
                *rest = close + 2;
            return r.packetId;
        }
    }
    return 0;
}

/// Length of name when text starts with it (ASCII case-insensitive) and the name ends there; 0 otherwise.
static size_t mentionMatchLen(const char *text, const char *name)
{
    const size_t n = strlen(name);
    if (n == 0 || strncasecmp(text, name, n) != 0)
        return 0;
    const uint8_t next = (uint8_t)text[n];
    const bool wordChar =
        (next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || next == '_';
    return wordChar ? 0 : n;
}

size_t MeshCoreCompanion::prepareGroupText(const char *text, uint32_t replyId, char *out, size_t cap)
{
    concurrency::RecursiveLockGuard guard(lock);
    if (!text || !out || cap == 0)
        return 0;

    size_t o = 0;
    auto put = [&](const char *s, size_t n) {
        const size_t room = cap - 1 - o;
        const size_t k = n < room ? n : room;
        memcpy(&out[o], s, k);
        o += k;
    };

    // MeshCore shows a reply as "@[name] " in front of the text.
    const char *replied = replyId ? recentSender(replyId) : nullptr;
    // A body that already opens with a tag (the ping answer does, with the name cleaned) is not tagged a second time.
    const bool alreadyTagged = strncmp(text, "@[", 2) == 0;
    if (replied && !alreadyTagged) {
        put("@[", 2);
        put(replied, strlen(replied));
        put("] ", 2);
    }

    // MeshCore marks a mention as "@[name]"; a bare "@name" is just text there. Names we know are matched whole (longest
    // first, so "Bob Builder" beats "Bob"); an unknown one is taken as a single word.
    for (const char *p = text; *p;) {
        const bool wordStart = p == text || p[-1] == ' ' || p[-1] == '\n';
        if (*p == '@' && p[1] != '[' && p[1] != ' ' && p[1] != 0 && wordStart) {
            char best[MeshCoreNode::NAME_LEN] = {};
            size_t bestLen = 0;
            auto consider = [&](const char *name, size_t nameLen) {
                char known[MeshCoreNode::NAME_LEN];
                if (nameLen == 0 || nameLen >= sizeof(known))
                    return;
                memcpy(known, name, nameLen);
                known[nameLen] = 0;
                const size_t m = mentionMatchLen(p + 1, known);
                if (m > bestLen) {
                    bestLen = m;
                    memcpy(best, known, nameLen + 1);
                }
            };
            nodes.forEach([&](MeshCoreNode &n) {
                if (n.type == MeshCoreNode::CHAT)
                    consider(n.name, strlen(n.name));
            });
            for (const RecentText &r : recent)
                consider(r.sender, strlen(r.sender));
            // Nodes of MeshCore people kept in the node DB (shown as "name (MC)") outlive the recent-senders list.
            for (size_t n = 0; nodeDB && n < nodeDB->getNumMeshNodes(); n++) {
                const meshtastic_NodeInfoLite *lite = nodeDB->getMeshNodeByIndex(n);
                if (!nodeInfoLiteIsMeshCorePeer(lite))
                    continue;
                const size_t len = strlen(lite->long_name);
                if (len > 5 && strcmp(&lite->long_name[len - 5], " (MC)") == 0)
                    consider(lite->long_name, len - 5);
            }
            if (bestLen > 0) {
                put("@[", 2);
                put(best, strlen(best));
                put("]", 1);
                p += 1 + bestLen;
                continue;
            }
            // Unknown: one word, without the punctuation that ends a sentence.
            size_t word = 0;
            while (p[1 + word] && p[1 + word] != ' ' && p[1 + word] != '\n')
                word++;
            while (word > 0 && strchr(".,;:!?)", p[word]))
                word--;
            if (word > 0) {
                put("@[", 2);
                put(p + 1, word);
                put("]", 1);
                p += 1 + word;
                continue;
            }
        }
        put(p, 1);
        p++;
    }
    LOG_DEBUG("MeshCore text prepared: reply=%u known=%d", (unsigned)replyId, replied != nullptr);
    out[o] = 0;
    return o;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
