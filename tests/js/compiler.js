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

// -1 .toFixed binds the member access first
assertEq(-1.5.toFixed(1), -1.5, "unary minus on a member access");
print("compiler ok");
