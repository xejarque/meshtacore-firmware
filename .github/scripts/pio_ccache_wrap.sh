#!/usr/bin/env bash
# Put ccache in front of every PlatformIO toolchain compiler (ESP32, nRF52, ...).
#
# usage: pio_ccache_wrap.sh [--restore] [<platformio-packages-dir>]
#
# Why this and not CC="ccache gcc" (the old .github/scripts/pio_ccache.py): ESP-IDF's CMake step runs the compiler
# as a single executable path and its link step passes ccache a response file, so a prefixed CC/CXX breaks every
# ESP32 build. Here the compiler itself is replaced by a tiny `sh` wrapper that calls ccache, so SCons and CMake
# both see a normal executable. Idea and layout taken from lazynoda/meshtastic-builder (docker/ccache_wrap.sh).
#
# Each compiler is copied to <package>/.mb-real-bin/, a sibling of bin/, so gcc still finds cc1/as/ld relative to
# its own path. Every other entry of bin/ is linked there too (the ARM gcc loads libiconv from $ORIGIN, the per-chip
# xtensa drivers exec xtensa-esp-elf-gcc from their own directory). Idempotent: a wrapper is never wrapped twice.
#
# --restore puts the real compilers back, for a machine whose toolchains are shared with normal builds.
#
# Does nothing unless ccache is installed: a missing cache must cost speed, never a build.

set -euo pipefail

MARKER="mb-ccache-wrapper"
restore=0
if [[ "${1:-}" == "--restore" ]]; then
  restore=1
  shift
fi
packages_dir="${1:-${PLATFORMIO_CORE_DIR:-${HOME}/.platformio}/packages}"

[[ -d "${packages_dir}" ]] || exit 0

is_wrapper() {
  head -c 200 "$1" 2>/dev/null | grep -q "${MARKER}"
}

# Package name and version from package.json are stable and change exactly when the toolchain does. The default
# compiler check (mtime) changes whenever a package is unpacked again.
toolchain_id() {
  local pkg_dir="$1" name version
  name="$(basename "${pkg_dir}")"
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("version", ""))' \
    "${pkg_dir}/package.json" 2>/dev/null || true)"
  printf '%s@%s' "${name}" "${version:-unknown}" | tr -c 'A-Za-z0-9._+@-' '_'
}

mirror_bin() {
  local bin_dir="$1" real_dir="$2" entry name
  mkdir -p "${real_dir}"
  for entry in "${bin_dir}"/* "${bin_dir}"/.[!.]*; do
    [[ -e "${entry}" || -L "${entry}" ]] || continue
    name="$(basename "${entry}")"
    [[ -e "${real_dir}/${name}" || -L "${real_dir}/${name}" ]] && continue
    if [[ -L "${entry}" ]]; then
      # Links stay links (xtensa-esp-elf-cc -> xtensa-esp-elf-gcc), resolving to the real compiler inside real_dir.
      cp -P "${entry}" "${real_dir}/${name}"
    else
      ln -s "../bin/${name}" "${real_dir}/${name}"
    fi
  done
}

shopt -s nullglob

if [[ "${restore}" -eq 1 ]]; then
  restored=0
  for real_dir in "${packages_dir}"/toolchain-*/.mb-real-bin; do
    bin_dir="$(dirname "${real_dir}")/bin"
    for compiler in "${bin_dir}"/*-gcc "${bin_dir}"/*-g++ "${bin_dir}"/*-c++ "${bin_dir}"/*-cc; do
      [[ -f "${compiler}" && ! -L "${compiler}" ]] || continue
      is_wrapper "${compiler}" || continue
      name="$(basename "${compiler}")"
      [[ -f "${real_dir}/${name}" && ! -L "${real_dir}/${name}" ]] || continue
      cp -p "${real_dir}/${name}" "${compiler}"
      restored=$((restored + 1))
    done
    rm -rf "${real_dir}"
  done
  echo "ccache: restored ${restored} compiler(s) under ${packages_dir}."
  exit 0
fi

command -v ccache >/dev/null 2>&1 || { echo "ccache not found, building without it"; exit 0; }
ccache_bin="$(command -v ccache)"

wrapped=0
for bin_dir in "${packages_dir}"/toolchain-*/bin; do
  pkg_dir="$(dirname "${bin_dir}")"
  real_dir="${pkg_dir}/.mb-real-bin"
  id="$(toolchain_id "${pkg_dir}")"
  mirrored=0
  for compiler in "${bin_dir}"/*-gcc "${bin_dir}"/*-g++ "${bin_dir}"/*-c++ "${bin_dir}"/*-cc; do
    [[ -e "${compiler}" ]] || continue
    # A symlinked name already reaches the wrapper of its target (head follows the link); a wrapper from an earlier
    # run is skipped too.
    is_wrapper "${compiler}" && continue
    name="$(basename "${compiler}")"
    if [[ "${mirrored}" -eq 0 ]]; then
      mirror_bin "${bin_dir}" "${real_dir}"
      mirrored=1
    fi
    # -L: the real compiler must be a file of its own, never a link back into bin/ (which now holds the wrapper).
    rm -f "${real_dir}/${name}"
    cp -L "${compiler}" "${real_dir}/${name}"
    tmp="$(mktemp "${bin_dir}/.${name}.XXXXXX")"
    cat >"${tmp}" <<EOF
#!/bin/sh
# ${MARKER} (${id}), written by .github/scripts/pio_ccache_wrap.sh
# A compiler that calls another one (the per-chip xtensa drivers call xtensa-esp-elf-gcc) must reach it without a
# second trip through ccache.
if [ -n "\${MB_CCACHE_ACTIVE:-}" ]; then
  exec "${real_dir}/${name}" "\$@"
fi
MB_CCACHE_ACTIVE=1 CCACHE_COMPILERCHECK='string:${id}' exec "${ccache_bin}" "${real_dir}/${name}" "\$@"
EOF
    chmod 755 "${tmp}"
    mv -f "${tmp}" "${compiler}"
    wrapped=$((wrapped + 1))
  done
done

echo "ccache: wrapped ${wrapped} compiler(s) under ${packages_dir}."
