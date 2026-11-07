// LoRa preset maths for the preset wizard. Estimates, not measurements: they rank presets
// against each other (speed vs reach), they do not predict a real link.

// Demodulator SNR limit per spreading factor (SX126x datasheet figures).
const SNR_LIMIT = { 5: -2.5, 6: -5, 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20 };
const NOISE_FIGURE_DB = 6;

// Raw LoRa bit rate in bit/s. crDen is the denominator of the coding rate (4/5 -> 5).
export function bitrate(sf, bwKHz, crDen) {
  return sf * ((bwKHz * 1000) / 2 ** sf) * (4 / crDen);
}

// Receiver sensitivity in dBm; lower (more negative) hears weaker signals, i.e. reaches further.
export function sensitivity(sf, bwKHz) {
  return -174 + 10 * Math.log10(bwKHz * 1000) + NOISE_FIGURE_DB + SNR_LIMIT[sf];
}

// Extra link budget over a reference preset, in dB. Every 6 dB roughly doubles free-space range.
export function reachGainDb(preset, reference) {
  return sensitivity(reference.sf, reference.bw) - sensitivity(preset.sf, preset.bw);
}

export function describe(preset) {
  return { ...preset, bitrate: bitrate(preset.sf, preset.bw, preset.cr), sensitivity: sensitivity(preset.sf, preset.bw) };
}

// The node's own LORA reply reports 62 for 62.5 kHz and 31 for 31.25 kHz, so BW and frequency
// match with a tolerance. The coding rate is left out: the node can report a rate other than the table's.
export function matchPreset(lora, presets) {
  if (!lora) return null;
  return presets.find((p) => p.sf === lora.sf && Math.abs(p.bw - lora.bw) < 0.5 && Math.abs(p.freq - lora.freq) < 0.01) || null;
}

export function groupPresets(presets) {
  const groups = new Map();
  for (const p of presets) {
    if (!groups.has(p.group)) groups.set(p.group, []);
    groups.get(p.group).push(p);
  }
  return groups;
}

// 2.4 GHz presets need a 2.4 GHz capable chip (LR1121 / LR2021); single-band radios refuse them.
export const needsDualBand = (p) => p.region === "LORA_24";

// Human wording for the trade-off, relative to the preset in use.
export function compareTo(preset, reference) {
  const speed = bitrate(preset.sf, preset.bw, preset.cr) / bitrate(reference.sf, reference.bw, reference.cr);
  const reach = reachGainDb(preset, reference);
  const speedText = Math.abs(speed - 1) < 0.05 ? "misma velocidad" : speed > 1 ? `${speed.toFixed(1)}× más rápido` : `${(1 / speed).toFixed(1)}× más lento`;
  const reachText = Math.abs(reach) < 0.5 ? "mismo alcance" : reach > 0 ? `+${reach.toFixed(1)} dB de alcance` : `${reach.toFixed(1)} dB de alcance`;
  return { speed, reach, text: `${speedText}, ${reachText}` };
}
