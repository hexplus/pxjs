// WeakMap / WeakSet hold keys weakly (ephemerons); WeakRef lets go of its
// target once nothing else holds it and the current job has ended.
const wm = new WeakMap(), ws = new WeakSet();
let keep = { name: "kept" };
wm.set(keep, { owner: keep, big: "x".repeat(1000) }); // value refers back to its key
ws.add(keep);
assertEq(wm.get(keep).owner, keep);
assert(ws.has(keep));

// many entries whose keys die: the heap must not grow with them
function churn(n) {
    for (let i = 0; i < n; i++) {
        const k = { i };
        wm.set(k, { self: k, pad: new Array(20).fill(i) });
        ws.add(k);
    }
}
gc();
const before = heapUsed();
for (let r = 0; r < 5; r++) { churn(2000); gc(); }
const after = heapUsed();
assert(after - before < 64 * 1024, `weak entries leaked: ${after - before} bytes`);
assertEq(wm.get(keep).owner, keep);          // live key: entry stays
assert(ws.has(keep));

// a value reachable only through a live key's entry survives
{
    const a = {}, b = {};
    wm.set(a, b);
    wm.set(b, "via b");
    gc();
    assertEq(wm.get(wm.get(a)), "via b");
}

// deleting and re-adding still works after entries were cleared
{
    const k = {};
    wm.set(k, 1);
    wm.delete(k);
    assert(!wm.has(k));
    wm.set(k, 2);
    assertEq(wm.get(k), 2);
}

// WeakRef
let target = { v: 42 };
const ref = new WeakRef(target);
assertEq(ref.deref().v, 42);
assertEq(new WeakRef(keep).deref(), keep);
let bad = "";
try { new WeakRef(1); } catch (e) { bad = e.name; }
assertEq(bad, "TypeError");
target = null;
// The spec keeps a WeakRef's target alive until the current synchronous run
// and its microtasks are over (ClearKeptObjects); the host's job loop
// clears it afterwards, so within this script the target is still there.
gc();
assertEq(ref.deref().v, 42);
// FinalizationRegistry: the callback runs (as a job) after the target is
// collected; unregister cancels.
{
    const cleaned = [];
    const reg = new FinalizationRegistry((held) => cleaned.push(held));
    (function () {
        for (let i = 0; i < 3; i++) reg.register({ i }, "obj" + i);
        const t = {};
        reg.register(t, "cancelled", t);
        assert(reg.unregister(t));
        assert(!reg.unregister({}));
    })();
    const alive = {};
    reg.register(alive, "alive");
    gc();
    Promise.resolve().then(() => {
        assertEq(cleaned.sort().join(), "obj0,obj1,obj2");
        assertEq(alive.constructor, Object);
        print("finreg ok");
    });
    let e1 = "", e2 = "";
    try { new FinalizationRegistry(1); } catch (e) { e1 = e.name; }
    try { reg.register(1, "x"); } catch (e) { e2 = e.name; }
    assertEq(e1 + e2, "TypeErrorTypeError");
}
print("weak ok");
