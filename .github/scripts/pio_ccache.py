# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
"""Opt-in ccache wrapper for PlatformIO builds (CI and local).

Enable it without touching platformio.ini:
    PLATFORMIO_EXTRA_SCRIPTS=post:.github/scripts/pio_ccache.py pio run -e <env>

The userPrefs defines (-D...) are only appended to the project env (src/), so framework and
libraries are identical across custom builds and come out of the cache. Pair it with
CCACHE_NODIRECT=1 (hash the preprocessed output, not the -D list) so unchanged src/ files hit too.
"""

import shutil

Import("env", "projenv")

ccache = shutil.which("ccache")
if env.get("PIOPLATFORM") == "espressif32":
    # Prefixing CC/CXX with ccache breaks ESP-IDF: its CMake step runs the compiler as one executable path, and the
    # link step hands ccache a response file ("ccache: error: execute_noreturn of @...tmp failed"). ESP32 builds
    # have to run without it.
    print("ccache skipped: not compatible with the ESP-IDF build")
elif ccache:
    # projenv (src/, with the userPrefs defines) is a clone made before this script runs.
    for target in (env, projenv):
        for var in ("CC", "CXX"):
            target.Replace(**{var: f"{ccache} {target.subst('$' + var)}"})
    print("ccache enabled:", ccache)
else:
    print("ccache not found, building without it")
