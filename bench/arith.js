// The PSP probe's arithmetic loops (pspx-runtime/tests/psp/memprobe.js).
const log = typeof console !== "undefined" ? console.log : print;
function time(label, fn) {
    const t0 = Date.now();
    const r = fn();
    log(label, Date.now() - t0, "ms", r);
}
time("1M int32 adds", () => { let s = 0; for (let i = 0; i < 1000000; i++) s = (s + i) | 0; return s; });
time("1M small int adds", () => { let s = 0; for (let i = 0; i < 1000000; i++) s = (s + (i & 7)) | 0; return s; });
time("1M double adds", () => { let s = 0.5; for (let i = 0; i < 1000000; i++) s += 0.25; return s; });
time("1M empty loop", () => { let i = 0; for (; i < 1000000; i++); return i; });
