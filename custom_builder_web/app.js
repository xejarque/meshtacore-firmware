// Meshtacore Custom Firmware Builder - Web Frontend

const REPO_OWNER = "xejarque";
const REPO_NAME = "meshtacore-firmware";
const DEFAULT_BRANCH = "main";
const WORKFLOW_FILE = "custom_builder.yml";
// GitHub answers 414 to the new-issue URL beyond roughly 8 KB.
const MAX_ISSUE_URL_LENGTH = 8000;

// Generates random 4-character hex suffix (e.g. "A3DC")
function getRandomHex(len = 4) {
  const chars = "0123456789ABCDEF";
  let res = "";
  for (let i = 0; i < len; i++) {
    res += chars[Math.floor(Math.random() * 16)];
  }
  return res;
}

function escapeAttr(text) {
  return String(text).replace(
    /[&<>"']/g,
    (c) =>
      ({
        "&": "&amp;",
        "<": "&lt;",
        ">": "&gt;",
        '"': "&quot;",
        "'": "&#39;",
      })[c],
  );
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
  const mqttProxy = document.getElementById("mqttProxy");
  const mqttNoNetworkNotice = document.getElementById("mqttNoNetworkNotice");
  const wifiNoNetworkNotice = document.getElementById("wifiNoNetworkNotice");
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

  // Preset Selector UI Elements
  const presetCompanion = document.getElementById("presetCompanion");
  const presetSolarRepeater = document.getElementById("presetSolarRepeater");
  const presetCompanionLabel = document.getElementById("presetCompanionLabel");
  const presetSolarRepeaterLabel = document.getElementById(
    "presetSolarRepeaterLabel",
  );
  const solarRepeaterOptions = document.getElementById("solarRepeaterOptions");
  const solarIncludeGps = document.getElementById("solarIncludeGps");
  const solarRadioDriver = document.getElementById("solarRadioDriver");
  const hasScreen = document.getElementById("hasScreen");
  const enableBluetooth = document.getElementById("enableBluetooth");
  const gpsMode = document.getElementById("gpsMode");

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
  const deviceRole = document.getElementById("deviceRole");
  const nexusBaseTexts = document.getElementById("nexusBaseTexts");
  const meshcoreRelayMode = document.getElementById("meshcoreRelayMode");
  const rebroadcastMode = document.getElementById("rebroadcastMode");
  const muteNoticeBaseTexts = document.getElementById("muteNoticeBaseTexts");
  const muteNoticeRebroadcast = document.getElementById(
    "muteNoticeRebroadcast",
  );
  const muteNoticeMeshcoreRelay = document.getElementById(
    "muteNoticeMeshcoreRelay",
  );
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

  // Role Restrictions (e.g. CLIENT_MUTE must not repeat or force base texts)
  let nexusManifest = null; // options.manifest.json, cargado por loadNexusOptions()
  let prevBaseTexts = nexusBaseTexts ? nexusBaseTexts.checked : true;
  let prevRebroadcast = rebroadcastMode ? rebroadcastMode.value : "ALL";
  let prevMeshcoreRelay = meshcoreRelayMode ? meshcoreRelayMode.value : "AUTO";

  function syncRoleRestrictions() {
    if (!deviceRole) return;
    const isMute = deviceRole.value === "CLIENT_MUTE";

    if (isMute) {
      if (nexusBaseTexts) {
        if (!nexusBaseTexts.disabled) {
          prevBaseTexts = nexusBaseTexts.checked;
        }
        nexusBaseTexts.checked = false;
        nexusBaseTexts.disabled = true;
      }
      if (muteNoticeBaseTexts) muteNoticeBaseTexts.style.display = "block";

      if (rebroadcastMode) {
        if (!rebroadcastMode.disabled) {
          prevRebroadcast = rebroadcastMode.value;
        }
        rebroadcastMode.value = "NONE";
        rebroadcastMode.disabled = true;
      }
      if (muteNoticeRebroadcast) muteNoticeRebroadcast.style.display = "block";

      if (meshcoreRelayMode) {
        if (!meshcoreRelayMode.disabled) {
          prevMeshcoreRelay = meshcoreRelayMode.value;
        }
        meshcoreRelayMode.value = "OFF";
        meshcoreRelayMode.disabled = true;
      }
      if (muteNoticeMeshcoreRelay)
        muteNoticeMeshcoreRelay.style.display = "block";
    } else {
      if (nexusBaseTexts) {
        nexusBaseTexts.disabled = false;
        nexusBaseTexts.checked =
          prevBaseTexts !== undefined ? prevBaseTexts : true;
      }
      if (muteNoticeBaseTexts) muteNoticeBaseTexts.style.display = "none";

      if (rebroadcastMode) {
        rebroadcastMode.disabled = false;
        rebroadcastMode.value =
          prevRebroadcast && prevRebroadcast !== "NONE"
            ? prevRebroadcast
            : "ALL";
      }
      if (muteNoticeRebroadcast) muteNoticeRebroadcast.style.display = "none";

      if (meshcoreRelayMode) {
        meshcoreRelayMode.disabled = false;
        meshcoreRelayMode.value =
          prevMeshcoreRelay && prevMeshcoreRelay !== "OFF"
            ? prevMeshcoreRelay
            : "AUTO";
      }
      if (muteNoticeMeshcoreRelay)
        muteNoticeMeshcoreRelay.style.display = "none";
    }

    if (window.NexusOptions && nexusManifest)
      window.NexusOptions.applyRoles(nexusManifest, deviceRole.value);
    if (typeof updateLivePreview === "function") updateLivePreview();
  }

  if (deviceRole) {
    deviceRole.addEventListener("change", syncRoleRestrictions);
    syncRoleRestrictions();
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
    if (boardTarget.value === "nrf52_promicro_diy_sx1262-noscreen") {
      if (presetSolarRepeater) presetSolarRepeater.checked = true;
      if (solarRadioDriver) solarRadioDriver.value = "sx1262";
      updatePresetUI();
    }
    applyBoardCapabilities();
  });

  // Preset Selector Logic (Companion vs Repetidor Solar / Azoteas & Montañas)
  function updatePresetUI() {
    const isSolar = presetSolarRepeater && presetSolarRepeater.checked;
    if (isSolar) {
      if (presetCompanionLabel) presetCompanionLabel.classList.remove("active");
      if (presetSolarRepeaterLabel)
        presetSolarRepeaterLabel.classList.add("active");
      if (solarRepeaterOptions) solarRepeaterOptions.classList.remove("hidden");

      // Auto-set optimal repeater settings
      if (deviceRole && deviceRole.value !== "ROUTER") {
        deviceRole.value = "ROUTER";
        syncRoleRestrictions();
      }
      if (hasScreen) hasScreen.checked = false;

      // Check GPS option
      const incGps = solarIncludeGps ? solarIncludeGps.value : "no";
      if (incGps === "no" && gpsMode) {
        gpsMode.value = "NOT_PRESENT";
      }

      // Check Radio Driver for ProMicro
      const rDriver = solarRadioDriver ? solarRadioDriver.value : "sx1262";
      // Only the plain sx1262 env is upgraded to its headless twin. A board the user chose explicitly (e.g. the
      // TCXO one) is never replaced behind their back.
      if (rDriver === "sx1262") {
        if (boardTarget.value === "nrf52_promicro_diy_sx1262") {
          boardTarget.value = "nrf52_promicro_diy_sx1262-noscreen";
        }
      }
    } else {
      if (presetSolarRepeaterLabel)
        presetSolarRepeaterLabel.classList.remove("active");
      if (presetCompanionLabel) presetCompanionLabel.classList.add("active");
      if (solarRepeaterOptions) solarRepeaterOptions.classList.add("hidden");

      if (deviceRole && deviceRole.value === "ROUTER") {
        deviceRole.value = "CLIENT";
        syncRoleRestrictions();
      }
      if (hasScreen) hasScreen.checked = true;
      if (gpsMode) gpsMode.value = "ENABLED";

      if (boardTarget.value === "nrf52_promicro_diy_sx1262-noscreen") {
        boardTarget.value = "nrf52_promicro_diy_tcxo";
      }
    }

    // ROUTER is offered only with the solar repeater preset: elsewhere the firmware would silently fall back to CLIENT.
    const routerOption = deviceRole
      ? deviceRole.querySelector('option[value="ROUTER"]')
      : null;
    if (routerOption) routerOption.disabled = !isSolar;

    applyBoardCapabilities();
    if (typeof updateLivePreview === "function") updateLivePreview();
  }

  // A board with no network of its own can only reach MQTT through the phone, and has no WiFi
  // config at all. The selector marks those groups with data-networking="0".
  let proxyAutoSet = false;

  function boardHasNetworking() {
    if (!boardTarget || boardTarget.value === "custom") return true;
    const opt = boardTarget.selectedOptions && boardTarget.selectedOptions[0];
    const group = opt && opt.parentElement;
    return !(group && group.dataset && group.dataset.networking === "0");
  }

  function applyBoardCapabilities() {
    const wired = boardHasNetworking();

    if (mqttNoNetworkNotice)
      mqttNoNetworkNotice.style.display = wired ? "none" : "block";
    if (wifiNoNetworkNotice)
      wifiNoNetworkNotice.style.display = wired ? "none" : "block";

    if (wifiEnabled) {
      wifiEnabled.disabled = !wired;
      if (!wired && wifiEnabled.checked) {
        wifiEnabled.checked = false;
        if (wifiFields) wifiFields.classList.add("hidden");
      }
    }

    // Without a network of its own, MQTT only works through the phone proxy.
    if (mqttProxy) {
      if (!wired && !mqttProxy.checked) {
        mqttProxy.checked = true;
        proxyAutoSet = true;
      } else if (wired && proxyAutoSet) {
        mqttProxy.checked = false;
        proxyAutoSet = false;
      }
    }

    if (typeof updateLivePreview === "function") updateLivePreview();
  }

  if (mqttProxy) {
    mqttProxy.addEventListener("change", () => {
      proxyAutoSet = false;
    });
  }

  if (presetCompanion)
    presetCompanion.addEventListener("change", updatePresetUI);
  if (presetSolarRepeater)
    presetSolarRepeater.addEventListener("change", updatePresetUI);

  if (solarIncludeGps) {
    solarIncludeGps.addEventListener("change", () => {
      if (solarIncludeGps.value === "no" && gpsMode) {
        gpsMode.value = "NOT_PRESENT";
      } else if (solarIncludeGps.value === "yes" && gpsMode) {
        gpsMode.value = "ENABLED";
      }
      if (typeof updateLivePreview === "function") updateLivePreview();
    });
  }

  if (solarRadioDriver) {
    solarRadioDriver.addEventListener("change", () => {
      if (solarRadioDriver.value === "sx1262") {
        if (boardTarget.value === "nrf52_promicro_diy_sx1262") {
          boardTarget.value = "nrf52_promicro_diy_sx1262-noscreen";
        }
      } else if (solarRadioDriver.value === "all") {
        if (boardTarget.value === "nrf52_promicro_diy_sx1262-noscreen") {
          boardTarget.value = "nrf52_promicro_diy_tcxo";
        }
      }
      if (typeof updateLivePreview === "function") updateLivePreview();
    });
  }

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

  // Tópico raíz: muestra como valor real el que se usará (msh/<región>) mientras el usuario no escriba uno propio.
  const mqttRootInput = document.getElementById("mqttRoot");
  const loraRegionSelect = document.getElementById("loraRegion");
  function syncMqttRootDefault() {
    if (mqttRootInput.dataset.userEdited === "1") return;
    mqttRootInput.value = `msh/${loraRegionSelect.value}`;
  }
  mqttRootInput.addEventListener("input", () => {
    mqttRootInput.dataset.userEdited = mqttRootInput.value.trim() ? "1" : "0";
    if (!mqttRootInput.value.trim()) syncMqttRootDefault();
  });
  loraRegionSelect.addEventListener("change", syncMqttRootDefault);
  syncMqttRootDefault();

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
          <input type="text" class="ch-input ch-field-name" data-idx="${idx}" value="${escapeAttr(ch.name)}" maxlength="11" placeholder="Nombre" />
        </td>
        <td>
          <input type="text" class="ch-input font-mono ch-field-psk" data-idx="${idx}" value="${escapeAttr(ch.psk)}" placeholder="AQ== / Ag== / Clave" title="Clave en Base64 o Hexadecimal" />
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
        if (window.NexusOptions) window.NexusOptions.refreshBitLabels();
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
    // Los nombres de la tabla también son las etiquetas de "Canales donde responde el bot".
    if (window.NexusOptions) window.NexusOptions.refreshBitLabels();
  }

  renderChannelsTable();

  // Reset to Spanish defaults
  btnResetChannels.addEventListener("click", () => {
    channelsData = JSON.parse(JSON.stringify(DEFAULT_CHANNELS));
    renderChannelsTable();
    updateLivePreview();
  });

  // Configure single channel only (Ch 0)
  btnSingleChannel.addEventListener("click", () => {
    channelsData.forEach((ch, idx) => {
      ch.enabled = idx === 0;
    });
    renderChannelsTable();
    updateLivePreview();
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
    const forceCleanInstall =
      document.getElementById("forceCleanInstall").checked;
    const deviceRole = document.getElementById("deviceRole").value;
    const isMute = deviceRole === "CLIENT_MUTE";
    const mcRelay = isMute
      ? "OFF"
      : document.getElementById("meshcoreRelayMode").value;
    const mcAdvert =
      document.getElementById("meshcoreAdvertHours").value.trim() || "12";
    const mcRegions = document
      .getElementById("meshcoreRegionsDef")
      .value.trim();
    const mcHome = document.getElementById("meshcoreHomeRegion").value.trim();
    const mcDef = document.getElementById("meshcoreDefaultRegion").value.trim();
    const baseTexts = isMute
      ? false
      : document.getElementById("nexusBaseTexts")
        ? document.getElementById("nexusBaseTexts").checked
        : true;
    const keyChoice = nexusMasterKey.value;
    const masterKey =
      keyChoice === "custom" ? customMasterKey.value.trim() : keyChoice;

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
    const rebroadcastVal = isMute
      ? "NONE"
      : rebroadcastMode
        ? rebroadcastMode.value
        : "ALL";
    const blePinVal = fixedBlePin ? fixedBlePin.value.trim() : "";
    const nodeInfoSecsVal = nodeInfoBroadcastSecs
      ? nodeInfoBroadcastSecs.value.trim()
      : "43200";
    const posSecsVal = positionBroadcastInterval
      ? positionBroadcastInterval.value.trim()
      : "2147483647";
    const telemSecsVal = deviceTelemInterval
      ? deviceTelemInterval.value.trim()
      : "2147483647";
    const smartPosVal = smartPosition ? smartPosition.checked : false;

    const isMqttOn = mqttEnabled.checked;
    const isMqttProxy = document.getElementById("mqttProxy").checked;
    const mAddress = document.getElementById("mqttAddress").value.trim();
    const mRoot = document.getElementById("mqttRoot").value.trim();
    const mUser = document.getElementById("mqttUsername").value.trim();
    const mPass = document.getElementById("mqttPassword").value.trim();
    const mEnc = document.getElementById("mqttEncryption").checked;
    const mTls = document.getElementById("mqttTls").checked;

    const wifiOn = wifiEnabled.checked;
    const wifiSsid = document.getElementById("wifiSsid").value.trim();
    const wifiPassword = document.getElementById("wifiPassword").value.trim();

    const isSolarPreset = presetSolarRepeater && presetSolarRepeater.checked;
    const includeGps = solarIncludeGps ? solarIncludeGps.value : "no";
    const radioDriver = solarRadioDriver ? solarRadioDriver.value : "sx1262";

    let extraBuildFlags = [];
    if (isSolarPreset) {
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_SCREEN=1");
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_DISPLAY=1");
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_CANNEDMESSAGES=1");
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_ATAK=1");
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_WAYPOINT=1");
      extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_RANGETEST=1");
      // A relay has no phone to serve: no MeshCore companion (Ed25519 checks on every frame, contact store in flash).
      extraBuildFlags.push("-DMESHTASTIC_ENABLE_MESHCORE_COMPANION=0");

      if (includeGps === "no") {
        extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_GPS=1");
      }
    } else {
      if (!hasScreen) {
        extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_SCREEN=1");
        extraBuildFlags.push("-DMESHTASTIC_EXCLUDE_DISPLAY=1");
      }
    }
    const extraFlagsStr = extraBuildFlags.join(" ");

    return {
      target: target,
      custom_target: customTarget,
      node_preset: isSolarPreset ? "solar_repeater" : "companion",
      solar_include_gps: includeGps,
      solar_radio_driver: radioDriver,
      extra_build_flags: extraFlagsStr,
      owner_long_name: longName.substring(0, 24),
      owner_short_name: shortName.substring(0, 4),
      has_screen: hasScreen,
      enable_bluetooth: enableBluetooth,
      force_clean_install: forceCleanInstall,
      enable_meshcore: true,
      meshcore_advert_hours: mcAdvert,
      meshcore_relay_mode: mcRelay,
      meshcore_regions_def: mcRegions,
      meshcore_home_region: mcHome,
      meshcore_default_region: mcDef,
      nexus_base_texts: baseTexts,
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
      mqtt_proxy: isMqttProxy,
      mqtt_address: mAddress || BuildPrefs.MQTT_DEFAULTS.address,
      mqtt_root: mRoot || `msh/${loraRegion}`,
      mqtt_username: mUser || BuildPrefs.MQTT_DEFAULTS.username,
      mqtt_password: mPass || BuildPrefs.MQTT_DEFAULTS.password,
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
    };
  }

  // 1-Click Build via GitHub Issue Bot (No token needed, works for any user)
  if (btnBuildViaIssue) {
    btnBuildViaIssue.addEventListener("click", async () => {
      const data = getFormData();
      let prefs;
      try {
        prefs = buildUserPrefs(data);
      } catch (err) {
        if (issueStatus) {
          issueStatus.className = "status-msg status-error";
          issueStatus.classList.remove("hidden");
          issueStatus.textContent = `Revisa la configuración: ${err.message}`;
        }
        return;
      }

      const issueTitle = `[Build Request] ${data.target} - ${data.owner_short_name || "Node"}`;

      const configPayload = {
        target: data.target,
        custom_target: data.custom_target,
        node_preset: data.node_preset,
        owner_long_name: data.owner_long_name,
        owner_short_name: data.owner_short_name,
        has_screen: data.has_screen,
        enable_bluetooth: data.enable_bluetooth,
        device_role: data.device_role,
        lora_region: data.lora_region,
        lora_preset_mode: data.lora_preset_mode,
        timezone_string: data.timezone_string,
        extra_build_flags: data.extra_build_flags,
        custom_userprefs: prefs,
      };

      const presetLabel =
        data.node_preset === "solar_repeater"
          ? "🏔️ Repetidor Solar / Azoteas & Montañas (Ultra-ligero)"
          : "📱 Companion / Personal (Estándar)";

      let encryptedConfig;
      try {
        encryptedConfig = await BuildCrypto.encryptBuildRequest(configPayload);
      } catch (err) {
        if (issueStatus) {
          issueStatus.className = "status-msg status-error";
          issueStatus.classList.remove("hidden");
          issueStatus.textContent = `No se pudo cifrar la solicitud: ${err.message}`;
        }
        return;
      }

      const issueBody = `### ⚙️ Solicitud de Compilación Automática de Firmware Meshtacore

- **Placa Objetivo:** \`${data.target}\`
- **Perfil de Nodo:** ${presetLabel}
- **Nombre del Nodo:** \`${data.owner_long_name}\` (\`${data.owner_short_name}\`)
- **Zona Horaria:** \`${data.timezone_string}\`
- **Rol:** \`${data.device_role}\` | **Región LoRa:** \`${data.lora_region}\`
- **Flags Extra de Optimización:** \`${data.extra_build_flags || "Ninguno (Estándar)"}\`

🔒 _La configuración (claves, contraseñas...) viaja cifrada: solo el bot de compilación puede leerla._

\`\`\`meshtacore-build-config-enc
${encryptedConfig}
\`\`\`

> ℹ️ _Pulsa **"Submit new issue"** abajo para que el bot de GitHub Actions empiece a compilar tu firmware y te adjunte el enlace de descarga en este mismo hilo._`;

      const url = `https://github.com/${REPO_OWNER}/${REPO_NAME}/issues/new?title=${encodeURIComponent(issueTitle)}&body=${encodeURIComponent(issueBody)}`;

      if (url.length > MAX_ISSUE_URL_LENGTH) {
        if (issueStatus) {
          issueStatus.className = "status-msg status-error";
          issueStatus.classList.remove("hidden");
          issueStatus.textContent = `La solicitud es demasiado larga para GitHub (${url.length} caracteres, máximo ${MAX_ISSUE_URL_LENGTH}). Desactiva canales o campos opcionales, o usa el botón de compilación con token.`;
        }
        return;
      }

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
    let prefs;
    try {
      prefs = buildUserPrefs(data);
    } catch (err) {
      dispatchStatus.className = "status-msg status-error";
      dispatchStatus.textContent = `Revisa la configuración: ${err.message}`;
      return;
    }

    let encryptedConfig;
    try {
      encryptedConfig = await BuildCrypto.encryptBuildRequest({
        custom_userprefs: prefs,
      });
    } catch (err) {
      dispatchStatus.className = "status-msg status-error";
      dispatchStatus.textContent = `No se pudo cifrar la solicitud: ${err.message}`;
      return;
    }

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
        mqtt_address: data.mqtt_address,
        gps_mode: data.gps_mode,
        fixed_latitude: data.fixed_latitude,
        fixed_longitude: data.fixed_longitude,
        fixed_altitude: data.fixed_altitude,
        wifi_ssid: data.wifi_ssid,
        // Secrets (wifi password, master key, PSKs...) only travel inside the encrypted blob.
        encrypted_config: encryptedConfig,
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
    return BuildPrefs.buildUserPrefs(data, {
      channels: channelsData,
      nexusManifest,
      nexusOptions: window.NexusOptions,
    });
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

  // Generate & Download userPrefs.jsonc
  btnDownloadJson.addEventListener("click", () => {
    const data = getFormData();
    let prefs;
    try {
      prefs = buildUserPrefs(data);
    } catch (err) {
      dispatchStatus.className = "status-msg status-error";
      dispatchStatus.classList.remove("hidden");
      dispatchStatus.textContent = `Revisa la configuración: ${err.message}`;
      return;
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

  // Opciones avanzadas de Nexus: se dibujan desde options.manifest.json y se emiten con el mismo módulo.
  async function loadNexusOptions() {
    const container = document.getElementById("nexusOptionsContainer");
    if (!container || !window.NexusOptions) return;
    try {
      const res = await fetch("options.manifest.json", { cache: "no-cache" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      nexusManifest = await res.json();
      window.NexusOptions.render(nexusManifest, container, {
        channelName: (i) => (channelsData[i] ? channelsData[i].name : ""),
      });
      window.NexusOptions.applyRoles(nexusManifest, deviceRole.value);
      updateLivePreview();
    } catch (err) {
      container.textContent = `No se pudieron cargar las opciones avanzadas: ${err.message}`;
    }
  }
  loadNexusOptions();

  // Initial UI state (board-dependent availability) + preview
  applyBoardCapabilities();
});
