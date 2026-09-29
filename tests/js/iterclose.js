// Leaving loops and generators early: iterators are closed, finally blocks
// run, the stack stays balanced.
{
    let n = 0;
    o: for (const a of [1, 2]) { for (const b of [1, 2]) { if (b === 2) continue o; n++; } }
    assertEq(n, 2);
    let m = 0;
    p: for (const a of [1, 2]) { for (const b of [1, 2]) { if (b === 2) break p; m++; } }
    assertEq(m, 1);
    // continue/break across for-in and a try/finally in between
    const log = [];
    q: for (const a of [1, 2]) {
        try {
            for (const k in { x: 1, y: 2 }) {
                if (k === "y") continue q;
                log.push(a + k);
            }
        } finally {
            log.push("f" + a);
        }
    }
    assertEq(log.join(), "1x,f1,2x,f2");
}
// return from inside nested for-of
{
    function first(rows) {
        for (const r of rows) for (const c of r) if (c > 1) return c;
        return -1;
    }
    assertEq(first([[0, 1], [5, 6]]), 5);
    assertEq(first([[0]]), -1);
    let s = 0;
    for (let i = 0; i < 100; i++) s += first([[1, 2]]); // the stack must not grow
    assertEq(s, 200);
}
// generator cleanup when a consumer stops early
{
    const log = [];
    function* g() { try { yield 1; yield 2; } finally { log.push("closed"); } }
    for (const x of g()) break;
    assertEq(log.join(), "closed");
    const [a] = g();
    assertEq(a, 1);
    function f() { for (const x of g()) return x; }
    assertEq(f(), 1);
    assertEq(log.length >= 2, true);
}
// generator.return() runs finally blocks, skips catch, can be overridden
{
    const log = [];
    function* g() { try { yield 1; yield 2; } catch (e) { log.push("caught"); } finally { log.push("fin"); } }
    const it = g(); it.next();
    assertEq(JSON.stringify(it.return(42)), '{"value":42,"done":true}');
    assertEq(log.join(), "fin");
    assertEq(JSON.stringify(it.next()), '{"done":true}');
    function* h() { try { try { yield 1; } finally { log.push("inner"); } } finally { log.push("outer"); } }
    const i2 = h(); i2.next();
    assertEq(i2.return(5).value, 5);
    assertEq(log.join(), "fin,inner,outer");
    function* k() { try { yield 1; } finally { yield "cleanup"; log.push("after"); } }
    const i3 = k(); i3.next();
    assertEq(i3.return(7).value, "cleanup");
    const last = i3.next();
    assertEq(last.value, 7);
    assertEq(last.done, true);
    function* m() { try { yield 1; } finally { return "override"; } }
    const i4 = m(); i4.next();
    assertEq(i4.return(1).value, "override");
    // not started / finished: no code runs
    const i5 = g();
    assertEq(i5.return(3).value, 3);
    assertEq(i5.next().done, true);
}
print("iterclose ok");
