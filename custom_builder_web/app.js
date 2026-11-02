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
    psk: "AQ==",
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
    psk: "Ag==",
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
    psk: "Ag==",
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
    psk: "AQ==",
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
    psk: "AQ==",
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
  const btnBuildViaIssue = document.getElementById("btnBuildViaIssue");
  const issueStatus = document.getElementById("issueStatus");

  // Timezone UI Elements
  const timezoneString = document.getElementById("timezoneString");
  const tzBadge = document.getElementById("tzBadge");
  const tzHelper = document.getElementById("tzHelper");
  const btnTzPeninsula = document.getElementById("btnTzPeninsula");
  const btnTzCanarias = document.getElementById("btnTzCanarias");
  const btnTzUtc = document.getElementById("btnTzUtc");

  // Custom LoRa UI Elements
  const loraPreset = document.getElementById("loraPreset");
  const btnToggleCustomLora = document.getElementById("btnToggleCustomLora");
  const customLoraContainer = document.getElementById("customLoraContainer");
  const loraOverrideFreq = document.getElementById("loraOverrideFreq");
  const loraBandwidth = document.getElementById("loraBandwidth");
  const loraSpreadFactor = document.getElementById("loraSpreadFactor");
  const loraCodingRate = document.getElementById("loraCodingRate");
  const loraTxPower = document.getElementById("loraTxPower");
  const loraHopLimit = document.getElementById("loraHopLimit");
  const loraChannelNum = document.getElementById("loraChannelNum");
  const loraOkToMqtt = document.getElementById("loraOkToMqtt");
  const loraIgnoreMqtt = document.getElementById("loraIgnoreMqtt");

  // Advanced Behavior UI Elements
  const rebroadcastMode = document.getElementById("rebroadcastMode");
  const fixedBlePin = document.getElementById("fixedBlePin");
  const nodeInfoBroadcastSecs = document.getElementById(
    "nodeInfoBroadcastSecs",
  );
  const positionBroadcastInterval = document.getElementById(
    "positionBroadcastInterval",
  );
  const deviceTelemInterval = document.getElementById("deviceTelemInterval");
  const smartPosition = document.getElementById("smartPosition");

  // Live JSON preview textarea
  const prefsJsonPreview = document.getElementById("prefsJsonPreview");

  // Timezone Helpers & Auto-Detection
  function setTimezone(val, label, isManual = false) {
    if (timezoneString) timezoneString.value = val;
    if (tzBadge) {
      tzBadge.textContent = isManual ? `🔧 Manual: ${label}` : `🌍 ${label}`;
    }
    if (tzHelper) {
      tzHelper.textContent = isManual
        ? `Configuración manual seleccionada: ${label}.`
        : `Detectada automáticamente: ${label}. Ajusta el reloj de hardware y pantalla del nodo.`;
    }
    if (typeof updateLivePreview === "function") updateLivePreview();
  }

  if (btnTzPeninsula) {
    btnTzPeninsula.addEventListener("click", () => {
      setTimezone("CET-1CEST,M3.5.0,M10.5.0/3", "Península (CET)", true);
    });
  }
  if (btnTzCanarias) {
    btnTzCanarias.addEventListener("click", () => {
      setTimezone("WET0WEST,M3.5.0/1,M10.5.0", "Canarias (WET)", true);
    });
  }
  if (btnTzUtc) {
    btnTzUtc.addEventListener("click", () => {
      setTimezone("UTC0", "UTC", true);
    });
  }

  function detectInitialTimezone() {
    try {
      const iana = Intl.DateTimeFormat().resolvedOptions().timeZone || "";
      const lower = iana.toLowerCase();
      if (lower.includes("canary")) {
        setTimezone(
          "WET0WEST,M3.5.0/1,M10.5.0",
          "Canarias (Detectada por navegador)",
        );
      } else if (
        iana.includes("Madrid") ||
        iana.includes("Paris") ||
        iana.includes("Berlin") ||
        iana.includes("Rome") ||
        iana.includes("Brussels") ||
        iana.includes("Amsterdam")
      ) {
        setTimezone(
          "CET-1CEST,M3.5.0,M10.5.0/3",
          "Península / Europa Central (CET)",
        );
      } else if (
        iana.includes("London") ||
        iana.includes("Lisbon") ||
        iana.includes("Dublin")
      ) {
        setTimezone("GMT0BST,M3.5.0/1,M10.5.0", "Reino Unido / Portugal (GMT)");
      } else if (iana.includes("America/Buenos_Aires")) {
        setTimezone("ART3", "Argentina (ART)");
      } else if (iana.includes("America/Mexico")) {
        setTimezone("CST6", "México (CST)");
      } else if (iana.includes("UTC") || iana.includes("GMT")) {
        setTimezone("UTC0", "UTC");
      } else {
        setTimezone("CET-1CEST,M3.5.0,M10.5.0/3", `Navegador: ${iana}`);
      }
    } catch (e) {
      setTimezone("CET-1CEST,M3.5.0,M10.5.0/3", "Península (CET)");
    }
  }
  detectInitialTimezone();

  function syncTimezoneWithCoords(lat, lon) {
    if (lat >= 27.0 && lat <= 29.8 && lon >= -18.5 && lon <= -13.0) {
      setTimezone(
        "WET0WEST,M3.5.0/1,M10.5.0",
        "Canarias (Detectada por ubicación)",
      );
    } else if (lat >= 35.0 && lat <= 44.5 && lon >= -9.5 && lon <= 4.5) {
      setTimezone(
        "CET-1CEST,M3.5.0,M10.5.0/3",
        "Península (Detectada por ubicación)",
      );
    }
  }

  // Custom LoRa Container Toggle
  function syncCustomLoraVisibility() {
    if (loraPreset.value === "custom") {
      customLoraContainer.classList.remove("hidden");
    } else {
      customLoraContainer.classList.add("hidden");
    }
    if (typeof updateLivePreview === "function") updateLivePreview();
  }

  if (loraPreset) {
    loraPreset.addEventListener("change", syncCustomLoraVisibility);
  }

  if (btnToggleCustomLora) {
    btnToggleCustomLora.addEventListener("click", () => {
      if (customLoraContainer.classList.contains("hidden")) {
        loraPreset.value = "custom";
        customLoraContainer.classList.remove("hidden");
      } else {
        customLoraContainer.classList.add("hidden");
        if (loraPreset.value === "custom") {
          loraPreset.value = "custom_sfnarrow_default";
        }
      }
      if (typeof updateLivePreview === "function") updateLivePreview();
    });
  }

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
          <input type="text" class="ch-input font-mono ch-field-psk" data-idx="${idx}" value="${ch.psk}" placeholder="AQ== / Ag== / Clave" title="Clave en Base64 o Hexadecimal" />
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
    syncTimezoneWithCoords(lat, lon);
    if (typeof updateLivePreview === "function") updateLivePreview();
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
      syncTimezoneWithCoords(lat, lon);
      if (typeof updateLivePreview === "function") updateLivePreview();
    }
  }

  fixedLat.addEventListener("input", syncMapFromInputs);
  fixedLon.addEventListener("input", syncMapFromInputs);

  async function fetchIpLocation() {
    mapStatus.innerHTML = "🌐 Consultando ubicación aproximada por red/IP...";
    try {
      const res = await fetch("https://ipwho.is/");
      const data = await res.json();
      if (data && data.success && data.latitude && data.longitude) {
        const lat = data.latitude;
        const lon = data.longitude;
        fixedLat.value = lat.toFixed(6);
        fixedLon.value = lon.toFixed(6);
        if (map && marker) {
          if (!map.hasLayer(marker)) marker.addTo(map);
          marker.setLatLng([lat, lon]);
          map.setView([lat, lon], 13);
        }
        const cityStr = [data.city, data.region].filter(Boolean).join(", ");
        mapStatus.innerHTML = `📍 Ubicación detectada por red (${cityStr || "Aproximada"}): <strong>${lat.toFixed(6)}, ${lon.toFixed(6)}</strong>.<br><small style="color: #79c0ff;">El navegador o el sistema denegó el GPS exacto. Puedes afinar la posición haciendo clic o arrastrando el marcador en el mapa.</small>`;
        syncTimezoneWithCoords(lat, lon);
        if (
          data.timezone &&
          data.timezone.id &&
          data.timezone.id.toLowerCase().includes("canary")
        ) {
          setTimezone(
            "WET0WEST,M3.5.0/1,M10.5.0",
            "Canarias (Detectada por IP)",
          );
        }
        if (typeof updateLivePreview === "function") updateLivePreview();
      } else {
        throw new Error(data.message || "Sin respuesta");
      }
    } catch (err) {
      mapStatus.innerHTML = `⚠️ No se pudo obtener la ubicación automática (${err.message}). Puedes hacer clic directamente en cualquier punto del mapa para fijar tu posición.`;
    }
  }

  btnGeolocate.addEventListener("click", () => {
    mapStatus.textContent = "Obteniendo ubicación del dispositivo...";
    if (!navigator.geolocation) {
      fetchIpLocation();
      return;
    }

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
        mapStatus.innerHTML = `✅ Ubicación detectada vía GPS/WiFi: <strong>${lat.toFixed(6)}, ${lon.toFixed(6)}</strong>${alt ? ` (Alt: ${alt}m)` : ""}`;
        syncTimezoneWithCoords(lat, lon);
        if (typeof updateLivePreview === "function") updateLivePreview();
      },
      (err) => {
        console.warn("navigator.geolocation falló, recurriendo a IP:", err);
        fetchIpLocation();
      },
      { enableHighAccuracy: false, timeout: 8000, maximumAge: 300000 },
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
    if (typeof updateLivePreview === "function") updateLivePreview();
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
    const loraPresetVal = document.getElementById("loraPreset").value;
    const gpsMode = document.getElementById("gpsMode").value;
    const lat = fixedLat.value.trim();
    const lon = fixedLon.value.trim();
    const alt = fixedAlt.value.trim();

    const tzStr = timezoneString
      ? timezoneString.value.trim()
      : "CET-1CEST,M3.5.0,M10.5.0/3";

    // Custom LoRa fields
    const freqVal = loraOverrideFreq
      ? loraOverrideFreq.value.trim()
      : "869.618";
    const bwVal = loraBandwidth ? loraBandwidth.value : "62";
    const sfVal = loraSpreadFactor ? loraSpreadFactor.value : "7";
    const crVal = loraCodingRate ? loraCodingRate.value : "6";
    const txVal = loraTxPower ? loraTxPower.value.trim() : "22";
    const hopVal = loraHopLimit ? loraHopLimit.value.trim() : "7";
    const chNumVal = loraChannelNum ? loraChannelNum.value.trim() : "0";
    const okMqttVal = loraOkToMqtt ? loraOkToMqtt.checked : true;
    const ignMqttVal = loraIgnoreMqtt ? loraIgnoreMqtt.checked : false;

    // Advanced behavior
    const rebroadcastVal = rebroadcastMode ? rebroadcastMode.value : "ALL";
    const blePinVal = fixedBlePin ? fixedBlePin.value.trim() : "";
    const nodeInfoSecsVal = nodeInfoBroadcastSecs
      ? nodeInfoBroadcastSecs.value.trim()
      : "10800";
    const posSecsVal = positionBroadcastInterval
      ? positionBroadcastInterval.value.trim()
      : "1800";
    const telemSecsVal = deviceTelemInterval
      ? deviceTelemInterval.value.trim()
      : "900";
    const smartPosVal = smartPosition ? smartPosition.checked : false;

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
      enable_meshcore: true,
      meshcore_advert_hours: mcAdvert,
      meshcore_relay_mode: mcRelay,
      meshcore_regions_def: mcRegions,
      meshcore_home_region: mcHome,
      meshcore_default_region: mcDef,
      nexus_master_key: masterKey,
      device_role: deviceRole,
      lora_region: loraRegion,
      lora_preset_mode: loraPresetVal,
      lora_override_freq: freqVal,
      lora_bandwidth: bwVal,
      lora_spread_factor: sfVal,
      lora_coding_rate: crVal,
      lora_tx_power: txVal,
      lora_hop_limit: hopVal,
      lora_channel_num: chNumVal,
      lora_ok_to_mqtt: okMqttVal,
      lora_ignore_mqtt: ignMqttVal,
      timezone_string: tzStr,
      rebroadcast_mode: rebroadcastVal,
      fixed_ble_pin: blePinVal,
      node_info_broadcast_secs: nodeInfoSecsVal,
      position_broadcast_interval: posSecsVal,
      device_telem_interval: telemSecsVal,
      smart_position: smartPosVal,
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

  // 1-Click Build via GitHub Issue Bot (No token needed, works for any user)
  if (btnBuildViaIssue) {
    btnBuildViaIssue.addEventListener("click", () => {
      const data = getFormData();
      const prefs = buildUserPrefs(data);

      const issueTitle = `[Build Request] ${data.target} - ${data.owner_short_name || "Node"}`;

      const configPayload = {
        target: data.target,
        custom_target: data.custom_target,
        owner_long_name: data.owner_long_name,
        owner_short_name: data.owner_short_name,
        has_screen: data.has_screen,
        enable_bluetooth: data.enable_bluetooth,
        device_role: data.device_role,
        lora_region: data.lora_region,
        lora_preset_mode: data.lora_preset_mode,
        timezone_string: data.timezone_string,
        custom_userprefs: prefs,
      };

      const issueBody = `### ⚙️ Solicitud de Compilación Automática de Firmware Meshtacore

- **Placa Objetivo:** \`${data.target}\`
- **Nombre del Nodo:** \`${data.owner_long_name}\` (\`${data.owner_short_name}\`)
- **Zona Horaria:** \`${data.timezone_string}\`
- **Rol:** \`${data.device_role}\` | **Región LoRa:** \`${data.lora_region}\`

\`\`\`json:meshtacore-build-config
${JSON.stringify(configPayload, null, 2)}
\`\`\`

> ℹ️ _Pulsa **"Submit new issue"** abajo para que el bot de GitHub Actions empiece a compilar tu firmware y te adjunte el enlace de descarga en este mismo hilo._`;

      const url = `https://github.com/${REPO_OWNER}/${REPO_NAME}/issues/new?title=${encodeURIComponent(issueTitle)}&body=${encodeURIComponent(issueBody)}`;

      if (issueStatus) {
        issueStatus.className = "status-msg status-success";
        issueStatus.classList.remove("hidden");
        issueStatus.innerHTML = `
          <strong>🚀 Redirigiendo a GitHub...</strong><br>
          Se abrirá una nueva pestaña con tu solicitud preconfigurada. Haz clic en <em>"Submit new issue"</em> para iniciar la compilación automática.<br>
          <a href="${url}" target="_blank" style="color: #58a6ff; font-weight: 600; text-decoration: underline; margin-top: 6px; display: inline-block;">
            Abrir solicitud de compilación en GitHub ↗
          </a>
        `;
      }

      window.open(url, "_blank");
    });
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
    const prefs = buildUserPrefs(data);

    const payload = {
      ref: DEFAULT_BRANCH,
      inputs: {
        target: data.target,
        custom_target: data.custom_target,
        owner_long_name: data.owner_long_name,
        owner_short_name: data.owner_short_name,
        has_screen: data.has_screen ? "true" : "false",
        device_role: data.device_role,
        lora_region: data.lora_region,
        lora_preset_mode: data.lora_preset_mode,
        channels_preset: data.channels_preset,
        enable_bluetooth: data.enable_bluetooth ? "true" : "false",
        enable_meshcore: data.enable_meshcore ? "true" : "false",
        nexus_master_key: data.nexus_master_key,
        mqtt_address: data.mqtt_address,
        gps_mode: data.gps_mode,
        fixed_latitude: data.fixed_latitude,
        fixed_longitude: data.fixed_longitude,
        fixed_altitude: data.fixed_altitude,
        wifi_ssid: data.wifi_ssid,
        wifi_password: data.wifi_password,
        custom_userprefs_json: JSON.stringify(prefs),
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

  function buildUserPrefs(data) {
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

    // MeshCore is always enabled in Meshtacore firmware
    if (data.meshcore_advert_hours) {
      prefs["USERPREFS_MESHCORE_ADVERT_HOURS"] = data.meshcore_advert_hours;
    }
    const modeVal =
      { AUTO: "0", OFF: "1", ON: "2" }[data.meshcore_relay_mode] || "0";
    prefs["USERPREFS_MESHCORE_RELAY_MODE"] = modeVal;

    // Regions: only define if non-empty (empty = all permitted)
    if (data.meshcore_regions_def && data.meshcore_regions_def.trim()) {
      prefs["USERPREFS_MESHCORE_REGIONS_DEF"] =
        data.meshcore_regions_def.trim();
    }
    if (data.meshcore_home_region && data.meshcore_home_region.trim()) {
      prefs["USERPREFS_MESHCORE_HOME_REGION"] =
        data.meshcore_home_region.trim();
    }
    if (data.meshcore_default_region && data.meshcore_default_region.trim()) {
      prefs["USERPREFS_MESHCORE_DEFAULT_REGION"] =
        data.meshcore_default_region.trim();
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
      if (data.lora_ok_to_mqtt) {
        prefs["USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT"] = "true";
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

    // Timezone configuration (Auto-detected or custom POSIX)
    if (data.timezone_string) {
      prefs["USERPREFS_TZ_STRING"] = data.timezone_string;
    }

    // Advanced behavior options (Rebroadcast, intervals, BLE PIN)
    if (data.rebroadcast_mode) {
      prefs["USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE"] =
        `meshtastic_Config_DeviceConfig_RebroadcastMode_${data.rebroadcast_mode}`;
    }
    if (data.fixed_ble_pin) {
      prefs["USERPREFS_FIXED_BLUETOOTH"] = data.fixed_ble_pin;
    }
    if (data.node_info_broadcast_secs) {
      prefs["USERPREFS_CONFIG_DEVICE_NODE_INFO_BROADCAST_SECS"] =
        data.node_info_broadcast_secs;
    }
    if (data.position_broadcast_interval) {
      prefs["USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL"] =
        data.position_broadcast_interval;
    }
    if (data.device_telem_interval) {
      prefs["USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL"] =
        data.device_telem_interval;
    }
    if (data.smart_position) {
      prefs["USERPREFS_CONFIG_SMART_POSITION_ENABLED"] = "true";
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

    return prefs;
  }

  // Live preview updater
  function updateLivePreview() {
    if (!prefsJsonPreview) return;
    try {
      const data = getFormData();
      const prefs = buildUserPrefs(data);
      prefsJsonPreview.value = JSON.stringify(prefs, null, 2);
    } catch (err) {
      console.warn("Error generando previsualización JSON:", err);
    }
  }

  // Hook all inputs in form for live preview
  const formEl = document.getElementById("builderForm");
  if (formEl) {
    formEl.addEventListener("input", updateLivePreview);
    formEl.addEventListener("change", updateLivePreview);
  }

  // Hook channel table events to also update live preview
  const originalRenderChannelsTable = renderChannelsTable;
  renderChannelsTable = function () {
    originalRenderChannelsTable();
    updateLivePreview();
  };

  // Generate & Download userPrefs.jsonc
  btnDownloadJson.addEventListener("click", () => {
    const data = getFormData();
    const prefs = buildUserPrefs(data);

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

  // Initial preview render
  updateLivePreview();
});
