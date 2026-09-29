// The standard library.
const a = [5, 1, 4, 2, 3];
assertEq(a.slice(1, 3), [1, 4]);
assertEq(a.map(x => x * 2), [10, 2, 8, 4, 6]);
assertEq(a.filter(x => x % 2), [5, 1, 3]);
assertEq(a.reduce((s, x) => s + x, 0), 15);
assertEq(a.find(x => x > 3), 5);
assertEq(a.findIndex(x => x === 4), 2);
assertEq(a.some(x => x > 4), true);
assertEq(a.every(x => x > 0), true);
assertEq(a.indexOf(4), 2);
assertEq(a.includes(9), false);
assertEq(a.join("-"), "5-1-4-2-3");
assertEq(a.slice().sort(), [1, 2, 3, 4, 5]);
assertEq(a.slice().sort((x, y) => y - x), [5, 4, 3, 2, 1]);
assertEq([10, 9, 1].sort(), [1, 10, 9]);
assertEq([3, 1, 2].reverse(), [2, 1, 3]);
const sp = [1, 2, 3, 4, 5];
assertEq(sp.splice(1, 2, "a", "b", "c"), [2, 3]);
assertEq(sp, [1, "a", "b", "c", 4, 5]);
const q = [1, 2];
q.push(3, 4);
assertEq(q.pop(), 4);
assertEq(q.shift(), 1);
q.unshift(0);
assertEq(q, [0, 2, 3]);
assertEq([1, [2, [3, [4]]]].flat(Infinity), [1, 2, 3, 4]);
assertEq([1, 2].flatMap(x => [x, x]), [1, 1, 2, 2]);
assertEq(Array.from("héllo"), ["h", "é", "l", "l", "o"]);
assertEq(Array.from({ length: 3 }, (_, i) => i * i), [0, 1, 4]);
assertEq(Array.isArray([]), true);
assertEq(new Array(3).length, 3);
assertEq([1, 2, 3].at(-1), 3);
assertEq([1, 2].concat([3], 4), [1, 2, 3, 4]);
assertEq(new Array(3).fill(0), [0, 0, 0]);

assertEq(Object.entries({ a: 1, b: 2 }), [["a", 1], ["b", 2]]);
assertEq(Object.assign({}, { a: 1 }, { b: 2 }), { a: 1, b: 2 });
const proto = { hi() { return "hi " + this.name; } };
const child = Object.create(proto);
child.name = "x";
assertEq(child.hi(), "hi x");
assertEq(Object.getPrototypeOf(child) === proto, true);
const frozen = Object.freeze({ k: 1 });
try { frozen.k = 2; } catch (e) { }
assertEq(frozen.k, 1);
assertEq(Object.isFrozen(frozen), true);
const dp = {};
Object.defineProperty(dp, "hidden", { value: 42, enumerable: false });
assertEq(dp.hidden, 42);
assertEq(Object.keys(dp), []);
assertEq({ a: 1 }.hasOwnProperty("a"), true);
assertEq(Object.fromEntries([["x", 1]]), { x: 1 });
assertEq(Object.prototype.toString.call([]), "[object Array]");

assertEq(JSON.stringify({ a: [1, "two", null, true], b: { c: 1.5 } }), '{"a":[1,"two",null,true],"b":{"c":1.5}}');
assertEq(JSON.stringify("q\"\n\u0001"), String.fromCharCode(34, 113, 92, 34, 92, 110, 92, 117, 48, 48, 48, 49, 34));
assertEq(JSON.stringify({ u: undefined, f() {}, n: NaN }), '{"n":null}');
assertEq(JSON.stringify([1, 2], null, 2), "[\n  1,\n  2\n]");
assertEq(JSON.stringify({ d: { toJSON() { return "D"; } } }), '{"d":"D"}');
const parsed = JSON.parse('{"x":[1,2,{"y":"z\u00e9"}],"n":-1.5e2,"t":true,"z":null}');
assertEq(parsed.x[2].y, "zé");
assertEq(parsed.n, -150);
assertEq(JSON.parse("[1,2]", (k, v) => typeof v === "number" ? v * 10 : v), [10, 20]);
try { JSON.parse("{bad}"); assert(false); } catch (e) { assertEq(e.name, "SyntaxError"); }
const cyc = {}; cyc.self = cyc;
try { JSON.stringify(cyc); assert(false); } catch (e) { assertEq(e.name, "TypeError"); }

assertEq(Math.max(1, 5, 3), 5);
assertEq(Math.min(), Infinity);
assertEq(Math.floor(-1.5), -2);
assertEq(Math.round(2.5), 3);
assertEq(Math.round(-2.5), -2);
assertEq(Math.abs(-3), 3);
assertEq(Math.sqrt(16), 4);
assertEq(Math.pow(2, 8), 256);
const r = Math.random();
assert(r >= 0 && r < 1);

assertEq(parseInt("42px"), 42);
assertEq(parseInt("ff", 16), 255);
assertEq(parseInt("0x10"), 16);
assertEq(parseFloat("3.14abc"), 3.14);
assert(isNaN(parseInt("x")));
assertEq(Number("12"), 12);
assertEq(Number(""), 0);
assert(isNaN(Number("12x")));
assertEq((255).toString(16), "ff");
assertEq((3.14159).toFixed(2), "3.14");
assertEq((0.000001234).toPrecision(2), "0.0000012");
assertEq(Number.isInteger(5), true);
assertEq(String(null), "null");
assertEq(String([1, [2, 3]]), "1,2,3");
assertEq(Boolean(""), false);
assertEq(encodeURIComponent("a b&é"), "a%20b%26%C3%A9");
assertEq(decodeURIComponent("a%20b%26%C3%A9"), "a b&é");

const e = new RangeError("bad", { cause: "c" });
assertEq(e.name + ":" + e.message + ":" + e.cause, "RangeError:bad:c");
assertEq(String(e), "RangeError: bad");
assert(e instanceof Error);
assert(typeof e.stack === "string");

function greet(greeting, name) { return greeting + ", " + name + (this && this.bang ? "!" : ""); }
assertEq(greet.call({ bang: true }, "hi", "a"), "hi, a!");
assertEq(greet.apply(null, ["yo", "b"]), "yo, b");
const bound = greet.bind({ bang: true }, "hey");
assertEq(bound("c"), "hey, c!");
assertEq(greet.name, "greet");
assertEq(greet.length, 2);

// --- object model (Test262 work) ---
function threw(f, E) { try { f(); } catch (x) { return x instanceof E; } return false; }
{
    // a function's length and name: { writable: false, enumerable: false, configurable: true }
    const d = Object.getOwnPropertyDescriptor(Math.max, "length");
    assertEq([d.value, d.writable, d.enumerable, d.configurable].join(), "2,false,false,true");
    assert(threw(() => { Math.max.length = 5; }, TypeError), "length is read-only");
    function f(a, b) {}
    assert(delete f.name && !f.hasOwnProperty("name") && f.name === "", "name deletes");
    Object.defineProperty(f, "length", { value: 9 });
    assertEq(f.length, 9);
    assertEq(Reflect.ownKeys(function g(x) {}).join(), "length,name,prototype");
    assert(threw(() => new Math.abs(1), TypeError), "built-in methods are not constructors");
    const b = f.bind(null, 1);
    assertEq(b.name + ":" + b.length, "bound :8");
    assertEq(Math.max.bind(null, 1).name, "bound max");
    assertEq(typeof Function.prototype, "function");
    assertEq(Function.prototype(), undefined);
}
{
    const o = Object.create({ p: 1 }, { x: { value: 1, enumerable: true }, y: { get() { return 2; } } });
    assertEq(Object.keys(o).join() + o.y + o.p, "x21");
    assert(!Object.getOwnPropertyDescriptor(o, "x").writable, "absent fields are false");
    assert(threw(() => Object.defineProperty(o, "x", { value: 2 }), TypeError), "non-writable, non-configurable");
    Object.defineProperty(o, "x", { value: 1 }); // the same value: allowed
    assert(threw(() => Object.defineProperty({}, "a", { get() {}, value: 1 }), TypeError), "accessor and value");
    const ds = Object.getOwnPropertyDescriptors([7]);
    assertEq(ds[0].value + ":" + ds.length.writable, "7:true");
    assertEq(Reflect.ownKeys({ b: 1, 2: 1, a: 1, [Symbol.iterator]: 1, 1: 1 }).length, 5);
    assertEq(Object.keys({ b: 1, 2: 1, a: 1, 1: 1 }).join(), "1,2,b,a");
}
{
    // arrays: index properties, length
    const a = [1, 2, 3, 4];
    Object.defineProperty(a, "1", { configurable: false });
    assert(threw(() => { a.length = 0; }, TypeError), "refused shrink throws");
    assertEq(a.length, 2);
    Object.defineProperty(a, "length", { writable: false });
    assert(threw(() => a.push(5), TypeError), "push onto a read-only length");
    const g = [];
    g[4294967294] = 1;
    assertEq(g.length, 4294967295);
    g[4294967295] = 2; // not an index
    assertEq(g.length, 4294967295);
    assert(threw(() => { g.length = -1; }, RangeError), "invalid length");
    const fz = Object.freeze([1, 2]);
    assert(Object.isFrozen(fz) && threw(() => { fz[0] = 9; }, TypeError), "frozen array");
    assertEq([1, , 3].indexOf(undefined), -1);
    assertEq([1, , 3].includes(undefined), true);
    assertEq([3, undefined, 1, , 2].sort().length, 5);
    class MyArray extends Array {}
    assert(new MyArray(1, 2).map(x => x) instanceof MyArray, "species");
    assertEq(Array.from({ length: 2, 0: "a", 1: "b" }).join(), "a,b");
    assertEq([1, [2, [3]]].flat(Infinity).join(), "1,2,3");
    assertEq(Array.prototype.concat.call(1, 2).length, 2);
}
{
    // proxies: invariants, and a long chain of trap-less ones is bounded
    const t = {};
    Object.defineProperty(t, "k", { value: 1 });
    const p = new Proxy(t, { get() { return 2; }, getOwnPropertyDescriptor() { return undefined; } });
    assert(threw(() => p.k, TypeError), "get must report a frozen value");
    assert(threw(() => Object.getOwnPropertyDescriptor(p, "k"), TypeError), "cannot hide");
    let q = {};
    for (let i = 0; i < 200; i++) q = new Proxy(q, {}); // deeper than the native depth limit
    assert(threw(() => { q.x = 1; }, RangeError), "deep proxy chain: set");
    assert(threw(() => Object.freeze(q), RangeError), "deep proxy chain: freeze");
    assert(threw(() => Object.keys(q), RangeError), "deep proxy chain: keys");
    assert(threw(() => Proxy({}, {}), TypeError), "Proxy needs new");
}
{
    assertEq(Object.prototype.toString.call(new String("x")), "[object String]");
    assertEq(String.prototype.toString.call(new String("ab")), "ab");
    assertEq(Object.getPrototypeOf(Object.create(null, {}) ), null);
    const o = {};
    o.__proto__ = Array.prototype;
    assert(o instanceof Array && o.__proto__ === Array.prototype, "__proto__");
    assertEq(Object.assign({}, { [Symbol.iterator]: 1 })[Symbol.iterator], 1);
}

print("builtins ok");
