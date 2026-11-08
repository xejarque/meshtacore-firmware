# MeshCore remote administration

This document describes the MeshCore repeater/room administration feature: what it does, how it is wired into Nexus, how it is persisted, what is verified, and what remains before it can be treated as production-ready.

## Where this work lives

| Item               | Value                                                          |
| ------------------ | -------------------------------------------------------------- |
| Worktree           | `/Users/xavier.ejarque/git/meshtacore-firmware-meshcore-admin` |
| Branch             | `feat/meshcore-admin`                                          |
| Base commit        | `27337dd78` (`perf(builder): cache compilation with ccache`)   |
| State              | Uncommitted working-tree changes                               |
| Feature flag       | `MESHTASTIC_ENABLE_MESHCORE_ADMIN`                             |
| Underlying feature | `MESHTASTIC_ENABLE_MESHCORE_COMPANION`                         |

The worktree is separate from the main checkout. Run every build, test, and format command from that directory.

## Scope and status

| Step                                                                                     | Status   |
| ---------------------------------------------------------------------------------------- | -------- |
| Add a build gate that keeps the feature off flash-constrained nRF52 builds with a screen | Done     |
| Add ROOM (`ADV_TYPE_ROOM`) support to the MeshCore node store and companion              | Done     |
| Add ANON_REQ login, PATH response, and CLI TXT_MSG codecs                                | Done     |
| Add the password/session/command state machine                                           | Done     |
| Wire `PASS`, `CMD`, and `ROOMS` into Nexus with asynchronous replies                     | Done     |
| Persist credentials under `/prefs/meshcore_admin.bin` on `/nexus SAVE`                   | Done     |
| Add native tests for persistence, login, CLI, timeouts, and retry                        | Done     |
| Update `docs/meshcore.md` and `docs/nexus-commands.md`                                   | Done     |
| Validate against a real repeater or room server                                          | Not done |
| Commit the work                                                                          | Not done |
| Triage unrelated full-suite failures                                                     | Not done |

The feature is intentionally limited to the companion build on ESP32 or on an nRF52 board without a screen. A build can override the default with `-DMESHTASTIC_ENABLE_MESHCORE_ADMIN=1` or `=0`; enabling it without the companion is a compile-time error.

## Operator interface

### Commands

```text
/nexus MCORE PASS <target> <password>
/nexus MCORE CMD <target> <command>
/nexus MCORE ROOMS
/nexus SAVE
```

`/nexus MESHCORE` is the long form and `MCORE` is the alias. Nexus also accepts the `/mcore` root.

Examples:

```text
/nexus MCORE PASS RPT-01 hunter2
/nexus MCORE CMD RPT-01 tx 22
/nexus MCORE CMD !4d12ab34 status
```

`PASS` stores a password in RAM. It is not written to flash until `/nexus SAVE`. There is no command that reads a password back.

`CMD` logs in with the stored password and sends one CLI command. If a successful admin session is less than 10 minutes old, the login is skipped and the command is sent directly.

### Targets

A target is resolved in this order:

1. A contact name, case-insensitive.
2. An even-length 2-8 hex prefix of the contact public key.
3. An 8 hex node number.

A leading `!` is optional. The resolved contact must be a `REPEATER` or `ROOM`. If a name or prefix matches more than one contact, the command is refused. Targets are a single token: a contact name containing spaces cannot be typed as a target; use a key prefix or node number instead.

Credentials saved with `PASS` remain usable if the contact is no longer in the live node table. Resolution falls back to the credential table.

### Reply flow

The command is asynchronous:

1. The immediate reply is `login sent` or `command sent`.
2. Nexus keeps the reply context (destination, channel, reply id, phone-only flag).
3. When the remote answers, the final text is sent as `<target>: <output>`.

A login that receives no answer is reported after 45 seconds. A command that receives no answer is retried once after 30 seconds with a fresh login; a second timeout reports `no command reply`.

Only one admin operation runs at a time. A second `CMD` is refused with `another command is in progress` or `previous reply still pending`.

## Implementation

### Components

| File                                      | Responsibility                                                                                |
| ----------------------------------------- | --------------------------------------------------------------------------------------------- |
| `src/mesh/MeshCoreAdmin.h/.cpp`           | Credential table, target resolution, login/command state machine, sessions, timeouts, events. |
| `src/mesh/MeshCoreAdminFile.h`            | `MCA` v1 credential image, CRC32, `encode`/`decode`.                                          |
| `src/mesh/MeshCoreCompanion.h/.cpp`       | Login, PATH response, and CLI text codecs; ROOM learning; admin hooks.                        |
| `src/mesh/MeshCoreRepeater.h`             | `ADV_TYPE_ROOM` and shared MeshCore frame constants.                                          |
| `src/mesh/RadioLibInterfaceMeshCore.cpp`  | Load/save `/prefs/meshcore_admin.bin`.                                                        |
| `src/modules/NexusMeshCoreCommand.cpp`    | `PASS`/`CMD`/`ROOMS` dispatch and `HELP`.                                                     |
| `src/modules/NexusModule.h/.cpp`          | Async admin reply context and polling in `runOnce()`.                                         |
| `src/modules/NexusMeshtacoreCommands.cpp` | Passes packet context into the MeshCore command handler.                                      |
| `src/modules/NexusCommands.cpp`           | Updated call site for the command context.                                                    |
| `src/mesh/PhoneAPI.cpp`                   | Maps a ROOM replay node to `ROLE_ROUTER`, not a messageable chat contact.                     |
| `test/test_meshcore_admin/test_main.cpp`  | Native regression suite.                                                                      |

### Protocol

- Login uses MeshCore `ANON_REQ` (`0x07`) with the target public-key hash, our public key, a 2-byte HMAC, and AES ciphertext.
- Repeater login plaintext: `[timestamp LE32][password]`.
- Room login plaintext: `[timestamp LE32][sync_since LE32][password]`.
- Login response is carried in a `PATH` frame (`0x08`) with extra type `RESPONSE` (`0x01`). Reading it as a different payload type is a regression guarded by the tests.
- Admin permission is granted when response byte `[6]` is `1`, or when `(permissions & 3) == 3`.
- CLI data is a direct `TXT_MSG` (`0x02`) with flags `(attempt & 3) | (TXT_TYPE_CLI_DATA << 2)`.
- The command text is limited to 160 bytes. The reply is one Nexus line and is truncated by the shared Nexus reply buffer.

### State and timing

| Constant             | Value  | Meaning                                   |
| -------------------- | ------ | ----------------------------------------- |
| `MAX_CREDENTIALS`    | 8      | Saved passwords                           |
| `MAX_SESSIONS`       | 8      | Cached admin sessions                     |
| `SESSION_TTL_MS`     | 10 min | Reuse an admin session without logging in |
| `LOGIN_TIMEOUT_MS`   | 45 s   | No login response                         |
| `COMMAND_TIMEOUT_MS` | 30 s   | No CLI response before retry              |
| `CMD_MAX`            | 160 B  | Maximum CLI command                       |
| `EVENT_MAX`          | 200 B  | One pending final event                   |

State transitions: `IDLE -> LOGIN_SENT -> COMMAND_SENT -> IDLE`. A non-admin login ends in `IDLE` without sending a command. A command timeout returns to `LOGIN_SENT` and retries the command once.

### Persistence

`/prefs/meshcore_admin.bin` is written only by `/nexus SAVE`. The format is:

- Magic `MCA`, version `1`.
- Up to 8 records.
- Each record: public key (32 B), node type (1 B), name (32 B), password length (1 B), password (16 B, NUL padded).
- CRC32 of the body at the end.

Passwords are stored in plaintext. Encrypting them with a key stored on the same device would not protect them from someone who can read the flash, and the repeater expects the password itself.

A corrupt, truncated, wrong-version, or CRC-mismatched image loads no credentials. A valid image with zero records clears the table.

### Threading

`MeshCoreAdmin` owns a `concurrency::Lock`; every public operation and the radio-side callbacks take it. `MeshCoreCompanion::nextTxTimestamp()` has its own lock. The Nexus module only starts an operation and polls the final event; the radio-side `MeshCoreCompanion::service()` drives timeouts and retries.

## Build and test

Targeted suite:

```bash
./bin/test-native-docker.sh -f test_meshcore_admin
```

Result: 11 tests, 0 failures.

Full suite:

```bash
./bin/test-native-docker.sh
```

Current result on this branch: 1673 test cases, 15 failed, 1648 succeeded; 10 suites errored. The new admin suite is green; the failures are in older suites (`test_nodedb_boot_recovery`, `test_admin_radio`, `test_event_channel_router`, `test_meshtastic_path_command`, `test_meshcore_node_store`, `test_stream_framing`, `test_nodedb_lora_slot`, `test_meshcore_region`, `test_userprefs_channels`, `test_channel_keys`). Their suite sources are unchanged, but shared files such as `PhoneAPI.cpp` were touched, so establish a baseline before blaming the feature. Do not treat this branch as full-suite green until those are triaged separately.

Builds verified from a clean environment:

| Environment             | Flash                           | RAM                         | Feature state            |
| ----------------------- | ------------------------------- | --------------------------- | ------------------------ |
| `thinknode_m1_meshcore` | 765,040 / 815,104 B (93.9%)     | 116,916 / 248,832 B (47.0%) | Admin on                 |
| `heltec-v3`             | 2,216,855 / 3,342,336 B (66.3%) | 171,512 / 327,680 B (52.3%) | Admin on                 |
| `rak4631`               | 643,788 / 815,104 B (79.0%)     | 122,180 / 248,832 B (49.1%) | Admin on (no screen)     |
| `muzi-base`             | 775,544 / 815,104 B (95.1%)     | 125,172 / 248,832 B (50.3%) | Admin off (screen build) |

Build commands:

```bash
~/.platformio/penv/bin/pio run -e <env>
```

`pio run -e native` on macOS is not a reliable test host because of a pre-existing clang VLA initializer issue in `NotificationRenderer`. Use the Docker test runner.

## Operational notes

- **Plaintext passwords**: anyone with flash read access can recover them. Treat the device as a credential store.
- **No password erase command**: a credential can be overwritten with `PASS <target> <new>`. The table is full at 8 entries; there is no delete/read-back command yet.
- **Retry can execute twice**: a lost CLI response makes the node log in again and resend the same command. Commands must be safe to repeat.
- **No clock, no command**: `CMD` requires a valid wall-clock time. `PASS` does not.
- **One operation globally**: login/command state is not per target. Wait for the final reply before sending another command.
- **No output is ambiguous**: a command that produces no CLI text looks like a timeout. After the retry it reports `no command reply`, not success.
- **Admin scope is narrow**: there is no channel, region, or advert configuration through this path. It runs CLI commands only.
- **ROOM is infrastructure**: rooms are listed and administered, but are not messageable contacts. `PhoneAPI` maps them to `ROLE_ROUTER`.
- **Session cache is local**: changing a password with `PASS` invalidates the cached session. Changing the identity aborts sessions but keeps saved credentials.
- **Save coupling**: `/nexus SAVE` writes the MeshCore config, regions, and admin credentials. `saveMeshCoreConfig()` returns `false` if the admin image cannot be written, but `NexusModule::saveMeshCoreSettings()` currently ignores that return value; check the log for `MeshCore admin: could not save`.
- **Formatting**: run `trunk fmt` on the files being changed, not over the whole checkout; this fork has unrelated files that the formatter rewrites.
- **Flash headroom**: `thinknode_m1_meshcore` is at 93.9% and `muzi-base` at 95.1% even with admin off. Re-check the target budget before adding more code.

## Pending work

1. Validate against real MeshCore repeater and room-server firmware, including wrong-password, non-admin, and no-output cases.
2. Triage the unrelated full-suite failures or confirm they also fail on the base commit.
3. Commit the worktree changes on `feat/meshcore-admin`.
4. Optionally add credential deletion/read-back and a build-time or runtime switch for the admin feature.
