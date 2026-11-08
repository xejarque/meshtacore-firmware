#include "MeshCoreAdmin.h"

#if MESHTASTIC_ENABLE_MESHCORE_ADMIN

#include "MeshCoreCompanion.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

MeshCoreAdmin meshCoreAdmin;

static const char *takeWord(const char *args, char *word, size_t wordLen)
{
    while (*args == ' ')
        args++;
    size_t n = 0;
    while (args[n] != '\0' && args[n] != ' ') {
        if (n + 1 < wordLen)
            word[n] = args[n];
        n++;
    }
    word[n < wordLen ? n : wordLen - 1] = '\0';
    const char *rest = args + n;
    while (*rest == ' ')
        rest++;
    return rest;
}

static bool isHex(const char *s, size_t len)
{
    if (!len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

static void setName(char *dst, size_t cap, const char *src)
{
    const size_t len = src ? strnlen(src, cap - 1) : 0;
    if (len)
        memcpy(dst, src, len);
    memset(dst + len, 0, cap - len);
}

static bool isAdminType(uint8_t type)
{
    return type == MeshCoreNode::REPEATER || type == MeshCoreNode::ROOM;
}

bool MeshCoreAdmin::handleCommand(const char *args, char *reply, size_t replyLen, bool &waitForEvent)
{
    waitForEvent = false;
    if (!args || !reply || replyLen == 0)
        return false;

    char first[40], verb[8], targetWord[40];
    const char *rest = takeWord(args, first, sizeof(first));
    if (!first[0])
        return false;

    bool pass;
    if (strcasecmp(first, "PASS") == 0) {
        pass = true;
        rest = takeWord(rest, targetWord, sizeof(targetWord));
    } else if (strcasecmp(first, "CMD") == 0) {
        pass = false;
        rest = takeWord(rest, targetWord, sizeof(targetWord));
    } else {
        setName(targetWord, sizeof(targetWord), first);
        rest = takeWord(rest, verb, sizeof(verb));
        if (strcasecmp(verb, "PASS") == 0)
            pass = true;
        else if (strcasecmp(verb, "CMD") == 0)
            pass = false;
        else
            return false;
    }

    if (!targetWord[0]) {
        snprintf(reply, replyLen, "MESHTACORE: target required");
        return true;
    }

    concurrency::LockGuard guard(&lock);

    Target t;
    bool ambiguous = false;
    if (!resolveTarget(targetWord, &t, &ambiguous)) {
        snprintf(reply, replyLen, "MESHTACORE: %s target %s", ambiguous ? "ambiguous" : "unknown", targetWord);
        return true;
    }

    if (pass)
        return handlePass(t, rest, reply, replyLen);
    return handleCmd(t, rest, reply, replyLen, waitForEvent);
}

bool MeshCoreAdmin::handlePass(const Target &t, const char *password, char *reply, size_t replyLen)
{
    if (phaseId != IDLE) {
        snprintf(reply, replyLen, "MESHTACORE: another command is in progress");
        return true;
    }
    if (!password || !password[0]) {
        snprintf(reply, replyLen, "MESHTACORE: password required");
        return true;
    }
    const size_t len = strlen(password);
    if (len > MeshCoreCompanion::MAX_PASSWORD_LEN) {
        snprintf(reply, replyLen, "MESHTACORE: password too long (max %u)", (unsigned)MeshCoreCompanion::MAX_PASSWORD_LEN);
        return true;
    }

    Credential *c = findCredential(t.pub);
    if (!c) {
        if (credCount >= MAX_CREDENTIALS) {
            snprintf(reply, replyLen, "MESHTACORE: credential table full");
            return true;
        }
        c = &creds[credCount++];
        *c = Credential();
    }

    memcpy(c->pub, t.pub, 32);
    c->type = t.type;
    setName(c->name, sizeof(c->name), t.name);
    c->passLen = (uint8_t)len;
    memset(c->pass, 0, sizeof(c->pass));
    memcpy(c->pass, password, len);
    invalidateSession(t.pub);

    snprintf(reply, replyLen, "MESHTACORE: password set for %s (RAM; /nexus SAVE persists)", t.name[0] ? t.name : "target");
    return true;
}

bool MeshCoreAdmin::handleCmd(const Target &t, const char *text, char *reply, size_t replyLen, bool &waitForEvent)
{
    if (!text || !text[0]) {
        snprintf(reply, replyLen, "MESHTACORE: command required");
        return true;
    }
    if (strlen(text) > CMD_MAX) {
        snprintf(reply, replyLen, "MESHTACORE: command too long (max %u)", (unsigned)CMD_MAX);
        return true;
    }
    if (eventReady) {
        snprintf(reply, replyLen, "MESHTACORE: previous reply still pending");
        return true;
    }
    if (phaseId != IDLE) {
        snprintf(reply, replyLen, "MESHTACORE: another command is in progress");
        return true;
    }
    if (!findCredential(t.pub)) {
        snprintf(reply, replyLen, "MESHTACORE: no password for %s; use PASS first", t.name[0] ? t.name : "target");
        return true;
    }

    Session *s = findSession(t.pub);
    const bool cached = s && s->used && Throttle::isWithinTimespanMs(s->lastOkMs, SESSION_TTL_MS);
    if (cached && (s->perms & 3) != 3) {
        snprintf(reply, replyLen, "MESHTACORE: %s is not admin (permissions %u)", t.name[0] ? t.name : "target",
                 (unsigned)s->perms);
        return true;
    }

    if (!getValidTime(RTCQualityFromNet)) {
        snprintf(reply, replyLen, "MESHTACORE: no valid clock yet");
        return true;
    }

    target = t;
    setName(command, sizeof(command), text);
    attempt = 0;
    retries = 0;

    const bool sent = cached ? sendCommand() : sendLogin();
    if (!sent) {
        phaseId = IDLE;
        snprintf(reply, replyLen, "MESHTACORE: cannot send to %s (radio, identity or clock)", t.name[0] ? t.name : "target");
        return true;
    }

    waitForEvent = true;
    snprintf(reply, replyLen, "MESHTACORE: %s to %s", cached ? "command sent" : "login sent", t.name[0] ? t.name : "target");
    return true;
}

bool MeshCoreAdmin::sendLogin()
{
    const Credential *c = findCredential(target.pub);
    const uint8_t *seed = meshCoreCompanion.getIdentitySeed();
    const uint8_t *ourPub = meshCoreCompanion.getIdentityPub();
    const uint32_t epoch = getValidTime(RTCQualityFromNet);
    if (!c || !seed || !ourPub || !epoch)
        return false;

    uint8_t frame[96];
    const size_t len =
        MeshCoreCompanion::encodeAnonLogin(seed, ourPub, target.pub, meshCoreCompanion.nextTxTimestamp(epoch), UINT32_MAX,
                                           target.type == MeshCoreNode::ROOM, c->pass, frame, sizeof(frame));
    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (!len || !iface || !iface->queueMeshCoreFrame(frame, len))
        return false;

    phaseId = LOGIN_SENT;
    startedMs = Time::getMillis();
    return true;
}

bool MeshCoreAdmin::sendCommand()
{
    const Credential *c = findCredential(target.pub);
    const uint8_t *seed = meshCoreCompanion.getIdentitySeed();
    const uint8_t *ourPub = meshCoreCompanion.getIdentityPub();
    const uint32_t epoch = getValidTime(RTCQualityFromNet);
    if (!c || !seed || !ourPub || !epoch)
        return false;

    uint8_t secret[32];
    if (!MeshCoreCompanion::sharedSecret(seed, target.pub, secret))
        return false;

    uint8_t frame[255];
    const uint8_t flags = (attempt & 3) | MeshCoreCompanion::TXT_FLAGS_CLI;
    const size_t len = MeshCoreCompanion::encodeDirectText(
        secret, target.pub[0], ourPub[0], meshCoreCompanion.nextTxTimestamp(epoch), flags, command, frame, sizeof(frame));
    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (!len || !iface || !iface->queueMeshCoreFrame(frame, len))
        return false;

    attempt = (attempt + 1) & 3;
    phaseId = COMMAND_SENT;
    startedMs = Time::getMillis();
    return true;
}

bool MeshCoreAdmin::onPathResponse(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    concurrency::LockGuard guard(&lock);
    if (phaseId != LOGIN_SENT || !raw || info.payload_len < 2 || (size_t)info.header_len + 2 > len)
        return false;

    const uint8_t *seed = meshCoreCompanion.getIdentitySeed();
    const uint8_t *ourPub = meshCoreCompanion.getIdentityPub();
    if (!seed || !ourPub)
        return false;

    const uint8_t *payload = raw + info.header_len;
    if (payload[0] != ourPub[0] || payload[1] != target.pub[0])
        return false;

    uint8_t secret[32], extra[32];
    size_t extraLen = 0;
    if (!MeshCoreCompanion::sharedSecret(seed, target.pub, secret) ||
        !MeshCoreCompanion::decodePathResponse(secret, raw, len, info, extra, sizeof(extra), &extraLen) || extraLen < 13)
        return false;

    if (extra[4] != 0) {
        phaseId = IDLE;
        finishWithEvent("%s login refused (code %u)", target.name[0] ? target.name : "target", (unsigned)extra[4]);
        return true;
    }

    const uint8_t perms = extra[7];
    const bool admin = extra[6] == 1 || (perms & 3) == 3;
    noteSession(target.pub, perms);
    if (!admin) {
        phaseId = IDLE;
        finishWithEvent("%s login ok but not admin (permissions %u)", target.name[0] ? target.name : "target", (unsigned)perms);
        return true;
    }

    if (!sendCommand()) {
        phaseId = IDLE;
        finishWithEvent("%s login ok but command could not be sent", target.name[0] ? target.name : "target");
    }
    return true;
}

bool MeshCoreAdmin::onCliText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info)
{
    concurrency::LockGuard guard(&lock);
    if (phaseId != COMMAND_SENT || !raw || info.payload_len < 2 || (size_t)info.header_len + 2 > len)
        return false;

    const uint8_t *seed = meshCoreCompanion.getIdentitySeed();
    const uint8_t *ourPub = meshCoreCompanion.getIdentityPub();
    if (!seed || !ourPub)
        return false;

    const uint8_t *payload = raw + info.header_len;
    if (payload[0] != ourPub[0] || payload[1] != target.pub[0])
        return false;

    uint8_t secret[32];
    char text[CMD_MAX + 1];
    uint32_t timestamp = 0;
    uint8_t flags = 0;
    if (!MeshCoreCompanion::sharedSecret(seed, target.pub, secret) ||
        !MeshCoreCompanion::decodeDirectTextType(secret, 1, raw, len, info, text, sizeof(text), &timestamp, &flags))
        return false;

    noteSession(target.pub, 3);
    phaseId = IDLE;
    finishWithEvent("%s: %s", target.name[0] ? target.name : "target", text[0] ? text : "(no output)");
    return true;
}

void MeshCoreAdmin::service()
{
    concurrency::LockGuard guard(&lock);
    if (phaseId == LOGIN_SENT && !Throttle::isWithinTimespanMs(startedMs, LOGIN_TIMEOUT_MS)) {
        phaseId = IDLE;
        finishWithEvent("no login response from %s (wrong password, out of range, or relay off)",
                        target.name[0] ? target.name : "target");
    } else if (phaseId == COMMAND_SENT && !Throttle::isWithinTimespanMs(startedMs, COMMAND_TIMEOUT_MS)) {
        if (retries == 0) {
            retries = 1;
            phaseId = IDLE;
            if (!sendLogin())
                finishWithEvent("no command reply from %s", target.name[0] ? target.name : "target");
        } else {
            phaseId = IDLE;
            finishWithEvent("no command reply from %s", target.name[0] ? target.name : "target");
        }
    }
}

bool MeshCoreAdmin::pollEvent(char *out, size_t cap)
{
    concurrency::LockGuard guard(&lock);
    if (!eventReady || !out || !cap)
        return false;
    snprintf(out, cap, "%s", event);
    eventReady = false;
    return true;
}

void MeshCoreAdmin::abort()
{
    concurrency::LockGuard guard(&lock);
    phaseId = IDLE;
    attempt = 0;
    retries = 0;
    command[0] = '\0';
    target = Target();
    clearEventLocked();
    for (auto &s : sessions)
        s = Session();
}

size_t MeshCoreAdmin::exportFile(uint8_t *buf, size_t cap)
{
    concurrency::LockGuard guard(&lock);
    return MeshCoreAdminFile::encode(creds, credCount, buf, cap);
}

size_t MeshCoreAdmin::importFile(const uint8_t *buf, size_t len)
{
    concurrency::LockGuard guard(&lock);
    Credential loaded[MAX_CREDENTIALS];
    bool valid = false;
    const size_t count = MeshCoreAdminFile::decode(buf, len, loaded, MAX_CREDENTIALS, &valid);
    if (!valid)
        return 0;

    for (size_t i = 0; i < count; i++)
        creds[i] = loaded[i];
    credCount = count;
    for (auto &s : sessions)
        s = Session();
    phaseId = IDLE;
    attempt = 0;
    retries = 0;
    command[0] = '\0';
    clearEventLocked();
    return count;
}

size_t MeshCoreAdmin::credentialCount() const
{
    concurrency::LockGuard guard(&lock);
    return credCount;
}

const MeshCoreAdmin::Credential *MeshCoreAdmin::credentialAt(size_t i) const
{
    concurrency::LockGuard guard(&lock);
    return i < credCount ? &creds[i] : nullptr;
}

MeshCoreAdmin::Phase MeshCoreAdmin::phase() const
{
    concurrency::LockGuard guard(&lock);
    return phaseId;
}

bool MeshCoreAdmin::hasCredential(const uint8_t pub[32]) const
{
    concurrency::LockGuard guard(&lock);
    return findCredential(pub) != nullptr;
}

const MeshCoreAdmin::Credential *MeshCoreAdmin::findCredential(const uint8_t pub[32]) const
{
    for (size_t i = 0; i < credCount; i++)
        if (memcmp(creds[i].pub, pub, 32) == 0)
            return &creds[i];
    return nullptr;
}

MeshCoreAdmin::Credential *MeshCoreAdmin::findCredential(const uint8_t pub[32])
{
    for (size_t i = 0; i < credCount; i++)
        if (memcmp(creds[i].pub, pub, 32) == 0)
            return &creds[i];
    return nullptr;
}

MeshCoreAdmin::Session *MeshCoreAdmin::findSession(const uint8_t pub[32])
{
    for (auto &s : sessions)
        if (s.used && memcmp(s.pub, pub, 32) == 0)
            return &s;
    return nullptr;
}

const MeshCoreAdmin::Session *MeshCoreAdmin::findSession(const uint8_t pub[32]) const
{
    for (const auto &s : sessions)
        if (s.used && memcmp(s.pub, pub, 32) == 0)
            return &s;
    return nullptr;
}

void MeshCoreAdmin::noteSession(const uint8_t pub[32], uint8_t perms)
{
    Session *s = findSession(pub);
    if (!s) {
        Session *oldest = &sessions[0];
        for (auto &candidate : sessions) {
            if (!candidate.used) {
                oldest = &candidate;
                break;
            }
            if (candidate.lastOkMs < oldest->lastOkMs)
                oldest = &candidate;
        }
        s = oldest;
        *s = Session();
        memcpy(s->pub, pub, 32);
    }
    s->used = true;
    s->lastOkMs = Time::getMillis();
    s->perms = perms;
}

void MeshCoreAdmin::invalidateSession(const uint8_t pub[32])
{
    Session *s = findSession(pub);
    if (s)
        *s = Session();
}

bool MeshCoreAdmin::resolveTarget(const char *arg, Target *out, bool *ambiguous) const
{
    *ambiguous = false;
    if (!arg || !arg[0] || !out)
        return false;

    const char *hex = arg[0] == '!' ? arg + 1 : arg;
    const size_t hexLen = strlen(hex);
    if (isHex(hex, hexLen) && hexLen <= 8) {
        if (hexLen == 8) {
            const uint32_t nodeNum = (uint32_t)strtoul(hex, nullptr, 16);
            const MeshCoreNode *n = meshCoreCompanion.nodes.findByNodeNum(nodeNum);
            if (n && isAdminType(n->type)) {
                memcpy(out->pub, n->pub, 32);
                out->type = n->type;
                setName(out->name, sizeof(out->name), n->name);
                return true;
            }
        }

        if ((hexLen & 1) == 0) {
            uint8_t prefix[4];
            const size_t prefixLen = hexLen / 2;
            for (size_t i = 0; i < prefixLen; i++) {
                char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                prefix[i] = (uint8_t)strtoul(pair, nullptr, 16);
            }

            MeshCoreNode *matches[8];
            const size_t total = meshCoreCompanion.nodes.findByPrefix(prefix, (uint8_t)prefixLen, matches, 8);
            Target found[8];
            size_t foundCount = 0;
            if (total > 8) {
                *ambiguous = true;
                return false;
            }
            for (size_t i = 0; i < total; i++) {
                if (!isAdminType(matches[i]->type))
                    continue;
                bool dup = false;
                for (size_t j = 0; j < foundCount; j++)
                    if (memcmp(found[j].pub, matches[i]->pub, 32) == 0)
                        dup = true;
                if (!dup && foundCount < 8) {
                    memcpy(found[foundCount].pub, matches[i]->pub, 32);
                    found[foundCount].type = matches[i]->type;
                    setName(found[foundCount].name, sizeof(found[foundCount].name), matches[i]->name);
                    foundCount++;
                }
            }
            for (size_t i = 0; i < credCount; i++) {
                if (!isAdminType(creds[i].type) || memcmp(creds[i].pub, prefix, prefixLen) != 0)
                    continue;
                bool dup = false;
                for (size_t j = 0; j < foundCount; j++)
                    if (memcmp(found[j].pub, creds[i].pub, 32) == 0)
                        dup = true;
                if (!dup && foundCount < 8) {
                    memcpy(found[foundCount].pub, creds[i].pub, 32);
                    found[foundCount].type = creds[i].type;
                    setName(found[foundCount].name, sizeof(found[foundCount].name), creds[i].name);
                    foundCount++;
                }
            }
            if (foundCount > 1) {
                *ambiguous = true;
                return false;
            }
            if (foundCount == 1) {
                *out = found[0];
                return true;
            }
        }
        return false;
    }

    const MeshCoreNode *match = nullptr;
    size_t matchCount = 0;
    meshCoreCompanion.nodes.forEach([&](const MeshCoreNode &n) {
        if (isAdminType(n.type) && strcasecmp(n.name, arg) == 0) {
            match = &n;
            matchCount++;
        }
    });
    if (matchCount > 1) {
        *ambiguous = true;
        return false;
    }
    if (match) {
        memcpy(out->pub, match->pub, 32);
        out->type = match->type;
        setName(out->name, sizeof(out->name), match->name);
        return true;
    }

    const Credential *cred = nullptr;
    size_t credCountMatch = 0;
    for (size_t i = 0; i < credCount; i++) {
        if (isAdminType(creds[i].type) && strcasecmp(creds[i].name, arg) == 0) {
            cred = &creds[i];
            credCountMatch++;
        }
    }
    if (credCountMatch > 1) {
        *ambiguous = true;
        return false;
    }
    if (!cred)
        return false;

    memcpy(out->pub, cred->pub, 32);
    out->type = cred->type;
    setName(out->name, sizeof(out->name), cred->name);
    return true;
}

void MeshCoreAdmin::finishWithEvent(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(event, sizeof(event), fmt, ap);
    va_end(ap);
    eventReady = true;
}

void MeshCoreAdmin::clearEventLocked()
{
    event[0] = '\0';
    eventReady = false;
}

#endif // MESHTASTIC_ENABLE_MESHCORE_ADMIN
