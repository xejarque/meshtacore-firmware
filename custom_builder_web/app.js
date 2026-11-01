// Meshtacore Custom Firmware Builder - Web Frontend

const REPO_OWNER = "xejarque";
const REPO_NAME = "meshtacore-firmware";
const DEFAULT_BRANCH = "meshtacore";
const WORKFLOW_FILE = "custom_builder.yml";

document.addEventListener("DOMContentLoaded", () => {
  const boardTarget = document.getElementById("boardTarget");
  const customTargetGroup = document.getElementById("customTargetGroup");
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
    const ownerLongName =
      document.getElementById("ownerLongName").value.trim() ||
      "Meshtacore Node";
    const ownerShortName =
      document.getElementById("ownerShortName").value.trim() || "MTC";
    const hasScreen = document.getElementById("hasScreen").checked;
    const enableBluetooth = document.getElementById("enableBluetooth").checked;
    const enableMeshcore = document.getElementById("enableMeshcore")
      ? document.getElementById("enableMeshcore").checked
      : true;
    const deviceRole = document.getElementById("deviceRole").value;
    const loraRegion = document.getElementById("loraRegion").value;
    const loraPreset = document.getElementById("loraPreset").value;
    const channelsPreset = document.getElementById("channelsPreset").value;
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
      owner_long_name: ownerLongName.substring(0, 24),
      owner_short_name: ownerShortName.substring(0, 4),
      has_screen: hasScreen,
      enable_bluetooth: enableBluetooth,
      enable_meshcore: enableMeshcore,
      device_role: deviceRole,
      lora_region: loraRegion,
      lora_preset_mode: loraPreset,
      channels_preset: channelsPreset,
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
      USERPREFS_CONFIG_OWNER_LONG_NAME: data.owner_long_name,
      USERPREFS_CONFIG_OWNER_SHORT_NAME: data.owner_short_name,
      USERPREFS_CONFIG_DEVICE_ROLE: `meshtastic_Config_DeviceConfig_Role_${data.device_role}`,
      USERPREFS_CONFIG_LORA_REGION: `meshtastic_Config_LoRaConfig_RegionCode_${data.lora_region}`,
      USERPREFS_CONFIG_GPS_MODE: `meshtastic_Config_PositionConfig_GpsMode_${data.gps_mode}`,
    };

    if (!data.has_screen) {
      prefs["MESHTASTIC_EXCLUDE_SCREEN"] = "1";
      prefs["HAS_SCREEN"] = "0";
      prefs["USERPREFS_CONFIG_DISPLAY_SCREEN_ON_SECS"] = "1";
    }

    if (!data.enable_bluetooth) {
      prefs["MESHTASTIC_EXCLUDE_BLUETOOTH"] = "1";
      prefs["HAS_BLUETOOTH"] = "0";
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
      prefs["USERPREFS_CHANNEL_0_PSK"] = "{ 0x01 }";
      prefs["USERPREFS_CHANNEL_0_PRECISION"] = "16";
      prefs["USERPREFS_CHANNEL_0_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_0_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_0_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_1_NAME"] = "Test";
      prefs["USERPREFS_CHANNEL_1_PSK"] = "{ 0x02 }";
      prefs["USERPREFS_CHANNEL_1_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_1_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_1_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_1_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_2_NAME"] = "Bots";
      prefs["USERPREFS_CHANNEL_2_PSK"] = "{ 0x02 }";
      prefs["USERPREFS_CHANNEL_2_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_2_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_2_UPLINK_ENABLED"] = "true";
      prefs["USERPREFS_CHANNEL_2_DOWNLINK_ENABLED"] = "true";

      prefs["USERPREFS_CHANNEL_3_NAME"] = "Catalunya";
      prefs["USERPREFS_CHANNEL_3_PSK"] = "{ 0x01 }";
      prefs["USERPREFS_CHANNEL_3_PRECISION"] = "0";
      prefs["USERPREFS_CHANNEL_3_IS_MUTED"] = "false";
      prefs["USERPREFS_CHANNEL_3_UPLINK_ENABLED"] = "false";
      prefs["USERPREFS_CHANNEL_3_DOWNLINK_ENABLED"] = "false";

      prefs["USERPREFS_CHANNEL_4_NAME"] = "Barcelona";
      prefs["USERPREFS_CHANNEL_4_PSK"] = "{ 0x01 }";
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
