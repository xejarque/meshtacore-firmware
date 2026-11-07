"""Run: python3 -m unittest discover -s .github/scripts -p 'test_*.py'"""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_custom_userprefs as gen  # noqa: E402

MANIFEST = json.loads(gen.DEFAULT_MANIFEST.read_text(encoding="utf-8"))
OPTS = {o["id"]: o for o in MANIFEST["options"]}


class ManifestValidation(unittest.TestCase):
    def test_valid_values_are_kept(self):
        prefs = {
            "USERPREFS_NEXUS_PPING_ENABLED": "true",
            "USERPREFS_NEXUS_DROP_MASK": "0x1000000000001",
            "USERPREFS_MESHCORE_FLOOD_MAX": "10",
            "USERPREFS_NEXUS_PPING_CHANNELS": "5",
            "NEXUS_MASTER_KEY_2": "{ 0x01 }",
        }
        before = dict(prefs)
        self.assertEqual(gen.drop_invalid_manifest_options(prefs, gen.DEFAULT_MANIFEST), [])
        self.assertEqual(prefs, before)

    def test_invalid_values_are_dropped_and_reported(self):
        prefs = {
            "USERPREFS_NEXUS_PPING_ENABLED": "yes",
            "USERPREFS_MESHCORE_FLOOD_MAX": "65",
            "USERPREFS_NEXUS_PREAMBLE": "5",
            "USERPREFS_NEXUS_ZERO_HOP_CONFIG": "0x100",
            "USERPREFS_NEXUS_PPING_EMOJI_MODE": "7",
            "NEXUS_MASTER_KEY_3": "abc",
            "USERPREFS_CONFIG_LORA_REGION": "whatever",  # not a manifest key: untouched
        }
        dropped = gen.drop_invalid_manifest_options(prefs, gen.DEFAULT_MANIFEST)
        self.assertEqual(len(dropped), 6)
        self.assertEqual(prefs, {"USERPREFS_CONFIG_LORA_REGION": "whatever"})

    def test_json_booleans_become_c_literals(self):
        prefs = {"USERPREFS_NEXUS_PPING_ENABLED": True, "USERPREFS_NEXUS_PPING_FLAGS": "False"}
        self.assertEqual(gen.drop_invalid_manifest_options(prefs, gen.DEFAULT_MANIFEST), [])
        self.assertEqual(prefs, {"USERPREFS_NEXUS_PPING_ENABLED": "true", "USERPREFS_NEXUS_PPING_FLAGS": "false"})

    def test_every_nexus_manifest_key_is_read_by_the_firmware(self):
        # The key list lives in the manifest, the JS emitter and NexusUserPrefs.cpp: a key the firmware never reads would be
        # accepted by the builder and silently do nothing.
        modules = Path(__file__).resolve().parents[2] / "src" / "modules"
        source = "".join((modules / name).read_text(encoding="utf-8") for name in ("NexusUserPrefs.cpp", "NexusModule.h"))
        missing = [o["key"] for o in MANIFEST["options"] if o["key"].startswith("USERPREFS_NEXUS_") and o["key"] not in source]
        self.assertEqual(missing, [])

    def test_missing_manifest_is_not_an_error(self):
        prefs = {"USERPREFS_MESHCORE_FLOOD_MAX": "999"}
        self.assertEqual(gen.drop_invalid_manifest_options(prefs, Path("/nonexistent.json")), [])

    def test_every_option_default_is_valid(self):
        for opt in MANIFEST["options"]:
            default = opt.get("default")
            if default in (None, ""):
                continue
            if opt["type"] == "bool":
                value = str(default).lower()
            elif opt["type"] == "enum":  # the firmware gets the emitted text, not the form value
                choice = next(v for v in opt["values"] if v["value"] == str(default))
                value = opt.get("prefix", "") + choice.get("emit", choice["value"])
            else:
                value = str(default)
            self.assertIsNone(gen.option_value_error(opt, value), opt["id"])


class ChannelKeys(unittest.TestCase):
    def test_accepted_forms(self):
        self.assertEqual(gen.format_psk("AQ=="), "{ 0x01 }")
        self.assertEqual(gen.format_psk("2"), "{ 0x02 }")
        self.assertEqual(gen.format_psk(""), "{ 0x01 }")
        self.assertEqual(gen.format_psk("0x" + "ab" * 16), "{ " + ", ".join(["0xab"] * 16) + " }")

    def test_hex_without_prefix_is_hex_not_base64(self):
        # 32 hex digits are also valid base64 and would decode to 24 junk bytes.
        self.assertEqual(gen.format_psk("00" * 16), "{ " + ", ".join(["0x00"] * 16) + " }")
        self.assertEqual(len(gen.format_psk("ab" * 32).split(",")), 32)

    def test_unusable_keys_fail_instead_of_falling_back_to_the_public_key(self):
        for bad in ("not base64!", "AAAA", "300", "0xzz", "{ 0x01; rm }"):
            with self.assertRaises(ValueError, msg=bad):
                gen.format_psk(bad)


class CustomPrefsSanitizing(unittest.TestCase):
    def test_unknown_keys_and_non_array_braces_are_dropped(self):
        extra = {
            "USERPREFS_CONFIG_DEVICE_ROLE": "meshtastic_Config_DeviceConfig_Role_ROUTER",
            "NEXUS_MASTER_KEY_1": "{ 0x01, 0x02 }",
            "USERPREFS_CHANNEL_0_PSK": "{ 0x01 } -include /etc/passwd",
            "FOO -fplugin=x": "1",
            "CFLAGS": "-O0",
        }
        dropped = gen.sanitize_custom_prefs(extra)
        self.assertEqual(sorted(extra), ["NEXUS_MASTER_KEY_1", "USERPREFS_CONFIG_DEVICE_ROLE"])
        self.assertEqual(len(dropped), 3)


if __name__ == "__main__":
    unittest.main()
