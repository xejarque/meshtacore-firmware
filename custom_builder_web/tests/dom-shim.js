// Mínimo DOM para probar nexus-options.js sin navegador: solo lo que ese módulo usa. No es un DOM completo.
const camelToKebab = (s) => s.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());

class TextNode {
  constructor(text) {
    this.text = text;
    this.isText = true;
  }
  get textContent() {
    return this.text;
  }
}

class Element {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.attrs = {};
    this.listeners = {};
    this.parent = null;
    this.hidden = false;
    this.checked = false;
    this.disabled = false;
    this.value = "";
    const self = this;
    this.dataset = new Proxy(
      {},
      {
        get: (_, k) => self.attrs["data-" + camelToKebab(String(k))],
        set: (_, k, v) => {
          self.attrs["data-" + camelToKebab(String(k))] = String(v);
          return true;
        },
      },
    );
  }
  set className(v) {
    this.attrs.class = v;
  }
  setAttribute(k, v) {
    this.attrs[k] = String(v);
  }
  getAttribute(k) {
    return this.attrs[k] === undefined ? null : this.attrs[k];
  }
  appendChild(c) {
    if (c.parent) c.parent.children = c.parent.children.filter((x) => x !== c);
    c.parent = this;
    this.children.push(c);
    return c;
  }
  replaceChildren(...nodes) {
    this.children = [];
    nodes.forEach((n) => this.appendChild(n));
  }
  set textContent(v) {
    this.children = [];
    if (v) this.appendChild(new TextNode(String(v)));
  }
  get textContent() {
    return this.children.map((c) => c.textContent).join("");
  }
  addEventListener(type, fn) {
    (this.listeners[type] = this.listeners[type] || []).push(fn);
  }
  // Como un clic del usuario: cambia el estado y dispara `change`, que sube hasta los ancestros.
  click() {
    if (this.disabled) return;
    this.checked = !this.checked;
    this.dispatch("change");
  }
  dispatch(type) {
    for (let n = this; n; n = n.parent)
      (n.listeners[type] || []).forEach((fn) => fn({ target: this }));
  }
  *descendants() {
    for (const c of this.children) {
      if (c.isText) continue;
      yield c;
      yield* c.descendants();
    }
  }
  matches(sel) {
    const m = /^([a-z0-9]*)((?:\[[^\]]+\])*)(:checked)?$/.exec(sel);
    if (!m) throw new Error("selector no soportado por el shim: " + sel);
    if (m[1] && m[1] !== this.tag) return false;
    for (const a of m[2].matchAll(/\[([^\]=]+)(?:="([^"]*)")?\]/g)) {
      const v = this.attrs[a[1]];
      if (v === undefined || (a[2] !== undefined && v !== a[2])) return false;
    }
    return !m[3] || this.checked;
  }
  querySelectorAll(sel) {
    return [...this.descendants()].filter((n) => n.matches(sel));
  }
  querySelector(sel) {
    return this.querySelectorAll(sel)[0] || null;
  }
}

function makeDocument() {
  const body = new Element("body");
  return {
    body,
    createElement: (tag) => new Element(tag),
    createTextNode: (t) => new TextNode(t),
    getElementById: (id) =>
      [...body.descendants()].find((n) => n.attrs.id === id) || null,
    querySelectorAll: (sel) => body.querySelectorAll(sel),
    querySelector: (sel) => body.querySelector(sel),
  };
}

module.exports = { makeDocument };
