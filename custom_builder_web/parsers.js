// Parsers: Nexus text replies -> plain objects. Written against real captures
// (tests/fixtures/gat562-capture.json), not against the documentation.

// The node pages long replies as "P1: ...", "P2: ..." and cuts them at a byte budget, so a page
// can end in the middle of a record ("...:DLF@:2:-7" / "P2: :K+ ..."). ble.js hands the pages
// over joined by "\n"; drop that join and the "Pn: " marker to get the original text back.
export function joinPages(text) {
  return String(text || "")
    .replace(/^\S+ MESHTACORE:? Paging [^\n]*\n?/, "") // "🔴 MESHTACORE: Paging X..." banner
    .replace(/\nP\d+: /g, "")
    .replace(/^P1: /, "")
    .trim();
}

const num = (s) => (s === undefined ? null : Number(s));

export function parseLora(text) {
  const m = /SF:(\d+) BW:([\d.]+) CR:(\d+)\s+Preset:(\d+) \((\w+)\)\s+Freq:([\d.]+)MHz\s+Power:(-?\d+)dBm/.exec(text);
  if (!m) return null;
  return { sf: num(m[1]), bw: num(m[2]), cr: num(m[3]), preset: num(m[4]), presetState: m[5], freq: num(m[6]), power: num(m[7]) };
}

export function parseLoraStatus(text) {
  const g = (re) => {
    const m = re.exec(text);
    return m ? num(m[1]) : null;
  };
  const status = {
    freq: g(/Freq:\s*([\d.]+)MHz/),
    bw: g(/BW:([\d.]+)kHz/),
    sf: g(/SF:(\d+)/),
    cr: g(/CR:(\d+)/),
    preambleSymbols: g(/Pre:(\d+)/),
    preambleMs: g(/Pre:\d+ \((\d+)ms\)/),
    slotMs: g(/Slot:(\d+)ms/),
    cwMin: g(/CW:(\d+)-/),
    cwMax: g(/CW:\d+-(\d+)/),
    chipPower: g(/ChipPwr:(-?\d+)dBm/),
    bitrate: g(/Speed:(\d+) b\/s/),
  };
  const sw = /SW:0x([0-9A-Fa-f]+)/.exec(text);
  status.syncWord = sw ? parseInt(sw[1], 16) : null;
  return status.freq === null ? null : status;
}

// "!id:name:hops:snr:flags" records separated by spaces. The name may hold anything but is
// followed by ":<hops>:<snr>:", so match it lazily. hops -1 marks this node itself.
export function parseNodes(text) {
  const body = joinPages(text).replace(/^MESHTACORE NodeDB:\n?/, "");
  const re = /!([0-9a-f]{8}):(.*?):(-?\d+):(-?\d+):(\S+?)(?= !|\s*$)/g;
  const nodes = [];
  for (const m of body.matchAll(re)) {
    nodes.push({ id: "!" + m[1], name: m[2], hops: num(m[3]), snr: num(m[4]), flags: m[5], self: num(m[3]) < 0 });
  }
  return nodes;
}

export function parseChannelStats(text) {
  const body = joinPages(text).replace(/^MESHTACORE Channel Stats:\n?/, "");
  const total = /Total: (\d+) pkts \(([\d.]+)(KB|B)\)/.exec(body);
  const channels = [];
  const re = /Ch(\d+)\(([^)]*)\): (\d+) pkts \(([\d.]+)%\) ([\d.]+)(KB|B) \(([\d.]+)%\)/g;
  for (const m of body.matchAll(re)) {
    channels.push({
      index: num(m[1]),
      name: m[2],
      known: m[2] !== "??",
      packets: num(m[3]),
      packetShare: num(m[4]),
      bytes: Math.round(num(m[5]) * (m[6] === "KB" ? 1024 : 1)),
      byteShare: num(m[7]),
    });
  }
  return {
    totalPackets: total ? num(total[1]) : null,
    totalBytes: total ? Math.round(num(total[2]) * (total[3] === "KB" ? 1024 : 1)) : null,
    channels,
  };
}

// "0:SFNarrow(1) 1:Nexus(2) ..." - the number in brackets is the role.
export function parseChannels(text) {
  const out = [];
  for (const m of String(text).matchAll(/(\d+):(\S+?)\((\d+)\)/g)) out.push({ index: num(m[1]), name: m[2], role: num(m[3]) });
  return out;
}

export function parsePower(text) {
  const m = /ChU:([\d.]+)% TxU:([\d.]+)%/.exec(text);
  if (!m) return null;
  const v = /V1:([\d.]+)V I1:(-?[\d.]+)mA/.exec(text);
  return { channelUtil: num(m[1]), txUtil: num(m[2]), voltage: v ? num(v[1]) : null, current: v ? num(v[2]) : null, hasSensor: !/No Sensor Data/.test(text) };
}

export function parseVolt(text) {
  const m = /Bat: ([\d.]+)V, Hib: (YES|NO)/.exec(text);
  return m ? { battery: num(m[1]), hibernating: m[2] === "YES" } : null;
}

export function parseInfo(text) {
  const body = joinPages(text).replace(/^MESHTACORE Info:\n?/, "");
  const head = /(![0-9a-f]{8}) \[([^\]]*)\] (\w+)/.exec(body);
  const g = (label) => {
    const m = new RegExp("^" + label + ": (.*)$", "m").exec(body);
    return m ? m[1].trim() : null;
  };
  if (!head) return null;
  return { id: head[1], shortName: head[2], role: head[3], mac: g("MAC"), version: g("Ver"), target: g("Tgt"), repo: g("Repo"), built: g("Built"), flags: g("Flags") };
}
