#!/usr/bin/env bash
# Copies the firmware of the newest published release into custom_builder_web/firmware/ so GitHub Pages
# serves it from the same origin as the builder. The browser cannot download release assets itself:
# the final redirect (release-assets.githubusercontent.com) sends no CORS headers.
#
# Only what an update needs is kept: the plain ESP32 .bin (app image) and the nRF52 .zip (DFU package).
# .factory.bin and .uf2 are left out to keep the Pages site small.
#
# Usage: GH_TOKEN=... bin/pages-firmware.sh [output-dir]
set -euo pipefail

OUT="${1:-custom_builder_web/firmware}"

TAG="$(gh release list --exclude-drafts --limit 1 --json tagName --jq '.[0].tagName')"
if [ -z "$TAG" ]; then
	echo "No published release found: leaving the firmware folder empty" >&2
	mkdir -p "$OUT"
	printf '{"tag":null,"files":[]}\n' >"$OUT/index.json"
	exit 0
fi

rm -rf "$OUT"
mkdir -p "$OUT"
gh release download "$TAG" --dir "$OUT" --pattern 'firmware-*.bin' --pattern 'firmware-*.zip'
rm -f "$OUT"/*.factory.bin

# One entry per file; the page derives board and version from the name.
FILES="$(
	cd "$OUT"
	for f in firmware-*; do
		printf '%s\t%s\n' "$f" "$(wc -c <"$f" | tr -d ' ')"
	done | jq -R -s 'split("\n") | map(select(length > 0) | split("\t") | {name: .[0], size: (.[1] | tonumber)})'
)"

jq -n --arg tag "$TAG" --argjson files "$FILES" '{tag: $tag, files: $files}' >"$OUT/index.json"
echo "Published $TAG: $(jq '.files | length' "$OUT/index.json") files, $(du -sh "$OUT" | cut -f1)"
