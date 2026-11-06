# Claude Code instructions

> **TL;DR**
>
> |                |                                                                                                                                         |
> | -------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
> | Local tests    | `./bin/run-tests.sh` (exit 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED · 4 BUSY · 5 ABORTED · 6 UNSUPPORTED); `--status` first               |
> | Hardware tests | [meshtastic/meshtastic-mcp](https://github.com/meshtastic/meshtastic-mcp) (`MESHTASTIC_FIRMWARE_ROOT` → this checkout)                  |
> | Format         | `trunk fmt`                                                                                                                             |
> | ESP32 builds   | one `~/.platformio/penv` on `pioarduino==6.1.19`; ESP platform stays 55.03.311 - see [Build toolchain](#build-toolchain-pio-and-python) |
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

## Build toolchain (pio and Python)

ESP32 and nRF52 both build from the **single shared venv at `~/.platformio/penv`**, and that venv must be pinned to **`pioarduino==6.1.19`**. That version is not arbitrary: it is what every `nexus_*.yml` workflow installs, and it is the last core that survives ESP-IDF linking (see the FortranCommon trap below). Matching CI locally is the point - a locally "working" toolchain that CI does not use hides regressions instead of catching them.

```bash
rm -rf ~/.platformio/penv
python3.12 -m venv ~/.platformio/penv
~/.platformio/penv/bin/pip install -U "pioarduino==6.1.19"
```

Exactly one distribution must be listed, and it must be `6.1.19`. Two lines means the venv is mixed and broken:

```bash
~/.platformio/penv/bin/pio --version && ~/.platformio/penv/bin/pip list | grep -Ei '^(platformio|pioarduino) '
```

### The core and the ESP platform are a matched pair

`variants/esp32/esp32-common.ini` pins platform **`55.03.311`** on purpose. Do not bump it to `55.03.312+` while the core stays at 6.1.19:

| Platform                                | `engines`  | Works with 6.1.19                           |
| --------------------------------------- | ---------- | ------------------------------------------- |
| `55.03.311` (meshtastic fork)           | `>=6.1.19` | yes                                         |
| `55.03.312`, `55.03.312-1` (pioarduino) | `>=6.2.0`  | **no** - aborts with `IncompatiblePlatform` |

Either both move or neither does. Upstream's 2.8.2 merge carries `e09cec6ad` ("Update pioarduino to v3.3.12-1"), the commit that bumped this to 312 - it was reverted here deliberately.

### FortranCommon: why the core is pinned at all

PlatformIO 6.2.0 **wipes `tool-scons` during ESP-IDF builds** and the link then dies with `ModuleNotFoundError: No module named 'SCons.Tool.FortranCommon'`. It reinstalls three times and ends up deleting the package, so patching `FortranCommon.py` does not help - the whole package vanishes. Do not reach for `pio pkg install`, a hand-written shim, or `pip install platformio` to "fix" this; the fix is the 6.1.19 pin. Related trap: the pioarduino `scons-4.11.1.zip` registry artifact is only a ~12 KB descriptor and the real `scons-local` payload is fetched separately, so hand-installing that URL yields an empty package.

### Verify with a clean build, never an incremental one

An incremental `pio run` can report SUCCESS while never touching the scons install path, so it proves nothing about the toolchain. Always drop the env directory first:

```bash
rm -rf .pio/build/heltec-v3
pio run -e heltec-v3          # expect SUCCESS, ~66% flash
```

With 6.1.19 + 311 a clean `heltec-v3` build finishes in ~220 s at 65.9% flash. A run that dies in ~30 s at the link step with the FortranCommon error means the core was upgraded to 6.2.x.

**Never let two cores share one checkout.** `.pio/build/` is a single directory for all envs, and `clean_build_dir()` deletes the whole thing whenever `compute_project_checksum()` changes - and that checksum includes the core version string. Two cores therefore wipe each other's artifacts on every alternation, leaving `release/` empty with no build error to point at.
