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


def format_psk(psk_val: str) -> str:
    psk_str = str(psk_val).strip()
    if not psk_str:
        return "{ 0x01 }"
    if psk_str.startswith("{") and psk_str.endswith("}"):
        return psk_str
    if psk_str.isdigit():
        val = int(psk_str)
        return f"{{ 0x{val:02x} }}"
    if psk_str.startswith("0x") or psk_str.startswith("0X"):
        try:
            raw = bytes.fromhex(psk_str[2:])
            return "{ " + ", ".join(f"0x{b:02x}" for b in raw) + " }"
        except ValueError:
            pass
    try:
        raw = base64.b64decode(psk_str)
        return "{ " + ", ".join(f"0x{b:02x}" for b in raw) + " }"
    except Exception:
        try:
            raw = bytes.fromhex(psk_str)
            return "{ " + ", ".join(f"0x{b:02x}" for b in raw) + " }"
        except Exception:
            return "{ 0x01 }"


DEFAULT_MANIFEST = (
    Path(__file__).resolve().parents[2] / "custom_builder_web" / "options.manifest.json"
)


def option_value_error(opt: dict, value: str):
    """Why `value` is not valid for a manifest option, or None when it is.

    Same rules as emitOption() in custom_builder_web/nexus-options.js: the web builder only emits valid values, so a
    failure here means a hand-written custom_userprefs_json.
    """
    kind = opt["type"]
    if kind == "bool":
        # A JSON true/false reaches here as "True"/"False" once str() has run on it.
        return None if value.lower() in ("true", "false") else "expected true or false"
    if kind == "int":
        if not re.fullmatch(r"-?\d+", value):
            return "expected a whole number"
        if not opt["min"] <= int(value) <= opt["max"]:
            return f"expected {opt['min']}..{opt['max']}"
        return None
    if kind == "string":
        return None if len(value) <= opt["maxLen"] else f"longer than {opt['maxLen']}"
    if kind == "enum":
        # `emit` is the text the firmware gets for a choice; `prefix` is prepended to every choice.
        allowed = [opt.get("prefix", "") + v.get("emit", v["value"]) for v in opt["values"]]
        return None if value in allowed else f"expected one of {allowed}"
    if kind == "bitmask":
        try:
            number = int(value, 0)
        except ValueError:
            return "expected a decimal or 0x.. number"
        return None if 0 <= number < (1 << opt["width"]) else f"does not fit in {opt['width']} bits"
    if kind == "key":
        return None if value.startswith("{") and value.endswith("}") else "expected {0x.., ...}"
    return None


def drop_invalid_manifest_options(prefs: dict, manifest_path: Path) -> list:
    """Removes the manifest options whose value is out of range, so a typo cannot reach the firmware build.

    Returns one message per dropped key.
    """
    if not manifest_path.exists():
        return []
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    dropped = []
    for opt in manifest["options"]:
        key = opt["key"]
        if key not in prefs:
            continue
        error = option_value_error(opt, str(prefs[key]))
        if error:
            dropped.append(f"{key}={prefs[key]!r} dropped: {error}")
            del prefs[key]
        elif opt["type"] == "bool":
            # The firmware build needs the C++ literal: a JSON true arrives as "True".
            prefs[key] = str(prefs[key]).lower()
    return dropped


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
    parser.add_argument(
        "--enable-meshcore",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument("--meshcore-advert-hours", default="12")
    parser.add_argument("--meshcore-relay-mode", default="AUTO")
    parser.add_argument(
        "--meshcore-regions-def",
        default="",
        help="Hierarchical regions tree (empty = all regions permitted)",
    )
    parser.add_argument(
        "--meshcore-home-region",
        default="",
        help="Home region prefix (empty = none)",
    )
    parser.add_argument(
        "--meshcore-default-region",
        default="",
        help="Default region prefix for adverts (empty = none)",
    )
    parser.add_argument(
        "--nexus-base-texts",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument("--nexus-master-key", default="")
    parser.add_argument("--device-role", default="CLIENT")
    parser.add_argument("--lora-region", default="EU_868")
    parser.add_argument("--lora-preset-mode", default="custom_sfnarrow_default")
    parser.add_argument("--lora-override-frequency", default="")
    parser.add_argument("--lora-bandwidth", default="")
    parser.add_argument("--lora-spread-factor", default="")
    parser.add_argument("--lora-coding-rate", default="")
    parser.add_argument("--lora-tx-power", default="")
    parser.add_argument("--lora-hop-limit", default="")
    parser.add_argument("--lora-channel-num", default="")
    parser.add_argument(
        "--lora-ok-to-mqtt",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument(
        "--lora-ignore-mqtt",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=False,
    )
    parser.add_argument("--timezone-string", default="CET-1CEST,M3.5.0,M10.5.0/3")
    parser.add_argument("--rebroadcast-mode", default="ALL")
    parser.add_argument("--fixed-bluetooth", default="")
    parser.add_argument("--node-info-broadcast-secs", default="")
    parser.add_argument("--position-broadcast-interval", default="")
    parser.add_argument("--device-telem-interval", default="")
    parser.add_argument(
        "--smart-position",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=False,
    )
    parser.add_argument("--channels-preset", default="repo_default_5_channels")
    parser.add_argument(
        "--channels-json", default="", help="JSON string with custom channels array"
    )
    parser.add_argument(
        "--mqtt-enabled",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument("--mqtt-address", default="mqtt.meshtastic.es")
    parser.add_argument("--mqtt-username", default="meshdev")
    parser.add_argument("--mqtt-password", default="large4cats")
    parser.add_argument("--mqtt-root", default="msh")
    parser.add_argument(
        "--mqtt-encryption-enabled",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=True,
    )
    parser.add_argument(
        "--mqtt-tls-enabled",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=False,
    )
    parser.add_argument(
        "--mqtt-proxy-to-client",
        type=lambda v: v.lower() in ("true", "1", "yes"),
        default=False,
        help="Route MQTT through the connected phone instead of a direct network connection",
    )
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
    parser.add_argument(
        "--custom-userprefs-json",
        default="",
        help="Complete custom userPrefs JSON string to merge/override",
    )
    parser.add_argument("--output", default="userPrefs.jsonc")
    parser.add_argument(
        "--manifest",
        default=str(DEFAULT_MANIFEST),
        help="Builder options manifest used to validate the Nexus options",
    )

    args = parser.parse_args()
    prefs = load_userprefs(Path(args.file))

    # Owner identity (UTF-8 byte limits: long <= 24, short <= 4)
    # If set to empty or containing *RANDOM*, leave unset so firmware uses its
    # native unique hardware MAC based name: "Meshtacore <hex>" and "<hex>"
    if args.owner_long_name and "*RANDOM*" not in args.owner_long_name.upper():
        prefs["USERPREFS_CONFIG_OWNER_LONG_NAME"] = args.owner_long_name[:24]
    else:
        prefs.pop("USERPREFS_CONFIG_OWNER_LONG_NAME", None)

    if args.owner_short_name and "*RANDOM*" not in args.owner_short_name.upper():
        prefs["USERPREFS_CONFIG_OWNER_SHORT_NAME"] = args.owner_short_name[:4]
    else:
        prefs.pop("USERPREFS_CONFIG_OWNER_SHORT_NAME", None)

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

    NEXUS_PREDEFINED_KEYS = {
        "eevee": "{ 0x9a, 0xd2, 0x47, 0xb0, 0x2f, 0x4f, 0xa5, 0x08, 0x95, 0x3f, 0x5c, 0x1b, 0x6e, 0x13, 0x93, 0x98, 0x2c, 0xb3, 0xd6, 0xf3, 0x19, 0x3b, 0x87, 0xa5, 0xc8, 0x33, 0x2d, 0xce, 0x67, 0xae, 0x9d, 0x15 }",
        "bloom": "{ 0x6c, 0x5c, 0xa6, 0xe8, 0xfa, 0x3f, 0xe9, 0x46, 0x19, 0x6b, 0x82, 0x14, 0xa7, 0xe2, 0xd9, 0x96, 0xed, 0x6a, 0xe1, 0x10, 0xe4, 0xe5, 0xf3, 0xe8, 0xc6, 0x1b, 0xfb, 0xc4, 0x9e, 0x8b, 0x8f, 0x61 }",
        "mew": "{ 0x11, 0x0c, 0x24, 0x41, 0x08, 0x40, 0xd7, 0xca, 0xec, 0x9f, 0xb8, 0xf8, 0x71, 0x99, 0xcb, 0xcc, 0xb6, 0xac, 0xd9, 0x56, 0x77, 0x14, 0x9b, 0x3e, 0xee, 0xf2, 0x9c, 0x4c, 0xf5, 0x08, 0x12, 0x60 }",
        "fennekin": "{ 0xe7, 0x05, 0x7e, 0xe7, 0xeb, 0x80, 0xf9, 0x04, 0x0d, 0xf2, 0x30, 0xff, 0x0c, 0x4a, 0x09, 0x74, 0x94, 0xb9, 0x47, 0x92, 0xa0, 0x65, 0x78, 0xcb, 0xfd, 0xeb, 0xdc, 0xda, 0x57, 0x73, 0x26, 0x7e }",
    }

    # MeshCore subsystem (Sniffer, Relay, Companion, Hybrid Sync) is ALWAYS enabled in Meshtacore
    prefs.pop("MESHTASTIC_ENABLE_MESHCORE_SNIFFER", None)
    prefs.pop("MESHTASTIC_ENABLE_MESHCORE_RELAY", None)
    prefs.pop("MESHTASTIC_ENABLE_MESHCORE_COMPANION", None)
    prefs.pop("MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC", None)

    if args.meshcore_advert_hours and args.meshcore_advert_hours.isdigit():
        prefs["USERPREFS_MESHCORE_ADVERT_HOURS"] = args.meshcore_advert_hours
    mode_val = {"AUTO": "0", "OFF": "1", "ON": "2"}.get(
        args.meshcore_relay_mode.upper(), "0"
    )
    prefs["USERPREFS_MESHCORE_RELAY_MODE"] = mode_val

    # Regions: by default empty (all permitted without restriction)
    if args.meshcore_regions_def and args.meshcore_regions_def.strip():
        prefs["USERPREFS_MESHCORE_REGIONS_DEF"] = args.meshcore_regions_def.strip()
    else:
        prefs.pop("USERPREFS_MESHCORE_REGIONS_DEF", None)

    if args.meshcore_home_region and args.meshcore_home_region.strip():
        prefs["USERPREFS_MESHCORE_HOME_REGION"] = args.meshcore_home_region.strip()
    else:
        prefs.pop("USERPREFS_MESHCORE_HOME_REGION", None)

    if args.meshcore_default_region and args.meshcore_default_region.strip():
        prefs["USERPREFS_MESHCORE_DEFAULT_REGION"] = (
            args.meshcore_default_region.strip()
        )
    else:
        prefs.pop("USERPREFS_MESHCORE_DEFAULT_REGION", None)

    # Nexus Master Key (Remote Admin)
    if args.nexus_master_key:
        k = args.nexus_master_key.strip().lower()
        if k in NEXUS_PREDEFINED_KEYS:
            prefs["NEXUS_MASTER_KEY_1"] = NEXUS_PREDEFINED_KEYS[k]
        elif k.startswith("{") and k.endswith("}"):
            prefs["NEXUS_MASTER_KEY_1"] = args.nexus_master_key.strip()
        else:
            try:
                prefs["NEXUS_MASTER_KEY_1"] = b64_to_c_array(args.nexus_master_key)
            except Exception:
                pass
    else:
        prefs.pop("NEXUS_MASTER_KEY_1", None)

    # Nexus Base Texts
    if args.nexus_base_texts:
        prefs["USERPREFS_NEXUS_BASE_TEXTS"] = "true"
    else:
        prefs["USERPREFS_NEXUS_BASE_TEXTS"] = "false"

    # Device role
    role_enum = f"meshtastic_Config_DeviceConfig_Role_{args.device_role.upper()}"
    prefs["USERPREFS_CONFIG_DEVICE_ROLE"] = role_enum

    # LoRa Region
    region_enum = f"meshtastic_Config_LoRaConfig_RegionCode_{args.lora_region.upper()}"
    prefs["USERPREFS_CONFIG_LORA_REGION"] = region_enum

    # LoRa Preset / Custom Profile
    if args.lora_preset_mode == "custom_sfnarrow_default":
        prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "false"
        prefs["USERPREFS_LORACONFIG_BANDWIDTH"] = "62"
        prefs["USERPREFS_LORACONFIG_SPREAD_FACTOR"] = "7"
        prefs["USERPREFS_LORACONFIG_CODING_RATE"] = "6"
        prefs["USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY"] = "869.618"
        prefs["USERPREFS_LORACONFIG_TX_POWER"] = "22"
        prefs["USERPREFS_CONFIG_LORA_HOP_LIMIT"] = "7"
        prefs["USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT"] = "true"
        prefs.pop("USERPREFS_LORACONFIG_MODEM_PRESET", None)
    elif args.lora_preset_mode == "custom":
        prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "false"
        prefs["USERPREFS_LORACONFIG_BANDWIDTH"] = str(args.lora_bandwidth or "62")
        prefs["USERPREFS_LORACONFIG_SPREAD_FACTOR"] = str(
            args.lora_spread_factor or "7"
        )
        prefs["USERPREFS_LORACONFIG_CODING_RATE"] = str(args.lora_coding_rate or "6")
        prefs["USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY"] = str(
            args.lora_override_frequency or "869.618"
        )
        if args.lora_tx_power:
            prefs["USERPREFS_LORACONFIG_TX_POWER"] = str(args.lora_tx_power)
        if args.lora_hop_limit:
            prefs["USERPREFS_CONFIG_LORA_HOP_LIMIT"] = str(args.lora_hop_limit)
        if args.lora_channel_num:
            prefs["USERPREFS_LORACONFIG_CHANNEL_NUM"] = str(args.lora_channel_num)
        if args.lora_ok_to_mqtt:
            prefs["USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT"] = "true"
        if args.lora_ignore_mqtt:
            prefs["USERPREFS_CONFIG_LORA_IGNORE_MQTT"] = "true"
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

    # Timezone String
    if args.timezone_string:
        prefs["USERPREFS_TZ_STRING"] = args.timezone_string

    # Advanced behavior settings
    if args.rebroadcast_mode:
        prefs["USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE"] = (
            f"meshtastic_Config_DeviceConfig_RebroadcastMode_{args.rebroadcast_mode.upper()}"
        )
    if args.fixed_bluetooth:
        prefs["USERPREFS_FIXED_BLUETOOTH"] = str(args.fixed_bluetooth)
    if args.node_info_broadcast_secs:
        prefs["USERPREFS_CONFIG_DEVICE_NODE_INFO_BROADCAST_SECS"] = str(
            args.node_info_broadcast_secs
        )
    if args.position_broadcast_interval:
        prefs["USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL"] = str(
            args.position_broadcast_interval
        )
    if args.device_telem_interval:
        prefs["USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL"] = str(
            args.device_telem_interval
        )
    if args.smart_position:
        prefs["USERPREFS_CONFIG_SMART_POSITION_ENABLED"] = "true"

    # Channels configuration
    custom_channels = None
    if args.channels_json:
        try:
            custom_channels = json.loads(args.channels_json)
        except Exception as e:
            print(f"Warning: could not parse channels_json: {e}", file=sys.stderr)

    if custom_channels and isinstance(custom_channels, list):
        active = [c for c in custom_channels if c.get("enabled", True)]
        # Clean existing channel prefs
        for i in range(8):
            for k in list(prefs.keys()):
                if k.startswith(f"USERPREFS_CHANNEL_{i}_"):
                    del prefs[k]

        count = min(len(active), 8)
        prefs["USERPREFS_CHANNELS_TO_WRITE"] = str(count)
        for i, ch in enumerate(active[:8]):
            prefs[f"USERPREFS_CHANNEL_{i}_NAME"] = str(ch.get("name", ""))[:11]
            prefs[f"USERPREFS_CHANNEL_{i}_PSK"] = format_psk(str(ch.get("psk", "AQ==")))
            prefs[f"USERPREFS_CHANNEL_{i}_PRECISION"] = str(ch.get("precision", 0))
            prefs[f"USERPREFS_CHANNEL_{i}_IS_MUTED"] = (
                "true" if ch.get("muted", False) else "false"
            )
            prefs[f"USERPREFS_CHANNEL_{i}_UPLINK_ENABLED"] = (
                "true" if ch.get("uplink", False) else "false"
            )
            prefs[f"USERPREFS_CHANNEL_{i}_DOWNLINK_ENABLED"] = (
                "true" if ch.get("downlink", False) else "false"
            )
    elif args.channels_preset == "single_channel_default":
        prefs["USERPREFS_CHANNELS_TO_WRITE"] = "1"
        for i in range(1, 8):
            for k in list(prefs.keys()):
                if k.startswith(f"USERPREFS_CHANNEL_{i}_"):
                    del prefs[k]
    elif args.channels_preset == "repo_default_5_channels":
        # Defaults: uplink enabled only for Meshtastic channels, downlink disabled
        prefs["USERPREFS_CHANNELS_TO_WRITE"] = "5"
        prefs["USERPREFS_CHANNEL_0_NAME"] = "SFNarrow"
        prefs["USERPREFS_CHANNEL_0_PSK"] = "{ 0x01 }"
        prefs["USERPREFS_CHANNEL_0_PRECISION"] = "16"
        prefs["USERPREFS_CHANNEL_0_IS_MUTED"] = "false"
        prefs["USERPREFS_CHANNEL_0_UPLINK_ENABLED"] = "true"
        prefs["USERPREFS_CHANNEL_0_DOWNLINK_ENABLED"] = "false"

        prefs["USERPREFS_CHANNEL_1_NAME"] = "Test"
        prefs["USERPREFS_CHANNEL_1_PSK"] = "{ 0x02 }"
        prefs["USERPREFS_CHANNEL_1_PRECISION"] = "0"
        prefs["USERPREFS_CHANNEL_1_IS_MUTED"] = "false"
        prefs["USERPREFS_CHANNEL_1_UPLINK_ENABLED"] = "true"
        prefs["USERPREFS_CHANNEL_1_DOWNLINK_ENABLED"] = "false"

        prefs["USERPREFS_CHANNEL_2_NAME"] = "Bots"
        prefs["USERPREFS_CHANNEL_2_PSK"] = "{ 0x02 }"
        prefs["USERPREFS_CHANNEL_2_PRECISION"] = "0"
        prefs["USERPREFS_CHANNEL_2_IS_MUTED"] = "false"
        prefs["USERPREFS_CHANNEL_2_UPLINK_ENABLED"] = "true"
        prefs["USERPREFS_CHANNEL_2_DOWNLINK_ENABLED"] = "false"

        prefs["USERPREFS_CHANNEL_3_NAME"] = "Catalunya"
        prefs["USERPREFS_CHANNEL_3_PSK"] = "{ 0x01 }"
        prefs["USERPREFS_CHANNEL_3_PRECISION"] = "0"
        prefs["USERPREFS_CHANNEL_3_IS_MUTED"] = "false"
        prefs["USERPREFS_CHANNEL_3_UPLINK_ENABLED"] = "true"
        prefs["USERPREFS_CHANNEL_3_DOWNLINK_ENABLED"] = "false"

        prefs["USERPREFS_CHANNEL_4_NAME"] = "Barcelona"
        prefs["USERPREFS_CHANNEL_4_PSK"] = "{ 0x01 }"
        prefs["USERPREFS_CHANNEL_4_PRECISION"] = "0"
        prefs["USERPREFS_CHANNEL_4_IS_MUTED"] = "false"
        prefs["USERPREFS_CHANNEL_4_UPLINK_ENABLED"] = "true"
        prefs["USERPREFS_CHANNEL_4_DOWNLINK_ENABLED"] = "false"

    # MQTT configuration
    if args.mqtt_enabled:
        prefs["USERPREFS_MQTT_ENABLED"] = "1"
        prefs["USERPREFS_MQTT_ADDRESS"] = args.mqtt_address or "mqtt.meshtastic.es"
        prefs["USERPREFS_MQTT_USERNAME"] = args.mqtt_username or "meshdev"
        prefs["USERPREFS_MQTT_PASSWORD"] = args.mqtt_password or "large4cats"
        # A regional broker (not mqtt.meshtastic.org) never gets the region appended by the
        # firmware, so a bare "msh" would publish outside the msh/<region> tree.
        prefs["USERPREFS_MQTT_ROOT_TOPIC"] = args.mqtt_root or f"msh/{args.lora_region}"
        prefs["USERPREFS_MQTT_ENCRYPTION_ENABLED"] = (
            "true" if args.mqtt_encryption_enabled else "false"
        )
        prefs["USERPREFS_MQTT_TLS_ENABLED"] = (
            "true" if args.mqtt_tls_enabled else "false"
        )
    else:
        # Firmware default is MQTT on (Spanish broker): "off" must be explicit, not an absent key.
        prefs["USERPREFS_MQTT_ENABLED"] = "0"
        prefs.pop("USERPREFS_MQTT_ADDRESS", None)
        prefs.pop("USERPREFS_MQTT_USERNAME", None)
        prefs.pop("USERPREFS_MQTT_PASSWORD", None)
        prefs.pop("USERPREFS_MQTT_ROOT_TOPIC", None)
        prefs.pop("USERPREFS_MQTT_ENCRYPTION_ENABLED", None)
        prefs.pop("USERPREFS_MQTT_TLS_ENABLED", None)

    if args.mqtt_proxy_to_client:
        prefs["USERPREFS_MQTT_PROXY_TO_CLIENT_ENABLED"] = "true"
    else:
        prefs.pop("USERPREFS_MQTT_PROXY_TO_CLIENT_ENABLED", None)

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

    # Direct custom_userprefs_json merge
    if args.custom_userprefs_json:
        try:
            extra_prefs = json.loads(args.custom_userprefs_json)
            if isinstance(extra_prefs, dict):
                for k, v in extra_prefs.items():
                    if v is None:
                        prefs.pop(k, None)
                    else:
                        prefs[k] = str(v)
        except Exception as e:
            print(
                f"Warning: could not merge custom_userprefs_json: {e}", file=sys.stderr
            )

    for message in drop_invalid_manifest_options(prefs, Path(args.manifest)):
        print(f"::warning::{message}", file=sys.stderr)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(prefs, f, indent=2)

    print(f"Successfully generated {args.output} with {len(prefs)} keys.")


if __name__ == "__main__":
    main()
