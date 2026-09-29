// Application startup workload: define the app's code (classes, closures,
// a small component system), create its state, and build a UI tree of
// plain objects, as a UI library does before its first render. The
// engine's own startup (px_new) is reported by the PSP bench separately.
const t0 = now();

class Node {
    constructor(tag, props, children) {
        this.tag = tag;
        this.props = props || {};
        this.children = children || [];
        this.layout = { x: 0, y: 0, w: 0, h: 0 };
        this.dirty = true;
    }
    append(c) { this.children.push(c); return this; }
}

function h(tag, props, ...children) { return new Node(tag, props, children); }

function signal(v) {
    const subs = [];
    return {
        get: () => v,
        set: nv => { v = nv; for (const s of subs) s(v); },
        sub: f => { subs.push(f); },
    };
}

const store = {
    user: signal({ name: "Player", level: 3 }),
    items: signal([]),
    settings: signal({ volume: 7, lang: "en", theme: "dark" }),
};
const list = [];
for (let i = 0; i < 300; i++) list.push({ id: i, name: "Entry " + i, value: i * 3 % 17, on: i % 2 === 0 });
store.items.set(list);

function Row(it) {
    return h("row", { key: it.id, class: it.on ? "on" : "off" },
        h("text", { value: it.name }), h("text", { value: String(it.value) }), h("toggle", { checked: it.on }));
}
function Screen() {
    const u = store.user.get();
    return h("screen", { title: "Library" },
        h("header", null, h("text", { value: u.name + " (level " + u.level + ")" })),
        h("list", { rows: 300 }, ...store.items.get().map(Row)),
        h("footer", null, h("text", { value: "Volume " + store.settings.get().volume })));
}

let nodes = 0, updates = 0;
function count(n) { nodes++; for (const c of n.children) count(c); }
const tree = Screen();
count(tree);
store.settings.sub(() => updates++);
for (let i = 0; i < 10; i++) store.settings.set({ volume: i, lang: "en", theme: "dark" });
const ms = now() - t0;
print("app_startup: " + ms.toFixed(1) + " ms, " + nodes + " nodes, " + updates + " updates");
