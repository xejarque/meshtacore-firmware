// Meshtacore Custom Firmware Builder - Web Frontend

const REPO_OWNER = "xejarque";
const REPO_NAME = "meshtacore-firmware";
const DEFAULT_BRANCH = "meshtacore";
const WORKFLOW_FILE = "custom_builder.yml";

const NEXUS_PREDEFINED_KEYS = {
  eevee:
    "{ 0x9a, 0xd2, 0x47, 0xb0, 0x2f, 0x4f, 0xa5, 0x08, 0x95, 0x3f, 0x5c, 0x1b, 0x6e, 0x13, 0x93, 0x98, 0x2c, 0xb3, 0xd6, 0xf3, 0x19, 0x3b, 0x87, 0xa5, 0xc8, 0x33, 0x2d, 0xce, 0x67, 0xae, 0x9d, 0x15 }",
  bloom:
    "{ 0x6c, 0x5c, 0xa6, 0xe8, 0xfa, 0x3f, 0xe9, 0x46, 0x19, 0x6b, 0x82, 0x14, 0xa7, 0xe2, 0xd9, 0x96, 0xed, 0x6a, 0xe1, 0x10, 0xe4, 0xe5, 0xf3, 0xe8, 0xc6, 0x1b, 0xfb, 0xc4, 0x9e, 0x8b, 0x8f, 0x61 }",
  mew: "{ 0x11, 0x0c, 0x24, 0x41, 0x08, 0x40, 0xd7, 0xca, 0xec, 0x9f, 0xb8, 0xf8, 0x71, 0x99, 0xcb, 0xcc, 0xb6, 0xac, 0xd9, 0x56, 0x77, 0x14, 0x9b, 0x3e, 0xee, 0xf2, 0x9c, 0x4c, 0xf5, 0x08, 0x12, 0x60 }",
  fennekin:
    "{ 0xe7, 0x05, 0x7e, 0xe7, 0xeb, 0x80, 0xf9, 0x04, 0x0d, 0xf2, 0x30, 0xff, 0x0c, 0x4a, 0x09, 0x74, 0x94, 0xb9, 0x47, 0x92, 0xa0, 0x65, 0x78, 0xcb, 0xfd, 0xeb, 0xdc, 0xda, 0x57, 0x73, 0x26, 0x7e }",
};

document.addEventListener("DOMContentLoaded", () => {
  const boardTarget = document.getElementById("boardTarget");
  const customTargetGroup = document.getElementById("customTargetGroup");
  const autoRandomName = document.getElementById("autoRandomName");
  const ownerLongName = document.getElementById("ownerLongName");
  const ownerShortName = document.getElementById("ownerShortName");
  const enableMeshcore = document.getElementById("enableMeshcore");
  const meshcoreSettings = document.getElementById("meshcoreSettings");
  const nexusMasterKey = document.getElementById("nexusMasterKey");
  const customMasterKeyGroup = document.getElementById("customMasterKeyGroup");
  const customMasterKey = document.getElementById("customMasterKey");
  const channelsPreset = document.getElementById("channelsPreset");
  const wifiEnabled = document.getElementById("wifiEnabled");
  const wifiFields = document.getElementById("wifiFields");
  const githubToken = document.getElementById("githubToken");
  const btnDispatch = document.getElementById("btnDispatch");
  const btnOpenActions = document.getElementById("btnOpenActions");
  const btnDownloadJson = document.getElementById("btnDownloadJson");
  const dispatchStatus = document.getElementById("dispatchStatus");

  // Load saved token if present
  const savedToken = localStorage.getItem("meshtacore_gh_token");
  if (savedToken) {
    githubToken.value = savedToken;
  }

  githubToken.addEventListener("input", () => {
    localStorage.setItem("meshtacore_gh_token", githubToken.value.trim());
  });

  // Toggle custom board input
  boardTarget.addEventListener("change", () => {
    if (boardTarget.value === "custom") {
      customTargetGroup.classList.remove("hidden");
    } else {
      customTargetGroup.classList.add("hidden");
    }
  });

  // Toggle Auto Random Name vs Custom Name
  autoRandomName.addEventListener("change", () => {
    if (autoRandomName.checked) {
      ownerLongName.value = "Meshtacore *RANDOM*";
      ownerShortName.value = "*RANDOM*";
      ownerLongName.disabled = true;
      ownerShortName.disabled = true;
    } else {
      ownerLongName.disabled = false;
      ownerShortName.disabled = false;
      if (ownerLongName.value.includes("*RANDOM*")) {
        ownerLongName.value = "Meshtacore Node";
        ownerShortName.value = "MTC";
      }
      ownerLongName.focus();
    }
  });

  // Toggle MeshCore settings card
  enableMeshcore.addEventListener("change", () => {
    if (enableMeshcore.checked) {
      meshcoreSettings.classList.remove("hidden");
    } else {
      meshcoreSettings.classList.add("hidden");
    }
  });

  // Toggle Custom Master Key input
  nexusMasterKey.addEventListener("change", () => {
    if (nexusMasterKey.value === "custom") {
      customMasterKeyGroup.classList.remove("hidden");
    } else {
      customMasterKeyGroup.classList.add("hidden");
    }
  });

  // Toggle channels table view
  channelsPreset.addEventListener("change", () => {
    const rows = document.querySelectorAll(".channels-table tbody tr");
    if (channelsPreset.value === "single_channel_default") {
      rows.forEach((row, idx) => {
        if (idx > 0) row.classList.add("hidden");
      });
    } else {
      rows.forEach((row) => row.classList.remove("hidden"));
    }
  });

  // Toggle WiFi fields
  wifiEnabled.addEventListener("change", () => {
    if (wifiEnabled.checked) {
      wifiFields.classList.remove("hidden");
    } else {
      wifiFields.classList.add("hidden");
    }
  });

  // Open Actions in GitHub
  btnOpenActions.addEventListener("click", () => {
    const url = `https://github.com/${REPO_OWNER}/${REPO_NAME}/actions/workflows/${WORKFLOW_FILE}`;
    window.open(url, "_blank");
  });

  // Collect inputs into object matching workflow dispatch
  function getFormData() {
    const target = boardTarget.value;
    const customTarget = document.getElementById("customTarget").value.trim();
    const isAutoName = autoRandomName.checked;
    const longName = isAutoName
      ? "Meshtacore *RANDOM*"
      : ownerLongName.value.trim() || "Meshtacore Node";
    const shortName = isAutoName
      ? "*RANDOM*"
      : ownerShortName.value.trim() || "MTC";
    const hasScreen = document.getElementById("hasScreen").checked;
    const enableBluetooth = document.getElementById("enableBluetooth").checked;
    const isMeshcoreOn = enableMeshcore.checked;
    const mcRelay = document.getElementById("meshcoreRelayMode").value;
    const mcAdvert =
      document.getElementById("meshcoreAdvertHours").value.trim() || "12";
    const mcRegions = document
      .getElementById("meshcoreRegionsDef")
      .value.trim();
    const mcHome = document.getElementById("meshcoreHomeRegion").value.trim();
    const mcDef = document.getElementById("meshcoreDefaultRegion").value.trim();
    const keyChoice = nexusMasterKey.value;
    const masterKey =
      keyChoice === "custom" ? customMasterKey.value.trim() : keyChoice;

    const deviceRole = document.getElementById("deviceRole").value;
    const loraRegion = document.getElementById("loraRegion").value;
    const loraPreset = document.getElementById("loraPreset").value;
    const chPreset = channelsPreset.value;
    const gpsMode = document.getElementById("gpsMode").value;
    const fixedLat = document.getElementById("fixedLat").value.trim();
    const fixedLon = document.getElementById("fixedLon").value.trim();
    const fixedAlt = document.getElementById("fixedAlt").value.trim();
    const wifiOn = wifiEnabled.checked;
    const wifiSsid = document.getElementById("wifiSsid").value.trim();
    const wifiPassword = document.getElementById("wifiPassword").value.trim();

    return {
      target: target,
      custom_target: customTarget,
      owner_long_name: longName.substring(0, 24),
      owner_short_name: shortName.substring(0, 4),
      has_screen: hasScreen,
      enable_bluetooth: enableBluetooth,
      enable_meshcore: isMeshcoreOn,
      meshcore_advert_hours: mcAdvert,
      meshcore_relay_mode: mcRelay,
      meshcore_regions_def: mcRegions,
      meshcore_home_region: mcHome,
      meshcore_default_region: mcDef,
      nexus_master_key: masterKey,
      device_role: deviceRole,
      lora_region: loraRegion,
      lora_preset_mode: loraPreset,
      channels_preset: chPreset,
      gps_mode: gpsMode,
      fixed_latitude: fixedLat,
      fixed_longitude: fixedLon,
      fixed_altitude: fixedAlt,
      wifi_enabled: wifiOn,
      wifi_ssid: wifiSsid,
      wifi_password: wifiPassword,
      admin_key: "",
      extra_build_flags: "",
    };
  }

  // Trigger GitHub Actions workflow dispatch via REST API
  btnDispatch.addEventListener("click", async () => {
    const token = githubToken.value.trim();
    dispatchStatus.className = "status-msg status-loading";
    dispatchStatus.classList.remove("hidden");
    dispatchStatus.textContent = "Conectando con GitHub Actions...";

    if (!token) {
      dispatchStatus.className = "status-msg status-error";
      dispatchStatus.innerHTML = `
        <strong>Se requiere un GitHub Token</strong><br>
        Introduce tu Personal Access Token con permiso <code>workflow</code> para disparar la compilación,
        o utiliza el botón <em>"Abrir pestaña Actions en GitHub"</em> para ejecutarlo directamente en la web de GitHub.
      `;
      return;
    }

    const data = getFormData();
    const payload = {
      ref: DEFAULT_BRANCH,
      inputs: {
        target: data.target,
        custom_target: data.custom_target,
        owner_long_name: data.owner_long_name,
        owner_short_name: data.owner_short_name,
        has_screen: data.has_screen ? "true" : "false",
        enable_bluetooth: data.enable_bluetooth ? "true" : "false",
        enable_meshcore: data.enable_meshcore ? "true" : "false",
        meshcore_advert_hours: data.meshcore_advert_hours,
        meshcore_relay_mode: data.meshcore_relay_mode,
        meshcore_regions_def: data.meshcore_regions_def,
        meshcore_home_region: data.meshcore_home_region,
        meshcore_default_region: data.meshcore_default_region,
        nexus_master_key: data.nexus_master_key,
        device_role: data.device_role,
        lora_region: data.lora_region,
        lora_preset_mode: data.lora_preset_mode,
        channels_preset: data.channels_preset,
        gps_mode: data.gps_mode,
        fixed_latitude: data.fixed_latitude,
        fixed_longitude: data.fixed_longitude,
        fixed_altitude: data.fixed_altitude,
        wifi_enabled: data.wifi_enabled ? "true" : "false",
        wifi_ssid: data.wifi_ssid,
        wifi_password: data.wifi_password,
        admin_key: data.admin_key,
        extra_build_flags: data.extra_build_flags,
      },
    };

    try {
      const url = `https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/actions/workflows/${WORKFLOW_FILE}/dispatches`;
      const res = await fetch(url, {
        method: "POST",
        headers: {
          Accept: "application/vnd.github+json",
          Authorization: `Bearer ${token}`,
          "X-GitHub-Api-Version": "2022-11-28",
          "Content-Type": "application/json",
        },
        body: JSON.stringify(payload),
      });

      if (res.status === 204) {
        dispatchStatus.className = "status-msg status-success";
        const runUrl = `https://github.com/${REPO_OWNER}/${REPO_NAME}/actions/workflows/${WORKFLOW_FILE}`;
        dispatchStatus.innerHTML = `
          <strong>🚀 ¡Compilación iniciada con éxito!</strong><br>
          GitHub está compilando tu firmware personalizado. En 2-3 minutos estará disponible.<br>
          <a href="${runUrl}" target="_blank" style="color: #58a6ff; font-weight: 600; text-decoration: underline; margin-top: 4px; display: inline-block;">
            Ver progreso de compilación en GitHub Actions ↗
          </a>
        `;
      } else {
        const errorData = await res.json().catch(() => ({}));
        dispatchStatus.className = "status-msg status-error";
        dispatchStatus.innerHTML = `
          <strong>Error ${res.status}:</strong> ${errorData.message || res.statusText}<br>
          Verifica que tu token tenga el permiso de <code>workflow</code> y que la rama sea correcta.
        `;
      }
    } catch (err) {
      dispatchStatus.className = "status-msg status-error";
      dispatchStatus.textContent = `Error de red: ${err.message}`;
    }
  });

  // Generate & Download userPrefs.jsonc
  btnDownloadJson.addEventListener("click", () => {
    const data = getFormData();
    const prefs = {
      USERPREFS_CONFIG_DEVICE_ROLE: `meshtastic_Config_DeviceConfig_Role_${data.device_role}`,
      USERPREFS_CONFIG_LORA_REGION: `meshtastic_Config_LoRaConfig_RegionCode_${data.lora_region}`,
      USERPREFS_CONFIG_GPS_MODE: `meshtastic_Config_PositionConfig_GpsMode_${data.gps_mode}`,
    };

    // If not auto MAC name, write specific custom name
    if (!autoRandomName.checked) {
      prefs["USERPREFS_CONFIG_OWNER_LONG_NAME"] = data.owner_long_name;
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

    if (!data.enable_meshcore) {
      prefs["MESHTASTIC_ENABLE_MESHCORE_SNIFFER"] = "0";
      prefs["MESHTASTIC_ENABLE_MESHCORE_RELAY"] = "0";
      prefs["MESHTASTIC_ENABLE_MESHCORE_COMPANION"] = "0";
      prefs["MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC"] = "0";
    } else {
      if (data.meshcore_advert_hours) {
        prefs["USERPREFS_MESHCORE_ADVERT_HOURS"] = data.meshcore_advert_hours;
      }
      const modeVal =
        { AUTO: "0", OFF: "1", ON: "2" }[data.meshcore_relay_mode] || "0";
      prefs["USERPREFS_MESHCORE_RELAY_MODE"] = modeVal;
      if (data.meshcore_regions_def) {
        prefs["USERPREFS_MESHCORE_REGIONS_DEF"] = data.meshcore_regions_def;
      }
      if (data.meshcore_home_region) {
        prefs["USERPREFS_MESHCORE_HOME_REGION"] = data.meshcore_home_region;
      }
      if (data.meshcore_default_region) {
        prefs["USERPREFS_MESHCORE_DEFAULT_REGION"] =
          data.meshcore_default_region;
      }
    }

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
    } else {
      const p = data.lora_preset_mode
        .replace("modem_preset_", "")
        .toUpperCase();
      prefs["USERPREFS_LORACONFIG_USE_PRESET"] = "true";
      prefs["USERPREFS_LORACONFIG_MODEM_PRESET"] =
        `meshtastic_Config_LoRaConfig_ModemPreset_${p}`;
    }

    if (data.channels_preset === "repo_default_5_channels") {
      prefs["USERPREFS_CHANNELS_TO_WRITE"] = "5";
      prefs["USERPREFS_CHANNEL_0_NAME"] = "SFNarrow";
      prefs["USERPREFS_CHANNEL_0_PSK"] = "{ 0x01 }"; // 1 -> AQ==
      prefs["USERPREFS_CHANNEL_0_PRECISION"] = "16";
      prefs["USERPREFS_CHANNEL_0_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_0_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_0_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_1_NAME"] = "Test";
      prefs["USERPREFS_CHANNEL_1_PSK"] = "{ 0x02 }"; // 2 -> Ag==
      prefs["USERPREFS_CHANNEL_1_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_1_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_1_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_1_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_2_NAME"] = "Bots";
      prefs["USERPREFS_CHANNEL_2_PSK"] = "{ 0x02 }"; // 2 -> Ag==
      prefs["USERPREFS_CHANNEL_2_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_2_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_2_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_2_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_3_NAME"] = "Catalunya";
      prefs["USERPREFS_CHANNEL_3_PSK"] = "{ 0x01 }"; // 1 -> AQ==
      prefs["USERPREFS_CHANNEL_3_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_3_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_3_UPLINK_ENABLED"] = "false";
      prefs["USERPREFS_CHANNEL_3_DOWNLINK_ENABLED"] = "false";

      prefs["USERPREFS_CHANNEL_4_NAME"] = "Barcelona";
      prefs["USERPREFS_CHANNEL_4_PSK"] = "{ 0x01 }"; // 1 -> AQ==
      prefs["USERPREFS_CHANNEL_4_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_4_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_4_UPLINK_ENABLED"] = "false";
      prefs["USERPREFS_CHANNEL_4_DOWNLINK_ENABLED"] = "false";
    }

    if (data.fixed_latitude && data.fixed_longitude) {
      prefs["USERPREFS_FIXED_GPS"] = "1";
      prefs["USERPREFS_FIXED_GPS_LAT"] = data.fixed_latitude;
      prefs["USERPREFS_FIXED_GPS_LON"] = data.fixed_longitude;
      if (data.fixed_altitude) {
        prefs["USERPREFS_FIXED_GPS_ALT"] = data.fixed_altitude;
      }
    }

    if (data.wifi_enabled && data.wifi_ssid) {
      prefs["USERPREFS_NETWORK_WIFI_ENABLED"] = "true";
      prefs["USERPREFS_NETWORK_WIFI_SSID"] = data.wifi_ssid;
      prefs["USERPREFS_NETWORK_WIFI_PSK"] = data.wifi_password;
    }

    const blob = new Blob([JSON.stringify(prefs, null, 2)], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "userPrefs.jsonc";
    a.click();
    URL.revokeObjectURL(url);
  });
});
