/*
 * Traducción del formulario del builder a las claves USERPREFS_* que recibe el generador (y de ahí el firmware).
 *
 * Es pura (sin DOM) para poder probarla desde Node: custom_builder_web/tests/build-prefs.test.js. app.js le pasa por
 * `ctx` lo único que no es del formulario: la tabla de canales, el manifest de Nexus y el módulo nexus-options.js.
 */
(function (root) {
  "use strict";

  // Valores que el builder aplica cuando el usuario deja vacío el campo MQTT (el generador tiene su propia copia).
  const MQTT_DEFAULTS = {
    address: "mqtt.meshtastic.es",
    username: "meshdev",
    password: "large4cats",
  };

  const NEXUS_PREDEFINED_KEYS = {
    eevee:
      "{ 0x9a, 0xd2, 0x47, 0xb0, 0x2f, 0x4f, 0xa5, 0x08, 0x95, 0x3f, 0x5c, 0x1b, 0x6e, 0x13, 0x93, 0x98, 0x2c, 0xb3, 0xd6, 0xf3, 0x19, 0x3b, 0x87, 0xa5, 0xc8, 0x33, 0x2d, 0xce, 0x67, 0xae, 0x9d, 0x15 }",
    bloom:
      "{ 0x6c, 0x5c, 0xa6, 0xe8, 0xfa, 0x3f, 0xe9, 0x46, 0x19, 0x6b, 0x82, 0x14, 0xa7, 0xe2, 0xd9, 0x96, 0xed, 0x6a, 0xe1, 0x10, 0xe4, 0xe5, 0xf3, 0xe8, 0xc6, 0x1b, 0xfb, 0xc4, 0x9e, 0x8b, 0x8f, 0x61 }",
    mew: "{ 0x11, 0x0c, 0x24, 0x41, 0x08, 0x40, 0xd7, 0xca, 0xec, 0x9f, 0xb8, 0xf8, 0x71, 0x99, 0xcb, 0xcc, 0xb6, 0xac, 0xd9, 0x56, 0x77, 0x14, 0x9b, 0x3e, 0xee, 0xf2, 0x9c, 0x4c, 0xf5, 0x08, 0x12, 0x60 }",
    fennekin:
      "{ 0xe7, 0x05, 0x7e, 0xe7, 0xeb, 0x80, 0xf9, 0x04, 0x0d, 0xf2, 0x30, 0xff, 0x0c, 0x4a, 0x09, 0x74, 0x94, 0xb9, 0x47, 0x92, 0xa0, 0x65, 0x78, 0xcb, 0xfd, 0xeb, 0xdc, 0xda, 0x57, 0x73, 0x26, 0x7e }",
  };

  const VALID_PSK_LENGTHS = [1, 16, 32]; // bytes: default-key index, AES-128, AES-256

  function bytesToCArray(bytes) {
    return (
      "{ " +
      bytes.map((b) => "0x" + b.toString(16).padStart(2, "0")).join(", ") +
      " }"
    );
  }

  // Convert any PSK format (1, 2, AQ==, Base64, Hex) to C byte array format.
  // Throws for anything that is not a usable key: it used to fall back to { 0x01 }, the publicly known default key,
  // so a typo silently put the channel on a key everybody has.
  function formatPskToCArray(pskVal) {
    const val = String(pskVal || "").trim();
    if (!val) return "{ 0x01 }";
    if (val.startsWith("{") && val.endsWith("}")) {
      if (
        !/^\{\s*0[xX][0-9a-fA-F]{1,2}(\s*,\s*0[xX][0-9a-fA-F]{1,2}){0,63}\s*\}$/.test(
          val,
        )
      ) {
        throw new Error("la clave { ... } debe ser una lista de bytes 0x..");
      }
      return val;
    }
    let bytes;
    if (
      /^[0-9a-fA-F]+$/.test(val) &&
      (val.length === 32 || val.length === 64)
    ) {
      // 32/64 hex digits are also valid base64 text (and may be all digits): hex wins over both readings.
      bytes = val.match(/../g).map((h) => parseInt(h, 16));
    } else if (/^\d+$/.test(val)) {
      const num = parseInt(val, 10);
      if (num > 0xff) throw new Error(`el índice ${num} no cabe en un byte`);
      return `{ 0x${num.toString(16).padStart(2, "0")} }`;
    } else if (/^0[xX][0-9a-fA-F]+$/.test(val) && val.length % 2 === 0) {
      bytes = val
        .slice(2)
        .match(/../g)
        .map((h) => parseInt(h, 16));
    } else {
      let bin;
      try {
        bin = atob(val);
      } catch (e) {
        throw new Error("no es Base64 ni hexadecimal válido");
      }
      bytes = Array.from(bin, (c) => c.charCodeAt(0));
    }
    if (!VALID_PSK_LENGTHS.includes(bytes.length)) {
      throw new Error(
        `la clave tiene ${bytes.length} bytes y debe tener 1, 16 o 32`,
      );
    }
    return bytesToCArray(bytes);
  }

  // data: lo que devuelve getFormData() en app.js.
  function buildUserPrefs(data, ctx) {
    const prefs = {
      USERPREFS_CONFIG_DEVICE_ROLE: `meshtastic_Config_DeviceConfig_Role_${data.device_role}`,
      USERPREFS_CONFIG_LORA_REGION: `meshtastic_Config_LoRaConfig_RegionCode_${data.lora_region}`,
      USERPREFS_CONFIG_GPS_MODE: `meshtastic_Config_PositionConfig_GpsMode_${data.gps_mode}`,
    };

    if (data.owner_long_name) {
      prefs["USERPREFS_CONFIG_OWNER_LONG_NAME"] = data.owner_long_name;
    }
    if (data.owner_short_name) {
      prefs["USERPREFS_CONFIG_OWNER_SHORT_NAME"] = data.owner_short_name;
    }

    if (!data.has_screen) {
      prefs["MESHTASTIC_EXCLUDE_SCREEN"] = "1";
      prefs["HAS_SCREEN"] = "0";
      prefs["USERPREFS_CONFIG_DISPLAY_SCREEN_ON_SECS"] = "1";
    }

    if (!data.enable_bluetooth) {
      prefs["MESHTASTIC_EXCLUDE_BLUETOOTH"] = "1";
      prefs["HAS_BLUETOOTH"] = "0";
    }

    // Opt-in wipe on first boot, so the compiled defaults replace the stored config.
    if (data.force_clean_install) {
      prefs["USERPREFS_FORCE_CLEAN_INSTALL"] = "1";
    }

    // MeshCore advert/relay/regions and Nexus base texts are emitted from options.manifest.json (see the end of this function).

    if (data.nexus_master_key) {
      const k = data.nexus_master_key.toLowerCase();
      if (NEXUS_PREDEFINED_KEYS[k]) {
        prefs["NEXUS_MASTER_KEY_1"] = NEXUS_PREDEFINED_KEYS[k];
      } else if (k.startsWith("{") && k.endsWith("}")) {
        prefs["NEXUS_MASTER_KEY_1"] = data.nexus_master_key;
      }
    }

    if (data.lora_preset_mode === "custom_sfnarrow_default") {
      prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "false";
      prefs["USERPREFS_LORACONFIG_BANDWIDTH"] = "62";
      prefs["USERPREFS_LORACONFIG_SPREAD_FACTOR"] = "7";
      prefs["USERPREFS_LORACONFIG_CODING_RATE"] = "6";
      prefs["USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY"] = "869.618";
      prefs["USERPREFS_LORACONFIG_TX_POWER"] = "22";
      prefs["USERPREFS_CONFIG_LORA_HOP_LIMIT"] = "7";
      prefs["USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT"] = "true";
    } else if (data.lora_preset_mode === "custom") {
      prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "false";
      prefs["USERPREFS_LORACONFIG_BANDWIDTH"] = data.lora_bandwidth || "62";
      prefs["USERPREFS_LORACONFIG_SPREAD_FACTOR"] =
        data.lora_spread_factor || "7";
      prefs["USERPREFS_LORACONFIG_CODING_RATE"] = data.lora_coding_rate || "6";
      prefs["USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY"] =
        data.lora_override_freq || "869.618";
      if (data.lora_tx_power !== "") {
        prefs["USERPREFS_LORACONFIG_TX_POWER"] = data.lora_tx_power;
      }
      if (data.lora_hop_limit !== "") {
        prefs["USERPREFS_CONFIG_LORA_HOP_LIMIT"] = data.lora_hop_limit;
      }
      if (data.lora_channel_num !== "") {
        prefs["USERPREFS_LORACONFIG_CHANNEL_NUM"] = data.lora_channel_num;
      }
      if (data.lora_ignore_mqtt) {
        prefs["USERPREFS_CONFIG_LORA_IGNORE_MQTT"] = "true";
      }
    } else {
      const p = data.lora_preset_mode
        .replace("modem_preset_", "")
        .toUpperCase();
      prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "true";
      prefs["USERPREFS_LORACONFIG_MODEM_PRESET"] =
        `meshtastic_Config_LoRaConfig_ModemPreset_${p}`;
    }

    // OK to MQTT applies to every preset, not only the custom one: without the bit other gateways do not uplink our packets.
    prefs["USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT"] = data.lora_ok_to_mqtt
      ? "true"
      : "false";

    // Timezone, rebroadcast mode, BLE PIN, intervals and smart position also come from options.manifest.json.

    // Write channels
    const activeChannels = ctx.channels.filter((c) => c.enabled);
    if (activeChannels.length > 0) {
      prefs["USERPREFS_CHANNELS_TO_WRITE"] = String(
        Math.min(activeChannels.length, 8),
      );
      activeChannels.slice(0, 8).forEach((ch, idx) => {
        prefs[`USERPREFS_CHANNEL_${idx}_NAME`] = ch.name;
        try {
          prefs[`USERPREFS_CHANNEL_${idx}_PSK`] = formatPskToCArray(ch.psk);
        } catch (err) {
          throw new Error(`Canal "${ch.name}": ${err.message}`);
        }
        prefs[`USERPREFS_CHANNEL_${idx}_PRECISION`] = String(ch.precision);
        prefs[`USERPREFS_CHANNEL_${idx}_IS_MUTED`] = "false";
        prefs[`USERPREFS_CHANNEL_${idx}_UPLINK_ENABLED`] = ch.uplink
          ? "true"
          : "false";
        prefs[`USERPREFS_CHANNEL_${idx}_DOWNLINK_ENABLED`] = ch.downlink
          ? "true"
          : "false";
      });
    }

    // MQTT: always emit the switch, or the generator's default (enabled) wins.
    prefs["USERPREFS_MQTT_ENABLED"] = data.mqtt_enabled ? "1" : "0";
    if (data.mqtt_enabled) {
      prefs["USERPREFS_MQTT_ADDRESS"] =
        data.mqtt_address || MQTT_DEFAULTS.address;
      prefs["USERPREFS_MQTT_ROOT_TOPIC"] =
        data.mqtt_root || `msh/${data.lora_region}`;
      prefs["USERPREFS_MQTT_USERNAME"] =
        data.mqtt_username || MQTT_DEFAULTS.username;
      prefs["USERPREFS_MQTT_PASSWORD"] =
        data.mqtt_password || MQTT_DEFAULTS.password;
      prefs["USERPREFS_MQTT_ENCRYPTION_ENABLED"] = data.mqtt_encryption_enabled
        ? "true"
        : "false";
      prefs["USERPREFS_MQTT_TLS_ENABLED"] = data.mqtt_tls_enabled
        ? "true"
        : "false";
      prefs["USERPREFS_MQTT_PROXY_TO_CLIENT_ENABLED"] = data.mqtt_proxy
        ? "true"
        : "false";
    }

    // Fixed Location
    if (data.fixed_latitude && data.fixed_longitude) {
      prefs["USERPREFS_FIXED_GPS"] = "1";
      prefs["USERPREFS_FIXED_GPS_LAT"] = data.fixed_latitude;
      prefs["USERPREFS_FIXED_GPS_LON"] = data.fixed_longitude;
      if (data.fixed_altitude) {
        prefs["USERPREFS_FIXED_GPS_ALT"] = data.fixed_altitude;
      }
    }

    // WiFi
    if (data.wifi_enabled && data.wifi_ssid) {
      prefs["USERPREFS_NETWORK_WIFI_ENABLED"] = "true";
      prefs["USERPREFS_NETWORK_WIFI_SSID"] = data.wifi_ssid;
      prefs["USERPREFS_NETWORK_WIFI_PSK"] = data.wifi_password;
    }

    // Only the solar repeater profile may run ROUTER; the firmware restricts it in any other build (NodeDB.cpp).
    if (
      data.node_preset === "solar_repeater" &&
      data.device_role === "ROUTER"
    ) {
      prefs["USERPREFS_ALLOW_ROUTER_ROLE"] = "1";
    }

    // Opciones avanzadas de Nexus (manifest). Un formulario sin tocar no añade nada.
    if (ctx.nexusManifest && ctx.nexusOptions) {
      const nexusValues = ctx.nexusOptions.remapChannelBits(
        ctx.nexusManifest,
        ctx.nexusOptions.readValues(ctx.nexusManifest, data),
        ctx.channels.map((c) => c.enabled),
      );
      const problems = ctx.nexusOptions.invalidValues(
        ctx.nexusManifest,
        nexusValues,
      );
      if (problems.length > 0) {
        throw new Error(problems.join("; "));
      }
      Object.assign(
        prefs,
        ctx.nexusOptions.emitNexusPrefs(ctx.nexusManifest, nexusValues),
      );
    }

    return prefs;
  }

  const api = {
    MQTT_DEFAULTS,
    NEXUS_PREDEFINED_KEYS,
    formatPskToCArray,
    buildUserPrefs,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.BuildPrefs = api;
})(typeof window !== "undefined" ? window : globalThis);
