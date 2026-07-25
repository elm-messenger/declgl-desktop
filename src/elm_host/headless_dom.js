(function (global) {
  "use strict";

  const ELEMENT_NODE = 1;
  const TEXT_NODE = 3;
  const DOCUMENT_NODE = 9;
  const FRAGMENT_NODE = 11;

  class EventTarget {
    constructor() { this.__listeners = Object.create(null); }
    addEventListener(type, callback, options) {
      if (typeof callback !== "function") return;
      const capture = options === true || !!(options && options.capture);
      (this.__listeners[type] || (this.__listeners[type] = []))
        .push({ callback, capture });
    }
    removeEventListener(type, callback, options) {
      const capture = options === true || !!(options && options.capture);
      const list = this.__listeners[type];
      if (!list) return;
      this.__listeners[type] = list.filter(
        x => x.callback !== callback || x.capture !== capture
      );
    }
    dispatchEvent(event) { return dispatch(this, event); }
  }

  class HostEvent {
    constructor(type, init) {
      Object.assign(this, init || {});
      this.type = type;
      this.bubbles = this.bubbles !== false;
      this.cancelable = this.cancelable !== false;
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
      this.eventPhase = 0;
      this.__stop = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this.__stop = true; }
    stopImmediatePropagation() { this.__stop = true; this.__stopImmediate = true; }
  }

  function callListeners(target, event, capture, phase) {
    const list = (target.__listeners[event.type] || []).slice();
    event.currentTarget = target;
    event.eventPhase = phase;
    for (const entry of list) {
      if (entry.capture !== capture) continue;
      entry.callback.call(target, event);
      if (event.__stopImmediate) break;
    }
    event.__stopImmediate = false;
  }

  function dispatch(target, event) {
    if (!(event instanceof HostEvent)) event = new HostEvent(event.type, event);
    event.target = target;
    const path = [];
    for (let node = target.parentNode; node; node = node.parentNode) path.push(node);
    if (target !== document && !path.includes(document)) path.push(document);
    if (target !== global && !path.includes(global)) path.push(global);
    for (let i = path.length - 1; i >= 0 && !event.__stop; --i)
      callListeners(path[i], event, true, 1);
    if (!event.__stop) {
      callListeners(target, event, true, 2);
      if (!event.__stopImmediate) callListeners(target, event, false, 2);
    }
    if (event.bubbles) {
      for (let i = 0; i < path.length && !event.__stop; ++i)
        callListeners(path[i], event, false, 3);
    }
    event.currentTarget = null;
    event.eventPhase = 0;
    return !event.defaultPrevented;
  }

  class Node extends EventTarget {
    constructor(type, name) {
      super();
      this.nodeType = type;
      this.nodeName = name;
      this.parentNode = null;
      this.childNodes = [];
    }
    appendChild(child) {
      if (child.nodeType === FRAGMENT_NODE) {
        for (const item of child.childNodes.slice()) this.appendChild(item);
        return child;
      }
      if (child.parentNode) child.parentNode.removeChild(child);
      child.parentNode = this;
      this.childNodes.push(child);
      return child;
    }
    insertBefore(child, before) {
      if (before == null) return this.appendChild(child);
      const index = this.childNodes.indexOf(before);
      if (index < 0) throw new Error("insertBefore: reference is not a child");
      if (child.nodeType === FRAGMENT_NODE) {
        for (const item of child.childNodes.slice()) this.insertBefore(item, before);
        return child;
      }
      if (child.parentNode) child.parentNode.removeChild(child);
      child.parentNode = this;
      this.childNodes.splice(index, 0, child);
      return child;
    }
    removeChild(child) {
      const index = this.childNodes.indexOf(child);
      if (index < 0) throw new Error("removeChild: node is not a child");
      this.childNodes.splice(index, 1);
      child.parentNode = null;
      return child;
    }
    replaceChild(child, old) {
      const index = this.childNodes.indexOf(old);
      if (index < 0) throw new Error("replaceChild: node is not a child");
      if (child.parentNode) child.parentNode.removeChild(child);
      child.parentNode = this;
      old.parentNode = null;
      this.childNodes[index] = child;
      return old;
    }
    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
    get textContent() { return this.childNodes.map(x => x.textContent).join(""); }
    set textContent(value) {
      this.childNodes.forEach(x => { x.parentNode = null; });
      this.childNodes = [];
      if (value !== "" && value != null) this.appendChild(new TextNode(String(value)));
    }
  }

  class TextNode extends Node {
    constructor(text) { super(TEXT_NODE, "#text"); this.data = String(text); }
    get textContent() { return this.data; }
    set textContent(value) { this.data = String(value); }
    get length() { return this.data.length; }
    replaceData(offset, count, value) {
      this.data = this.data.slice(0, offset) + value + this.data.slice(offset + count);
    }
  }

  class Element extends Node {
    constructor(tag, namespaceURI) {
      super(ELEMENT_NODE, String(tag).toUpperCase());
      this.tagName = this.nodeName;
      this.namespaceURI = namespaceURI || null;
      this.attributes = Object.create(null);
      this.style = Object.create(null);
      this.id = "";
      this.value = "";
      this.checked = false;
      this.width = 0;
      this.height = 0;
    }
    setAttribute(name, value) {
      value = String(value);
      this.attributes[name] = value;
      if (name === "id") this.id = value;
    }
    getAttribute(name) { return name in this.attributes ? this.attributes[name] : null; }
    removeAttribute(name) {
      delete this.attributes[name];
      if (name === "id") this.id = "";
    }
    setAttributeNS(_ns, name, value) { this.setAttribute(name, value); }
    removeAttributeNS(_ns, name) { this.removeAttribute(name); }
    focus() { document.activeElement = this; }
    blur() { if (document.activeElement === this) document.activeElement = null; }
    getContext() { return null; }
    getBoundingClientRect() {
      return { x: 0, y: 0, left: 0, top: 0, right: this.width,
        bottom: this.height, width: this.width, height: this.height };
    }
  }

  class Document extends Node {
    constructor() {
      super(DOCUMENT_NODE, "#document");
      this.documentElement = new Element("html");
      this.body = new Element("body");
      this.appendChild(this.documentElement);
      this.documentElement.appendChild(this.body);
      this.activeElement = null;
      this.title = "";
      this.location = { href: "declgl://app/" };
    }
    createElement(tag) { return new Element(tag); }
    createElementNS(ns, tag) { return new Element(tag, ns); }
    createTextNode(text) { return new TextNode(text); }
    createDocumentFragment() { return new Node(FRAGMENT_NODE, "#document-fragment"); }
    getElementById(id) {
      function find(node) {
        if (node.id === id) return node;
        for (const child of node.childNodes || []) {
          const hit = find(child); if (hit) return hit;
        }
        return null;
      }
      return find(this);
    }
  }

  const windowTarget = new EventTarget();
  global.__listeners = windowTarget.__listeners;
  global.addEventListener = EventTarget.prototype.addEventListener;
  global.removeEventListener = EventTarget.prototype.removeEventListener;
  global.dispatchEvent = EventTarget.prototype.dispatchEvent;
  const document = new Document();
  global.window = global;
  global.self = global;
  global.document = document;
  global.Node = Node;
  global.Element = Element;
  global.Event = HostEvent;
  global.MouseEvent = HostEvent;
  global.KeyboardEvent = HostEvent;
  global.navigator = { userAgent: "declgl-desktop", platform: "desktop" };
  global.location = document.location;
  global.history = { go() {}, pushState() {}, replaceState() {} };
  global.scroll = function () {};
  global.performance = {
    timeOrigin: __declglHost.timeOrigin(),
    now: () => __declglHost.now()
  };
  global.setTimeout = (fn, delay) => __declglHost.setTimer(fn, Number(delay) || 0, false);
  global.clearTimeout = id => __declglHost.clearTimer(id);
  global.requestAnimationFrame = fn => __declglHost.setTimer(fn, 0, true);
  global.cancelAnimationFrame = id => __declglHost.clearTimer(id);
  global.console = {
    log: (...args) => __declglHost.log("info", args.map(String).join(" ")),
    warn: (...args) => __declglHost.log("warn", args.map(String).join(" ")),
    error: (...args) => __declglHost.log("error", args.map(String).join(" "))
  };

  const root = document.createElement("div");
  root.id = "declgl-elm-root";
  document.body.appendChild(root);
  global.__declglRoot = root;
  global.__declglDispatch = function (type, init) {
    let target = document.getElementById("elm-regl-canvas") || __declglRoot;
    if (type.indexOf("key") === 0 && document.activeElement) target = document.activeElement;
    return target.dispatchEvent(new HostEvent(type, init));
  };
  global.__declglSetViewport = function (width, height) {
    width = Math.max(0, Math.round(Number(width) || 0));
    height = Math.max(0, Math.round(Number(height) || 0));
    global.innerWidth = width;
    global.innerHeight = height;
    for (const node of [document.documentElement, document.body]) {
      node.clientWidth = width;
      node.clientHeight = height;
      node.offsetWidth = width;
      node.offsetHeight = height;
      node.scrollWidth = width;
      node.scrollHeight = height;
    }
  };
  global.__declglDispatchResize = function () {
    return global.dispatchEvent(new HostEvent("resize", {
      bubbles: false,
      target: global,
      innerWidth: global.innerWidth,
      innerHeight: global.innerHeight
    }));
  };
})(globalThis);
