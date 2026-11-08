#!/usr/bin/env bash
# Borra por privacidad el rastro de las compilaciones del builder con más de MAX_AGE_HOURS horas:
#   1. issues de build (se ELIMINAN, no solo se cierran: llevan la configuración y los nombres de nodo)
#   2. artifacts de Actions "meshtacore-*" y "meshtastic-custom-*"
#   3. bundles cifrados de la rama de datos (la rama se reescribe sin historial)
#   4. assets de la release "custom-builds" (solo sus assets; la release sigue)
# Las demás releases (las públicas) NO se tocan.
#
# Uso: GH_TOKEN=<PAT admin> .github/scripts/cleanup_builds.sh [--apply]
# Sin --apply solo lista lo que borraría (dry-run).
set -euo pipefail

REPO="${REPO:-${GITHUB_REPOSITORY:-xejarque/meshtacore-firmware}}"
MAX_AGE_HOURS="${MAX_AGE_HOURS:-168}"
DATA_BRANCH="${DATA_BRANCH:-custom-builds-data}"
APPLY=false
[ "${1:-}" = "--apply" ] && APPLY=true

now_epoch=$(date -u +%s)
cutoff_epoch=$((now_epoch - MAX_AGE_HOURS * 3600))

is_old() { # $1 = fecha ISO-8601 UTC (2026-11-08T10:00:00Z)
  local ts
  ts=$(date -u -d "$1" +%s 2>/dev/null || date -u -j -f "%Y-%m-%dT%H:%M:%SZ" "$1" +%s)
  [ "$ts" -lt "$cutoff_epoch" ]
}

echo "Repo: $REPO · umbral: ${MAX_AGE_HOURS} h · modo: $($APPLY && echo APPLY || echo DRY-RUN)"

# 1. Issues de build
echo "== Issues de build"
gh issue list --repo "$REPO" --state all --limit 1000 \
  --json number,title,body,createdAt,id \
  --jq '.[] | select((.title|contains("[Build Request]")) or (.body|contains("meshtacore-build-config"))) | [.number,.createdAt,.id] | @tsv' |
  while IFS=$'\t' read -r num created node_id; do
    is_old "$created" || continue
    echo "issue #$num ($created)"
    if $APPLY; then
      gh api graphql -f query='mutation($id:ID!){deleteIssue(input:{issueId:$id}){clientMutationId}}' -f id="$node_id" >/dev/null
    fi
  done

# 2. Artifacts de Actions
echo "== Artifacts"
gh api --paginate "repos/$REPO/actions/artifacts?per_page=100" \
  --jq '.artifacts[] | select(.name|test("^(meshtacore|meshtastic-custom)-")) | [.id,.name,.created_at] | @tsv' |
  while IFS=$'\t' read -r id name created; do
    is_old "$created" || continue
    echo "artifact $name ($created)"
    if $APPLY; then gh api -X DELETE "repos/$REPO/actions/artifacts/$id" >/dev/null; fi
  done

# 4. Assets de la release custom-builds (nunca otras releases)
echo "== Release custom-builds (solo assets)"
if gh api "repos/$REPO/releases/tags/custom-builds" >/dev/null 2>&1; then
  gh api --paginate "repos/$REPO/releases/tags/custom-builds" \
    --jq '.assets[] | select(.name|test("^(meshtacore|meshtastic-custom)-")) | [.id,.name,.created_at] | @tsv' |
    while IFS=$'\t' read -r id name created; do
      is_old "$created" || continue
      echo "asset $name ($created)"
      if $APPLY; then gh api -X DELETE "repos/$REPO/releases/assets/$id" >/dev/null; fi
    done
else
  echo "(la release no existe)"
fi

# 3. Bundles cifrados de la rama de datos
echo "== Rama $DATA_BRANCH"
REMOTE="https://x-access-token:${GH_TOKEN}@github.com/${REPO}.git"
if git ls-remote --exit-code --heads "$REMOTE" "$DATA_BRANCH" >/dev/null 2>&1; then
  WORK="$(mktemp -d)"
  trap 'rm -rf "$WORK"' EXIT
  git clone -q --depth 1 --branch "$DATA_BRANCH" "$REMOTE" "$WORK"
  removed=0
  for f in "$WORK"/*.zip.enc; do
    [ -e "$f" ] || continue
    stamp=$(basename "$f" | sed -E 's/.*-([0-9]{8})_([0-9]{6})\.zip\.enc$/\1 \2/')
    iso=$(echo "$stamp" | sed -E 's/^([0-9]{4})([0-9]{2})([0-9]{2}) ([0-9]{2})([0-9]{2})([0-9]{2})$/\1-\2-\3T\4:\5:\6Z/')
    is_old "$iso" || continue
    echo "bundle $(basename "$f") ($iso)"
    rm -f "$f"
    removed=$((removed + 1))
  done
  # Aunque no sobre nada, se aprovecha para descartar el historial con bundles ya borrados.
  if $APPLY && [ "$removed" -gt 0 ]; then
    cd "$WORK"
    git checkout -q --orphan clean
    git add -A
    git -c user.name="github-actions[bot]" -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
      commit -q --allow-empty -m "Prune builds older than ${MAX_AGE_HOURS}h"
    git push -q --force-with-lease="$DATA_BRANCH" "$REMOTE" "clean:$DATA_BRANCH"
  fi
else
  echo "(la rama no existe)"
fi
