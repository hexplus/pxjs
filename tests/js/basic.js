// The core language: values, operators, control flow, functions, closures.
print("hello", 1 + 2, "Björk");
assertEq(1 + 2 * 3, 7);
assertEq((1 + 2) * 3, 9);
assertEq(7 / 2, 3.5);
assertEq(7 % 3, 1);
assertEq(-7 % 3, -1);
assertEq(2 ** 10, 1024);
assertEq(0.1 + 0.2, 0.30000000000000004);
assertEq(1e21, 1e21);
assertEq(String(1e21), "1e+21");
assertEq(String(123.456), "123.456");
assertEq(String(-0), "0");
assertEq(1 / 0, Infinity);
assertEq(1073741823 + 1, 1073741824); // smi overflow into double
assertEq(5 & 3, 1);
assertEq(5 | 3, 7);
assertEq(-1 >>> 0, 4294967295);
assertEq(1 << 31, -2147483648);
assertEq("a" + 1, "a1");
assertEq("3" * "4", 12);
assertEq(typeof 1, "number");
assertEq(typeof "s", "string");
assertEq(typeof undefined, "undefined");
assertEq(typeof null, "object");
assertEq(typeof {}, "object");
assertEq(typeof function () {}, "function");
assertEq(typeof notDefinedAnywhere, "undefined");
assert(1 == "1" && null == undefined && !(null == 0) && NaN !== NaN);
assert("b" > "a" && 2 > 1 && !(NaN < 1));

let s = 0;
for (let i = 0; i < 10; i++) s += i;
assertEq(s, 45);
let w = 0;
while (w < 5) w++;
assertEq(w, 5);
do { w--; } while (w > 0);
assertEq(w, 0);
let brk = 0;
for (let i = 0; i < 100; i++) { if (i === 3) continue; if (i === 6) break; brk += i; }
assertEq(brk, 0 + 1 + 2 + 4 + 5);

function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }
assertEq(fact(10), 3628800);
assertEq(hoisted(), "ok");
function hoisted() { return "ok"; }

const add = (a, b = 10) => a + b;
assertEq(add(1), 11);
assertEq(add(1, 2), 3);

function counter() { let c = 0; return () => ++c; }
const c1 = counter(), c2 = counter();
c1(); c1();
assertEq(c1(), 3);
assertEq(c2(), 1);

// Closures capture per-iteration bindings.
const fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
assertEq(fns.map(f => f()), [0, 1, 2]);

// Named function expressions see themselves.
const f2 = function g(n) { return n ? g(n - 1) + 1 : 0; };
assertEq(f2(5), 5);

// switch
function sw(x) {
    switch (x) {
        case 1: return "one";
        case 2:
        case 3: return "two-three";
        default: return "other";
    }
}
assertEq([sw(1), sw(2), sw(3), sw(9)], ["one", "two-three", "two-three", "other"]);

// try/catch/finally
let log = [];
try { throw new Error("boom"); } catch (e) { log.push(e.message); } finally { log.push("f"); }
function tf() { try { return "t"; } finally { log.push("f2"); } }
assertEq(tf(), "t");
assertEq(log, ["boom", "f", "f2"]);
try { null.x; } catch (e) { assert(e instanceof TypeError, "TypeError"); }
try { undefinedVar; } catch (e) { assertEq(e.name, "ReferenceError"); }

// TDZ
try { tdz; let tdz = 1; assert(false); } catch (e) { assertEq(e.name, "ReferenceError"); }

// Objects and arrays
const o = { a: 1, b: "two", nested: { c: [1, 2, 3] }, "quoted-key": true, 5: "five" };
assertEq(o.nested.c[2], 3);
assertEq(Object.keys(o), ["5", "a", "b", "nested", "quoted-key"]);
o.a += 5;
assertEq(o.a, 6);
delete o.b;
assert(!("b" in o) && "a" in o);
const arr = [1, , 3];
assertEq(arr.length, 3);
assert(!(1 in arr));
arr[10] = 1;
assertEq(arr.length, 11);
let keys = [];
for (const k in { x: 1, y: 2 }) keys.push(k);
assertEq(keys, ["x", "y"]);
let vals = [];
for (const v of [10, 20]) vals.push(v);
for (const ch of "hé") vals.push(ch);
assertEq(vals, [10, 20, "h", "é"]);

// Template literals
const who = "PSP";
assertEq(`hello ${who} ${1 + 1}!`, "hello PSP 2!");

// Methods and this
const obj = { n: 2, twice() { return this.n * 2; } };
assertEq(obj.twice(), 4);
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.sum = function () { return this.x + this.y; };
const p = new Point(3, 4);
assertEq(p.sum(), 7);
assert(p instanceof Point);
assertEq(p.constructor, Point);

// Strings
assertEq("abc".length, 3);
assertEq("abc"[1], "b");
assertEq("Hello".toUpperCase(), "HELLO");
assertEq("  x ".trim(), "x");
assertEq("a,b,c".split(","), ["a", "b", "c"]);
assertEq("motörhead".toUpperCase(), "MOTÖRHEAD");
assertEq("x".padStart(3, "-"), "--x");
assertEq("abcabc".replaceAll("b", "_"), "a_ca_c");
assertEq("abc".indexOf("c"), 2);
let big = "";
for (let i = 0; i < 20000; i++) big += "x";
assertEq(big.length, 20000);

print("basic ok");
