// Pure logic of the keys and channels panel (no DOM): key formats, generation, the commands that
// change channels / admin keys / identity, channel share URLs and the redaction of secrets in logs.
import { joinPages } from "./parsers.js";

const enc = new TextEncoder();
export const CHANNEL_NAME_MAX = 11; // ChannelSettings.name holds 12 bytes including the terminator
export const OWNER_LONG_MAX = 39;
export const OWNER_SHORT_MAX = 4;
export const KEY_SIZES = [16, 32];

// ---------- key formats ----------
export const bytesToHex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
export const bytesToBase64 = (bytes) => btoa(String.fromCharCode(...bytes));

function hexToBytes(hex) {
  return Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16)));
}

function base64ToBytes(text) {
  const std = text.replace(/-/g, "+").replace(/_/g, "/");
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(std)) return null;
  const padded = std + "=".repeat((4 - (std.length % 4)) % 4);
  try {
    return Uint8Array.from(atob(padded), (c) => c.charCodeAt(0));
  } catch {
    return null;
  }
}

// Accepts hex (32 or 64 digits) or base64 (16 or 32 bytes), with stray spaces. -> {bytes, format} | {error}
export function parseKey(text, sizes = KEY_SIZES) {
  const clean = String(text).replace(/\s+/g, "");
  if (!clean) return { error: "Falta la clave." };
  const wanted = sizes.map((n) => `${n} bytes`).join(" o ");
  if (/^[0-9a-f]+$/i.test(clean) && sizes.includes(clean.length / 2)) return { bytes: hexToBytes(clean), format: "hex" };
  const bytes = base64ToBytes(clean);
  if (bytes && sizes.includes(bytes.length)) return { bytes, format: "base64" };
  return { error: `Clave no válida: hex o base64 de ${wanted}.` };
}

export function generateKey(size = 32, fillRandom = (a) => crypto.getRandomValues(a)) {
  if (!KEY_SIZES.includes(size)) throw new Error("tamaño de clave no válido");
  return fillRandom(new Uint8Array(size));
}

// ---------- reading the node ----------
// CH: "0:SFNarrow(1) 1:Nexus(2)"; CHASH: "0:SFNarrow(0x20/32)". Names may hold spaces.
export function parseChannelTable(chReply, chashReply) {
  const roles = new Map();
  for (const m of joinPages(chReply).matchAll(/(\d+):(.+?)\((\d+)\)(?=\s|$)/g)) roles.set(Number(m[1]), { name: m[2], role: Number(m[3]) });
  const hashes = new Map();
  for (const m of joinPages(chashReply).matchAll(/(\d+):(.+?)\(0x([0-9A-Fa-f]+)\/(\d+)\)/g)) hashes.set(Number(m[1]), { name: m[2], hash: Number(m[4]) });
  return [...roles].map(([index, r]) => ({ index, name: r.name, role: r.role, hash: hashes.has(index) ? hashes.get(index).hash : null }));
}

export const isTrustedChannel = (name) => /^nexus2?$/i.test(name);
export const isMeshCoreChannel = (name) => /^MC:/.test(name);

// TRUSTED: "[0] name (ON)" per line -> [{slot, name, active}]
export function parseTrusted(reply) {
  return [...joinPages(reply).matchAll(/\[(\d)\]\s*(.*?)\s*\((ON|OFF)\)/g)].map((m) => ({ slot: Number(m[1]), name: m[2], active: m[3] === "ON" }));
}

// ---------- validation ----------
const clean = (s) => String(s).trim();
const hasControl = (s) => /[\x00-\x1f;]/.test(s);

export function channelNameError(name, { allowSpaces }) {
  const n = clean(name);
  if (!n) return "Falta el nombre.";
  if (enc.encode(n).length > CHANNEL_NAME_MAX) return `El nombre admite como máximo ${CHANNEL_NAME_MAX} caracteres.`;
  if (hasControl(n)) return "El nombre tiene caracteres no permitidos.";
  if (!allowSpaces && /\s/.test(n)) return "Al crear un canal el nombre no puede llevar espacios.";
  return null;
}

// ---------- commands (each throws a Spanish Error when the input is not valid) ----------
const prefix = (unprotected) => (unprotected ? "U" : "");

function requireKey(text) {
  const k = parseKey(text);
  if (k.error) throw new Error(k.error);
  return bytesToBase64(k.bytes);
}

export function addChannelCommand(name, keyText) {
  const err = channelNameError(name, { allowSpaces: false });
  if (err) throw new Error(err);
  return keyText ? `ADDCH ${clean(name)} ${requireKey(keyText)}` : `ADDCH ${clean(name)}`;
}

export function renameCommand(index, name, unprotected = false) {
  const err = channelNameError(name, { allowSpaces: true });
  if (err) throw new Error(err);
  return `${prefix(unprotected)}CHNAME ${index} ${clean(name)}`;
}

export function channelKeyCommand(index, keyText, unprotected = false) {
  return `${prefix(unprotected)}CHPSK ${index} ${requireKey(keyText)}`;
}

export function deleteChannelCommand(index, unprotected = false) {
  if (index === 0) throw new Error("El canal principal (0) no se puede borrar.");
  return `${prefix(unprotected)}DELCH ${index}`;
}

// Admin keys are 32 bytes; the firmware takes 64 hex digits.
export function adminKeyCommand(slot, keyText) {
  if (!Number.isInteger(slot) || slot < 1 || slot > 4) throw new Error("El hueco debe ser de 1 a 4.");
  const k = parseKey(keyText, [32]);
  if (k.error) throw new Error(k.error);
  return `SKEY ${slot} ${bytesToHex(k.bytes)}`;
}

export function wipeAdminKeyCommand(slot) {
  if (!Number.isInteger(slot) || slot < 1 || slot > 4) throw new Error("El hueco debe ser de 1 a 4.");
  return `SKEY ${slot} WIPE`;
}

export function trustedCommand(slot, name) {
  if (slot !== 0 && slot !== 1) throw new Error("El hueco debe ser 0 o 1.");
  if (name === null) return `TRUSTED ${slot} OFF`;
  const n = clean(name);
  if (!n || n.length > 15 || hasControl(n)) throw new Error("El nombre debe tener de 1 a 15 caracteres.");
  if (n.toUpperCase() === "OFF") throw new Error("«OFF» desactiva el hueco; usa el botón Desactivar.");
  return `TRUSTED ${slot} ${n}`;
}

export function ownerCommand(shortName, longName) {
  const s = clean(shortName);
  const l = clean(longName);
  if (!s || s.length > OWNER_SHORT_MAX || /\s/.test(s) || hasControl(s)) throw new Error(`El nombre corto lleva de 1 a ${OWNER_SHORT_MAX} caracteres, sin espacios.`);
  if (!l || enc.encode(l).length > OWNER_LONG_MAX || hasControl(l)) throw new Error(`El nombre largo lleva de 1 a ${OWNER_LONG_MAX} caracteres.`);
  return `OWNER ${s} ${l}`;
}

// MeshCore shows its channel key as 32 hex digits; the Meshtastic side wants it as base64.
export function meshCoreKeyToBase64(hex) {
  const k = parseKey(hex, [16]);
  if (k.error || k.format !== "hex") throw new Error("Pon los 32 dígitos hex que MeshCore muestra como clave del canal.");
  return bytesToBase64(k.bytes);
}

// ---------- sharing ----------
// Meshtastic channel URL: https://meshtastic.org/e/#<base64url(ChannelSet)>; ChannelSet.settings = 1,
// ChannelSettings.psk = 2, name = 3. `add=true` makes the receiving app add it instead of replacing its channels.
export function channelUrl(name, pskBytes) {
  const varint = (n) => (n < 128 ? [n] : [(n & 127) | 128, n >> 7]);
  const field = (num, bytes) => [(num << 3) | 2, ...varint(bytes.length), ...bytes];
  const settings = [...field(2, pskBytes), ...field(3, enc.encode(name))];
  const set = field(1, settings);
  const b64 = bytesToBase64(set).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  return `https://meshtastic.org/e/?add=true#${b64}`;
}

// ---------- keeping secrets out of logs and recordings ----------
// What can precede the command in what is logged: "(DM) ", "/mtcl " or "/mtc-device !1234abcd ".
const LOGGED_PREFIX = String.raw`((?:\([^)]*\)\s+)?(?:\/[\w-]+\s+)?(?:!\w{8}\s+)?`;

export function redact(command) {
  const secret = (rest) => new RegExp(`^${LOGGED_PREFIX}${rest})\\S+`, "i");
  return String(command)
    .replace(secret(String.raw`U?CHPSK\s+\S+\s+`), "$1***")
    .replace(secret(String.raw`ADDCH\s+\S+\s+`), "$1***")
    .replace(secret(String.raw`SKEY\s+\d\s+(?!OFF\b|WIPE\b)`), "$1***")
    .replace(secret(String.raw`MESHCORE\s+KEY\s+`), "$1***");
}
