// Classes, destructuring, spread/rest, getters/setters, symbols, iterators,
// generators, optional chaining, logical assignment, labels.

// ---- classes
class Animal {
    static count = 0;
    legs = 4;
    constructor(name) {
        this.name = name;
        Animal.count++;
    }
    speak() { return `${this.name} makes a sound`; }
    get description() { return `${this.name} (${this.legs} legs)`; }
    set nickname(v) { this._nick = v.toUpperCase(); }
    get nickname() { return this._nick; }
    static create(name) { return new this(name); }
    toString() { return `Animal(${this.name})`; }
}
class Dog extends Animal {
    tricks = [];
    constructor(name, breed) {
        super(name);
        this.breed = breed;
    }
    speak() { return super.speak() + " (woof)"; }
    learn(...tricks) { this.tricks.push(...tricks); return this; }
}
const d = new Dog("Rex", "lab");
assertEq(d.speak(), "Rex makes a sound (woof)");
assertEq(d.description, "Rex (4 legs)");
d.nickname = "rexy";
assertEq(d.nickname, "REXY");
assertEq(d.learn("sit", "roll").tricks, ["sit", "roll"]);
assert(d instanceof Dog && d instanceof Animal);
assertEq(Animal.count, 1);
assertEq(Dog.create("Fido").name, "Fido");
assertEq(Animal.count, 2);
assertEq(String(d), "Animal(Rex)");
assertEq(Object.keys(d), ["legs", "name", "tricks", "breed", "_nick"]);
try { Dog("x"); assert(false); } catch (e) { assertEq(e.name, "TypeError"); }
class Point {
    constructor(x, y) { this.x = x; this.y = y; }
    static get origin() { return new Point(0, 0); }
    [Symbol.iterator]() { return [this.x, this.y][Symbol.iterator](); }
}
assertEq([...new Point(1, 2)], [1, 2]);
assertEq(Point.origin.x, 0);
const Anon = class { hi() { return "hi"; } };
assertEq(new Anon().hi(), "hi");
assertEq(Anon.name, "Anon");
class Err extends Error {
    constructor(msg) { super(msg); this.name = "Err"; }
}
const err = new Err("bad");
assert(err instanceof Err && err instanceof Error);
assertEq(err.message, "bad");
class Default extends Dog {}
assertEq(new Default("D", "x").breed, "x");

// ---- new.target, accessors in object literals
function Probe() { return new.target === Probe; }
assert(new Probe() instanceof Probe);
const acc = { _v: 1, get v() { return this._v * 10; }, set v(x) { this._v = x; } };
acc.v = 5;
assertEq(acc.v, 50);
const od = {};
Object.defineProperty(od, "computed", { get() { return 42; }, enumerable: true });
assertEq(od.computed, 42);
assertEq(Object.getOwnPropertyDescriptor(acc, "_v").value, 5);

// ---- destructuring
const { a, b: { c = 3 } = {}, ...others } = { a: 1, b: {}, x: 9, y: 8 };
assertEq([a, c, others], [1, 3, { x: 9, y: 8 }]);
const [p, , q = 10, ...rest] = [1, 2, undefined, 4, 5];
assertEq([p, q, rest], [1, 10, [4, 5]]);
let s1 = 1, s2 = 2;
[s1, s2] = [s2, s1];
assertEq([s1, s2], [2, 1]);
const target = {};
({ m: target.m, n: target["n"] } = { m: "M", n: "N" });
assertEq(target, { m: "M", n: "N" });
function params({ x = 1, y } = {}, [z] = [7], ...more) { return [x, y, z, more]; }
assertEq(params({ y: 2 }, undefined, 8, 9), [1, 2, 7, [8, 9]]);
assertEq(params(), [1, undefined, 7, []]);
for (const [k, v] of Object.entries({ one: 1 })) assertEq(k + v, "one1");
try { const { z } = null; assert(false); } catch (e) { assertEq(e.name, "TypeError"); }

// ---- spread / rest / arguments
function sum() { let t = 0; for (let i = 0; i < arguments.length; i++) t += arguments[i]; return t; }
assertEq(sum(...[1, 2, 3], 4), 10);
assertEq(Math.max(...[3, 9, 2]), 9);
assertEq([..."héllo"], ["h", "é", "l", "l", "o"]);
assertEq({ ...{ a: 1 }, b: 2, ...{ a: 3 } }, { a: 3, b: 2 });
const arrowArgs = function () { return (() => arguments[0])(); };
assertEq(arrowArgs("outer"), "outer");
assertEq(new Date(...[2020, 0, 2]).getDate(), 2);

// ---- optional chaining, nullish, logical assignment
const deep = { a: { b: { f() { return "called"; } } } };
assertEq(deep?.a?.b?.f?.(), "called");
assertEq(deep.x?.y.z, undefined);
assertEq(deep.a.nope?.(), undefined);
assertEq(deep?.["a"]?.b ? 1 : 0, 1);
assertEq(null ?? "dflt", "dflt");
assertEq(0 ?? "dflt", 0);
const la = { n: null, z: 0, t: 1 };
la.n ??= "set";
la.z ||= 5;
la.t &&= 7;
assertEq(la, { n: "set", z: 5, t: 7 });

// ---- labels
let found = null;
outer: for (let i = 0; i < 5; i++) {
    for (let j = 0; j < 5; j++) {
        if (i * j === 6) { found = [i, j]; break outer; }
        if (j > i) continue outer;
    }
}
assertEq(found, [2, 3]);
block: { if (true) break block; assert(false); }

// ---- symbols
const sym = Symbol("tag");
const so = { [sym]: 1, plain: 2 };
assertEq(so[sym], 1);
assertEq(Object.keys(so), ["plain"]);
assertEq(Object.getOwnPropertySymbols(so).length, 1);
assertEq(sym.description, "tag");
assertEq(Symbol.for("k") === Symbol.for("k"), true);
assertEq(String(sym), "Symbol(tag)");

// ---- iterators and generators
function* range(n) { for (let i = 0; i < n; i++) yield i; }
assertEq([...range(4)], [0, 1, 2, 3]);
function* echo() { let x = yield 1; while (true) x = yield x * 2; }
const g = echo();
assertEq([g.next().value, g.next(5).value, g.next(6).value], [1, 10, 12]);
function* withReturn() { yield 1; return 2; }
assertEq([...withReturn()], [1]);
const wr = withReturn();
assertEq([wr.next(), wr.next(), wr.next()].map(r => r.done), [false, true, true]);
function* delegating() { yield 0; yield* range(3); yield* [9]; }
assertEq([...delegating()], [0, 0, 1, 2, 9]);
function* throwing() { try { yield 1; } catch (e) { yield "caught " + e; } }
const tg = throwing();
tg.next();
assertEq(tg.throw("x").value, "caught x");
const it = { from: 1, to: 3, [Symbol.iterator]() { let v = this.from, t = this.to; return { next: () => v <= t ? { value: v++, done: false } : { value: undefined, done: true } }; } };
assertEq(Array.from(it), [1, 2, 3]);
let closureGen = function* () { let captured = "c"; const f = () => captured; yield f; captured = "d"; yield f; };
const cg = closureGen();
const f1 = cg.next().value;
assertEq(f1(), "c");
cg.next();
assertEq(f1(), "d");
assertEq([...[1, 2].entries()], [[0, 1], [1, 2]]);
assertEq([..."ab"].length, 2);

// ---- tagged templates
function tag(strs, ...vals) { return strs.join("|") + ":" + vals.join(","); }
assertEq(tag`a${1}b${2}c`, "a|b|c:1,2");

print("es2015 ok");

// ---- private class members
class Counter {
    #count = 0;
    static #instances = 0;
    constructor() { Counter.#instances++; }
    #bump(by) { this.#count += by; return this; }
    inc() { return this.#bump(1); }
    get value() { return this.#count; }
    static get instances() { return Counter.#instances; }
    static isCounter(o) { return #count in o; }
}
const ctr = new Counter();
ctr.inc().inc();
new Counter();
assertEq(ctr.value, 2);
assertEq(Counter.instances, 2);
assertEq(Counter.isCounter(ctr), true);
assertEq(Counter.isCounter({}), false);
assertEq(Object.keys(ctr), []);
assertEq(JSON.stringify(ctr), "{}");
print("private ok");
