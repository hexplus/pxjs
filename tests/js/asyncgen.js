// Async generators and for await.
const log = [];
const tick = (v) => new Promise((r) => r(v));

async function* counter(n) {
    for (let i = 0; i < n; i++) {
        await tick();
        yield i;
    }
    return "end";
}

async function* withFinally() {
    try {
        yield 1;
        yield 2;
    } finally {
        await tick();
        log.push("cleanup");
    }
}

async function* yieldsPromises() {
    yield tick("a");            // yield awaits its operand
    yield Promise.resolve("b");
}

async function* throws() {
    yield 1;
    throw new Error("boom");
}

async function main() {
    // manual protocol
    const g = counter(2);
    assert(g[Symbol.asyncIterator]() === g);
    const p1 = g.next(), p2 = g.next(), p3 = g.next(), p4 = g.next(); // queued
    assert(p1 instanceof Promise);
    assertEq(JSON.stringify(await p1), '{"value":0,"done":false}');
    assertEq(JSON.stringify(await p2), '{"value":1,"done":false}');
    assertEq(JSON.stringify(await p3), '{"value":"end","done":true}');
    assertEq((await p4).done, true);

    // for await over an async generator
    const seen = [];
    for await (const v of counter(3)) seen.push(v);
    assertEq(seen.join(), "0,1,2");

    // yield awaits
    const ys = [];
    for await (const v of yieldsPromises()) ys.push(v);
    assertEq(ys.join(), "a,b");

    // for await over sync iterables: each value awaited
    const vals = [];
    for await (const v of [tick(1), 2, Promise.resolve(3)]) vals.push(v);
    assertEq(vals.join(), "1,2,3");
    for await (const ch of "hi") vals.push(ch);
    assertEq(vals.join(), "1,2,3,h,i");

    // break closes the async generator (its finally awaits)
    for await (const v of withFinally()) {
        if (v === 1) break;
    }
    assertEq(log.join(), "cleanup");

    // return from inside for await also closes
    async function firstOf(gen) { for await (const v of gen) return v; }
    assertEq(await firstOf(withFinally()), 1);
    assertEq(log.join(), "cleanup,cleanup");

    // return() and throw() requests
    const w = withFinally();
    await w.next();
    const r = await w.return(42);
    assertEq(r.value, 42);
    assertEq(r.done, true);
    assertEq(log.length, 3);
    const t = counter(5);
    await t.next();
    let caught = "";
    try { await t.throw(new Error("injected")); } catch (e) { caught = e.message; }
    assertEq(caught, "injected");
    assertEq((await t.next()).done, true);

    // errors propagate to the awaiting loop
    let err = "";
    try {
        for await (const v of throws()) log.push("v" + v);
    } catch (e) { err = e.message; }
    assertEq(err, "boom");

    // custom async iterable
    const custom = {
        [Symbol.asyncIterator]() {
            let i = 0;
            return { next: () => Promise.resolve({ value: i, done: i++ >= 2 }), return: () => { log.push("custom-return"); return Promise.resolve({ done: true }); } };
        },
    };
    const cs = [];
    for await (const v of custom) cs.push(v);
    assertEq(cs.join(), "0,1");
    for await (const v of custom) break;
    assert(log.includes("custom-return"));

    // labeled continue across nested for await
    const pairs = [];
    outer: for await (const a of counter(2)) {
        for await (const b of counter(3)) {
            if (b === 1) continue outer;
            pairs.push(`${a}${b}`);
        }
    }
    assertEq(pairs.join(), "00,10");
    return "done";
}

// yield* in async generators (ES2023 14.4.14, 27.6, 27.1.4)
async function delegation() {
    const drain = async (it) => { const out = []; for await (const v of it) out.push(v); return out; };
    // to an async iterator and to a sync one (CreateAsyncFromSyncIterator);
    // the yield* evaluates to the inner return value
    async function* inner() { yield "a"; return "ret"; }
    async function* outer() { const r = yield* inner(); yield r; yield* ["s", Promise.resolve("p")]; }
    assertEq((await drain(outer())).join(), "a,ret,s,p");
    // nested delegation
    async function* deep(n) { if (n) { yield n; yield* deep(n - 1); } }
    assertEq((await drain(deep(3))).join(), "3,2,1");

    // each inner result is awaited (a thenable result too); values are not
    const log = [];
    const thenableIter = {
        [Symbol.asyncIterator]() { return this; },
        i: 0,
        next(v) {
            log.push("next " + v);
            const r = this.i++ ? { done: true, value: "end" } : { done: false, value: Promise.resolve(1) };
            return { then(res) { res(r); } };
        },
    };
    let it = (async function* () { return yield* thenableIter; })();
    const first = await it.next("ignored");
    assert(first.value instanceof Promise, "an inner value is handed out as it is");
    assertEq(await it.next("x"), { value: "end", done: true });
    assertEq(log, ["next undefined", "next x"]);

    // the inner result must be an object
    const badIter = { [Symbol.asyncIterator]() { return this; }, next() { return 1; } };
    let err;
    try { await (async function* () { yield* badIter; })().next(); } catch (e) { err = e; }
    assert(err instanceof TypeError, "a non-object inner result is a TypeError");

    // throw() forwarded; without throw(): the inner iterator is closed, then TypeError
    const closed = [];
    const noThrow = {
        [Symbol.asyncIterator]() { return this; },
        next() { return { done: false, value: 1 }; },
        return() { closed.push("return"); return {}; },
    };
    it = (async function* () { try { yield* noThrow; } catch (e) { yield e.constructor.name; } })();
    await it.next();
    assertEq(await it.throw(new Error("x")), { value: "TypeError", done: false });
    assertEq(closed, ["return"]);

    // return() without a return method: the received value is awaited and returned
    const noReturn = { [Symbol.asyncIterator]() { return this; }, next() { return { done: false, value: 1 }; } };
    const fin = [];
    it = (async function* () { try { yield* noReturn; } finally { fin.push("finally"); } })();
    await it.next();
    assertEq(await it.return(Promise.resolve("r")), { value: "r", done: true });
    assertEq(fin, ["finally"]);

    // rejections propagate into the generator, where they can be caught
    const rejecting = { [Symbol.asyncIterator]() { return this; }, next() { return Promise.reject("nope"); } };
    it = (async function* () { try { yield* rejecting; } catch (e) { yield "caught " + e; } })();
    assertEq(await it.next(), { value: "caught nope", done: false });
    it = (async function* () { yield* rejecting; })();
    try { await it.next(); err = null; } catch (e) { err = e; }
    assertEq(err, "nope");
    assertEq(await it.next(), { value: undefined, done: true });

    // reentrancy: a request made while the generator runs is queued, not refused
    let self, inside;
    const reenter = {
        [Symbol.asyncIterator]() { return this; },
        n: 0,
        next() { if (!this.n++) inside = self.next(); return { done: this.n > 2, value: this.n }; },
    };
    self = (async function* () { yield* reenter; })();
    assertEq(await self.next(), { value: 1, done: false });
    assertEq(await inside, { value: 2, done: false });
    assertEq(await self.next(), { value: undefined, done: true });

    // closed early by for-await's break: return() reaches the inner iterator
    const cleanup = [];
    async function* innerCleanup() { try { yield 1; yield 2; } finally { cleanup.push("inner"); } }
    async function* outerCleanup() { try { yield* innerCleanup(); } finally { cleanup.push("outer"); } }
    for await (const v of outerCleanup()) break;
    assertEq(cleanup, ["inner", "outer"]);

    // `return x` awaits x inside the generator: a rejection can be caught there
    it = (async function* () { try { return Promise.reject("r"); } catch (e) { return "caught " + e; } })();
    assertEq(await it.next(), { value: "caught r", done: true });

    // for await over a sync iterator closes it when a value rejects
    let syncClosed = 0;
    const sync = {
        [Symbol.iterator]() { return this; },
        next() { return { done: false, value: Promise.reject("bad") }; },
        return() { syncClosed++; return {}; },
    };
    try { for await (const v of sync) {} err = null; } catch (e) { err = e; }
    assertEq(err, "bad");
    assertEq(syncClosed, 1);
    return "done";
}
delegation().then((v) => print("asyncgen delegation ok"), (e) => { print("asyncgen delegation FAILED", e); throw e; });

let finished = "";
main().then((v) => { finished = v; }, (e) => { finished = "FAILED: " + e + "\n" + e.stack; });
// the runner drains the job queue after the script: check at the end of it
globalThis.__check = () => finished;
Promise.resolve().then(function poll(n) {
    if (finished) {
        assertEq(finished, "done");
        print("asyncgen ok");
        return;
    }
    return new Promise((r) => r()).then(poll);
});
