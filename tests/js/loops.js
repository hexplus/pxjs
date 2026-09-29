// Loops and the compiler's shortcuts around them: the for-loop layout
// (update compiled after the body), per-iteration bindings, TDZ checks
// elided after a declaration, x++/x-- as statements, SET+POP -> PUT.

// continue lands on the update
{
    const seen = [];
    for (let i = 0; i < 6; i++) {
        if (i % 2) continue;
        seen.push(i);
    }
    assertEq(seen.join(), "0,2,4");
}
// labeled continue to an outer for
{
    const seen = [];
    outer: for (let i = 0; i < 3; i++) {
        for (let j = 0; j < 3; j++) {
            if (j === 1) continue outer;
            seen.push(`${i}${j}`);
        }
    }
    assertEq(seen.join(), "00,10,20");
}
// break, and a loop with no test / no update
{
    let n = 0;
    for (;;) { if (++n === 5) break; }
    assertEq(n, 5);
    let k = 0;
    for (let i = 0; i < 3;) { i++; k += i; }
    assertEq(k, 6);
    for (k = 0; k < 10; k += 3);
    assertEq(k, 12);
}
// per-iteration bindings: closures in the body, the test, the update
{
    const fs = [];
    for (let i = 0; i < 3; i++) fs.push(() => i);
    assertEq(fs.map((f) => f()).join(), "0,1,2");

    const gs = [];
    for (let i = 0; i < 3; i++) { let j = i * 10; gs.push(() => j); }
    assertEq(gs.map((f) => f()).join(), "0,10,20");

    const hs = [];
    for (let i = 0; i < 3; i++, hs.push(() => i));
    assertEq(hs.map((f) => f()).join(), "1,2,3");

    const ts = [];
    for (let i = 0; ts.push(() => i), i < 2; i++);
    assertEq(ts.map((f) => f()).join(), "0,1,2");

    // a closure that mutates its iteration's copy
    const inc = [];
    for (let i = 0; i < 3; i++) inc.push(() => ++i);
    assertEq(inc.map((f) => f()).join(), "1,2,3");
}
// the update with a template and nested groups
{
    let s = "";
    for (let i = 0; i < 3; s += `${[i][0]}${(i, "")}`, i++);
    assertEq(s, "012");
}
// x++ / ++x / x-- as statements, on let/var/param, across the SMI edge
{
    let a = 1073741822;
    a++; a++;
    assertEq(a, 1073741824);
    var b = -1073741823;
    b--; b--;
    assertEq(b, -1073741825);
    let c = "5";
    c++;
    assertEq(c, 6);
    let d = { valueOf() { return 41; } };
    ++d;
    assertEq(d, 42);
    let e = 1.5;
    e--;
    assertEq(e, 0.5);
    function p(x) { x++; ++x; x--; return x; }
    assertEq(p(10), 11);
    // as values they still yield the old / new value
    let f = 1;
    const g = f++, h = ++f;
    assertEq(`${f} ${g} ${h}`, "3 1 3");
}
// a jump that lands right after a store must still pop its own value
{
    let x = 0, y = 0;
    for (let i = 0; i < 4; i++) i % 2 ? (x = i) : (y = i);
    assertEq(`${x} ${y}`, "3 2");
    let z;
    z = true ? (x = 5) : (y = 6);
    assertEq(`${x} ${y} ${z}`, "5 2 5");
    let w = 0;
    (w > 0) && (w = 1);
    w || (w = 2);
    assertEq(w, 2);
}
// TDZ: still enforced where the declaration may not have run
{
    let r = "";
    try { tdz; } catch (e) { r += e.name; }
    let tdz = 1;
    assertEq(r, "ReferenceError");

    const kinds = [];
    for (const v of [0, 1]) {
        switch (v) {
            case 0:
                let x = "zero";
                kinds.push(x);
                break;
            case 1:
                try { kinds.push(x); } catch (e) { kinds.push(e.name); }
        }
    }
    assertEq(kinds.join(), "zero,ReferenceError");

    // a hoisted function can run before the declaration it reads
    let out;
    try { early(); } catch (e) { out = e.name; }
    let late = 1;
    function early() { return late; }
    assertEq(out, "ReferenceError");
    assertEq(early(), 1);

    // the declaration's own initialiser
    try { let self = self + 1; } catch (e) { out = e.name + "2"; }
    assertEq(out, "ReferenceError2");

    // in a loop body: each iteration's binding starts uninitialised
    const res = [];
    for (let i = 0; i < 2; i++) {
        try { res.push(q); } catch (e) { res.push(e.name); }
        let q = i;
        res.push(q);
    }
    assertEq(res.join(), "ReferenceError,0,ReferenceError,1");
}
// while / do-while / for-of / for-in unchanged
{
    let i = 0, s = 0;
    while (i < 5) s += i++;
    assertEq(s, 10);
    do s--; while (s > 5);
    assertEq(s, 5);
    const fs = [];
    for (const v of [1, 2]) fs.push(() => v);
    assertEq(fs.map((f) => f()).join(), "1,2");
    const ks = [];
    for (const k in { a: 1, b: 2 }) ks.push(k);
    assertEq(ks.join(), "a,b");
}
// generators and async functions suspending inside a for body
{
    function* gen() { for (let i = 0; i < 3; i++) yield () => i; }
    assertEq([...gen()].map((f) => f()).join(), "0,1,2");
}
print("loops ok");
