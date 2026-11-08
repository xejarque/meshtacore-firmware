#pragma once

#include "MeshCoreAdminFile.h"
#include "MeshCoreRepeater.h"
#include "concurrency/Lock.h"
#include "concurrency/LockGuard.h"
#include <stddef.h>
#include <stdint.h>

#if MESHTASTIC_ENABLE_MESHCORE_ADMIN

/**
 * Remote administration of MeshCore repeaters and room servers from Nexus: one saved password per node, a login
 * handshake, and the CLI command carried as TXT_MSG/CLI_DATA. All the radio work happens in the radio thread through
 * MeshCoreCompanion::service(); the Nexus module only starts an operation and polls for the final event.
 */
class MeshCoreAdmin
{
  public:
    using Credential = MeshCoreAdminFile::Credential;

    static constexpr size_t MAX_CREDENTIALS = MeshCoreAdminFile::MAX_CREDENTIALS;
    static constexpr size_t MAX_SESSIONS = 8;
    static constexpr size_t CMD_MAX = 160; // MeshCore MAX_TEXT_LEN
    static constexpr size_t EVENT_MAX = 200;
    static constexpr uint32_t SESSION_TTL_MS = 10 * 60 * 1000;
    static constexpr uint32_t LOGIN_TIMEOUT_MS = 45 * 1000;
    static constexpr uint32_t COMMAND_TIMEOUT_MS = 30 * 1000;

    enum Phase : uint8_t { IDLE = 0, LOGIN_SENT = 1, COMMAND_SENT = 2 };

    /**
     * Handle /mcore PASS|CMD. Targets may come first (34fa CMD tx 22) or after the verb (CMD 34fa tx 22).
     * Returns false when args is not a PASS/CMD command, so the caller keeps dispatching.
     */
    bool handleCommand(const char *args, char *reply, size_t replyLen, bool &waitForEvent);

    /// Poll the one pending final event. Returns false when there is none; clears it when true.
    bool pollEvent(char *out, size_t cap);

    /// Drain timeouts and retry one lost command. Called from the MeshCore service tick.
    void service();

    /// Drop any operation and session (identity changed, or a new command took over).
    void abort();

    /// Login response carried by a PATH frame. Returns true when it was ours and consumed.
    bool onPathResponse(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info);

    /// CLI reply carried by a TXT_MSG. Returns true when it was ours and consumed.
    bool onCliText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info);

    /// Flash image helpers, used by RadioLibInterfaceMeshCore.
    size_t exportFile(uint8_t *buf, size_t cap);
    size_t importFile(const uint8_t *buf, size_t len);

    size_t credentialCount() const;
    const Credential *credentialAt(size_t i) const;
    Phase phase() const;
    bool hasCredential(const uint8_t pub[32]) const;

  private:
    struct Session {
        uint8_t pub[32] = {};
        uint32_t lastOkMs = 0;
        uint8_t perms = 0;
        bool used = false;
    };

    struct Target {
        uint8_t pub[32] = {};
        uint8_t type = 0;
        char name[MeshCoreNode::NAME_LEN] = {};
    };

    mutable concurrency::Lock lock;
    Credential creds[MAX_CREDENTIALS];
    size_t credCount = 0;
    Session sessions[MAX_SESSIONS];

    Phase phaseId = IDLE;
    Target target;
    char command[CMD_MAX + 1] = {};
    uint8_t attempt = 0;
    uint8_t retries = 0;
    uint32_t startedMs = 0;

    bool eventReady = false;
    char event[EVENT_MAX] = {};

    bool handlePass(const Target &t, const char *password, char *reply, size_t replyLen);
    bool handleCmd(const Target &t, const char *text, char *reply, size_t replyLen, bool &waitForEvent);
    bool resolveTarget(const char *arg, Target *out, bool *ambiguous) const;
    const Credential *findCredential(const uint8_t pub[32]) const;
    Credential *findCredential(const uint8_t pub[32]);
    Session *findSession(const uint8_t pub[32]);
    const Session *findSession(const uint8_t pub[32]) const;
    void noteSession(const uint8_t pub[32], uint8_t perms);
    void invalidateSession(const uint8_t pub[32]);

    bool sendLogin();
    bool sendCommand();
    void finishWithEvent(const char *fmt, ...);
    void clearEventLocked();
};

extern MeshCoreAdmin meshCoreAdmin;

#endif // MESHTASTIC_ENABLE_MESHCORE_ADMIN
