// Proxy: traps, forwarding when a trap is missing, revocation.
{
    const log = [];
    const target = { a: 1, b: 2 };
    const p = new Proxy(target, {
        get(t, k, r) { log.push("get " + String(k)); return k in t ? t[k] : `no ${String(k)}`; },
        set(t, k, v) { log.push("set " + k); t[k] = v * 10; return true; },
        has(t, k) { return k === "hidden" ? false : k in t; },
        deleteProperty(t, k) { log.push("del " + k); delete t[k]; return true; },
    });
    assertEq(p.a, 1);
    assertEq(p.zzz, "no zzz");
    p.c = 3;
    assertEq(target.c, 30);
    assert("a" in p && !("hidden" in p));
    delete p.b;
    assert(!("b" in target));
    assertEq(log.join("|"), "get a|get zzz|set c|del b");
    // property access sites run through the trap every time (no stale cache)
    const got = [];
    for (let i = 0; i < 5; i++) got.push(p.a);
    assertEq(got.join(), "1,1,1,1,1");
}
// no handler traps: everything forwards to the target
{
    const t = { x: 1 };
    const p = new Proxy(t, {});
    p.y = 2;
    assertEq(t.y, 2);
    assertEq(p.x + p.y, 3);
    assertEq(Object.keys(p).join(), "x,y");
    assertEq(JSON.stringify(p), '{"x":1,"y":2}');
    assert(Object.getPrototypeOf(p) === Object.prototype);
    assert(p.hasOwnProperty("x"));
    const arr = new Proxy([1, 2, 3], {});
    assert(Array.isArray(arr));
    assertEq(arr.length, 3);
    assertEq(arr.map((v) => v * 2).join(), "2,4,6");
    assertEq([...arr].join(), "1,2,3");
}
// ownKeys / getOwnPropertyDescriptor / defineProperty / getPrototypeOf
{
    const p = new Proxy({}, {
        ownKeys() { return ["b", "a", Symbol("s")]; },
        getOwnPropertyDescriptor(t, k) { return { value: k.toUpperCase(), enumerable: k !== "a", configurable: true }; },
        defineProperty(t, k, d) { t["def_" + k] = d.value; return true; },
        getPrototypeOf() { return Array.prototype; },
    });
    assertEq(Object.keys(p).join(), "b");
    assertEq(Object.getOwnPropertyNames(p).join(), "b,a");
    assertEq(Object.getOwnPropertyDescriptor(p, "a").value, "A");
    Object.defineProperty(p, "q", { value: 7 });
    assert(p instanceof Array);
    assertEq(Object.getPrototypeOf(p), Array.prototype);
}
// functions: apply and construct
{
    function add(a, b) { return a + b; }
    const traced = new Proxy(add, { apply(t, self, args) { return t(...args) * 100; } });
    assertEq(traced(1, 2), 300);
    assertEq(typeof traced, "function");
    assertEq(traced.call(null, 2, 2), 400);
    class Pt { constructor(x) { this.x = x; } }
    const P2 = new Proxy(Pt, { construct(t, args) { return new t(args[0] * 2); } });
    assertEq(new P2(4).x, 8);
    const plain = new Proxy(Pt, {});
    const o = new plain(5);
    assert(o instanceof Pt);
    assertEq(o.x, 5);
    let threw = false;
    try { new Proxy({}, {})(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw);
}
// a proxy as a prototype
{
    const proto = new Proxy({}, { get: (t, k) => (typeof k === "string" ? "from proxy: " + k : undefined) });
    const o = Object.create(proto);
    o.own = 1;
    assertEq(o.own, 1);
    assertEq(o.missing, "from proxy: missing");
}
// revocable
{
    const { proxy, revoke } = Proxy.revocable({ v: 1 }, {});
    assertEq(proxy.v, 1);
    revoke();
    let msg = "";
    try { proxy.v; } catch (e) { msg = e.name; }
    assertEq(msg, "TypeError");
}
// a reactive-store pattern
{
    const changes = [];
    function reactive(obj) {
        return new Proxy(obj, {
            set(t, k, v) { const old = t[k]; t[k] = v; if (old !== v) changes.push(`${k}:${old}->${v}`); return true; },
        });
    }
    const state = reactive({ count: 0 });
    state.count++;
    state.count += 2;
    state.count = 3;
    assertEq(changes.join(), "count:0->1,count:1->3");
}
print("proxy ok");
