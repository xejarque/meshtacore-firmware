#!/usr/bin/env python3

"""Emit the shared Nexus target list (nexus-targets.txt) as a CI matrix.

nexus-targets.txt is the single source of truth for every builder. MeshCore is
enabled by default in src/configuration.h, so an env opts out explicitly with
-DMESHTASTIC_ENABLE_MESHCORE_*=0 in its own platformio.ini.
"""

import argparse
import json
import sys
from pathlib import Path

from platformio.project.config import ProjectConfig

TARGETS_FILE = Path(__file__).resolve().parent.parent / "nexus-targets.txt"


def read_targets(path):
  targets = []
  for line in path.read_text(encoding="utf-8").splitlines():
    target = line.split("#", 1)[0].strip()
    if target:
      targets.append(target)
  return targets


parser = argparse.ArgumentParser(description="Emit the shared Nexus target list")
parser.add_argument(
  "--space",
  action="store_true",
  help="Print a space-separated list instead of a JSON matrix",
)
args = parser.parse_args()

try:
  targets = read_targets(TARGETS_FILE)
except OSError as exc:
  print(f"error: cannot read {TARGETS_FILE}: {exc}", file=sys.stderr)
  sys.exit(1)

# Fail closed: an empty matrix would build nothing and still publish a release.
if not targets:
  print(f"error: no targets listed in {TARGETS_FILE}", file=sys.stderr)
  sys.exit(1)

known_envs = set(ProjectConfig.get_instance().envs())
missing = [name for name in targets if name not in known_envs]
if missing:
  print(f"error: targets without an [env:] stanza: {missing}", file=sys.stderr)
  sys.exit(1)

if args.space:
  print(" ".join(targets))
else:
  print(json.dumps([{"board": target} for target in targets]))
