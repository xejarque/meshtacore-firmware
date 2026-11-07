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


def adopt_unindexed(row, commands, lookup, category):
    """A command the section tables document but the alphabetical index forgot (STATS, TXPOWER...)."""
    names = [n for n in names_in(row[0]) if BARE_COMMAND.match(n)]
    syntax = names_in(row[1]) or [row[1]]
    # Tag, key and sub-command tables also have a bold-free first column; a real command's syntax starts with its name.
    if not names or syntax[0].split()[:1] != [names[0]]:
        return None
    entry = {
        "name": names[0],
        "aliases": names[1:],
        "category": category,
        "summary": "",
        "usage": [],
    }
    commands[names[0]] = entry
    for n in names:
        lookup[n] = names[0]
    return names[0]


SECTION_HEADING = re.compile(r"^## (\d+)\. (.+)$")
BARE_COMMAND = re.compile(r"^[A-Z][A-Z0-9_-]*$")


def attach_details(lines, commands):
    lookup = {
        n: c["name"] for c in commands.values() for n in [c["name"], *c["aliases"]]
    }
    current = None
    category = ""
    for line in lines:
        if line.startswith("## 17."):
            break
        heading = SECTION_HEADING.match(line)
        if heading:
            category = f"{heading.group(1)} {heading.group(2)}"
        if not is_table_row(line):
            current = None
            continue
        c = cells(line)
        if len(c) < 3 or c[0].lower() == "command":
            continue
        if c[0]:
            current = next((lookup[n] for n in names_in(c[0]) if n in lookup), None)
            if current is None:
                current = adopt_unindexed(c, commands, lookup, category)
        if current is None:
            continue
        syntax = names_in(c[1])
        if syntax or c[2]:
            commands[current]["usage"].append(
                {"syntax": syntax[0] if syntax else c[1], "description": c[2]}
            )


def main():
    lines = DOC.read_text(encoding="utf-8").splitlines()
    commands = parse_index(lines)
    attach_details(lines, commands)
    ordered = sorted(commands.values(), key=lambda c: (c["category"], c["name"]))
    payload = {"source": "docs/mtc-commands.md", "commands": ordered}
    OUT.write_text(
        json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8"
    )
    print(f"{len(ordered)} commands -> {OUT.relative_to(ROOT)}", file=sys.stderr)


if __name__ == "__main__":
    main()
