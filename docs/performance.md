# PXJS performance ledger

Every performance change, with its measurements. Timings marked *PSP* are
PPSSPP as a PSP-1000 at 222 MHz: comparative only. Real PSP-1000 numbers
go in their own column once measured.

## How to measure

| What | Command | Where |
|---|---|---|
| Workloads on the host, timings | `pxjs/tools/bench.sh` | any Linux; in PSPX: `scripts/pxjs.sh make ...` or the container |
| Workloads on the host, counters | `pxjs/tools/bench.sh --profile` | allocations by type, GCs (count, pauses, bytes reclaimed), inline-cache hits, the 30 most executed instructions |
| PSP (PPSSPP), PXJS vs QuickJS | `scripts/bench-psp.sh pxjs/bench/numconv.js`, `pxjs/bench/calls.js`, `pxjs/bench/workloads/<name>.js` | PSPX repository root |

The counters exist only in a `make PROFILE=1` build (`-DPX_PROFILE`); every
other build compiles them out (`PX_PROF(...)` expands to nothing).

Workloads (`bench/workloads/`):

- `rest_api.js`: a JSON response, then parse, filter/map, a UI model, and stringify the changes.
- `ui_events.js`: native input events call a JS handler, which updates state for a render.
- `timers.js`: native timer ticks resolve promises that async tasks await.
- `media_library.js`: a 1,500-track JSON library, then parse, sort, filter and format strings.
- `app_startup.js`: an app's classes, closures, signals and a 900-node UI tree.

## Baseline (before this batch), PSP

| Benchmark | PXJS | QuickJS |
|---|---:|---:|
| JSON.parse (numconv, per double) | 61.3 µs | 30.0 µs |
| JSON.stringify (numconv, per double) | 41.0 µs | 19.3 µs |
| double → string | 25.5 µs | 18.3 µs |
| string → double | 20.9 µs | 14.2 µs |
| toFixed(2) | 19.3 µs | 8.9 µs |
| native → JS call | 1309 ns | 750 ns |
| JS → JS call | 2338 ns | 1931 ns |
| empty loop iteration | 651 ns | 792 ns |
| startup | 7.85 ms | 3.97 ms |

## Batch 1 (2026-09-29): results, PSP

`scripts/verify.sh` passes all 7 steps after the batch: JS tests under ASan/UBSan and GC stress, the number checker (0 differences), OOM injection, and Test262 unchanged at 98.8%.

| Benchmark | Before | After | Change | QuickJS |
|---|---:|---:|---:|---:|
| JSON.parse (numconv) | 61.3 µs | 34.9 µs | −43% | 30.4 µs |
| JSON.stringify (numconv) | 41.0 µs | 14.3 µs | −65% | 19.4 µs |
| double → string | 25.5 µs | 21.7 µs | −15% | 18.1 µs |
| string → double | 20.9 µs | 20.7 µs | −1% | 14.1 µs |
| toFixed(2) | 19.3 µs | 15.5 µs | −20% | 9.0 µs |
| native → JS call | 1309 ns | 1006 ns | −23% | 750 ns |
| JS → JS call | 2338 ns | 2031 ns | −13% | 1929 ns |
| closure call | 2387 ns | 2107 ns | −12% | 2264 ns |
| empty loop iteration | 651 ns | 574 ns | −12% | 801 ns |
| numconv total | 1247 ms, 8 GCs, 795 kB | 938 ms, 7 GCs, 633 kB | −25% | 893 ms, 476 kB |

Workloads (after batch 1):

| Workload | PXJS | QuickJS |
|---|---:|---:|
| rest_api | 645 ms (parse 331, work 279, stringify 35) | 873 ms (parse 521, work 257, stringify 68) |
| media_library | 353 ms (parse 71, sort 117, search/format 164) | 517 ms (parse 150, sort 236, search/format 131) |
| ui_events | 258 ms (12.9 µs/event) | 251 ms (12.5 µs/event) |

What the counters showed (host, `tools/bench.sh --profile`):

- **Property reads on primitives always missed the inline caches.** For example `v.toFixed(2)` and `s.length`: numconv made 15,026 misses against 9,995 hits.
- **Global variable reads did a dictionary lookup every time.** Script-level functions and variables are globals, so this includes every call to a script-level function.
- **`===` followed by a branch is common,** from if-chains on a value (ui_events).

## Batch 2 (2026-09-29): changes

| # | Change | Why |
|---|---|---|
| 15 | Inline caches for primitive receivers: a string/number/boolean looks up from its prototype through the same cache as an object. String objects (`PX_T_BOXED`) are cacheable too. | `str.charCodeAt`, `n.toFixed`... hit the cache. Getters are never cached, so the receiver still reaches them. Indices and `length` are never cache keys. |
| 16 | `str.length` directly, before the generic path | The most common property of a primitive |
| 17 | A hint per global name: where it was last found in the global object's dictionary, checked against the entry's key on every use (so never stale), for reads and for writes to writable data properties | Global reads and writes without a hash lookup |
| 18 | `===`/`!==` + `JUMP_IF_FALSE` fused, as the compares in batch 1 | One dispatch fewer per test in if-chains |
| 19 | Exact fast path for decimal → double widened from 15 digits to any integer mantissa below 2^53 (most 16-digit strings) | Most doubles printed with 16 digits now parse with one IEEE operation |

## Batch 1 (2026-09-29): changes

| # | Change | Why it should help | Memory / code |
|---|---|---|---|
| 1 | Performance counters (`PX_PROFILE`), `pxjs --profile`, `tools/bench.sh`, five workloads | Measurement | None in normal builds |
| 2 | `px_str_at` inline (was a function call per code unit) | Every string scan: hashing, comparing, parsing, JSON | Slightly more code |
| 3 | JSON.parse reads the text through direct pointers (no call per character) | The scanner's inner loops | — |
| 4 | JSON.parse strings without escapes: made straight from the text; the escape path copies only from the first escape | No per-character copy into a UTF-16 buffer and back | — |
| 5 | JSON.parse keys: `px_intern_chars` finds an existing atom without allocating | Before: a new string per key, garbage right after interning | Fewer allocations, fewer GCs |
| 6 | JSON.parse integers of up to 9 digits: SMIs directly | No software-double arithmetic, no boxing (-0 still a double) | — |
| 7 | JSON.stringify writes into one Latin-1/UTF-16 buffer (doubling), and the string is made once from it | Before: UTF-8 bytes, one call per character, then UTF-8 decoding into the final string | Output buffer is 1-2 bytes per character instead of up to 3 |
| 8 | JSON.stringify: runs of plain characters copied at once; numbers written with `px_itoa`/`px_fmt_number` into a stack buffer | Before: a string per number, and the per-character path | Fewer allocations |
| 9 | JSON.stringify: array element keys made only if toJSON or a replacer needs them; "toJSON" interned once per call | Before: a key string per element, and a new "toJSON" string per object value | Fewer allocations |
| 10 | JSON.stringify: dense array elements read directly (holes, sparse arrays, Proxies take the general path) | Fewer generic property reads | — |
| 11 | dtoa: `bit_length` with `clz` (was a loop of up to 53 64-bit shifts), `estimate_k` in integer arithmetic (was software doubles and `ceil`) | Every number → string and toFixed/toPrecision/toExponential | — |
| 12 | `px_call`: a first path for plain JS functions (the host's callbacks), before the general checks | native → JS calls | — |
| 13 | `push_frame` always inline, its rare parts (`arguments`/rest arrays, overflow error) out of line | Every JS → JS and native → JS call | A little more code at 4 call sites |
| 14 | Fused compare-and-branch (`LT/LE/GT/GE` + `JUMP_IF_FALSE`): one instruction where the compiler finds the pair with no jump target between | One dispatch fewer per loop test (`i < n`) and per `if (a < b)` | 4 opcodes |

Each change keeps the old behaviour on every path user code can observe: toJSON, replacers, getters, Proxies, holes, `-0`, lone surrogates, and integers beyond 9 digits or with a fraction or exponent.

## Rejected or deferred (and why)

- **GC threshold as a fraction of free heap.** It would mean fewer collections on the numeric benchmark, but a longer sweep per collection, so a longer longest pause. A 60 fps app cares more about the longest pause. Deferred until the counters show collections costing real time in app workloads.
- **Eisel-Lemire parsing.** It needs about 10 KB of 128-bit power tables. The exact midpoint correction already handles up to 19 digits; revisit if string → double stays a hotspot on hardware.
- **Keeping JSON source strings alive to slice from them (zero-copy).** It would pin the whole text for as long as any value from it lives: bad for a 32 MB machine.
- **Further superinstructions** (get-local + compare, property load + call): wait for the instruction histogram from real workloads (`tools/bench.sh --profile`).
