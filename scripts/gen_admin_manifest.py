#!/usr/bin/env python3
"""Generate custom_builder_web/admin.commands.json from docs/mtc-commands.md.

Source of truth is the command reference: section 17 (alphabetical index) gives
name, aliases, category and one-line summary; the per-section tables give the
syntax variants and long descriptions, matched by the command's first token.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "mtc-commands.md"
OUT = ROOT / "custom_builder_web" / "admin.commands.json"

CODE = re.compile(r"`([^`]+)`")
SEPARATOR_ROW = re.compile(r"^\|[\s:|-]+\|?$")


def cells(line):
    parts = re.split(r"(?<!\\)\|", line.strip().strip("|"))
    return [c.strip().replace("\\|", "|") for c in parts]


def is_table_row(line):
    return line.lstrip().startswith("|") and not SEPARATOR_ROW.match(line.strip())


def names_in(cell):
    return [n.strip() for n in CODE.findall(cell)]


def parse_index(lines):
    commands = {}
    in_index = False
    for line in lines:
        if line.startswith("## 17."):
            in_index = True
            continue
        if not in_index or not is_table_row(line):
            continue
        c = cells(line)
        if len(c) < 3 or c[0].lower() == "command":
            continue
        names = names_in(c[0])
        if not names:
            continue
        commands[names[0]] = {
            "name": names[0],
            "aliases": names[1:],
            "category": c[1].replace("§", "").strip(),
            "summary": c[2],
            "usage": [],
        }
    return commands


def attach_details(lines, commands):
    lookup = {n: c["name"] for c in commands.values() for n in [c["name"], *c["aliases"]]}
    current = None
    for line in lines:
        if line.startswith("## 17."):
            break
        if not is_table_row(line):
            current = None
            continue
        c = cells(line)
        if len(c) < 3 or c[0].lower() == "command":
            continue
        if c[0]:
            current = next((lookup[n] for n in names_in(c[0]) if n in lookup), None)
        if current is None:
            continue
        syntax = names_in(c[1])
        if syntax or c[2]:
            commands[current]["usage"].append(
                {"syntax": syntax[0] if syntax else c[1], "description": c[2]}
            )


PRESETS_OUT = ROOT / "custom_builder_web" / "admin.presets.json"


def parse_presets(lines):
    """Rows of the 'PRESET settings reference' table: group | codes | long name | SF | BW | CR | freq | region.

    The command sent to the node is the LAST code of the row: `LF` / `LF868` -> LF868, because the bare
    `LF` is ambiguous between the Sub-GHz and the 2.4 GHz preset.
    """
    start = next(i for i, l in enumerate(lines) if l.startswith("**PRESET settings reference**"))
    presets, group = [], ""
    for line in lines[start:]:
        if line.startswith("_`PRESET` writes"):
            break
        if not is_table_row(line):
            continue
        c = cells(line)
        if len(c) != 8 or not c[3].isdigit():
            continue
        group = c[0].replace("*", "").strip() or group
        codes = names_in(c[1])
        presets.append(
            {
                "group": group,
                "command": codes[-1],
                "codes": codes,
                "longName": c[2].replace("`", "").strip(),
                "sf": int(c[3]),
                "bw": float(c[4]),
                "cr": int(c[5].split("/")[1]),
                "freq": float(c[6]),
                "region": c[7].replace("`", "").strip(),
            }
        )
    return presets


def main():
    lines = DOC.read_text(encoding="utf-8").splitlines()
    commands = parse_index(lines)
    attach_details(lines, commands)
    ordered = sorted(commands.values(), key=lambda c: (c["category"], c["name"]))
    payload = {"source": "docs/mtc-commands.md", "commands": ordered}
    OUT.write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{len(ordered)} commands -> {OUT.relative_to(ROOT)}", file=sys.stderr)
    presets = parse_presets(lines)
    PRESETS_OUT.write_text(json.dumps({"source": "docs/mtc-commands.md", "presets": presets}, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{len(presets)} presets -> {PRESETS_OUT.relative_to(ROOT)}", file=sys.stderr)


if __name__ == "__main__":
    main()
