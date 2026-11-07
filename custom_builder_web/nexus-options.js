/*
 * Opciones de Nexus del builder, dirigidas por options.manifest.json.
 *
 * El manifest es la única fuente: este módulo dibuja los controles y traduce sus valores a claves USERPREFS_*, y
 * .github/scripts/generate_custom_userprefs.py lo lee para validar lo que llega. Una opción con su valor por defecto (o
 * vacía) no se emite, así que un formulario sin tocar no cambia el userPrefs.jsonc.
 *
 * emitNexusPrefs() es pura (sin DOM) para poder probarla desde Node: custom_builder_web/tests/nexus-options.test.js.
 */
(function (root) {
  "use strict";

  function isEmpty(v) {
    return v === undefined || v === null || String(v).trim() === "";
  }

  function toBigInt(v) {
    try {
      return BigInt(v);
    } catch (e) {
      return 0n;
    }
  }

  // Misma conversión que b64_to_c_array() en generate_custom_userprefs.py.
  function base64ToCArray(b64) {
    const bin = root.atob
      ? root.atob(b64.trim())
      : Buffer.from(b64.trim(), "base64").toString("binary");
    const bytes = [];
    for (let i = 0; i < bin.length; i++) {
      bytes.push("0x" + bin.charCodeAt(i).toString(16).padStart(2, "0"));
    }
    return "{ " + bytes.join(", ") + " }";
  }

  // Devuelve el valor de USERPREFS para una opción, o null si no hay que emitirla.
  function emitOption(opt, value) {
    switch (opt.type) {
      case "bool": {
        if (typeof value !== "boolean" && value !== "true" && value !== "false")
          return null;
        const on = value === true || value === "true";
        return !opt.always && on === Boolean(opt.default)
          ? null
          : on
            ? "true"
            : "false";
      }
      case "int": {
        if (isEmpty(value)) return null;
        const n = Number(String(value).trim());
        if (!Number.isInteger(n) || n < opt.min || n > opt.max) return null;
        return !opt.always &&
          opt.default !== null &&
          opt.default !== undefined &&
          n === opt.default
          ? null
          : String(n);
      }
      case "string": {
        if (isEmpty(value)) return null;
        const s = String(value).trim().slice(0, opt.maxLen);
        return !opt.always && s === opt.default ? null : s;
      }
      case "enum": {
        const s = String(value);
        if (!opt.always && s === String(opt.default)) return null;
        const hit = opt.values.find((v) => v.value === s);
        // `emit` is the text the firmware gets for that choice; `prefix` is prepended to every choice.
        return hit
          ? (opt.prefix || "") + (hit.emit !== undefined ? hit.emit : hit.value)
          : null;
      }
      case "bitmask": {
        // value: lista de números de bit marcados.
        let mask = 0n;
        for (const bit of value || []) mask |= 1n << BigInt(bit);
        if (!opt.always && mask === toBigInt(opt.default || 0)) return null;
        return opt.format === "dec"
          ? mask.toString(10)
          : "0x" + mask.toString(16).toUpperCase();
      }
      case "key": {
        if (isEmpty(value)) return null;
        const s = String(value).trim();
        if (s.startsWith("{") && s.endsWith("}")) return s;
        try {
          return base64ToCArray(s);
        } catch (e) {
          return null;
        }
      }
      default:
        return null;
    }
  }

  // values: { <id de opción>: valor }. Devuelve { <clave USERPREFS>: texto }.
  function emitNexusPrefs(manifest, values) {
    const prefs = {};
    for (const opt of manifest.options) {
      if (!(opt.id in values)) continue;
      const out = emitOption(opt, values[opt.id]);
      if (out !== null) prefs[opt.key] = out;
    }
    return prefs;
  }

  // ---- DOM ----

  function el(tag, attrs, children) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === "class") node.className = v;
      else if (k === "text") node.textContent = v;
      else node.setAttribute(k, v);
    }
    for (const c of children || []) node.appendChild(c);
    return node;
  }

  function fieldId(opt) {
    return "nx_" + opt.id;
  }

  function helpText(opt, cmd = opt.cmd) {
    const t = cmd ? "Equivale a " : "";
    return el("small", { class: "helper-text" }, [
      document.createTextNode(t),
      el("code", { text: cmd || "" }),
    ]);
  }

  // Proveedor del nombre real de cada canal (índice = bit). Lo inyecta la página vía render().
  let channelNameFor = null;
  let renderedManifest = null;

  function channelName(bit) {
    return String((channelNameFor && channelNameFor(bit)) || "").trim();
  }

  // Etiqueta "Canal N · nombre" si la opción pide nombres de canal y la página conoce el nombre.
  function bitLabel(opt, b) {
    if (!opt.labelsFromChannels || !channelNameFor) return b.label;
    const name = channelName(b.bit);
    return name ? `Canal ${b.bit} · ${name}` : `Canal ${b.bit}`;
  }

  // Reparte los bits de una máscara de canales entre los que llevan el prefijo (p. ej. "MC:") y los demás, según el
  // nombre actual de cada canal. Pura: nameFor(bit) da el nombre.
  function splitBitsByName(opt, nameFor) {
    const out = { plain: [], prefixed: [] };
    for (const b of opt.bits)
      (String(nameFor(b.bit) || "").startsWith(opt.channelSplit.prefix)
        ? out.prefixed
        : out.plain
      ).push(b);
    return out;
  }

  function bitCheckbox(opt, b, checked) {
    const input = el("input", {
      type: "checkbox",
      "data-nx-bit": String(b.bit),
      "data-nx-opt": opt.id,
    });
    input.checked = checked;
    return input;
  }

  function bitLabelNode(opt, b, input) {
    const label = el("label", { class: "nx-bit" }, [
      input,
      document.createTextNode(" " + bitLabel(opt, b)),
    ]);
    label.dataset.nxBitLabel = String(b.bit);
    label.dataset.nxOpt = opt.id;
    // `roles` (opcional): la opción solo tiene efecto con esos device roles; fuera de ellos se oculta.
    if (Array.isArray(b.roles)) label.dataset.nxRoles = b.roles.join(",");
    return label;
  }

  function isDefaultOn(opt, b) {
    return ((toBigInt(opt.default || 0) >> BigInt(b.bit)) & 1n) === 1n;
  }

  function bitmaskControl(opt) {
    const box = el("div", { class: "nx-bits", id: fieldId(opt) });
    for (const b of opt.bits)
      box.appendChild(
        bitLabelNode(opt, b, bitCheckbox(opt, b, isDefaultOn(opt, b))),
      );
    return box;
  }

  // ---- máscara de canales repartida en dos listas (Meshtastic / MeshCore) ----

  function splitListId(opt, side) {
    return `${fieldId(opt)}__${side}`;
  }

  function splitSwitchId(opt) {
    return `${fieldId(opt)}__switch`;
  }

  function prefixedInputs(opt) {
    const list = document.getElementById(splitListId(opt, "prefixed"));
    return list ? Array.from(list.querySelectorAll("input[data-nx-opt]")) : [];
  }

  // El interruptor de MeshCore no es una opción más: refleja si hay algún canal MeshCore marcado (igual que
  // /mc PPING ON y OFF, que solo mueven los canales MC:).
  function syncSplitSwitch(opt) {
    const input = document.getElementById(splitSwitchId(opt));
    if (!input) return;
    const boxes = prefixedInputs(opt);
    input.checked = boxes.some((i) => i.checked);
    input.disabled = boxes.length === 0;
  }

  function onSplitSwitchChange(opt, input) {
    const boxes = prefixedInputs(opt);
    if (input.checked) {
      const wanted = opt.channelSplit.prefixed.switchChannel.toLowerCase();
      const bot = boxes.find(
        (i) => channelName(Number(i.dataset.nxBit)).toLowerCase() === wanted,
      );
      if (bot) bot.checked = true;
    } else {
      for (const i of boxes) i.checked = false;
    }
    // Sin canal MC:#bot no hay nada que activar: el interruptor vuelve a reflejar lo marcado.
    syncSplitSwitch(opt);
  }

  // Vuelve a repartir las casillas entre las dos listas, conservando lo marcado. Se llama al dibujar y al renombrar
  // un canal (un canal que empieza por el prefijo pasa a la lista de MeshCore).
  function fillSplitLists(opt) {
    const plain = document.getElementById(splitListId(opt, "plain"));
    const prefixed = document.getElementById(splitListId(opt, "prefixed"));
    if (!plain || !prefixed) return;
    const before = document.querySelectorAll(`input[data-nx-opt="${opt.id}"]`);
    const checked = new Set();
    if (before.length)
      before.forEach((i) => {
        if (i.checked) checked.add(Number(i.dataset.nxBit));
      });
    else for (const b of opt.bits) if (isDefaultOn(opt, b)) checked.add(b.bit);

    const parts = splitBitsByName(opt, channelName);
    for (const side of ["plain", "prefixed"]) {
      const list = side === "plain" ? plain : prefixed;
      list.textContent = "";
      for (const b of parts[side])
        list.appendChild(
          bitLabelNode(opt, b, bitCheckbox(opt, b, checked.has(b.bit))),
        );
      if (!parts[side].length)
        list.appendChild(
          el("small", {
            class: "helper-text",
            text:
              side === "prefixed"
                ? `Ningún canal de la tabla empieza por «${opt.channelSplit.prefix}».`
                : "Todos los canales de la tabla son de MeshCore.",
          }),
        );
    }
    syncSplitSwitch(opt);
  }

  function renderSplitPart(opt, side) {
    const cfg = opt.channelSplit[side];
    const list = el("div", { class: "nx-bits", id: splitListId(opt, side) });
    const group = el("div", { class: "form-group mt-2" }, [
      el("label", { text: cfg.label }),
      list,
      helpText(opt, cfg.cmd),
    ]);
    if (side !== "prefixed") return group;
    list.addEventListener("change", () => syncSplitSwitch(opt));
    const input = el("input", { type: "checkbox", id: splitSwitchId(opt) });
    input.addEventListener("change", () => onSplitSwitchChange(opt, input));
    return el("div", {}, [
      el("div", { class: "toggle-row mt-2" }, [
        el("div", { class: "toggle-label" }, [
          el("strong", { text: cfg.switchLabel }),
          helpText(opt, cfg.switchCmd),
        ]),
        el("label", { class: "switch" }, [
          input,
          el("span", { class: "slider" }),
        ]),
      ]),
      group,
    ]);
  }

  // Vuelve a escribir las etiquetas de los canales tras editar sus nombres en la tabla de canales.
  function refreshBitLabels() {
    if (!renderedManifest) return;
    for (const opt of renderedManifest.options) {
      if (opt.type !== "bitmask" || !opt.labelsFromChannels) continue;
      if (opt.channelSplit) {
        fillSplitLists(opt);
        continue;
      }
      for (const b of opt.bits) {
        const label = document.querySelector(
          `label[data-nx-bit-label="${b.bit}"][data-nx-opt="${opt.id}"]`,
        );
        const input = label && label.querySelector("input");
        if (label && input)
          label.replaceChildren(
            input,
            document.createTextNode(" " + bitLabel(opt, b)),
          );
      }
    }
  }

  function control(opt) {
    switch (opt.type) {
      case "bool": {
        const input = el("input", { type: "checkbox", id: fieldId(opt) });
        input.checked = Boolean(opt.default);
        return el("label", { class: "switch" }, [
          input,
          el("span", { class: "slider" }),
        ]);
      }
      case "int": {
        const input = el("input", {
          type: "number",
          id: fieldId(opt),
          class: "form-control",
          min: String(opt.min),
          max: String(opt.max),
        });
        if (opt.default !== null && opt.default !== undefined)
          input.value = String(opt.default);
        else input.placeholder = "Por defecto";
        return input;
      }
      case "string":
      case "key": {
        const input = el("input", {
          type: "text",
          id: fieldId(opt),
          class: "form-control",
        });
        if (opt.maxLen) input.maxLength = opt.maxLen;
        input.value = opt.default || "";
        return input;
      }
      case "enum": {
        const select = el("select", {
          id: fieldId(opt),
          class: "form-control",
        });
        for (const v of opt.values) {
          const o = el("option", { value: v.value, text: v.label });
          if (v.value === String(opt.default)) o.selected = true;
          select.appendChild(o);
        }
        return select;
      }
      case "bitmask":
        return bitmaskControl(opt);
      default:
        return el("span");
    }
  }

  function renderOption(opt) {
    if (opt.type === "bool") {
      return el("div", { class: "toggle-row mt-2" }, [
        el("div", { class: "toggle-label" }, [
          el("strong", { text: opt.label }),
          helpText(opt),
        ]),
        control(opt),
      ]);
    }
    return el("div", { class: "form-group mt-2" }, [
      el("label", { for: fieldId(opt), text: opt.label }),
      control(opt),
      helpText(opt),
    ]);
  }

  // Cuerpo de un grupo. Si el grupo declara `sections`, las opciones se agrupan bajo un subtítulo cada una; la parte de
  // una máscara de canales (`channelSplit`) que abre una sección (el interruptor y su lista) va la primera.
  function groupBody(manifest, g) {
    const options = manifest.options.filter((o) => o.group === g.id && !o.form);
    if (options.length === 0) return null;
    const body = el("div", {}, [
      el("p", { class: "helper-text", text: g.description }),
    ]);
    if (!g.sections) {
      for (const opt of options) body.appendChild(renderOption(opt));
      return body;
    }
    for (const s of g.sections) {
      const section = el("div", { class: "nx-section" }, [
        el("h4", { class: "nx-section-title", text: s.title }),
      ]);
      for (const opt of options)
        if (opt.channelSplit && opt.channelSplit.prefixed.section === s.id)
          section.appendChild(renderSplitPart(opt, "prefixed"));
      for (const opt of options) {
        if (opt.channelSplit) {
          if (opt.channelSplit.plain.section === s.id)
            section.appendChild(renderSplitPart(opt, "plain"));
        } else if (opt.section === s.id) section.appendChild(renderOption(opt));
      }
      body.appendChild(section);
    }
    return body;
  }

  // opts.channelName(i): nombre real del canal i (lo da la tabla de canales de la página).
  function render(manifest, container, opts = {}) {
    renderedManifest = manifest;
    channelNameFor =
      typeof opts.channelName === "function" ? opts.channelName : null;
    container.textContent = "";
    for (const g of manifest.groups) {
      // Las opciones con `form` ya tienen su control en builder.html: aquí solo se emiten.
      const body = groupBody(manifest, g);
      if (!body) continue;
      container.appendChild(
        el("details", { class: "collapsible mt-3" }, [
          el("summary", { text: g.title }),
          body,
        ]),
      );
    }
    for (const opt of manifest.options)
      if (opt.channelSplit && !opt.form) fillSplitLists(opt);
  }

  // Oculta y desmarca los bits con `roles` que no aplican al device role actual, para que no se emitan.
  function applyRoles(manifest, role) {
    if (!manifest) return;
    for (const opt of manifest.options) {
      if (opt.type !== "bitmask") continue;
      const labels = document.querySelectorAll(
        `label[data-nx-roles][data-nx-opt="${opt.id}"]`,
      );
      for (const label of labels) {
        const allowed = label.dataset.nxRoles.split(",").includes(role);
        label.hidden = !allowed;
        const input = label.querySelector("input");
        if (!allowed && input) input.checked = false;
      }
    }
  }

  // Las máscaras de canales (`labelsFromChannels`) se marcan por fila de la tabla, pero el firmware numera los canales por
  // su posición entre los activos. enabled[i] dice si la fila i se escribe; devuelve los valores con los bits traducidos
  // (un canal desactivado no llega al firmware, así que su bit se descarta).
  function remapChannelBits(manifest, values, enabled) {
    const out = { ...values };
    for (const opt of manifest.options) {
      if (opt.type !== "bitmask" || !opt.labelsFromChannels) continue;
      if (!Array.isArray(out[opt.id])) continue;
      out[opt.id] = out[opt.id]
        .filter((row) => enabled[row])
        .map((row) => enabled.slice(0, row).filter(Boolean).length);
    }
    return out;
  }

  // formData: lo que devuelve getFormData(); de ahí salen las opciones con `form`.
  function readValues(manifest, formData) {
    const values = {};
    for (const opt of manifest.options) {
      if (opt.form) {
        if (formData && formData[opt.form] !== undefined)
          values[opt.id] = formData[opt.form];
        continue;
      }
      if (opt.type === "bitmask") {
        // Las casillas pueden estar repartidas en varias listas (channelSplit): se leen por opción, no por contenedor.
        const inputs = document.querySelectorAll(
          `input[data-nx-opt="${opt.id}"]`,
        );
        if (inputs.length === 0) continue;
        values[opt.id] = Array.from(inputs)
          .filter((i) => i.checked)
          .map((i) => Number(i.getAttribute("data-nx-bit")));
        continue;
      }
      const node = document.getElementById(fieldId(opt));
      if (!node) continue;
      if (opt.type === "bool") values[opt.id] = node.checked;
      else values[opt.id] = node.value;
    }
    return values;
  }

  // Lo que emitOption() descartaría en silencio por estar fuera de rango o mal escrito: el usuario debe enterarse.
  function invalidValues(manifest, values) {
    const problems = [];
    for (const opt of manifest.options) {
      if (!(opt.id in values) || isEmpty(values[opt.id])) continue;
      const v = values[opt.id];
      if (opt.type === "int") {
        const n = Number(String(v).trim());
        if (!Number.isInteger(n) || n < opt.min || n > opt.max)
          problems.push(
            `${opt.label}: debe ser un entero entre ${opt.min} y ${opt.max}`,
          );
      } else if (
        opt.type === "string" &&
        String(v).trim().length > opt.maxLen
      ) {
        problems.push(`${opt.label}: máximo ${opt.maxLen} caracteres`);
      } else if (opt.type === "key") {
        const s = String(v).trim();
        if (!(s.startsWith("{") && s.endsWith("}"))) {
          try {
            base64ToCArray(s);
          } catch (e) {
            problems.push(`${opt.label}: clave no válida (Base64 o {0x..})`);
          }
        }
      }
    }
    return problems;
  }

  const api = {
    emitOption,
    emitNexusPrefs,
    render,
    refreshBitLabels,
    applyRoles,
    splitBitsByName,
    remapChannelBits,
    readValues,
    invalidValues,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.NexusOptions = api;
})(typeof window !== "undefined" ? window : globalThis);
