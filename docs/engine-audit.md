# PXJS engine audit

An audit of PXJS (`pxjs/`): how it is built, what it gets right and wrong, measured against Test262 and against the PSP-1000's limits. Started 2026-09-29.

- Conformance numbers come from [`tools/test262.py`](../tools/test262.py) against a pinned Test262 commit ([compatibility.md](compatibility.md)).
- Timings marked *PSP* were taken in PPSSPP set up as a PSP-1000 at 222 MHz. They compare approaches; they are not hardware facts.

## 1. Architecture map

```
 source text ──► lexer (px_lexer.c) ──► single-pass compiler (px_compiler.c)
                                          │  scopes, closures (upvalue descriptors), early errors,
                                          │  lookahead memo (group_ends / fn_ends), constant table
                                          ▼
                                   PxProto: bytecode + consts + line table + IC slots
                                          │  (px_opcodes.h: ~150 opcodes, byte-coded, u8/u16 operands)
                                          ▼
      VM (px_vm.c): one dispatch loop (switch), frames in an array (PxFrame), handler stack (PxHandler)
          │  JS→JS calls push a frame and continue in the same loop (no C recursion)
          │  native→JS re-enters the loop (bounded: max_native_depth, default 48)
          │  generators/async: the frame is copied into a PxGen and back (px_vm.c, px_asyncgen.c)
          ▼
      object model (px_object.c): shapes (transition tree, weak child links, hashed index for big
          │  shapes), dictionary mode, inline caches (PxIC per site, invalidated by shape epoch),
          │  arrays (dense PxVec + sparse → dictionary), accessors, Proxy (px_proxy.c)
          ▼
      heap (px_heap.c): one arena; size-segregated free lists + bump pointer; cell header
          │  (type, mark bit, size in 8-byte units)
          ▼
      GC: non-moving mark-sweep; explicit mark stack with overflow rescan (no recursion);
          weak maps as ephemerons (fixpoint), WeakRef, FinalizationRegistry (cleanup queued as
          jobs), native finalizers (px_set_finalizer), weak shape/atom tables swept first
```

Cross-cutting systems:

| System | Where | Notes |
|---|---|---|
| Exceptions | `vm->exception` + `PX_EXCEPTION` return value; handler stack in the VM | Natives return `PX_EXCEPTION` after `px_throw*`. The VM unwinds to the nearest `PxHandler` (try) or frame boundary. |
| Scopes | compile time: locals in frame slots, captured ones as `PxUpval` (open while on the stack, closed after) | Per-iteration `let` bindings are new upvalues per iteration. |
| Property lookup | `px_get`/`px_set`/`px_define` (px_object.c); ICs at `obj.name` sites | ICs cache receiver shape + holder shape. Every GC invalidates every IC (shape addresses may be reused). |
| Prototypes | `PxShape.proto`: the prototype is part of the shape | Changing a prototype gives the object a new shape, so the ICs stay correct. |
| Iterators | px_iter.c; `IteratorClose` on abrupt exits in the compiler's for-of/destructuring | |
| Jobs | a malloc'd FIFO ring (`vm->jobs`), drained by `px_run_jobs` | FinalizationRegistry cleanups are queued into it by the GC. |
| Promises | px_promise.c; a host rejection tracker reports unhandled rejections after the checkpoint | |
| Weak references | px_heap.c `weak_process` | |
| Native integration | pxjs.h: handles (`px_retain`) for long-lived values, a root stack (`px_push_root`) for temporaries, finalizers, typed-array bytes | |

## 2. Value representation

32-bit words: `…1` a 31-bit SMI; `…000` a pointer to an 8-byte-aligned cell (0 is never a value); `…010` specials (undefined, null, false, true, hole, exception). Numbers that don't fit an SMI are boxed `PxNumber` cells holding a `double`.

- **Checked by probes:**
  - SMI overflow promotes to a double: `0x3fffffff + 1 = 1073741824`, `1073741823 * 2 = 2147483646`.
  - `-0` is never an SMI: `Object.is(-0 * 1, -0)`, `1 / (0 * -1) === -Infinity`, `(-1) % 1` is `-0`.
  - Int32 conversions: `2147483648 | 0 = -2147483648`, `-1 >>> 0 = 4294967295`, `1 << 31 = -2147483648`, `5 >> 33 = 2`.
- **Pointer alignment:** every cell is 8-byte aligned, so pointer tags can't collide with SMIs or specials.
- **`-fno-strict-aliasing` is required.** Every cell is read through several struct types that share a leading prefix (`PxCell` → `PxObject` → `PxArray`...), and GCC -O2 miscompiled this without the flag. Making the code alias-clean would mean accessing cells through unions or `memcpy`, touching every file, for no measured gain. **Decision:** keep the flag. It is documented in both Makefiles, and a build without it is unsupported.
- **The cost:** every number outside ±2^30 is a 16-byte heap cell. On the PSP a double operation is a software routine anyway, so the allocation is not the dominant cost, but numbers are the most frequent allocation in float code. There is a fast path for boxing (`px_box_number`: free list or bump).

## 3. Memory: out-of-memory recovery

**Allocation-failure injection** (new): `px_set_alloc_failure(vm, n)` makes the n-th heap allocation fail as out of memory. `pxjs --fail-alloc N` runs a script that way, then checks that the VM still works (a check script and a full collection). [`tools/oom_test.py`](../tools/oom_test.py) drives it.

- **Result:** every allocation point of the whole JS suite was failed in turn, 145,965 runs under ASan/UBSan.
- **One bug found and fixed:** `lazy_prototype` popped one root too many when adding `prototype` failed.
- **Now:** 0 failures. Every allocation site reached by the suite recovers from out-of-memory: the error is catchable, and the heap and VM stay consistent.
- The arena is the only memory JS objects use. Engine side tables live outside it (shape hash indexes, the job queue, the handle table, the weak-map list, the finalizer list), and each has a failure path. For example, when the weak list can't grow, that map is traced strongly for the collection: correct, only not weak.

## 4. Garbage collector

- **Roots:** the value stack, frames (function, this, new.target, generator), open upvalues, the job queue, handles, the native root stack, the compiler's temporary roots, the pending exception, the preallocated OOM error, well-known symbols, atoms, prototypes and constructors, and the WeakRef keep-alive list. Suspended generator frames are heap cells, reached through their objects.
- **Marking** uses an explicit stack. When the stack is full, it sets an overflow flag and rescans the heap for marked, untraced cells. There is no C recursion, so a 20,000-deep linked list marks fine.
- **Weak processing** runs after marking:
  1. Ephemerons iterate to a fixpoint (a value that makes another key live).
  2. Dead WeakRef targets are cleared.
  3. FinalizationRegistry cleanups are queued as jobs.
  4. Native finalizers run (they only free native memory).
  5. The shape and atom tables are swept.
  6. The heap is swept.

  No JavaScript runs inside the GC.
- **Pause times, full collection, PSP (PPSSPP, 222 MHz, 6 MB heap):**

  | Live data | PXJS | QuickJS (reference) |
  |---|---:|---:|
  | empty | 0.9 ms | 0.6 ms |
  | 20k small objects | 20 ms | 26 ms |
  | 60k small objects | 52 ms | 78 ms |
  | 20k strings | 15 ms | 2.5 ms (reference counting: strings are not traced) |
  | 20k closures | 41 ms | 50 ms |
  | 20k-deep linked list | 14 ms | 25 ms |
  | 10k two-object cycles | 24 ms | 25 ms |

  The pause is proportional to live data (the sweep stops at the bump pointer). A 60 fps app can afford a pause of a few ms, so apps should keep their live heap to a few thousand objects, or accept a skipped frame when a collection happens.

  **Decision:** no generational or incremental collector. The simple collector is robust and its pauses are predictable at PSP heap sizes. Revisit only if real apps show pauses users notice.

## 5. C stack safety

Directly recursive C functions, and what bounds each:

| Function | Bound |
|---|---|
| compiler (`statement`, `binary`, `unary`, `new_expr`, patterns) | `Compiler.depth` nesting limit |
| `lex_template_chunk` | template nesting (bounded by the compiler's limits) |
| JSON `parse_value`, `revive`, `inspect` | `JSON_MAX_DEPTH` / `INSPECT_DEPTH` |
| structuredClone `clone` | `CLONE_MAX_DEPTH` |
| `Array.prototype.flat` (`flatten_into`) | `max_native_depth` |
| `run` (VM re-entry from natives) | `max_native_depth` |
| `re_run` (RegExp matcher) | under review by the RegExp work: backtracking must be bounded |
| `px_root_shape` | table growth only |
| `obj_freeze` / `obj_is_frozen` | not recursive (the earlier listing was wrong) |
| Proxy forwarding (a proxy whose target is a proxy...) | **was unbounded**: a chain of 20,000 trap-less proxies aborted the process on `set` and `Object.freeze` ("root stack overflow"). Calls through proxy chains threw RangeError correctly. Fix assigned to the built-ins work. |

## 6. Inline caches

Probes warmed an `obj.m` site, then mutated what it depends on. Each case gives the right result:

- shadowing on an intermediate prototype, and deleting the shadow;
- data→accessor on a prototype;
- `setPrototypeOf` on a prototype;
- data→accessor on the receiver;
- a Proxy at a warmed site.

The IC keys on the receiver's shape and the holder's shape. The prototype is part of the shape, and a prototype that changes gets a new shape.

## 7. Unicode

- Strings are Latin-1 or UTF-16, and `length` counts UTF-16 code units, so JavaScript sees UTF-16 semantics. Probes: `"a\u{1F600}b".length === 4`, a lone surrogate has length 1, `codePointAt`/`charCodeAt` are correct, and string iteration yields code points.
- **Finding:** the General_Category table (`px_unicode.c`) covers the BMP only, and it was generated from Python's `unicodedata`. The file says Unicode 13.0.0, while the container's Python has 14.0.0, so regenerating it is not reproducible. The RegExp work was asked to move it to pinned UCD files covering all planes.

## 8. Numbers and text

The conversions were the worst performance problem found, and they had correctness bugs.

**Before:**

- `Number::toString` found the shortest round-trip digits by printing at precision 1 to 17 with `snprintf` and reading each result back with `strtod`. On the PSP both are software-double routines in newlib. One conversion took **840 µs** in PPSSPP (QuickJS: 18 µs).
- Printing 5,000 doubles took 4.2 s, and so did `JSON.stringify` of an array of 5,000 doubles.
- `toFixed`, `toExponential` and `toPrecision` used printf, which rounds an exact tie to even. The spec rounds it up: `(2.5).toFixed(0)` gave `"2"` and `(0.5).toFixed(0)` gave `"0"`.
- `toExponential()` with no argument returned `toString()`, and the range checks ran in the wrong order.
- Parsing copied the text into 400-byte buffers:
  - `Number()` of a string of 400 or more characters was `NaN`;
  - `parseFloat` and numeric literals silently dropped the digits after ~392;
  - JSON rejected such numbers.
- Hex, octal and binary numbers above 2^53 were not correctly rounded.

**Now** ([`src/px_dtoa.c`](../src/px_dtoa.c), no printf):

- **Shortest digits:** Burger & Dybvig's free-format algorithm on exact big integers. It gives the shortest digits that read back as the same double, the closest such digits, and the even digit on a tie. A 64-bit fast path covers the common range, 1 to 2^53.
- **Fixed precision:** exact digits, ties rounded up as the spec says.
- **Decimal to double:**
  - up to 15 digits with a small exponent: one exact IEEE operation (Clinger's fast path);
  - up to 19 digits: a double approximation, corrected one unit at a time by exact comparison with the midpoints to its neighbours;
  - longer inputs: libc `strtod` on at most 800 significant digits plus a sticky digit, which rounds identically.
- **Radix 2, 8 and 16:** correctly rounded, with 61+ significant bits and a sticky bit.
- **Output is platform-independent:** it no longer depends on the C library's printf.
- **Verified:** [`tools/dtoa_check.py`](../tools/dtoa_check.py) compares 250,000 cases against exact references (Python's `decimal` module and `float` repr) with 0 differences. The cases are random bit patterns, powers of ten and their neighbours, denormals, exact ties, near-halfway decimal strings and 900-digit inputs. The JS test is `tests/js/number_conversion.js`.

PSP timings (PPSSPP, 222 MHz), µs per operation:

| | before | now | QuickJS |
|---|---:|---:|---:|
| double → string | 840 | 25.5 | 18.0 |
| string → double | 29 | 21 | 14 |
| `toFixed(2)` | 47 | 19 | 9 |
| `JSON.stringify`, per double | 853 | 41 | 19 |

**Host floating point:**

- The host build (32-bit x86) computed doubles on the x87 unit. The x87 works in 80 bits and rounds twice, where the PSP's software doubles round once, as IEEE 754 requires.
- The checker caught this: one decimal string parsed one unit off.
- The Makefile now builds with `-msse2 -mfpmath=sse`, and the fast path in `px_dtoa.c` is compiled only where `FLT_EVAL_METHOD == 0`.

## 9. Calls and startup

PSP (PPSSPP, 222 MHz), [`bench/calls.js`](../bench/calls.js), per call, loop overhead included:

| | PXJS | QuickJS |
|---|---:|---:|
| empty loop iteration | 0.65 µs | 0.80 µs |
| JS → JS | 2.29 µs | 1.93 µs |
| JS → native | 1.75 µs | 1.93 µs |
| JS → native, as a method | 1.57 µs | 2.20 µs |
| native → JS (`px_call`) | 1.31 µs | 0.75 µs |
| closure call | 2.39 µs | 2.26 µs |

Findings:

- **Native → JS is 1.7× QuickJS.** `px_call` re-enters the dispatch loop (`run`) for every call. It matters for callbacks called from C: timers, events, the UI tree.
- **The empty loop is six instructions per iteration** (`GET_LOCAL`, `GET_UPVAL_CHECK`, `LT`, `JUMP_IF_FALSE`, `INC_LOCAL`, `LOOP`): about 145 emulated cycles, 24 per instruction.
  - A fused compare-and-branch would remove one dispatch in six.
  - Not done: PPSSPP's timing is not the hardware's, and the gain should be measured on a PSP-1000 first.

**Startup:** `px_new` (all built-ins) takes 7.0 ms and leaves 58 kB live in 1,438 cells. QuickJS takes 4.0 ms and 145 kB.

## 10. Annex B

ECMA-262 requires Annex B only of web browsers.

**Decision:**

- **Kept:** the parts real code relies on: `__proto__` in object literals, `Object.prototype.__proto__`, `substr`, `trimLeft`/`trimRight`, lenient RegExp patterns, `toGMTString`.
- **Left out:**
  - sloppy-mode semantics (all code is strict);
  - HTML comments;
  - `escape`/`unescape`;
  - `__defineGetter__` and friends;
  - the String HTML methods;
  - `getYear`/`setYear`;
  - the RegExp legacy statics, which would add global state to every match.

`tools/test262.py` reports `annexB/` on its own, outside the ES2023 baseline.

**Probe result:** `{__proto__: proto}` did **not** set the prototype. It is being fixed in the compiler work, and `Object.prototype.__proto__` and `trimLeft`/`trimRight` in the built-ins work.

## 11. Code size (host -O2, x86 — relative sizes)

px_builtins 84 kB, px_compiler 75 kB, px_vm 44 kB, px_typed 37 kB, px_object 31 kB, px_regexp 30 kB, px_json 19 kB, px_proxy 16 kB, px_collections 15 kB, px_iter 14 kB, px_promise 13 kB, px_string 13 kB, px_lexer 13 kB, px_date 11 kB, px_unicode 9 kB, px_heap 8 kB, px_web 8 kB, px_asyncgen 6 kB, px_api 5 kB.

## 12. Test262 baseline (before this audit's fixes)

Test262 at `7ab7fafa`, ES2023 baseline, applicable tests only:

- The first run counted sloppy-mode variants and `eval`/`import()` tests, and 63.9% passed. That mixed deliberate exclusions with bugs.
- Classifying those exclusions as by design (all code is strict, no `eval`, no module loading at run time) gives 22,877 of 34,649, **66.0%**.

The largest clusters of failures:

- missing early errors (~2,000)
- `yield*` in async generators not implemented (~1,150)
- the attributes of `length`/`name` on built-ins (~640)
- `Object.create` with properties (~290)
- computed class field names (~240)
- primitive-wrapper checks (~660)
- RegExp (~940)

## 13. Result

After the fixes, on the same Test262 commit: **34,054 of 34,460 applicable ES2023 tests pass (98.8%)**, up from 66.0%. That run is `tools/verify.sh` (in PSPX: `scripts/verify.sh`), and all of its steps pass:

- the JS suite under ASan/UBSan and GC stress;
- the number conversion check;
- sampled out-of-memory injection;
- Test262;
- the runtime host tests;
- the PSP build.

The per-area numbers and the deliberate exclusions are in [compatibility.md](compatibility.md), which is generated from that run.

Known remaining deviations:

- `this` and `super()` inside arrow functions in a derived constructor;
- `arguments` is an Array;
- `Function.prototype.toString` returns no source text, so the function source is not kept in memory;
- native constructors read `newTarget.prototype` before validating their arguments;
- anonymous functions under computed keys in object literals are not named.
