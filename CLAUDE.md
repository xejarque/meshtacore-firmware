# Claude Code instructions

> **TL;DR**
>
> |                |                                                                                                                                         |
> | -------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
> | Local tests    | `./bin/run-tests.sh` (exit 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED · 4 BUSY · 5 ABORTED · 6 UNSUPPORTED); `--status` first               |
> | Hardware tests | [meshtastic/meshtastic-mcp](https://github.com/meshtastic/meshtastic-mcp) (`MESHTASTIC_FIRMWARE_ROOT` → this checkout)                  |
> | Format         | `trunk fmt`                                                                                                                             |
> | ESP32 builds   | one `~/.platformio/penv` on `pioarduino==6.2.0` + `platformio==6.2.0`; ESP platform 55.03.312-1 - see [Build toolchain](#build-toolchain-pio-and-python) |
> | Mirror docs    | `.github/copilot-instructions.md` (canonical) · `AGENTS.md`                                                                             |
>
> **Need this? It's here.**
>
> |                                                           |                                                                                        |
> | --------------------------------------------------------- | -------------------------------------------------------------------------------------- |
> | General helpers (clamp, UTF-8, string fmt…)               | `src/meshUtils.h`                                                                      |
> | Logging macros (LOG_DEBUG / INFO / WARN…)                 | `src/DebugConfiguration.h`                                                             |
> | Elapsed time / deadlines (never bare `millis()` compares) | `src/mesh/Throttle.h`                                                                  |
> | New module skeleton                                       | inherit `ProtobufModule<T>` in `src/mesh/ProtobufModule.h`                             |
> | Observer / event wiring                                   | `src/Observer.h`                                                                       |
> | Test naming (the `src/` rule does **not** apply)          | [copilot-instructions.md#test-naming](.github/copilot-instructions.md#test-naming)     |
> | Test comments (the 1-2 line limit does **not** apply)     | [copilot-instructions.md#test-comments](.github/copilot-instructions.md#test-comments) |

**Read `.github/copilot-instructions.md` first.** That file is the canonical agent-facing document for this repo. It covers project layout, coding conventions, the build system, CI/CD, the native C++ test suite, and the MCP Server & Hardware Test Harness. Read it top-to-bottom before starting any non-trivial change.

This file (`CLAUDE.md`) is a short pointer for Claude Code sessions. Slash commands live in `.claude/commands/`.

## Use the meshtastic MCP for everything it covers

Prefer the `mcp__meshtastic__*` tools over raw `pio`, `esptool`, `nrfutil` or ad-hoc Python scripts: build (`build`, `build_start`, `build_poll`), flash (`flash_start`, `pio_flash`, `update_flash`, `erase_and_flash`), serial and logs (`serial_*`, `logs_window`), device info and config (`device_info`, `get_config`, `set_config`), reboot, `send_text`, packets and events windows. Their replies and errors come back structured and compact, which is cheaper to read than shell output. Fall back to the shell only when no MCP tool covers the task (for example `./bin/run-tests.sh`, Docker tests, or sending `/mtcl` commands to the node itself).

## Build toolchain (pio and Python)

ESP32 and nRF52 both build from the **single shared venv at `~/.platformio/penv`**, pinned to **exactly `6.2.0`** for both `pioarduino` and `platformio`. This is what every workflow and `bin/build-esp32.sh` install, so a local build matches CI.

```bash
rm -rf ~/.platformio/penv
python3.12 -m venv ~/.platformio/penv
~/.platformio/penv/bin/pip install "pioarduino==6.2.0" "platformio==6.2.0"
```

Check it (both lines must say `6.2.0`):

```bash
~/.platformio/penv/bin/pio --version && ~/.platformio/penv/bin/pip list | grep -Ei '^(platformio|pioarduino) '
```

**Never `6.2.0.post1`.** It is a prerelease that sorts below `6.2.0` for semver, so the ESP platform's `engines.platformio >=6.2.0` check fails with `IncompatiblePlatform`. Pin the exact release (CI also exports `UV_CONSTRAINT`/`PIP_CONSTRAINT` for this).

### The core and the ESP platform are a matched pair

`variants/esp32/esp32-common.ini` uses platform **`55.03.312-1`** (pioarduino, `engines >=6.2.0`). Going back to the meshtastic fork `55.03.311` (`engines >=6.1.19`) works with either core. Either both move or neither does.

The old FortranCommon trap (`No module named 'SCons.Tool.FortranCommon'` at ESP-IDF link) was tied to the core/platform combination of the 6.1.19 era; a clean `heltec-v3` build with 6.2.0 + 312-1 verified on 2026-11-13 links without it. If it ever comes back, do not hand-install `tool-scons` or patch `FortranCommon.py`: re-check the pins above first.

### Verify with a clean build, never an incremental one

An incremental `pio run` can report SUCCESS while never touching the scons install path, so it proves nothing about the toolchain. Always drop the env directory first:

```bash
rm -rf .pio/build/heltec-v3
pio run -e heltec-v3          # expect SUCCESS, ~66% flash
```

With 6.2.0 + 312-1 a clean `heltec-v3` build ends in SUCCESS (about 8 min on a cold cache). A run that dies in ~30 s at link with the FortranCommon error means the venv is mixed or on `6.2.0.post1`.

**Never let two cores share one checkout.** `.pio/build/` is a single directory for all envs, and `clean_build_dir()` deletes the whole thing whenever `compute_project_checksum()` changes - and that checksum includes the core version string. Two cores therefore wipe each other's artifacts on every alternation, leaving `release/` empty with no build error to point at.
