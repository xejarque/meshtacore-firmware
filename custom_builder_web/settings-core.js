// Pure logic of the Nexus settings panel (no DOM): reading a node's state out of its text replies
// and building the commands that change it. Entries come from settings/*.json.
import { joinPages } from "./parsers.js";

// A Nexus reply carries "MESHTACORE" (the first page) or a page marker "P2: " (the following ones).
// Anything else that reaches the phone is ordinary mesh traffic forwarded by the node.
export function isNexusReply(text) {
  return /MESHTACORE/.test(text) || /^P\d+: /.test(text);
}

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const norm = (s) => String(s).toUpperCase();

// flags -> { KEY: true|false|null }; null = the reply does not say.
//   mode "tokens": one capture group holds the space separated names of the ACTIVE flags (DROPS).
//   mode "pairs":  every flag appears as `KEY: on|off` somewhere in the reply (SECURITY, FEATURES, INVALID).
export function readFlags(entry, reply) {
  const text = joinPages(reply);
  const out = {};
  if (entry.read.mode === "tokens") {
    const m = new RegExp(entry.read.pattern, "s").exec(text);
    const active = m ? new Set(m[1].split(/\s+/).filter(Boolean).map(norm)) : null;
    for (const f of entry.flags) out[f.key] = active ? [f.key, ...(f.aliases || [])].some((k) => active.has(norm(k))) : null;
    return out;
  }
  for (const f of entry.flags) {
    // readPattern: a regex of its own whose group 1 is ON|OFF (replies like "ZH OFF (POS:OFF ...)").
    const m = new RegExp(f.readPattern || `(?:^|[\\s])${escapeRe(f.readKey || f.key)}\\s*:\\s*(on|off)\\b`, "im").exec(text);
    out[f.key] = m ? ["on", "1"].includes(m[1].toLowerCase()) : null;
  }
  return out;
}

// Single value: first capture group of read.pattern, or null.
export function readValue(entry, reply) {
  const m = new RegExp(entry.read.pattern, "im").exec(joinPages(reply));
  return m ? m[1].trim() : null;
}

// Numeric fields -> { state: "ON"|"OFF"|null, values: {key: number|null} }.
// read.pattern captures the fields in order; read.state (optional regex, group 1) says whether the feature is active.
export function readValues(entry, reply) {
  const text = joinPages(reply);
  const m = new RegExp(entry.read.pattern, "im").exec(text);
  const s = entry.read.state ? new RegExp(entry.read.state, "im").exec(text) : null;
  const values = {};
  entry.fields.forEach((f, i) => (values[f.key] = m ? Number(m[i + 1]) : null));
  return { state: s ? s[1].toUpperCase() : null, values };
}

// The command that applies numeric fields; refuses anything outside each field's range.
export function valuesCommand(entry, values) {
  for (const f of entry.fields) {
    const v = Number(values[f.key]);
    if (!Number.isInteger(v) || v < f.min || v > f.max) throw new Error(`«${f.title}» debe estar entre ${f.min} y ${f.max}`);
  }
  return fill(entry.write.set, values);
}

// One choice out of several. read.pattern's group 1 is matched against each option's `read` (or `value`).
// -> { current: option|null, raw: captured text|null }
export function readEnum(entry, reply) {
  const m = new RegExp(entry.read.pattern, "im").exec(joinPages(reply));
  const raw = m ? m[1].trim() : null;
  const current = raw === null ? null : entry.options.find((o) => norm(o.read ?? o.value) === norm(raw)) || null;
  return { current, raw };
}

export function enumCommand(entry, value) {
  if (!entry.options.some((o) => o.value === value)) throw new Error("opción no válida");
  return fill(entry.write.set, { value });
}

// List of items -> [{value, label}] or null when the reply is not the expected one.
//   read.items:   a regex matched globally; group 1 is the value to send back, the whole match the label.
//   read.pattern: one capture group holding the items, split on whitespace.
export function readList(entry, reply) {
  const text = joinPages(reply);
  if (!/MESHTACORE/.test(text)) return null;
  if (entry.read.items) return [...text.matchAll(new RegExp(entry.read.items, "g"))].map((m) => ({ value: m[1], label: m[0] }));
  const m = new RegExp(entry.read.pattern, "s").exec(text);
  return m ? m[1].split(/\s+/).filter(Boolean).map((s) => ({ value: s, label: s })) : null;
}

// A reply for a human: pages joined, the "🔴 MESHTACORE:" prefix dropped.
export function cleanReply(reply) {
  return joinPages(reply).replace(/^\S+\s+MESHTACORE:?\s*/, "").trim();
}

export function fill(template, values) {
  return template.replace(/\{(\w+)\}/g, (_, k) => {
    if (values[k] === undefined || values[k] === "") throw new Error(`falta el valor «${k}»`);
    return String(values[k]).trim();
  });
}

// The command that sets one flag to `on`.
export function flagCommand(entry, flag, on) {
  return fill(flag.write ? flag.write[on ? "on" : "off"] : entry.write[on ? "on" : "off"], { key: flag.writeKey || flag.key });
}

// Only characters a Nexus command can legitimately carry; keeps a typed value from smuggling a second command.
export function safeArg(value) {
  return /^[\w!:.,*#@=+\-\/ ]*$/.test(value) && !/\n/.test(value);
}

export function validateEntry(entry, knownCommands) {
  const problems = [];
  const need = (cond, msg) => cond || problems.push(`${entry.id}: ${msg}`);
  need(entry.id && entry.title && entry.help, "falta id, title o help");
  need(["read", "write", "danger"].includes(entry.risk), "risk inválido");
  need(["flags", "values", "list", "actions", "enum"].includes(entry.type), "type inválido");
  if (entry.type === "enum") need(entry.read && entry.read.pattern && entry.options && entry.options.every((o) => o.value && o.title && o.help) && entry.write && entry.write.set, "enum necesita read.pattern, options (value, title, help) y write.set");
  if (entry.type === "values") need(entry.read && entry.read.pattern && entry.fields && entry.fields.length && entry.write && entry.write.set, "values necesita read.pattern, fields y write.set");
  const cmds = [];
  for (const a of entry.actions || []) {
    need(a.title && a.cmd && a.help, "acción sin title, cmd o help");
    cmds.push(a.cmd.split(/\s+/)[0]);
  }
  if (entry.read && entry.read.cmd) cmds.push(entry.read.cmd.split(/\s+/)[0]);
  const templates = [];
  if (entry.write) templates.push(...Object.values(entry.write));
  for (const f of entry.flags || []) {
    need(f.key && f.title && f.help, `flag sin key, title o help (${f.key})`);
    if (f.write) templates.push(...Object.values(f.write));
  }
  for (const t of templates) cmds.push(t.split(/\s+/)[0]);
  for (const c of cmds) need(knownCommands.has(norm(c)), `el comando ${c} no está en el catálogo de comandos`);
  if (entry.type === "flags") need(entry.read && ["tokens", "pairs"].includes(entry.read.mode), "flags necesita read.mode");
  return problems;
}
