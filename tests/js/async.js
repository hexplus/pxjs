// Promises, async/await, the job queue.
const order = [];
Promise.resolve(1).then(v => order.push("then " + v));
queueMicrotask(() => order.push("microtask"));
order.push("sync");

const sleep0 = () => new Promise(r => queueMicrotask(r));

async function add(a, b) {
    await sleep0();
    return a + b;
}

async function fails() {
    await null;
    throw new Error("async boom");
}

async function main() {
    assertEq(await add(2, 3), 5);
    try {
        await fails();
        assert(false, "should have thrown");
    } catch (e) {
        assertEq(e.message, "async boom");
    }
    const all = await Promise.all([1, Promise.resolve(2), add(1, 2)]);
    assertEq(all, [1, 2, 3]);
    const settled = await Promise.allSettled([Promise.reject(new Error("no")), 5]);
    assertEq(settled.map(s => s.status), ["rejected", "fulfilled"]);
    assertEq(await Promise.race([new Promise(() => {}), Promise.resolve("fast")]), "fast");
    assertEq(await Promise.any([Promise.reject(1), Promise.resolve("any")]), "any");
    try { await Promise.any([Promise.reject(1)]); assert(false); } catch (e) { assertEq(e.errors, [1]); }
    let fin = "";
    await Promise.resolve("x").finally(() => { fin = "ran"; });
    assertEq(fin, "ran");
    const chained = await Promise.resolve(1).then(v => v + 1).then(v => { throw v * 10; }).catch(v => v + 5);
    assertEq(chained, 25);
    const thenable = { then(res) { res("thenable!"); } };
    assertEq(await thenable, "thenable!");
    const { promise, resolve } = Promise.withResolvers();
    queueMicrotask(() => resolve("later"));
    assertEq(await promise, "later");
    class Svc { async load() { await null; return this.v; } constructor() { this.v = 7; } }
    assertEq(await new Svc().load(), 7);
    const arrow = async (x) => (await x) * 2;
    assertEq(await arrow(Promise.resolve(21)), 42);
    let loops = 0;
    for (const p of [1, 2, 3]) loops += await p;
    assertEq(loops, 6);
    assertEq(order, ["sync", "then 1", "microtask"]);
    return "done";
}

main().then(r => print("async ok", r), e => { print("async FAILED", e); throw e; });

// ---- Test262 conformance work (async): regressions for what was fixed ----

// Sync yield*: next/throw/return reach the inner iterator; its result
// objects come out as they are; the frame resumes with the completion.
(function () {
    const log = [];
    const res = { value: "r1", done: false };
    const inner = {
        [Symbol.iterator]() { return this; },
        next(v) { log.push("next " + v); return res; },
        throw(e) { log.push("throw " + e); return { value: "caught", done: true }; },
        return(v) { log.push("return " + v); return { value: "ret " + v, done: true }; },
    };
    function* g() { const r = yield* inner; log.push("got " + r); yield "after"; }
    let it = g();
    assert(it.next("a") === res, "the inner result object is handed out as is");
    assertEq(it.throw("boom").value, "after");
    assertEq(log, ["next undefined", "throw boom", "got caught"]);
    it = g();
    it.next();
    let fin = 0;
    function* h() { try { yield* inner; } finally { fin++; } }
    it = h();
    it.next();
    assertEq(it.return(5), { value: "ret 5", done: true });
    assertEq(fin, 1);
    // no throw(): the iterator is closed, then a TypeError
    let closed = 0;
    const noThrow = { [Symbol.iterator]() { return this; }, next() { return { done: false }; },
                      return() { closed++; return {}; } };
    it = (function* () { yield* noThrow; })();
    it.next();
    let err;
    try { it.throw(1); } catch (e) { err = e; }
    assert(err instanceof TypeError && closed === 1, "throw without throw(): close + TypeError");
    // re-entering a delegating generator
    let self;
    const reenter = { [Symbol.iterator]() { return this; }, next() { return self.next(); } };
    self = (function* () { yield* reenter; })();
    try { self.next(); err = null; } catch (e) { err = e; }
    assert(err instanceof TypeError, "a delegating generator is running");
})();

// Generators bind their parameters at the call: errors are the call's.
(function () {
    function* g(a = (() => { throw new RangeError("param"); })()) { yield a; }
    let err;
    try { g(); } catch (e) { err = e; }
    assert(err instanceof RangeError, "generator parameter errors throw at the call");
    async function* ag([x]) { yield x; }
    try { ag(null); err = null; } catch (e) { err = e; }
    assert(err instanceof TypeError, "async generator parameter errors throw at the call");
    function* order(a = log.push("param")) { log.push("body"); }
    const log = [];
    const it = order();
    assertEq(log, ["param"]);
    it.next();
    assertEq(log, ["param", "body"]);
})();

// Prototypes: generator/async function kinds, iterators, tags.
(function () {
    const GenFn = Object.getPrototypeOf(function* () {});
    const AGenFn = Object.getPrototypeOf(async function* () {});
    const AFn = Object.getPrototypeOf(async function () {});
    assert(Object.getPrototypeOf(GenFn) === Function.prototype, "GeneratorFunction.prototype");
    assertEq(GenFn[Symbol.toStringTag], "GeneratorFunction");
    assertEq(AGenFn[Symbol.toStringTag], "AsyncGeneratorFunction");
    assertEq(AFn[Symbol.toStringTag], "AsyncFunction");
    assert(Object.getPrototypeOf(async () => {}) === AFn, "async arrows");
    assert(GenFn.prototype === Object.getPrototypeOf((function* () {}).prototype), "%GeneratorPrototype%");
    const AsyncIterProto = Object.getPrototypeOf(AGenFn.prototype);
    assert(AsyncIterProto[Symbol.asyncIterator].call(7) === 7, "%AsyncIteratorPrototype%");
    let err;
    try { new GenFn.constructor("yield 1"); } catch (e) { err = e; }
    assert(err instanceof TypeError, "no runtime code generation");
    const mapIt = new Map().keys(), setIt = new Set().values(), arrIt = [][Symbol.iterator]();
    assertEq(Object.prototype.toString.call(mapIt), "[object Map Iterator]");
    assertEq(Object.prototype.toString.call(setIt), "[object Set Iterator]");
    assertEq(Object.prototype.toString.call(""[Symbol.iterator]()), "[object String Iterator]");
    try { Object.getPrototypeOf(mapIt).next.call(setIt); err = null; } catch (e) { err = e; }
    assert(err instanceof TypeError, "a Map iterator's next rejects a Set iterator");
    try { Object.getPrototypeOf(arrIt).next.call(mapIt); err = null; } catch (e) { err = e; }
    assert(err instanceof TypeError, "an Array iterator's next rejects a Map iterator");
    const done = new Set([1]).values();
    done.next(); done.next();
    assertEq(Object.getPrototypeOf(setIt).next.call(done), { value: undefined, done: true });
})();

async function asyncConformance() {
    // yield* in async generators, over async and sync iterables
    async function* inner() { yield 1; yield 2; return "inner done"; }
    async function* outer() { const r = yield* inner(); yield r; yield* [3, Promise.resolve(4)]; }
    const got = [];
    for await (const v of outer()) got.push(v);
    assertEq(got, [1, 2, "inner done", 3, 4]);
    // return() forwarded to the inner iterator, finally blocks run
    const log = [];
    async function* guarded() { try { yield* inner(); } finally { log.push("outer finally"); } }
    let it = guarded();
    await it.next();
    assertEq(await it.return("x"), { value: "x", done: true });
    assertEq(log, ["outer finally"]);
    // throw() forwarded; inner catches
    async function* catcher() { try { yield "a"; } catch (e) { yield "caught " + e; } }
    it = (async function* () { yield* catcher(); })();
    await it.next();
    assertEq(await it.throw("E"), { value: "caught E", done: false });
    // `return x` awaits x in an async generator
    it = (async function* () { return Promise.resolve("awaited"); })();
    assertEq(await it.next(), { value: "awaited", done: true });
    // return(v) on a generator that has not started awaits v
    it = (async function* () { yield 1; })();
    assertEq(await it.return(Promise.resolve("v")), { value: "v", done: true });
    // a burst of requests settles in order, and the queue is let go
    it = (async function* () { for (let i = 0; i < 3; i++) yield i; })();
    const ps = [];
    for (let i = 0; i < 50; i++) ps.push(it.next());
    const rs = await Promise.all(ps);
    assertEq(rs.slice(0, 4).map(r => r.value), [0, 1, 2, undefined]);
    assert(rs[49].done, "later requests see a finished generator");
    // yield* ticks: a sync iterable's values are awaited through the wrapper
    const ticks = [];
    const tick = Promise.resolve();
    tick.then(() => ticks.push(1)).then(() => ticks.push(2)).then(() => ticks.push(3));
    it = (async function* () { yield* [0]; })();
    await it.next().then(() => ticks.push("next"));
    assertEq(ticks, [1, 2, 3, "next"]);
    // Await reads a promise's constructor (PromiseResolve)
    let reads = 0;
    const p = Promise.resolve(9);
    Object.defineProperty(p, "constructor", { get() { reads++; return Promise; } });
    assertEq(await p, 9);
    assertEq(reads, 1);
    // an async-from-sync iterator closes the sync iterator when a value rejects
    let closedSync = 0;
    const syncIt = { [Symbol.iterator]() { return this; }, next() { return { value: Promise.reject("no"), done: false }; },
                     return() { closedSync++; return {}; } };
    let caught;
    try { for await (const x of syncIt) {} } catch (e) { caught = e; }
    assertEq(caught, "no");
    assertEq(closedSync, 1);
    return "done";
}
asyncConformance().then(r => print("async conformance ok", r), e => { print("async conformance FAILED", e); throw e; });
