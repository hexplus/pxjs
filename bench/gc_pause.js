// GC pause times: a full collection with different amounts and shapes of
// live data. Run with scripts/bench-psp.sh pxjs/bench/gc_pause.js (PPSSPP)
// or on the host with the pxjs runner (it has gc() and now() too).
function pause(label, keep) {
    gc();
    let best = 1e9;
    for (let i = 0; i < 3; i++) {
        const t0 = now();
        gc();
        best = Math.min(best, now() - t0);
    }
    print(label + ": " + best.toFixed(2) + " ms");
    return keep;
}

pause("empty heap");
let live;
// shallow: many small objects in one array
live = []; for (let i = 0; i < 20000; i++) live.push({ a: i, b: i + 1 });
pause("20k small objects", live);
live = []; for (let i = 0; i < 60000; i++) live.push({ a: i, b: i + 1 });
pause("60k small objects", live);
// strings
live = []; for (let i = 0; i < 20000; i++) live.push("item-" + i + "-" + (i * 7));
pause("20k strings", live);
// closures
live = []; for (let i = 0; i < 20000; i++) { const k = i; live.push(() => k); }
pause("20k closures", live);
// deep: a 20000-long linked list (marking must not recurse)
live = null; for (let i = 0; i < 20000; i++) live = { next: live, v: i };
pause("20k-deep list", live);
// cyclic
live = []; for (let i = 0; i < 10000; i++) { const a = { i }, b = { a }; a.b = b; live.push(a); }
pause("10k cycles", live);
// mostly garbage: the pause depends on live data and the arena size
live = null;
for (let i = 0; i < 100000; i++) ({ x: i });
pause("after 100k garbage objects");
