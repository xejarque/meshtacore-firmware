// Remote nodes tab: pick another node from the local node's NodeDB, choose how to reach it (a Nexus channel or a
// direct message) and administer it with the same settings catalogue as the local node.
// `runLocal(cmd)` talks to the node on Bluetooth; `runRemote(id, cmd, route)` goes over the mesh to the other one.
import { parseNodes } from "./parsers.js";
import { parseChannelTable, isTrustedChannel } from "./keys-core.js";
import { SettingsPanel } from "./settings.js";
import { cleanReply } from "./settings-core.js";

const h = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  kids.forEach((k) => n.append(k));
  return n;
};

export const ROUTES = {
  channel: {
    title: "Por un canal Nexus",
    needs: ["El nodo remoto tiene ese canal con la misma clave que tú.", "Si en el remoto «Exigir firma» está activo, esta web no puede firmar las órdenes y las rechazará.", "Es un mensaje por canal: todos los nodos que lo oigan lo ven (la clave del canal los protege)."],
  },
  dm: {
    title: "Por mensaje directo",
    needs: ["En el remoto «Aceptar comandos por mensaje directo» está activo.", "Tu nodo está en la lista de confianza del remoto (clave pública de administrador).", "Tu nodo conoce la clave pública del remoto: si no, el mensaje no sale cifrado."],
  },
};

export class RemotePanel {
  constructor({ root, runLocal, runRemote, isConnected, catalogues }) {
    this.root = root;
    this.runLocal = runLocal;
    this.runRemote = runRemote;
    this.isConnected = isConnected;
    this.catalogues = catalogues;
    this.nodes = null;
    this.channels = null;
    this.target = null; // {id, name}
    this.via = "channel";
    this.channel = 1;
    this.test = null; // {ok, text, secs}
    this.manage = false;
    this.settings = null;
  }

  reset() {
    this.nodes = null;
    this.channels = null;
    this.target = null;
    this.test = null;
    this.manage = false;
    this.settings = null;
    this.render();
  }

  get route() {
    return this.via === "dm" ? { via: "dm" } : { via: "channel", channel: this.channel };
  }

  async loadNodes(btn) {
    btn.disabled = true;
    btn.textContent = "Leyendo…";
    try {
      this.nodes = parseNodes(await this.runLocal("NODES")).filter((n) => !n.self);
      const ch = parseChannelTable(await this.runLocal("CH"), "");
      this.channels = ch;
      const nexus = ch.find((c) => isTrustedChannel(c.name));
      if (nexus) this.channel = nexus.index;
    } catch (e) {
      this.nodes = [];
    }
    this.render();
  }

  async probe(btn) {
    btn.disabled = true;
    btn.textContent = "Esperando respuesta del nodo remoto…";
    const started = Date.now();
    let text = "";
    try {
      text = await this.runRemote(this.target.id, "ROLE", this.route);
    } catch (e) {
      text = "";
    }
    this.test = { ok: /MESHTACORE/.test(text), text: cleanReply(text), secs: Math.round((Date.now() - started) / 1000) };
    this.render();
  }

  startManaging() {
    this.manage = true;
    const label = `${this.target.name || "nodo"} (${this.target.id})`;
    this.settings = new SettingsPanel({
      root: h("div"),
      run: (cmd) => this.runRemote(this.target.id, cmd, this.route),
      catalogues: this.catalogues,
      isConnected: this.isConnected,
      label,
      autoRead: false,
    });
    this.render();
  }

  // ---------- pieces ----------
  destination() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "1. Nodo remoto"));
    if (!this.isConnected()) return card.append(h("p", { className: "muted" }, "Conecta primero tu nodo por Bluetooth: es el que enviará las órdenes por radio.")), card;
    const manual = h("input", { type: "text", placeholder: "o escribe un ID: !1a2b3c4d", spellcheck: false, value: this.target ? this.target.id : "" });
    const useManual = h("button", { textContent: "Usar este ID" });
    useManual.onclick = () => {
      if (!/^![0-9a-f]{8}$/i.test(manual.value.trim())) return alert("El ID debe ser como !1a2b3c4d (8 cifras hex).");
      this.target = { id: manual.value.trim().toLowerCase(), name: "" };
      this.test = null;
      this.manage = false;
      this.render();
    };
    const load = h("button", { className: "sx-add", textContent: this.nodes ? "Releer lista de nodos" : "Leer la lista de nodos de mi nodo" });
    load.onclick = () => this.loadNodes(load);
    card.append(h("div", { className: "sx-addrow" }, load));
    if (this.nodes) {
      const filter = h("input", { type: "search", placeholder: "Buscar por nombre o ID…" });
      const list = h("div", { className: "rx-nodes" });
      const draw = () => {
        const q = filter.value.toLowerCase();
        list.replaceChildren(
          ...this.nodes
            .filter((n) => !q || (n.name + n.id).toLowerCase().includes(q))
            .sort((a, b) => a.hops - b.hops)
            .slice(0, 60)
            .map((n) => {
              const b = h("button", { className: "rx-node" + (this.target && this.target.id === n.id ? " on" : "") }, h("b", {}, n.name || "(sin nombre)"), h("small", {}, `${n.id} · ${n.hops === 0 ? "directo" : n.hops + " saltos"} · SNR ${n.snr}`));
              b.onclick = () => ((this.target = { id: n.id, name: n.name }), (this.test = null), (this.manage = false), this.render());
              return b;
            }),
        );
      };
      filter.oninput = draw;
      draw();
      card.append(filter, list);
    }
    card.append(h("div", { className: "sx-addrow" }, manual, useManual));
    if (this.target) card.append(h("p", { className: "sx-state on" }, `Elegido: ${this.target.name || "(sin nombre)"} ${this.target.id}`));
    return card;
  }

  routeCard() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "2. Cómo llegar a él"));
    for (const [key, info] of Object.entries(ROUTES)) {
      const radio = h("input", { type: "radio", name: "rx-via", checked: this.via === key });
      radio.onchange = () => ((this.via = key), (this.test = null), this.render());
      const box = h("label", { className: "rx-route" + (this.via === key ? " on" : "") }, radio, h("div", {}, h("b", {}, info.title), h("ul", {}, ...info.needs.map((t) => h("li", {}, t)))));
      card.append(box);
    }
    if (this.via === "channel") {
      const select = h("select", {});
      for (const c of this.channels || []) select.append(h("option", { value: c.index, textContent: `${c.index}: ${c.name}`, selected: c.index === this.channel }));
      select.onchange = () => ((this.channel = Number(select.value)), (this.test = null));
      card.append(h("label", {}, "Canal ", this.channels ? select : h("small", { className: "muted" }, "Lee antes la lista de nodos para ver tus canales.")));
    }
    return card;
  }

  testCard() {
    const card = h("section", { className: "sx-card" }, h("h3", {}, "3. Probar la conexión"), h("p", { className: "sx-help" }, "Envía una consulta inocua (ROLE) al nodo remoto y espera su respuesta. Por radio puede tardar de unos segundos a casi un minuto."));
    const run = h("button", { className: "sx-add", textContent: "Probar", disabled: !this.target || !this.isConnected() });
    run.onclick = () => this.probe(run);
    card.append(h("div", { className: "sx-addrow" }, run));
    if (this.test) {
      card.append(h("p", { className: "sx-note " + (this.test.ok ? "ok" : "err") }, this.test.ok ? `✓ Respondió en ${this.test.secs} s: ${this.test.text}` : `Sin respuesta tras ${this.test.secs} s. Puede no estar al alcance, o haber rechazado la orden en silencio. Revisa los requisitos de la vía elegida.`));
    }
    const go = h("button", { className: this.test && this.test.ok ? "sx-add" : "sx-clear", textContent: this.test && this.test.ok ? "Administrar este nodo" : "Administrar de todos modos", disabled: !this.target || !this.isConnected() });
    go.onclick = () => this.startManaging();
    card.append(h("div", { className: "sx-addrow" }, go));
    return card;
  }

  consoleCard() {
    const input = h("input", { type: "text", placeholder: "Comando, por ejemplo INFO", spellcheck: false });
    const out = h("pre", { className: "out" });
    const run = h("button", { className: "sx-add", textContent: "Enviar", disabled: !this.target });
    run.onclick = async () => {
      if (!input.value.trim()) return;
      if (!confirm(`Vas a enviar a ${this.target.id}:\n\n${input.value}\n\n¿Continuar?`)) return;
      run.disabled = true;
      out.textContent = "Esperando respuesta…";
      try {
        out.textContent = cleanReply(await this.runRemote(this.target.id, input.value, this.route)) || "(sin respuesta)";
      } catch (e) {
        out.textContent = "Error: " + e.message;
      }
      run.disabled = false;
    };
    input.onkeydown = (e) => e.key === "Enter" && run.click();
    return h("section", { className: "sx-card" }, h("h3", {}, "Comando libre"), h("p", { className: "sx-help" }, "Para lo que no está en los ajustes. Se envía tal cual al nodo remoto, con confirmación."), h("div", { className: "sx-addrow" }, input, run), out);
  }

  managing() {
    const wrap = h("div", {});
    wrap.append(h("div", { className: "rx-banner" }, h("b", {}, `Administrando ${this.target.name || "nodo"} ${this.target.id}`), h("span", {}, this.via === "dm" ? " · por mensaje directo" : ` · por el canal ${this.channel}`)));
    const host = h("div", { className: "rx-settings" });
    wrap.append(host);
    this.settings.root = host;
    this.settings.render();
    wrap.append(this.consoleCard());
    const back = h("button", { textContent: "Elegir otro nodo" });
    back.onclick = () => ((this.manage = false), (this.settings = null), this.render());
    wrap.append(h("div", { className: "sx-addrow" }, back));
    return wrap;
  }

  render() {
    if (this.manage && this.settings) return this.root.replaceChildren(this.managing());
    this.root.replaceChildren(
      h("p", { className: "sx-intro" }, "Administra otro nodo desde tu nodo, por radio. Tu nodo conectado por Bluetooth envía las órdenes y recoge las respuestas, que llegan despacio. Lo normal sigue siendo administrar tu propio nodo; usa esto con cuidado, porque un error aquí no se puede corregir desde el teclado."),
      this.destination(),
      this.routeCard(),
      this.testCard(),
    );
  }
}
