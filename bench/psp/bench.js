// PSP-sized version of bench/core.js (about a fifth of the work): the same
// script on PXJS and QuickJS, see main.c. Uses only print() and now().
function time(name, fn) {
    const t0 = now();
    const r = fn();
    print(name.padEnd(22), (now() - t0).toFixed(1), "ms", r);
}
time("empty loop 200k", () => { let i = 0; for (; i < 200000; i++); return i; });
time("int loop 200k", () => { let s = 0; for (let i = 0; i < 200000; i++) s = (s + i) | 0; return s; });
time("small int loop 200k", () => { let s = 0; for (let i = 0; i < 200000; i++) s = (s + (i & 7)) | 0; return s; });
time("double loop 200k", () => { let s = 0.5; for (let i = 0; i < 200000; i++) s += 0.25; return s; });
time("fib(20)", () => { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(20); });
time("objects 40k", () => { let t = 0; for (let i = 0; i < 40000; i++) { const o = { x: i, y: i + 1 }; t += o.x + o.y; } return t; });
time("array push 60k", () => { const a = []; for (let i = 0; i < 60000; i++) a.push(i); return a.length; });
time("array map/filter 20k", () => { const a = []; for (let i = 0; i < 20000; i++) a.push(i); return a.map(x => x * 2).filter(x => x % 3 == 0).length; });
time("string concat 20k", () => { let s = ""; for (let i = 0; i < 20000; i++) s += "x"; return s.length; });
time("property access 200k", () => { const o = { a: 1, b: 2, c: 3 }; let t = 0; for (let i = 0; i < 200000; i++) t += o.a + o.c; return t; });
time("closures 40k", () => { let t = 0; for (let i = 0; i < 40000; i++) { const f = () => i; t += f(); } return t; });
time("JSON round trip 400", () => { const o = { list: [] }; for (let i = 0; i < 400; i++) o.list.push({ id: i, name: "item " + i, tags: ["a", "b"] }); const s = JSON.stringify(o); return JSON.parse(s).list.length + s.length; });
time("method calls 100k", () => { function P(x) { this.x = x; } P.prototype.get = function () { return this.x; }; const p = new P(3); let t = 0; for (let i = 0; i < 100000; i++) t += p.get(); return t; });
time("class + template 20k", () => { class V { constructor(x, y) { this.x = x; this.y = y; } len() { return this.x * this.x + this.y * this.y; } } let s = ""; for (let i = 0; i < 20000; i++) { const v = new V(i, 2); s = `${v.len()}`; } return s; });
time("string methods 20k", () => { let n = 0; for (let i = 0; i < 20000; i++) { const s = "item-" + i; n += s.split("-")[1].length + s.indexOf("-") + s.toUpperCase().length; } return n; });
time("Map/Set 20k", () => { const m = new Map(), st = new Set(); for (let i = 0; i < 20000; i++) { m.set("k" + (i % 500), i); st.add(i % 700); } return m.size + st.size; });
