// Number <-> string conversion cost (soft doubles on the PSP).
const N = 5000;
const vals = [];
let seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
for (let i = 0; i < N; i++) vals.push(rnd() * 1000);
const ints = [];
for (let i = 0; i < N; i++) ints.push((rnd() * 1e6) | 0);
function time(name, fn) {
  const t0 = now();
  const r = fn();
  const ms = now() - t0;
  print(name + ": " + ms.toFixed(1) + " ms, " + ((ms * 1000) / N).toFixed(1) + " us/op" + (r === undefined ? "" : " (" + r + ")"));
}
let strs;
time("double -> string", () => { strs = vals.map(String); return strs[0]; });
time("int -> string", () => { let s = 0; for (const v of ints) s += String(v).length; return s; });
time("string -> double", () => { let s = 0; for (const v of strs) s += +v; return s.toFixed(3); });
time("toFixed(2)", () => { let s = 0; for (const v of vals) s += v.toFixed(2).length; return s; });
time("parseInt", () => { let s = 0; for (const v of ints) s += parseInt("" + v, 10); return s; });
time("JSON.stringify", () => JSON.stringify(vals).length);
time("JSON.parse", () => JSON.parse(JSON.stringify(vals)).length);
