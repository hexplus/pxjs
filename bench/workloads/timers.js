// Timer workload: a native timer -> a JS callback -> a Promise resolution
// -> microtasks (await). On the PSP bench the ticks come from C through
// callJS; elsewhere from a JS loop. Each tick settles one promise that an
// async task is waiting on.
const TICKS = 5000;
let waiting = [], done = 0, value = 0;

function sleep() { return new Promise(resolve => waiting.push(resolve)); }

async function task(id) {
    for (;;) {
        const v = await sleep();
        value += v + id;
        done++;
        if (done >= TICKS) return;
    }
}

function onTimer() {
    const w = waiting;
    waiting = [];
    for (let i = 0; i < w.length; i++) w[i](i);
}

for (let i = 0; i < 8; i++) task(i);
const t0 = now();

// The microtasks run between host calls: both runners drain the job queue
// after the script, so the ticks are driven from a chain of promise jobs.
let ticks = 0;
function tick() {
    if (typeof callJS === "function") callJS(onTimer, 1);
    else onTimer();
    ticks++;
    if (done < TICKS && ticks < TICKS * 2) return Promise.resolve().then(tick);
    const ms = now() - t0;
    print("timers: " + ms.toFixed(1) + " ms, " + ((ms * 1000) / done).toFixed(2) + " us/resolution, " + ticks +
          " ticks, checksum " + value);
}
Promise.resolve().then(tick);
