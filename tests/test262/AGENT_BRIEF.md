# Working on PXJS conformance (brief for parallel contributors)

PXJS (`pxjs/`) is a JavaScript engine written from scratch in C for the Sony PSP-1000 (333 MHz in-order MIPS,
32 MB RAM, 32-bit pointers, single-precision FPU only: **double arithmetic is software-emulated and slow**).
It is embedded by PSPX (`pspx-runtime/`) but must stay independent of it. Read `docs/engine.md` and
`pxjs/README.md` first, and skim `pxjs/src/px_internal.h` (value representation, heap cells, VM struct).

Design facts you must respect:
- 32-bit tagged values: 31-bit SMIs, pointers to heap cells, specials. Non-SMI numbers are heap cells.
- One fixed heap arena, non-moving mark-sweep GC. **Any value held across an allocation must be rooted**
  (`PX_ROOT(vm, v)` / `px_push_root` ... `px_pop_roots`). The GC-stress mode (collect on every
  allocation) exists to catch mistakes: every change must pass the JS suite under GC stress.
- JavaScript calls do not recurse on the C stack; do not add C recursion over user-controlled data
  (object graphs, nesting). Bound it or make it iterative.
- **All code is strict mode** (by design: PSPX runs bundled ES modules). No `eval`, no `new Function`,
  no sloppy mode, no `with`. BigInt, Intl, SharedArrayBuffer/Atomics are left out by design. Do not add them.
- Be frugal with code size and memory: no big tables, no new dependencies, no libc functions that pull
  in large code on the PSP. Prefer integer paths; never replace double semantics with float shortcuts.
- Match the surrounding code style (C11, 4-space indent, a comment explains *why*), no dead code.

## Tools

Everything builds and runs in the pinned Docker image `pspx-host-build` (32-bit gcc, python3). From the
repo root `C:\Projects\mine\pspx` in Git Bash:

- Test262 (pinned checkout in `pxjs/third_party/test262`, already fetched), with **your own** build and
  output directories so that several people can run at once:

      T262_BUILD=build/t262-<you> scripts/test262.sh --out build/test262-<you> --only built-ins/RegExp language/literals/regexp

  It prints the pass rate; details per test are in `pxjs/build/test262-<you>/test262-results.json`
  (`status`, `reason`, per-variant outcome). Add `--list FAIL` to print failing tests with reasons.
  A single test by hand: build the host (the script does it into `pxjs/<T262_BUILD>/test262_host`) and run
  `test262_host --strict harness/assert.js harness/sta.js [other includes] -- test/path.js` in the container.
  The baseline before this effort is in `pxjs/build/test262/test262-results.json` (do not write there).
- The engine's own suite (ASan + UBSan, then GC stress), with your own build directory:

      MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src/pxjs pspx-host-build \
          sh -c "make -s -j8 OUT=build/<you> && make -s OUT=build/<you> test"

  It must stay fully green. Add regression tests for what you fix to **your own** file
  `pxjs/tests/js/<you>.js` (plain scripts using `assert(cond, msg)`, `assertEq(actual, expected, msg)`,
  `print`; they run in strict mode semantics like everything else).

## Working alongside others

Several contributors edit the engine at the same time, each owning an area (below). Therefore:
- **Never use the Write tool on an existing file**; change files only with small, targeted Edit calls, and
  re-read the lines right before editing a file someone else may also be changing (`px_internal.h`,
  `px_vm.c`, `px_object.c`, `px_builtins.c`, `px_compiler.c`).
- Keep every file compilable after each edit. If the build breaks in code you did not touch, someone is
  mid-change: wait a minute and retry; do not "fix" their code.
- Stay in your area. If a failure's root cause is in another area, note it in your report instead.
- Known deviations you decide not to fix go into **your own** file
  `pxjs/tests/test262/expectations-<you>.txt` (format in `expectations.txt`: `EXPECTED_FAIL <path-or-prefix>
  <reason>`), each with a real reason. No blanket entries.
- Do not touch `pspx-runtime/` or `packages/`. Do not commit (this is not a git repository).

## Report

When done (or when your context is running low), report concisely: what you fixed (root causes, not test
lists), the pass counts for your area before and after, the remaining failure clusters with their causes,
any design decision you took, and anything you found that belongs to another area.
