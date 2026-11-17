// Run: node --test custom_builder_web/tests/
import test from "node:test";
import assert from "node:assert/strict";
import { friendlyError } from "../friendly-errors.js";

test("selector cancelado: explica cómo elegir el puerto", () => {
  assert.match(
    friendlyError(new Error("No port selected by the user.")),
    /No elegiste ningún puerto/,
  );
});

test("puerto ocupado: dice qué cerrar", () => {
  assert.match(
    friendlyError(new Error("Failed to open serial port.")),
    /Otra pestaña o programa/,
  );
});

test("sin respuesta de la placa: da la secuencia BOOT + RESET", () => {
  assert.match(
    friendlyError(new Error("Failed to connect with the device")),
    /BOOT.*RESET/,
  );
});

test("límite de GitHub se distingue de un fallo de red", () => {
  assert.match(friendlyError(new Error("HTTP 403")), /limitado/);
  assert.match(friendlyError(new Error("Failed to fetch")), /conexión/);
});

test("error desconocido: conserva el texto original y da una pista", () => {
  const text = friendlyError("algo raro");
  assert.match(text, /algo raro/);
  assert.match(text, /desconecta la placa/);
});
