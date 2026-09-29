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
