#!/usr/bin/env bash

##source export-javi.txt
export APP_REPO=nexus-firmware

export PLATFORMIO_BUILD_FLAGS='-DHAS_NEXUS=1 -DNEXUS_ENABLE_ADV -DAPP_REPO=$APP_REPO -DNEXUS_TRUSTED_CHANNEL_1=\"Nexus\" -DNEXUS_TRUSTED_CHANNEL_2=\"Nexus2\"'



# PlatformIO venv: its bundled Python satisfies Core 6.2; Homebrew's `pio` is on 3.15 and fails the version check.
PIO="$HOME/.platformio/penv/bin/pio"
command -v "$PIO" >/dev/null 2>&1 || PIO="pio"

TARGETS_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/nexus-targets.txt"
[[ -f "$TARGETS_FILE" ]] || { echo "Missing $TARGETS_FILE" >&2; exit 1; }
export TARGETS="$(grep -vE '^[[:space:]]*(#|$)' "$TARGETS_FILE" | tr '\n' ' ')"

for a in $TARGETS
do
	echo Bulding $a
	time "$PIO" run -e $a -t clean
	time "$PIO" run -e $a
	date
done
