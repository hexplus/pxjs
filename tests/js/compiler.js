// Language semantics the compiler implements (the compiler contributor's
// regression tests; early errors are covered by Test262).

// __proto__: v in an object literal sets the prototype (Annex B)
assertEq(Object.getPrototypeOf({ __proto__: Array.prototype }), Array.prototype, "__proto__ literal");
assertEq(Object.getPrototypeOf({ __proto__: null }), null, "__proto__: null");
assertEq(Object.getPrototypeOf({ __proto__: 1 }), Object.prototype, "__proto__: primitive is ignored");
assert(!Object.prototype.hasOwnProperty.call({ __proto__: null }, "__proto__"), "no own __proto__");
{
    const k = "__proto__";
    const o = { [k]: 5 };
    assertEq(Object.getOwnPropertyDescriptor(o, "__proto__").value, 5, "computed __proto__ is a property");
    const __proto__ = 7;
    const s = { __proto__ };
    assertEq(Object.getOwnPropertyDescriptor(s, "__proto__").value, 7, "shorthand __proto__ is a property");
    const m = { __proto__() { return 3; } };
    assertEq(m.__proto__(), 3, "method __proto__ is a property");
}

// a global var is defined on the global object, not set through
// Object.prototype.__proto__'s setter
var __proto__ = 2;
assertEq(__proto__, 2, "var __proto__");
assertEq({ __proto__ }.__proto__, 2, "shorthand __proto__ reads the var");

// (x) = v: a parenthesised reference is still a target
{
    let a;
    (a) = 3;
    assertEq(a, 3, "(a) = 3");
    const o = {};
    (o.p) = 4;
    assertEq(o.p, 4, "(o.p) = 4");
}

// names of functions: accessors, numeric keys, parenthesised, defaults
{
    const o = { get x() { return 1; }, set x(v) {}, 1() {} };
    const d = Object.getOwnPropertyDescriptor(o, "x");
    assertEq(d.get.name, "get x", "getter name");
    assertEq(d.set.name, "set x", "setter name");
    assertEq(o[1].name, "1", "numeric method name");
    const f = (function () {});
    assertEq(f.name, "f", "parenthesised anonymous function");
    const [g = function () {}] = [];
    assertEq(g.name, "g", "destructuring default name");
    let h;
    [h = () => 0] = [];
    assertEq(h.name, "h", "assignment pattern default name");
    function p(q = function () {}) { return q.name; }
    assertEq(p(), "q", "parameter default name");
    let n;
    n ??= function () {};
    assertEq(n.name, "n", "logical assignment name");
}

// function length stops at the first default or rest parameter
assertEq(((a, b = 1, c) => 0).length, 1, "length with a default");
assertEq(((a, ...r) => 0).length, 1, "length with a rest");

// classes: private methods, accessors, static privates, #x in obj
{
    class A {
        #v = 1;
        static #count = 0;
        #inc() { return ++this.#v; }
        get #double() { return this.#v * 2; }
        set #double(x) { this.#v = x / 2; }
        static #make() { A.#count++; return new A(); }
        static make() { return A.#make(); }
        static count() { return A.#count; }
        run() {
            this.#inc();
            this.#double = 10;
            return [this.#v, this.#double, #v in this, #inc in {}];
        }
        static has(o) { return #v in o; }
        setInc() { this.#inc = 1; }
    }
    const a = A.make();
    assertEq(a.run().join(), "5,10,true,false", "private members");
    assertEq(A.count(), 1, "static private");
    assert(A.has(a) && !A.has({}), "#x in obj");
    let threw = false;
    try { a.setInc(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "a private method is not writable");
    threw = false;
    try { A.prototype.run.call({}); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "private access on a foreign object throws");
}

// classes: computed field names are evaluated once, in order
{
    const log = [];
    const k = (n) => { log.push(n); return "f" + n; };
    class B {
        [k(1)] = 1;
        static [k(2)] = 2;
        [k(3)]() { return 3; }
    }
    assertEq(log.join(), "1,2,3", "computed keys in order");
    const b1 = new B(), b2 = new B();
    assertEq(b1.f1 + b2.f1 + B.f2 + b1.f3(), 7, "computed fields");
    assertEq(log.length, 3, "keys evaluated once");
}

// static blocks see the class; their vars are their own
{
    let seen;
    class C { static x = 1; static { var local = C.x + 1; seen = local; } }
    assertEq(seen, 2, "static block");
    assertEq(typeof local, "undefined", "static block var is local");
}

// destructuring assignment: target references first, iterators closed
{
    const order = [];
    const obj = { get p() { order.push("get"); return 1; } };
    const tgt = { set q(v) { order.push("set " + v); } };
    const t = () => { order.push("target"); return tgt; };
    ({ p: t().q } = obj);
    assertEq(order.join(), "target,get,set 1", "assignment order");
    let closed = 0;
    const iterable = {
        [Symbol.iterator]() {
            return { next() { return { value: undefined, done: false }; }, return() { closed++; return {}; } };
        }
    };
    const [x] = iterable;
    assertEq(closed, 1, "closed after a pattern");
    let caught;
    try {
        const [y = (() => { throw "boom"; })()] = iterable;
    } catch (e) {
        caught = e;
    }
    assert(caught === "boom" && closed === 2, "closed when a default throws");
    const { a, ...rest } = { a: 1, b: 2, [Symbol.for("s")]: 3 };
    assertEq(rest.b + rest[Symbol.for("s")], 5, "object rest keeps symbols");
    const key = "b";
    const { [key]: bb, ...rest2 } = { a: 1, b: 2 };
    assert(bb === 2 && !("b" in rest2) && rest2.a === 1, "computed key excluded from rest");
    let m, n2;
    [m, ...n2] = [1, 2, 3];
    assertEq(n2.join(), "2,3", "array rest");
}

// for (let x of x): the head's names are in their TDZ for the expression
{
    let threw = false;
    try { for (let q of [q]) {} } catch (e) { threw = e instanceof ReferenceError; }
    assert(threw, "for-of head TDZ");
}

// ------------------------------------------------------------ early errors
// Each category: sources that must not compile, and close ones that must.
function bad(src, what, module) { assert(syntaxError(src, module), what + ": " + src); }
function good(src, what, module) { assert(!syntaxError(src, module), what + " (valid): " + src); }

// declarations: duplicate lexical names, lexical/var conflicts
bad("let a; let a;", "duplicate let");
bad("let a; var a;", "let then var");
bad("{ let b; { var b; } }", "var through a block's let");
bad("{ function f() {} function f() {} }", "duplicate block function");
bad("switch (0) { case 0: let c; case 1: let c; }", "duplicate in a switch");
bad("switch (0) { case 1: function f() {} default: let f }", "function and let in a switch");
bad("switch (0) { case 1: var g; default: function g() {} }", "var and function in a switch");
good("switch (0) { case a ? b : c: function h() {} }", "?: in a case label");
{
    let r;
    switch (1) { case 1: r = sw(); function sw() { return "hoisted"; } }
    assertEq(r, "hoisted", "a function in a switch body is hoisted to its block");
    assertEq(typeof sw, "undefined", "and does not leak out");
}
bad("for (let i;;) { var i; }", "var clashes with a for head");
bad("try {} catch (e) { let e; }", "catch parameter redeclared");
bad("function f(a) { let a; }", "parameter redeclared by let");
bad("for (let x, y of []) {}", "two bindings in a for-of head");
bad("for (let x = 1 of []) {}", "initialiser in a for-of head");
good("function f(a) { var a; function a() {} }", "var and function over a parameter");
good("try {} catch (e) { var e; }", "var over a catch parameter");
good("function f() {} function f() {}", "top-level functions twice");

// strict-mode names
bad("var yield;", "yield binding");
bad("let let = 1;", "let binding");
bad("var implements;", "future reserved word");
bad("var v\\u0061r;", "escaped keyword");
bad("var eval;", "eval binding");
bad("arguments = 1;", "assignment to arguments");
bad("function f(eval) {}", "eval parameter");
bad("(x) => { delete x; }", "delete of an identifier");
bad("var a = 010;", "legacy octal");
bad("'\\08'", "legacy octal escape");
good("var \\u0061wait = 1;", "escaped await in a script");

// await and yield
bad("async function f() { var await; }", "await binding in async");
bad("async function f(a = await 1) {}", "await in async parameters");
bad("function* g(a = yield) {}", "yield in generator parameters");
bad("async function f() { (await) => 1; }", "await as an arrow parameter in async");
bad("function* g() { yield\n* 1 }", "yield * across a line break");
bad("for (async of []) ;", "for (async of");
good("for ((async) of []) ;", "for ((async) of");
bad("class C { static { var await; } }", "await in a static block");
bad("var await;", "await in a module", true);
good("function await() {}", "await as a function name in a script");

// assignment and destructuring targets
bad("1 = 2;", "literal target");
bad("a + b = 1;", "expression target");
bad("(a, b) = 1;", "comma target");
bad("a?.b = 1;", "optional chain target");
bad("[a + 1] = [];", "array pattern element");
bad("({ a: 1 } = {});", "object pattern element");
bad("[...a, b] = [];", "rest not last");
bad("[...a = 1] = [];", "rest with a default");
bad("({...{a}} = {});", "object rest pattern");
bad("++a++;", "update of an update");
good("[(a)] = []; ({ a: (b.c) } = {}); (a) = 1;", "parenthesised targets");

// parameters
bad("function f(a, a) {}", "duplicate parameters");
bad("(a, a) => 1", "duplicate arrow parameters");
bad("function f([a, a]) {}", "duplicate pattern parameters");
bad("function f(...a,) {}", "trailing comma after rest");
bad("function f(...a = 1) {}", "rest with a default");
bad("function f(a = 1) { 'use strict'; }", "use strict with non-simple parameters");
bad("({ get x(a) {} })", "getter with a parameter");
bad("({ set x() {} })", "setter without a parameter");
bad("({ set x(...a) {} })", "setter with a rest parameter");

// super and new.target
bad("function f() { super.x; }", "super outside a method");
bad("({ m() { super(); } })", "super() outside a constructor");
bad("new.target", "new.target at the top level");
good("function f() { return () => new.target; }", "new.target in an arrow");
good("({ m() { return () => super.x; } })", "super.x in an arrow in a method");

// classes
bad("class C { constructor() {} constructor() {} }", "duplicate constructor");
bad("class C { get constructor() {} }", "accessor constructor");
bad("class C { *constructor() {} }", "generator constructor");
bad("class C { constructor = 1 }", "field named constructor");
bad("class C { static prototype() {} }", "static prototype method");
bad("class C { static prototype = 1 }", "static prototype field");
bad("class C { #a; #a; }", "duplicate private name");
bad("class C { #constructor() {} }", "#constructor");
bad("class C { x = arguments; }", "arguments in a field");
bad("class C { x = () => arguments; }", "arguments in an arrow in a field");
bad("class C { m() { this.#x; } }", "undeclared private name");
bad("class C { #x; m() { delete this.#x; } }", "delete of a private field");
bad("class C { #x; m() { super.#x; } }", "super.#x");
bad("class C { #x; m() { return 1 + #x in this; } }", "#x in as an operand");
bad("class C { x y }", "two fields on one line");
bad("class C { #x; m() { const { #x: x } = this; } }", "private name in a pattern");
bad("({ #x: 1 })", "private name in an object literal");
bad("class C { static { return; } }", "return in a static block");
good("class C { get #a() {} set #a(v) {} static m() { return (() => arguments); } }", "getter and setter pair");
good("class C { #x; m(o) { return #x in o; } }", "#x in obj");

// statements
bad("if (1) let y;", "let as a statement body");
bad("if (1) function f() {}", "function as a statement body");
bad("while (0) async function f() {}", "async function as a statement body");
bad("l: function f() {}", "labelled function");
bad("a: a: ;", "duplicate label");
bad("break;", "break outside a loop");
bad("x: { continue x; }", "continue to a block label");
bad("with ({}) {}", "with");

// operators and literals
bad("-x ** 2", "unary base of **");
bad("a ?? b || c", "?? mixed with ||");
bad("a?.b`t`", "template after an optional chain");
bad("new a?.b()", "optional chain in new");
bad("/a/gg", "duplicate regexp flag");
bad("/a/x", "unknown regexp flag");
bad("1__0", "double numeric separator");
bad("1_", "trailing numeric separator");
bad("0_1", "separator after a leading 0");
bad("({ __proto__: 1, __proto__: 2 })", "duplicate __proto__");
good("({ __proto__: 1, ['__proto__']: 2, __proto__() {} })", "__proto__ forms that define properties");
good("({ __proto__: a, __proto__: b } = {});", "duplicate __proto__ in a pattern");

// modules
bad("export var a; export var a;", "duplicate export", true);
bad("var a; export { a, a };", "duplicate export name", true);
bad("export { nope };", "export of an undeclared name", true);
bad("export { Number };", "export of a global", true);
bad("{ export var x; }", "export in a block", true);
bad("if (1) import 'm';", "import in a statement", true);
bad("var a; export { a \\u0061s b };", "escaped as", true);
bad("export { \"s\" };", "string local export", true);
bad("import { default } from 'm';", "default as a binding", true);
bad("import * as eval from 'm';", "eval import binding", true);
bad("export default function () {}();", "call after a default function", true);
bad("export default var x = 1;", "var after export default", true);
bad("export * from 'm' null;", "junk after export *", true);
good("var a; export { a as b, a as \"c\" }; export default class {} export * as ns from 'm';", "exports", true);
good("export default function () {} if (1) {}", "no ; after a default function", true);

// regexp literals may not span a line separator
bad("/a /", "LS in a regexp literal");

// parameters are in their TDZ until initialised, in order
function refError(f) { try { f(); } catch (e) { return e instanceof ReferenceError; } return false; }
assert(refError(() => { function f(a = b, b) {} f(); }), "default reads a later parameter");
assert(refError(() => { function f(a = a) {} f(); }), "default reads its own parameter");
assert(refError(() => { function f(a = x, { x }) {} f(undefined, { x: 1 }); }), "default reads a later pattern name");
assert(refError(() => { function f({ a = a }) {} f({}); }), "pattern default reads its own name");
assertEq((function (a, b = a) { return b; })(3), 3, "default reads an earlier parameter");
assertEq((function (a = () => b, b = 2) { return a(); })(), 2, "a closure reads a later parameter after");
assertEq((function ({ x }, a = x) { return a; })({ x: 5 }), 5, "default reads an earlier pattern name");
const namedFe = function h(a = h) { return a; };
assertEq(namedFe(), namedFe, "a named function expression's name in its defaults");
// after a private name, '/' divides
class DivPrivate { #x = 6; m() { return this.#x / 2 / 1; } }
assertEq(new DivPrivate().m(), 3, "this.#x / 2");

// o[k] op= v: TypeError for a null base before the key converts; the key converts once
function errName(f) { try { f(); } catch (e) { return e.name; } return "none"; }
assertEq(errName(() => { const b = null; b[{ toString() { throw new RangeError(); } }] ^= 1; }), "TypeError", "null base first");
let keyConversions = 0;
const keyObj = { toString() { keyConversions++; return "k"; } }, target = { k: 1 };
target[keyObj] += 1;
target[keyObj]++;
assertEq(keyConversions + "," + target.k, "2,3", "the key converts once per compound assignment");
// an `async function` expression is not a declaration
var asyncFe;
asyncFe = async function asyncFe() {};
assertEq(typeof asyncFe, "function", "async function expression assigned to a var of its name");

// super references: assignment, compound, delete, computed method names
class SupA { get g() { return "A" + this.tag; } }
class SupB extends SupA {
    constructor() { super(); this.tag = "b"; }
    set1() { super.x = 5; return this.x; }
    set2(k) { super[k] = 7; return this[k]; }
    read() { return super.g + super["g"]; }
    del() { delete super.x; }
}
const supB = new SupB();
assertEq(supB.set1(), 5, "super.x = v sets on this");
assertEq(supB.set2("q"), 7, "super[k] = v");
assertEq(supB.read(), "AbAb", "super.g, super[k] read with this");
assertEq(errName(() => supB.del()), "ReferenceError", "delete super.x");
const nameSym = Symbol("s");
class Named { static [nameSym]() {} static get [Symbol()]() {} }
assertEq(Named[nameSym].name, "[s]", "computed symbol method name");
assertEq(Object.getOwnPropertyDescriptor(Named, Object.getOwnPropertySymbols(Named)[1]).get.name, "get ", "accessor with a symbol without description");

// -1 .toFixed binds the member access first
assertEq(-1.5.toFixed(1), -1.5, "unary minus on a member access");
print("compiler ok");
