// Newer library pieces: ES2023+ array methods, Set methods, String.raw,
// Object.defineProperties, AggregateError, typed array map/filter, and the
// web extras structuredClone / atob / btoa.
assertEq([1, 2, 3, 4].toSpliced(1, 2, "x").join(), "1,x,4");
assertEq([1, 2, 3].toSpliced(-1).join(), "1,2");
assertEq([1, 2, 3].toSpliced().join(), "1,2,3");
{
    const a = [1, 2, 3];
    a.toSpliced(0, 1);
    assertEq(a.length, 3);
}
assertEq([1, 2, 3, 4, 5].copyWithin(0, 3).join(), "4,5,3,4,5");
assertEq([1, 2, 3, 4, 5].copyWithin(1, 0, 2).join(), "1,1,2,4,5");
assertEq([1, 2, 3, 4, 5].copyWithin(-2, 0).join(), "1,2,3,1,2");

assertEq(String.raw`a\n${1}b\t`, "a\\n1b\\t");
assertEq(String.raw({ raw: ["x", "y", "z"] }, 1, 2, 3), "x1y2z");
{
    const tag = (s) => s.raw[0] + "|" + s[0];
    assertEq(tag`\x41`, "\\x41|A");
}

{
    const o = Object.defineProperties({}, { a: { value: 1, enumerable: true }, b: { get: () => 2 } });
    assertEq(o.a + o.b, 3);
    assertEq(Object.keys(o).join(), "a");
}

{
    const e = new AggregateError([new Error("x"), 2], "many");
    assertEq(e.name, "AggregateError");
    assertEq(e.message, "many");
    assertEq(e.errors.length, 2);
    assert(e instanceof Error && e instanceof AggregateError);
    let got;
    Promise.any([Promise.reject(1), Promise.reject(2)]).catch((err) => { got = err; });
    queueMicrotask(() => queueMicrotask(() => queueMicrotask(() => {
        assert(got instanceof AggregateError);
        assertEq(got.errors.join(), "1,2");
    })));
}

{
    const a = new Set([1, 2, 3]), b = new Set([3, 4]);
    assertEq([...a.union(b)].join(), "1,2,3,4");
    assertEq([...a.intersection(b)].join(), "3");
    assertEq([...a.difference(b)].join(), "1,2");
    assertEq([...a.symmetricDifference(b)].join(), "1,2,4");
    assert(new Set([1]).isSubsetOf(a));
    assert(!a.isSubsetOf(b));
    assert(a.isSupersetOf(new Set([1, 2])));
    assert(a.isDisjointFrom(new Set([9])));
    assert(!a.isDisjointFrom(b));
    // a set-like that is not a Set, smaller than the receiver
    const like = { size: 1, has: (x) => x === 2, keys: () => [2][Symbol.iterator]() };
    assertEq([...a.intersection(like)].join(), "2");
    assertEq([...a.difference(like)].join(), "1,3");
    assert(a.isSupersetOf(like));
    assertEq([...new Set([3, 1]).union(new Map([[5, "x"]]))].join(), "3,1,5");
    let threw = false;
    try { a.union([1]); } catch (e) { threw = e instanceof TypeError; }
    assert(threw);
}

{
    const u = new Uint8Array([1, 2, 3]);
    const m = u.map((x) => x * 100);
    assert(m instanceof Uint8Array);
    assertEq(m.join(), "100,200,44");
    const f = new Float32Array([1.5, -2, 3]).filter((x) => x > 0);
    assert(f instanceof Float32Array);
    assertEq(f.join(), "1.5,3");
}

// structuredClone
{
    const d = new Date(86400000);
    const src = { n: 1, s: "x", arr: [1, , 3], d, re: /a+/gi, m: new Map([[1, { deep: true }]]), st: new Set(["q"]),
                  nested: { list: [{ k: "v" }] }, u8: new Uint8Array([7, 8]) };
    src.self = src;
    src.arr.extra = "e";
    const c = structuredClone(src);
    assert(c !== src && c.self === c);
    assertEq(c.n + c.s, "1x");
    assertEq(c.arr.length, 3);
    assert(!(1 in c.arr));
    assertEq(c.arr.extra, "e");
    assert(c.d instanceof Date && c.d !== d && c.d.getTime() === 86400000);
    assert(c.re instanceof RegExp && c.re.source === "a+" && c.re.flags === "gi");
    assert(c.m.get(1).deep === true && c.m.get(1) !== src.m.get(1));
    assert(c.st.has("q"));
    assertEq(c.nested.list[0].k, "v");
    assert(c.u8 instanceof Uint8Array && c.u8[1] === 8 && c.u8.buffer !== src.u8.buffer);
    const shared = { x: 1 };
    const pair = structuredClone([shared, shared]);
    assert(pair[0] === pair[1]);
    const e = structuredClone(new RangeError("bad", { cause: 5 }));
    assert(e instanceof RangeError && e.message === "bad" && e.cause === 5);
    class P { constructor() { this.v = 1; } get g() { return 2; } }
    const pc = structuredClone(new P());
    assert(!(pc instanceof P) && pc.v === 1 && pc.g === undefined);
    for (const bad of [() => 1, Symbol("s"), { f() {} }, new WeakMap()]) {
        let name = "";
        try { structuredClone(bad); } catch (err) { name = err.name; }
        assertEq(name, "DataCloneError");
    }
    assertEq(structuredClone(5), 5);
    assertEq(structuredClone("s"), "s");
    assertEq(structuredClone(null), null);
}

// atob / btoa
assertEq(btoa("Hello, PSP!"), "SGVsbG8sIFBTUCE=");
assertEq(atob("SGVsbG8sIFBTUCE="), "Hello, PSP!");
assertEq(btoa(""), "");
assertEq(btoa("a"), "YQ==");
assertEq(btoa("ab"), "YWI=");
assertEq(atob("YQ"), "a");
assertEq(atob(" Y W I = "), "ab");
assertEq(atob(btoa("\xff\x00\x80")), "\xff\x00\x80");
{
    let n1 = "", n2 = "";
    try { btoa("€"); } catch (e) { n1 = e.name; }
    try { atob("a"); } catch (e) { n2 = e.name; }
    assertEq(n1 + n2, "InvalidCharacterErrorInvalidCharacterError");
}

// class static blocks see the class binding
{
    class A {
        static x;
        static { A.x = 5; this.y = A.x + 1; }
        static z = A.x * 2;
    }
    assertEq(A.x + A.y + A.z, 5 + 6 + 10);
}
// Map growth after deletes (the index size must stay a power of two)
{
    const m = new Map();
    for (let i = 0; i < 16; i++) m.set(i, i);
    for (let i = 0; i < 3; i++) m.delete(i);
    for (let i = 16; i < 60; i++) m.set(i, i);
    assertEq(m.size, 57);
    assertEq(m.get(59), 59);
    assert(!m.has(1) && m.has(3));
}
print("library ok");
