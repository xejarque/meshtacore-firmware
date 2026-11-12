#!/usr/bin/env bash
# Puts the unified OTA Loader next to the firmware in release/, so the bundle the web builder flashes
# carries it. The *.factory.bin does not include the loader (it lives in ota_1); without it the node
# answers "Device does not have a valid OTA Loader" to every Bluetooth update.
#
# Usage: fetch_ota_loader.sh <arch>      (arch: esp32 | esp32s3 | esp32c3 | esp32c6 | other)
# Only esp32 and esp32s3 have a unified loader; any other arch is a no-op.
set -euo pipefail

ARCH="${1:?arch missing}"
OUTDIR="${OUTDIR:-release}"

case "$ARCH" in
  esp32 | esp32s3) ;;
  *)
    echo "No unified OTA Loader for '$ARCH'; nothing to fetch."
    exit 0
    ;;
esac

FILE="mt-${ARCH}-ota.bin"
URL="https://github.com/meshtastic/esp32-unified-ota/releases/latest/download/${FILE}"

mkdir -p "$OUTDIR"
curl --fail --location --retry 3 --silent --show-error -o "$OUTDIR/$FILE" "$URL"

# A truncated or HTML error body would be flashed as a loader: refuse anything that is not a plausible image.
SIZE="$(wc -c <"$OUTDIR/$FILE" | tr -d " ")"
if [ "$SIZE" -lt 10000 ]; then
  echo "::error::$FILE is only $SIZE bytes; refusing to ship it as the OTA Loader."
  rm -f "$OUTDIR/$FILE"
  exit 1
fi
echo "OTA Loader $FILE ($SIZE bytes) added to $OUTDIR/"
