// Inline caches: a property-access site must see every change to the
// objects it has cached, and never read a slot of the wrong layout.
function getX(o) { return o.x; }
function setX(o, v) { o.x = v; }
function callM(o) { return o.m(); }

// monomorphic, then other shapes through the same site
{
    const a = { x: 1 }, b = { y: 0, x: 2 }, c = { x: 3, y: 4 };
    for (let i = 0; i < 3; i++) assertEq(getX(a), 1);
    assertEq(getX(b), 2);
    assertEq(getX(c), 3);
    assertEq(getX(a), 1);
    assertEq(getX({}), undefined);
    assertEq(getX(Object.create({ x: "proto" })), "proto");
    assertEq(getX([1, 2]), undefined);
    const arr = [1];
    arr.x = "arr";
    assertEq(getX(arr), "arr");
    assertEq(getX("str"), undefined);
    assertEq(getX(5), undefined);
}
// the cached object changes
{
    const o = { x: 1 };
    getX(o); getX(o);
    o.y = 2;                     // new shape
    assertEq(getX(o), 1);
    delete o.x;                  // dictionary mode
    assertEq(getX(o), undefined);
    o.x = 7;
    assertEq(getX(o), 7);

    const f = { x: 1 };
    setX(f, 2); setX(f, 3);
    Object.freeze(f);
    let threw = false;
    try { setX(f, 4); } catch (e) { threw = true; }
    assertEq(f.x, 3);            // sloppy mode would ignore; PXJS throws
    assert(threw || f.x === 3);

    const g = { x: 1 };
    getX(g);
    Object.defineProperty(g, "x", { get() { return "getter"; } });
    assertEq(getX(g), "getter");
    let seen;
    const h = { x: 0 };
    setX(h, 1);
    Object.defineProperty(h, "x", { set(v) { seen = v; }, get() { return "g"; } });
    setX(h, 9);
    assertEq(seen, 9);

    const p = { x: 1 };
    getX(p);
    Object.setPrototypeOf(p, { z: 1 });
    assertEq(getX(p), 1);
}
// methods on a prototype: override, replace, delete
{
    class A { m() { return "A"; } }
    class B extends A { }
    const a = new A(), b = new B();
    for (let i = 0; i < 3; i++) { assertEq(callM(a), "A"); assertEq(callM(b), "A"); }
    A.prototype.m = function () { return "A2"; };        // same slot, new value
    assertEq(callM(a), "A2");
    a.m = () => "own";                                    // own property shadows
    assertEq(callM(a), "own");
    const a2 = new A();
    assertEq(callM(a2), "A2");
    A.prototype.extra = 1;                                // prototype's shape changes
    assertEq(callM(a2), "A2");
    delete A.prototype.m;
    let err = "";
    try { callM(a2); } catch (e) { err = e.name; }
    assertEq(err, "TypeError");
    B.prototype.m = () => "B";
    assertEq(callM(b), "B");
}
// accessors are never cached as data
{
    let n = 0;
    const o = { get x() { return ++n; } };
    getX(o); getX(o);
    assertEq(getX(o), 3);
    class P { get x() { return "px"; } }
    const q = new P();
    getX(q);
    assertEq(getX(q), "px");
}
// out-of-line slots (more than the inline ones)
{
    const o = {};
    for (let i = 0; i < 10; i++) o["k" + i] = i;
    o.x = "far";
    for (let i = 0; i < 3; i++) assertEq(getX(o), "far");
    setX(o, "far2");
    assertEq(o.x, "far2");
}
// many keys: shapes with a hashed index, extended and deleted from
{
    const big = {};
    for (let i = 0; i < 40; i++) big["p" + i] = i;
    let s = 0;
    for (let r = 0; r < 50; r++) for (let i = 0; i < 40; i++) s += big["p" + i];
    assertEq(s, 50 * 780);
    assertEq(big.nope, undefined);
    big.extra = "e";                       // a child shape of the indexed one
    for (let r = 0; r < 20; r++) assertEq(big.extra + big.p39, "e39");
    assertEq(big.p0, 0);
    delete big.p5;
    assertEq(big.p5, undefined);
    assertEq(big.p6, 6);
    const twin = {};
    for (let i = 0; i < 40; i++) twin["p" + i] = -i;  // shares the shapes
    for (let r = 0; r < 20; r++) assertEq(twin.p17, -17);
    assertEq("p17" in twin, true);
    assertEq(Object.keys(twin).length, 40);
    // big class prototypes
    class K {}
    for (let i = 0; i < 30; i++) K.prototype["m" + i] = function () { return i; };
    const k = new K();
    let t = 0;
    for (let r = 0; r < 30; r++) t += k.m29() + k.m0();
    assertEq(t, 30 * 29);
}
// survives collections
{
    const objs = [];
    for (let i = 0; i < 2000; i++) objs.push({ x: i, pad: "p" + i });
    let s = 0;
    for (const o of objs) s += getX(o);
    gc();
    for (const o of objs) s += getX(o);
    assertEq(s, 2 * 1999 * 1000);
}
print("ic ok");
