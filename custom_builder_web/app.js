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

// Generates random 4-character hex suffix (e.g. "A3DC")
function getRandomHex(len = 4) {
  const chars = "0123456789ABCDEF";
  let res = "";
  for (let i = 0; i < len; i++) {
    res += chars[Math.floor(Math.random() * 16)];
  }
  return res;
}

// Convert any PSK format (1, 2, AQ==, Base64, Hex) to C byte array format
function formatPskToCArray(pskVal) {
  const val = String(pskVal || "").trim();
  if (!val) return "{ 0x01 }";
  if (val.startsWith("{") && val.endsWith("}")) return val;
  if (/^\d+$/.test(val)) {
    const num = parseInt(val, 10);
    return `{ 0x${num.toString(16).padStart(2, "0")} }`;
  }
  if (val.startsWith("0x") || val.startsWith("0X")) {
    const hex = val.slice(2).replace(/[^0-9a-fA-F]/g, "");
    const bytes = [];
    for (let i = 0; i < hex.length; i += 2) {
      bytes.push("0x" + hex.substring(i, i + 2));
    }
    return "{ " + bytes.join(", ") + " }";
  }
  try {
    const binStr = atob(val);
    const bytes = [];
    for (let i = 0; i < binStr.length; i++) {
      bytes.push("0x" + binStr.charCodeAt(i).toString(16).padStart(2, "0"));
    }
    return "{ " + bytes.join(", ") + " }";
  } catch (e) {
    return "{ 0x01 }";
  }
}

// Default Spanish community channel matrix
// Uplink enabled ONLY for Meshtastic channels by default, Downlink disabled
const DEFAULT_CHANNELS = [
  {
    slot: 0,
    enabled: true,
    name: "SFNarrow",
    psk: "1",
    uplink: true,
    downlink: false,
    precision: 16,
    tag: "Primario",
    tagClass: "tag-primary",
  },
  {
    slot: 1,
    enabled: true,
    name: "Test",
    psk: "2",
    uplink: true,
    downlink: false,
    precision: 0,
    tag: "Secundario",
    tagClass: "tag-secondary",
  },
  {
    slot: 2,
    enabled: true,
    name: "Bots",
    psk: "2",
    uplink: true,
    downlink: false,
    precision: 0,
    tag: "Secundario",
    tagClass: "tag-secondary",
  },
  {
    slot: 3,
    enabled: true,
    name: "Catalunya",
    psk: "1",
    uplink: true,
    downlink: false,
    precision: 0,
    tag: "Secundario",
    tagClass: "tag-secondary",
  },
  {
    slot: 4,
    enabled: true,
    name: "Barcelona",
    psk: "1",
    uplink: true,
    downlink: false,
    precision: 0,
    tag: "Secundario",
    tagClass: "tag-secondary",
  },
  {
    slot: 5,
    enabled: true,
    name: "MC:Public",
    psk: "AQ==",
    uplink: false,
    downlink: false,
    precision: 0,
    tag: "MeshCore",
    tagClass: "tag-mc",
  },
  {
    slot: 6,
    enabled: true,
    name: "MC:#test",
    psk: "AQ==",
    uplink: false,
    downlink: false,
    precision: 0,
    tag: "MeshCore",
    tagClass: "tag-mc",
  },
  {
    slot: 7,
    enabled: true,
    name: "MC:#bot",
    psk: "AQ==",
    uplink: false,
    downlink: false,
    precision: 0,
    tag: "MeshCore",
    tagClass: "tag-mc",
  },
];

let channelsData = JSON.parse(JSON.stringify(DEFAULT_CHANNELS));

document.addEventListener("DOMContentLoaded", () => {
  const boardTarget = document.getElementById("boardTarget");
  const customTargetGroup = document.getElementById("customTargetGroup");
  const ownerLongName = document.getElementById("ownerLongName");
  const ownerShortName = document.getElementById("ownerShortName");
  const btnRollRandomName = document.getElementById("btnRollRandomName");
  const enableMeshcore = document.getElementById("enableMeshcore");
  const meshcoreSettings = document.getElementById("meshcoreSettings");
  const nexusMasterKey = document.getElementById("nexusMasterKey");
  const customMasterKeyGroup = document.getElementById("customMasterKeyGroup");
  const customMasterKey = document.getElementById("customMasterKey");
  const channelsTableBody = document.getElementById("channelsTableBody");
  const btnResetChannels = document.getElementById("btnResetChannels");
  const btnSingleChannel = document.getElementById("btnSingleChannel");
  const mqttEnabled = document.getElementById("mqttEnabled");
  const mqttFields = document.getElementById("mqttFields");
  const fixedLat = document.getElementById("fixedLat");
  const fixedLon = document.getElementById("fixedLon");
  const fixedAlt = document.getElementById("fixedAlt");
  const btnGeolocate = document.getElementById("btnGeolocate");
  const btnClearLocation = document.getElementById("btnClearLocation");
  const mapStatus = document.getElementById("mapStatus");
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

  // Roll initial random name on load
  function rollRandomName() {
    const hex = getRandomHex(4);
    ownerLongName.value = `Meshtacore ${hex}`;
    ownerShortName.value = hex;
  }
  rollRandomName();

  btnRollRandomName.addEventListener("click", rollRandomName);

  // Toggle custom board target input
  boardTarget.addEventListener("change", () => {
    if (boardTarget.value === "custom") {
      customTargetGroup.classList.remove("hidden");
    } else {
      customTargetGroup.classList.add("hidden");
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

  // Toggle MQTT fields
  mqttEnabled.addEventListener("change", () => {
    if (mqttEnabled.checked) {
      mqttFields.classList.remove("hidden");
    } else {
      mqttFields.classList.add("hidden");
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

  // Render Editable Channels Table
  function renderChannelsTable() {
    channelsTableBody.innerHTML = "";
    channelsData.forEach((ch, idx) => {
      const tr = document.createElement("tr");
      if (!ch.enabled) tr.classList.add("ch-disabled");

      tr.innerHTML = `
        <td style="text-align: center;">
          <input type="checkbox" class="ch-checkbox ch-toggle-enable" data-idx="${idx}" ${ch.enabled ? "checked" : ""} />
        </td>
        <td>
          <code>Ch ${ch.slot}</code>
          <span class="tag ${ch.tagClass}">${ch.tag}</span>
        </td>
        <td>
          <input type="text" class="ch-input ch-field-name" data-idx="${idx}" value="${ch.name}" maxlength="11" placeholder="Nombre" />
        </td>
        <td>
          <input type="text" class="ch-input font-mono ch-field-psk" data-idx="${idx}" value="${ch.psk}" placeholder="1 / AQ== / Key" title="Índice (1 o 2) o Base64/Hex" />
        </td>
        <td style="text-align: center;">
          <input type="checkbox" class="ch-checkbox ch-field-uplink" data-idx="${idx}" ${ch.uplink ? "checked" : ""} />
        </td>
        <td style="text-align: center;">
          <input type="checkbox" class="ch-checkbox ch-field-downlink" data-idx="${idx}" ${ch.downlink ? "checked" : ""} />
        </td>
        <td>
          <select class="ch-input ch-field-precision" data-idx="${idx}">
            <option value="16" ${ch.precision === 16 ? "selected" : ""}>16 (~1.5m)</option>
            <option value="14" ${ch.precision === 14 ? "selected" : ""}>14 (~6m)</option>
            <option value="12" ${ch.precision === 12 ? "selected" : ""}>12 (~25m)</option>
            <option value="10" ${ch.precision === 10 ? "selected" : ""}>10 (~100m)</option>
            <option value="0" ${ch.precision === 0 ? "selected" : ""}>0 (Sin pos)</option>
          </select>
        </td>
      `;

      channelsTableBody.appendChild(tr);
    });

    // Wire table event listeners
    channelsTableBody.querySelectorAll(".ch-toggle-enable").forEach((el) => {
      el.addEventListener("change", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].enabled = e.target.checked;
        const row = e.target.closest("tr");
        if (e.target.checked) row.classList.remove("ch-disabled");
        else row.classList.add("ch-disabled");
      });
    });

    channelsTableBody.querySelectorAll(".ch-field-name").forEach((el) => {
      el.addEventListener("input", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].name = e.target.value.trim();
      });
    });

    channelsTableBody.querySelectorAll(".ch-field-psk").forEach((el) => {
      el.addEventListener("input", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].psk = e.target.value.trim();
      });
    });

    channelsTableBody.querySelectorAll(".ch-field-uplink").forEach((el) => {
      el.addEventListener("change", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].uplink = e.target.checked;
      });
    });

    channelsTableBody.querySelectorAll(".ch-field-downlink").forEach((el) => {
      el.addEventListener("change", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].downlink = e.target.checked;
      });
    });

    channelsTableBody.querySelectorAll(".ch-field-precision").forEach((el) => {
      el.addEventListener("change", (e) => {
        const i = parseInt(e.target.dataset.idx, 10);
        channelsData[i].precision = parseInt(e.target.value, 10);
      });
    });
  }

  renderChannelsTable();

  // Reset to Spanish defaults
  btnResetChannels.addEventListener("click", () => {
    channelsData = JSON.parse(JSON.stringify(DEFAULT_CHANNELS));
    renderChannelsTable();
  });

  // Configure single channel only (Ch 0)
  btnSingleChannel.addEventListener("click", () => {
    channelsData.forEach((ch, idx) => {
      ch.enabled = idx === 0;
    });
    renderChannelsTable();
  });

  // --- Interactive Leaflet Map & Geolocation ---
  let map = null;
  let marker = null;
  const SPAIN_CENTER = [40.4168, -3.7038];

  function initMap() {
    const mapContainer = document.getElementById("locationMap");
    if (window.L && mapContainer) {
      map = L.map("locationMap").setView(SPAIN_CENTER, 6);

      L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19,
        attribution: "© OpenStreetMap",
      }).addTo(map);

      marker = L.marker(SPAIN_CENTER, { draggable: true });

      marker.on("dragend", (e) => {
        const pos = e.target.getLatLng();
        setCoordsFromMap(pos.lat, pos.lng);
      });

      map.on("click", (e) => {
        if (!map.hasLayer(marker)) {
          marker.addTo(map);
        }
        marker.setLatLng(e.latlng);
        setCoordsFromMap(e.latlng.lat, e.latlng.lng);
      });
    }
  }

  function setCoordsFromMap(lat, lon) {
    fixedLat.value = lat.toFixed(6);
    fixedLon.value = lon.toFixed(6);
    mapStatus.innerHTML = `📍 Coordenadas seleccionadas: <strong>${lat.toFixed(6)}, ${lon.toFixed(6)}</strong>`;
  }

  function syncMapFromInputs() {
    const lat = parseFloat(fixedLat.value);
    const lon = parseFloat(fixedLon.value);
    if (!isNaN(lat) && !isNaN(lon) && map && marker) {
      if (!map.hasLayer(marker)) {
        marker.addTo(map);
      }
      marker.setLatLng([lat, lon]);
      map.setView([lat, lon], 14);
      mapStatus.innerHTML = `📍 Coordenadas seleccionadas: <strong>${lat.toFixed(6)}, ${lon.toFixed(6)}</strong>`;
    }
  }

  fixedLat.addEventListener("input", syncMapFromInputs);
  fixedLon.addEventListener("input", syncMapFromInputs);

  btnGeolocate.addEventListener("click", () => {
    if (!navigator.geolocation) {
      alert("Tu navegador no soporta la API de geolocalización.");
      return;
    }
    mapStatus.textContent = "Obteniendo ubicación actual vía GPS/WiFi...";
    navigator.geolocation.getCurrentPosition(
      (pos) => {
        const lat = pos.coords.latitude;
        const lon = pos.coords.longitude;
        const alt = pos.coords.altitude
          ? Math.round(pos.coords.altitude)
          : null;

        fixedLat.value = lat.toFixed(6);
        fixedLon.value = lon.toFixed(6);
        if (alt !== null) {
          fixedAlt.value = alt;
        }

        if (map && marker) {
          if (!map.hasLayer(marker)) marker.addTo(map);
          marker.setLatLng([lat, lon]);
          map.setView([lat, lon], 15);
        }
        mapStatus.innerHTML = `✅ Ubicación detectada: <strong>${lat.toFixed(6)}, ${lon.toFixed(6)}</strong>${alt ? ` (Alt: ${alt}m)` : ""}`;
      },
      (err) => {
        mapStatus.innerHTML = `⚠️ Error al obtener ubicación: ${err.message}`;
      },
      { enableHighAccuracy: true, timeout: 10000 },
    );
  });

  btnClearLocation.addEventListener("click", () => {
    fixedLat.value = "";
    fixedLon.value = "";
    fixedAlt.value = "";
    if (map && marker && map.hasLayer(marker)) {
      map.removeLayer(marker);
      map.setView(SPAIN_CENTER, 6);
    }
    mapStatus.textContent =
      "Haz clic o arrastra el marcador para fijar la posición exacta del nodo.";
  });

  // Initialize map
  initMap();

  // Open Actions in GitHub
  btnOpenActions.addEventListener("click", () => {
    const url = `https://github.com/${REPO_OWNER}/${REPO_NAME}/actions/workflows/${WORKFLOW_FILE}`;
    window.open(url, "_blank");
  });

  // Collect form inputs
  function getFormData() {
    const target = boardTarget.value;
    const customTarget = document.getElementById("customTarget").value.trim();
    const longName = ownerLongName.value.trim();
    const shortName = ownerShortName.value.trim();
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
    const gpsMode = document.getElementById("gpsMode").value;
    const lat = fixedLat.value.trim();
    const lon = fixedLon.value.trim();
    const alt = fixedAlt.value.trim();

    const isMqttOn = mqttEnabled.checked;
    const mAddress = document.getElementById("mqttAddress").value.trim();
    const mRoot = document.getElementById("mqttRoot").value.trim();
    const mUser = document.getElementById("mqttUsername").value.trim();
    const mPass = document.getElementById("mqttPassword").value.trim();
    const mEnc = document.getElementById("mqttEncryption").checked;
    const mTls = document.getElementById("mqttTls").checked;

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
      channels_preset: "repo_default_5_channels",
      channels_json: JSON.stringify(channelsData),
      mqtt_enabled: isMqttOn,
      mqtt_address: mAddress || "mqtt.meshtastic.es",
      mqtt_root: mRoot || "msh",
      mqtt_username: mUser || "meshdev",
      mqtt_password: mPass || "large4cats",
      mqtt_encryption_enabled: mEnc,
      mqtt_tls_enabled: mTls,
      gps_mode: gpsMode,
      fixed_latitude: lat,
      fixed_longitude: lon,
      fixed_altitude: alt,
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
        channels_json: data.channels_json,
        mqtt_enabled: data.mqtt_enabled ? "true" : "false",
        mqtt_address: data.mqtt_address,
        mqtt_username: data.mqtt_username,
        mqtt_password: data.mqtt_password,
        mqtt_root: data.mqtt_root,
        mqtt_encryption_enabled: data.mqtt_encryption_enabled
          ? "true"
          : "false",
        mqtt_tls_enabled: data.mqtt_tls_enabled ? "true" : "false",
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
          GitHub Actions está compilando tu firmware para la placa <strong>${data.target}</strong>.<br>
          <a href="${runUrl}" target="_blank" style="color: #58a6ff; font-weight: 600; text-decoration: underline; margin-top: 6px; display: inline-block;">
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

    // Write channels
    const activeChannels = channelsData.filter((c) => c.enabled);
    if (activeChannels.length > 0) {
      prefs["USERPREFS_CHANNELS_TO_WRITE"] = String(
        Math.min(activeChannels.length, 8),
      );
      activeChannels.slice(0, 8).forEach((ch, idx) => {
        prefs[`USERPREFS_CHANNEL_${idx}_NAME`] = ch.name;
        prefs[`USERPREFS_CHANNEL_${idx}_PSK`] = formatPskToCArray(ch.psk);
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

    // MQTT
    if (data.mqtt_enabled) {
      prefs["USERPREFS_MQTT_ENABLED"] = "1";
      prefs["USERPREFS_MQTT_ADDRESS"] =
        data.mqtt_address || "mqtt.meshtastic.es";
      prefs["USERPREFS_MQTT_ROOT_TOPIC"] = data.mqtt_root || "msh";
      prefs["USERPREFS_MQTT_USERNAME"] = data.mqtt_username || "meshdev";
      prefs["USERPREFS_MQTT_PASSWORD"] = data.mqtt_password || "large4cats";
      prefs["USERPREFS_MQTT_ENCRYPTION_ENABLED"] = data.mqtt_encryption_enabled
        ? "true"
        : "false";
      prefs["USERPREFS_MQTT_TLS_ENABLED"] = data.mqtt_tls_enabled
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
