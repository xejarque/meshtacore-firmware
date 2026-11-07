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
        if (mask === toBigInt(opt.default || 0)) return null;
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

  function helpText(opt) {
    const t = opt.cmd ? "Equivale a " : "";
    return el("small", { class: "helper-text" }, [
      document.createTextNode(t),
      el("code", { text: opt.cmd || "" }),
    ]);
  }

  // Proveedor del nombre real de cada canal (índice = bit). Lo inyecta la página vía render().
  let channelNameFor = null;
  let renderedManifest = null;

  // Etiqueta "Canal N · nombre" si la opción pide nombres de canal y la página conoce el nombre.
  function bitLabel(opt, b) {
    if (!opt.labelsFromChannels || !channelNameFor) return b.label;
    const name = String(channelNameFor(b.bit) || "").trim();
    return name ? `Canal ${b.bit} · ${name}` : `Canal ${b.bit}`;
  }

  function bitmaskControl(opt) {
    const box = el("div", { class: "nx-bits", id: fieldId(opt) });
    const defaultMask = toBigInt(opt.default || 0);
    for (const b of opt.bits) {
      const input = el("input", {
        type: "checkbox",
        "data-nx-bit": String(b.bit),
      });
      input.checked = ((defaultMask >> BigInt(b.bit)) & 1n) === 1n;
      const label = el("label", { class: "nx-bit" }, [
        input,
        document.createTextNode(" " + bitLabel(opt, b)),
      ]);
      label.dataset.nxBitLabel = String(b.bit);
      box.appendChild(label);
    }
    return box;
  }

  // Vuelve a escribir las etiquetas de los canales tras editar sus nombres en la tabla de canales.
  function refreshBitLabels() {
    if (!renderedManifest) return;
    for (const opt of renderedManifest.options) {
      if (opt.type !== "bitmask" || !opt.labelsFromChannels) continue;
      const box = document.getElementById(fieldId(opt));
      if (!box) continue;
      for (const b of opt.bits) {
        const label = box.querySelector(`label[data-nx-bit-label="${b.bit}"]`);
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

  // opts.channelName(i): nombre real del canal i (lo da la tabla de canales de la página).
  function render(manifest, container, opts = {}) {
    renderedManifest = manifest;
    channelNameFor =
      typeof opts.channelName === "function" ? opts.channelName : null;
    container.textContent = "";
    for (const g of manifest.groups) {
      const body = el("div", {}, [
        el("p", { class: "helper-text", text: g.description }),
      ]);
      // Las opciones con `form` ya tienen su control en builder.html: aquí solo se emiten.
      const shown = manifest.options.filter((o) => o.group === g.id && !o.form);
      if (shown.length === 0) continue;
      for (const opt of shown) body.appendChild(renderOption(opt));
      container.appendChild(
        el("details", { class: "collapsible mt-3" }, [
          el("summary", { text: g.title }),
          body,
        ]),
      );
    }
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
      const node = document.getElementById(fieldId(opt));
      if (!node) continue;
      if (opt.type === "bool") values[opt.id] = node.checked;
      else if (opt.type === "bitmask")
        values[opt.id] = Array.from(
          node.querySelectorAll("input[data-nx-bit]:checked"),
        ).map((i) => Number(i.getAttribute("data-nx-bit")));
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
    readValues,
    invalidValues,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.NexusOptions = api;
})(typeof window !== "undefined" ? window : globalThis);
