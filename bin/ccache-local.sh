#!/usr/bin/env bash
# ccache for local PlatformIO builds (ESP32 and nRF52), shared by every worktree of this repo.
#
#   bin/ccache-local.sh on       wrap the toolchain compilers with ccache (idempotent; rerun after PlatformIO
#                                installs or updates a toolchain)
#   bin/ccache-local.sh off      put the real compilers back
#   bin/ccache-local.sh stats    hits and misses of the shared cache
#   bin/ccache-local.sh clear    empty the cache
#   eval "$(bin/ccache-local.sh env)"    export CCACHE_DIR / CCACHE_BASEDIR / CCACHE_MAXSIZE in this shell
#
# The wrapper lives in ~/.platformio/packages, which every PlatformIO project on this machine shares, so `off`
# before uninstalling ccache (the wrappers call it by absolute path). Details: .github/scripts/pio_ccache_wrap.sh.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
wrap="${here}/../.github/scripts/pio_ccache_wrap.sh"

cache_dir="${MB_CCACHE_DIR:-${HOME}/.cache/meshtastic-ccache}"
max_size="${MB_CCACHE_MAXSIZE:-10G}"

# Worktrees live under <main checkout>/.claude/worktrees/<name>, so the main checkout is a common base: ccache
# rewrites absolute paths below it to relative ones and every worktree hits the same entries.
common_git_dir="$(cd "${here}/.." && git rev-parse --path-format=absolute --git-common-dir)"
base_dir="$(dirname "${common_git_dir}")"

case "${1:-}" in
  on)
    command -v ccache >/dev/null || { echo "ccache is not installed (brew install ccache)" >&2; exit 1; }
    bash "${wrap}"
    echo
    echo 'Now export the variables in the shell that runs pio:'
    echo '  eval "$('"${BASH_SOURCE[0]}"' env)"'
    ;;
  off)
    bash "${wrap}" --restore
    ;;
  env)
    echo "export CCACHE_DIR='${cache_dir}'"
    echo "export CCACHE_BASEDIR='${base_dir}'"
    echo "export CCACHE_MAXSIZE='${max_size}'"
    ;;
  stats)
    CCACHE_DIR="${cache_dir}" ccache -s
    ;;
  clear)
    CCACHE_DIR="${cache_dir}" ccache -C
    ;;
  *)
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
