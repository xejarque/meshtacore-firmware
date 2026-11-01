#!/usr/bin/env python3
"""Generate a tailored userPrefs.jsonc for custom firmware builds."""

import argparse
import base64
import json
import os
import re
import sys
from pathlib import Path


def load_userprefs(path: Path) -> dict:
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        clean = re.sub(r"//.*", "", f.read(), flags=re.MULTILINE)
        return json.loads(clean)


def b64_to_c_array(b64_str: str) -> str:
    raw = base64.b64decode(b64_str.strip())
    return "{ " + ", ".join(f"0x{b:02x}" for b in raw) + " }"


def main():
    parser = argparse.ArgumentParser(
        description="Customize userPrefs.jsonc for firmware build"
    )
    parser.add_argument(
        "--file", default="userPrefs.jsonc", help="Path to userPrefs.jsonc"
    )
    parser.add_argument("--owner-long-name", default="Meshtacore Node")
    parser.add_argument("--owner-short-name", default="MTC")
    parser.add_argument(
        "--has-screen", type=lambda v: v.lower() in ("true", "1", "yes"), default=True
    )
    parser.add_argument(
        "--enable-bluetooth",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument("--device-role", default="CLIENT")
    parser.add_argument("--lora-region", default="EU_868")
    parser.add_argument("--lora-preset-mode", default="custom_sfnarrow_default")
    parser.add_argument("--channels-preset", default="repo_default_5_channels")
    parser.add_argument(
        "--wifi-enabled",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=False,
    )
    parser.add_argument("--wifi-ssid", default="")
    parser.add_argument("--wifi-password", default="")
    parser.add_argument("--gps-mode", default="ENABLED")
    parser.add_argument("--fixed-latitude", default="")
    parser.add_argument("--fixed-longitude", default="")
    parser.add_argument("--fixed-altitude", default="")
    parser.add_argument("--admin-key", default="")
    parser.add_argument("--output", default="userPrefs.jsonc")

    args = parser.parse_args()
    prefs = load_userprefs(Path(args.file))

    # Owner identity (UTF-8 byte limits: long <= 24, short <= 4)
    if args.owner_long_name:
        prefs["USERPREFS_CONFIG_OWNER_LONG_NAME"] = args.owner_long_name[:24]
    if args.owner_short_name:
        prefs["USERPREFS_CONFIG_OWNER_SHORT_NAME"] = args.owner_short_name[:4]

    # Screen optimization
    if not args.has_screen:
        prefs["MESHTASTIC_EXCLUDE_SCREEN"] = "1"
        prefs["HAS_SCREEN"] = "0"
        prefs["USERPREFS_CONFIG_DISPLAY_SCREEN_ON_SECS"] = "1"
    else:
        prefs.pop("MESHTASTIC_EXCLUDE_SCREEN", None)
        prefs.pop("HAS_SCREEN", None)

    # Bluetooth optimization
    if not args.enable_bluetooth:
        prefs["MESHTASTIC_EXCLUDE_BLUETOOTH"] = "1"
        prefs["HAS_BLUETOOTH"] = "0"
    else:
        prefs.pop("MESHTASTIC_EXCLUDE_BLUETOOTH", None)
        prefs.pop("HAS_BLUETOOTH", None)

    # Device role
    role_enum = f"meshtastic_Config_DeviceConfig_Role_{args.device_role.upper()}"
    prefs["USERPREFS_CONFIG_DEVICE_ROLE"] = role_enum

    # LoRa Region
    region_enum = f"meshtastic_Config_LoRaConfig_RegionCode_{args.lora_region.upper()}"
    prefs["USERPREFS_CONFIG_LORA_REGION"] = region_enum

    # LoRa Preset / Custom SFNarrow
    if args.lora_preset_mode == "custom_sfnarrow_default":
        prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "false"
        prefs["USERPREFS_LORACONFIG_BANDWIDTH"] = "62"
        prefs["USERPREFS_LORACONFIG_SPREAD_FACTOR"] = "7"
        prefs["USERPREFS_LORACONFIG_CODING_RATE"] = "6"
        prefs["USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY"] = "869.618"
        prefs["USERPREFS_LORACONFIG_TX_POWER"] = "22"
        prefs["USERPREFS_CONFIG_LORA_HOP_LIMIT"] = "7"
        prefs.pop("USERPREFS_LORACONFIG_MODEM_PRESET", None)
    else:
        preset_clean = args.lora_preset_mode.replace("modem_preset_", "").upper()
        prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "true"
        prefs["USERPREFS_LORACONFIG_MODEM_PRESET"] = (
            f"meshtastic_Config_LoRaConfig_ModemPreset_{preset_clean}"
        )
        prefs.pop("USERPREFS_LORACONFIG_BANDWIDTH", None)
        prefs.pop("USERPREFS_LORACONFIG_SPREAD_FACTOR", None)
        prefs.pop("USERPREFS_LORACONFIG_CODING_RATE", None)
        prefs.pop("USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY", None)

    # Channels Preset
    if args.channels_preset == "single_channel_default":
        prefs["USERPREFS_CHANNELS_TO_WRITE"] = "1"
        for i in range(1, 8):
            for k in list(prefs.keys()):
                if k.startswith(f"USERPREFS_CHANNEL_{i}_"):
                    del prefs[k]
    elif args.channels_preset == "repo_default_5_channels":
        # Keep repository 5-channel defaults if not already present
        prefs.setdefault("USERPREFS_CHANNELS_TO_WRITE", "5")
        prefs.setdefault("USERPREFS_CHANNEL_0_NAME", "SFNarrow")
        prefs.setdefault("USERPREFS_CHANNEL_0_PSK", "{ 0x01 }")
        prefs.setdefault("USERPREFS_CHANNEL_0_PRECISION", "16")
        prefs.setdefault("USERPREFS_CHANNEL_0_IS_MUTED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_0_UPLINK_ENABLED", "true")
        prefs.setdefault("USERPREFS_CHANNEL_0_DOWNLINK_ENABLED", "true")

        prefs.setdefault("USERPREFS_CHANNEL_1_NAME", "Test")
        prefs.setdefault("USERPREFS_CHANNEL_1_PSK", "{ 0x02 }")
        prefs.setdefault("USERPREFS_CHANNEL_1_PRECISION", "0")
        prefs.setdefault("USERPREFS_CHANNEL_1_IS_MUTED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_1_UPLINK_ENABLED", "true")
        prefs.setdefault("USERPREFS_CHANNEL_1_DOWNLINK_ENABLED", "true")

        prefs.setdefault("USERPREFS_CHANNEL_2_NAME", "Bots")
        prefs.setdefault("USERPREFS_CHANNEL_2_PSK", "{ 0x02 }")
        prefs.setdefault("USERPREFS_CHANNEL_2_PRECISION", "0")
        prefs.setdefault("USERPREFS_CHANNEL_2_IS_MUTED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_2_UPLINK_ENABLED", "true")
        prefs.setdefault("USERPREFS_CHANNEL_2_DOWNLINK_ENABLED", "true")

        prefs.setdefault("USERPREFS_CHANNEL_3_NAME", "Catalunya")
        prefs.setdefault("USERPREFS_CHANNEL_3_PSK", "{ 0x01 }")
        prefs.setdefault("USERPREFS_CHANNEL_3_PRECISION", "0")
        prefs.setdefault("USERPREFS_CHANNEL_3_IS_MUTED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_3_UPLINK_ENABLED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_3_DOWNLINK_ENABLED", "false")

        prefs.setdefault("USERPREFS_CHANNEL_4_NAME", "Barcelona")
        prefs.setdefault("USERPREFS_CHANNEL_4_PSK", "{ 0x01 }")
        prefs.setdefault("USERPREFS_CHANNEL_4_PRECISION", "0")
        prefs.setdefault("USERPREFS_CHANNEL_4_IS_MUTED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_4_UPLINK_ENABLED", "false")
        prefs.setdefault("USERPREFS_CHANNEL_4_DOWNLINK_ENABLED", "false")

    # WiFi
    if args.wifi_enabled and args.wifi_ssid:
        prefs["USERPREFS_NETWORK_WIFI_ENABLED"] = "true"
        prefs["USERPREFS_NETWORK_WIFI_SSID"] = args.wifi_ssid
        prefs["USERPREFS_NETWORK_WIFI_PSK"] = args.wifi_password
    else:
        prefs.pop("USERPREFS_NETWORK_WIFI_ENABLED", None)
        prefs.pop("USERPREFS_NETWORK_WIFI_SSID", None)
        prefs.pop("USERPREFS_NETWORK_WIFI_PSK", None)

    # GPS Mode
    prefs["USERPREFS_CONFIG_GPS_MODE"] = (
        f"meshtastic_Config_PositionConfig_GpsMode_{args.gps_mode.upper()}"
    )

    # Fixed Position
    if args.fixed_latitude and args.fixed_longitude:
        prefs["USERPREFS_FIXED_GPS"] = "1"
        prefs["USERPREFS_FIXED_GPS_LAT"] = str(float(args.fixed_latitude))
        prefs["USERPREFS_FIXED_GPS_LON"] = str(float(args.fixed_longitude))
        if args.fixed_altitude:
            prefs["USERPREFS_FIXED_GPS_ALT"] = str(int(args.fixed_altitude))
    else:
        prefs.pop("USERPREFS_FIXED_GPS", None)
        prefs.pop("USERPREFS_FIXED_GPS_LAT", None)
        prefs.pop("USERPREFS_FIXED_GPS_LON", None)
        prefs.pop("USERPREFS_FIXED_GPS_ALT", None)

    # Admin Key
    if args.admin_key:
        try:
            prefs["USERPREFS_USE_ADMIN_KEY_0"] = b64_to_c_array(args.admin_key)
            prefs["USERPREFS_CONFIG_SECURITY_IS_MANAGED"] = "true"
        except Exception as e:
            print(f"Warning: could not parse admin_key as Base64: {e}", file=sys.stderr)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(prefs, f, indent=2)

    print(f"Successfully generated {args.output} with {len(prefs)} keys.")


if __name__ == "__main__":
    main()
