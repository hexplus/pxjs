// Call costs: JS -> JS, JS -> native, native -> JS. Needs the PSP bench host
// (bench/psp/main.c), which provides nop() and callJS(fn, n).
const N = 200000;
function f() {}
function time(name, fn) {
  const t0 = now();
  fn();
  const ms = now() - t0;
  print(name + ": " + ms.toFixed(1) + " ms, " + ((ms * 1e6) / N).toFixed(0) + " ns/call");
}
time("loop only", () => { for (let i = 0; i < N; i++); });
time("JS -> JS", () => { for (let i = 0; i < N; i++) f(); });
time("JS -> native", () => { for (let i = 0; i < N; i++) nop(); });
time("JS -> native method", () => { const o = { nop }; for (let i = 0; i < N; i++) o.nop(); });
time("native -> JS", () => callJS(f, N));
time("closure call", () => { let c = 0; const g = () => c++; for (let i = 0; i < N; i++) g(); });
