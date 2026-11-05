#!/usr/bin/env python3
"""Parse issue body for Meshtacore custom firmware build requests."""

import json
import os
import re
import sys
from pathlib import Path

from build_crypto import decrypt_request, mask_secrets


def load_valid_targets() -> set:
    targets = set()
    nexus_file = Path("nexus-targets.txt")
    if nexus_file.exists():
        with open(nexus_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    targets.add(line)
    return targets


def main():
    body = ""
    if len(sys.argv) >= 2:
        issue_file = Path(sys.argv[1])
        if issue_file.exists():
            body = issue_file.read_text(encoding="utf-8")
    if not body:
        body = os.getenv("ISSUE_BODY", "")

    if not body:
        print(
            "Error: No issue body provided via file or ISSUE_BODY environment variable",
            file=sys.stderr,
        )
        sys.exit(1)

    # The request is encrypted in the browser with the repo public key: issues are public.
    match = re.search(
        r"```meshtacore-build-config-enc\s*([A-Za-z0-9_\-=\s]+?)\s*```",
        body,
        re.DOTALL,
    )

    github_output = os.getenv("GITHUB_OUTPUT")

    def write_output(key: str, val: str):
        if github_output:
            with open(github_output, "a", encoding="utf-8") as out:
                out.write(f"{key}={val}\n")
        print(f"[OUTPUT] {key}={val}")

    if not match:
        write_output("valid", "false")
        write_output(
            "error_message",
            "No se encontró el bloque cifrado meshtacore-build-config-enc. "
            "Genera la solicitud desde el Web Builder (las solicitudes en texto plano ya no se aceptan).",
        )
        return

    try:
        request = decrypt_request(match.group(1), os.environ["BUILD_PRIVATE_KEY"])
        config = request["config"]
        Path("user_public_key.b64").write_text(
            request["user_public_key"], encoding="utf-8"
        )
    except Exception as e:
        write_output("valid", "false")
        write_output(
            "error_message",
            f"No se pudo descifrar la solicitud ({type(e).__name__}). Vuelve a generarla desde el Web Builder.",
        )
        return

    target = config.get("target", "").strip()
    custom_target = config.get("custom_target", "").strip()
    if target == "custom" and custom_target:
        target = custom_target

    if not target:
        write_output("valid", "false")
        write_output(
            "error_message", "No se especificó ninguna placa objetivo (target)."
        )
        return

    # Check if target is known or valid
    valid_targets = load_valid_targets()
    if valid_targets and target not in valid_targets:
        # Check if it exists in platformio.ini or variants
        pio_ini = Path("platformio.ini")
        if pio_ini.exists() and f"env:{target}" not in pio_ini.read_text(
            encoding="utf-8"
        ):
            write_output("valid", "false")
            write_output(
                "error_message",
                f"La placa '{target}' no es un entorno válido reconocido en Meshtacore.",
            )
            return

    long_name = str(config.get("owner_long_name", "Meshtacore Node"))[:24]
    short_name = str(config.get("owner_short_name", "MTC"))[:4]

    # Extract userprefs dictionary
    custom_prefs = config.get("custom_userprefs")
    if custom_prefs is None and "custom_userprefs_json" in config:
        try:
            custom_prefs = json.loads(config["custom_userprefs_json"])
        except Exception:
            custom_prefs = {}

    if not isinstance(custom_prefs, dict):
        custom_prefs = {}

    mask_secrets(custom_prefs)

    # Save to file for generate_custom_userprefs.py
    prefs_file = Path("requested_userprefs.json")
    with open(prefs_file, "w", encoding="utf-8") as f:
        json.dump(custom_prefs, f, indent=2)

    extra_flags_raw = str(config.get("extra_build_flags", "")).strip()
    clean_flags = []
    for token in extra_flags_raw.split():
        if re.match(r"^-[DU][A-Za-z0-9_]+(=[A-Za-z0-9_\\\".-]+)?$", token):
            clean_flags.append(token)

    write_output("valid", "true")
    write_output("target", target)
    write_output("owner_long_name", long_name)
    write_output("owner_short_name", short_name)
    write_output("extra_build_flags", " ".join(clean_flags))
    write_output("prefs_file", str(prefs_file.resolve()))


if __name__ == "__main__":
    main()
