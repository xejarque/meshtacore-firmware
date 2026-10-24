#!/usr/bin/env python3

"""Discover the firmware envs shipped by the 'community' builder.

An env belongs to the community set when its effective build_flags enable the
MeshCore feature switch (MESHTASTIC_ENABLE_MESHCORE, matched as a prefix so the
_SNIFFER/_RELAY/_COMPANION/_HYBRID_SYNC variants all match). Flags inherited
through 'extends' count, which a grep over variants/ would miss. A define set to
0 is a deliberate opt-out and does not qualify - that is how the native test
envs spell it.
"""

import argparse
import json
import sys

from platformio.project.config import ProjectConfig

MARKER = "MESHTASTIC_ENABLE_MESHCORE"

# Public builds we ship that are not MeshCore envs.
EXTRA_ENVS = ("gat562_mesh_trial_tracker",)

# Deliberate opt-outs. Add an env here (with a reason) instead of hiding it in
# the workflow, so discovery stays fail-closed.
EXCLUDE_ENVS = (
  # Throwaway diagnostic builds (MODEM/serial tracing, RX bench harness). They
  # exist to debug one board on the bench, not to ship to operators.
  "thinknode_m1_meshcore_debug",
  "thinknode_m1_meshcore_rxtest",
)


def enables_meshcore(flag):
  """True when any -D define of the marker inside the flag is not set to 0.

  One build_flags entry may hold several defines ('-DA=1 -DB=0'), so the
  entry has to be split before the value can be read.
  """
  for token in str(flag).split():
    if not token.startswith(f"-D{MARKER}"):
      continue
    _, _, value = token.partition("=")
    if value != "0":
      return True
  return False


parser = argparse.ArgumentParser(description="Discover the community targets")
parser.add_argument(
  "--space",
  action="store_true",
  help="Print a space-separated list instead of a JSON matrix",
)
args = parser.parse_args()

cfg = ProjectConfig.get_instance()
known_envs = cfg.envs()

targets = sorted(
  name
  for name in known_envs
  if any(enables_meshcore(flag) for flag in cfg.get(f"env:{name}", "build_flags"))
)
targets = sorted((set(targets) | set(EXTRA_ENVS)) - set(EXCLUDE_ENVS))

# Fail closed: an empty matrix would build nothing and still publish a release.
if not targets:
  print(
    f"error: no community targets found (no env enables {MARKER},"
    " or platformio is not importable)",
    file=sys.stderr,
  )
  sys.exit(1)

missing = [name for name in targets if name not in known_envs]
if missing:
  print(f"error: targets without an [env:] stanza: {missing}", file=sys.stderr)
  sys.exit(1)

if args.space:
  print(" ".join(targets))
else:
  print(json.dumps([{"board": name} for name in targets]))
