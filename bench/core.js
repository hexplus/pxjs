// Micro-benchmarks shared by PXJS and QuickJS (see bench/run.sh).
function time(name, fn) {
    const t0 = now();
    const r = fn();
    print(name.padEnd(22), (now() - t0).toFixed(1), "ms", r);
}
time("int loop 3M", () => { let s = 0; for (let i = 0; i < 3000000; i++) s = (s + i) | 0; return s; });
time("double loop 1M", () => { let s = 0.5; for (let i = 0; i < 1000000; i++) s += 0.25; return s; });
time("fib(25)", () => { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(25); });
time("objects 200k", () => { let t = 0; for (let i = 0; i < 200000; i++) { const o = { x: i, y: i + 1 }; t += o.x + o.y; } return t; });
time("array push 300k", () => { const a = []; for (let i = 0; i < 300000; i++) a.push(i); return a.length; });
time("array map/filter", () => { const a = []; for (let i = 0; i < 100000; i++) a.push(i); return a.map(x => x * 2).filter(x => x % 3 == 0).length; });
time("string concat 100k", () => { let s = ""; for (let i = 0; i < 100000; i++) s += "x"; return s.length; });
time("property access 1M", () => { const o = { a: 1, b: 2, c: 3 }; let t = 0; for (let i = 0; i < 1000000; i++) t += o.a + o.c; return t; });
time("closures 200k", () => { let t = 0; for (let i = 0; i < 200000; i++) { const f = () => i; t += f(); } return t; });
time("JSON round trip", () => { const o = { list: [] }; for (let i = 0; i < 2000; i++) o.list.push({ id: i, name: "item " + i, tags: ["a", "b"] }); const s = JSON.stringify(o); return JSON.parse(s).list.length + s.length; });
time("method calls 500k", () => { function P(x) { this.x = x; } P.prototype.get = function () { return this.x; }; const p = new P(3); let t = 0; for (let i = 0; i < 500000; i++) t += p.get(); return t; });
