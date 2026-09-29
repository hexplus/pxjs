# PXJS

**PXJS is a compact JavaScript engine for memory-constrained 32-bit systems, originally built for [PSPX](https://github.com/hexplus/pspx) and the Sony PSP.**

Written from scratch in C, PXJS uses the **PSP-1000 as its primary reference platform**: a 333 MHz in-order MIPS CPU with 32 MB of RAM and no hardware double-precision floating point.

PXJS currently passes **98.8% of its applicable ECMAScript 2023 Test262 baseline** — **34,054 of 34,460 tests** on the pinned Test262 commit `7ab7fafa`.

PXJS is a standalone C library with no dependencies beyond libc and libm. It builds for the PSP using PSPDEV and for 32-bit host systems for development and testing. PSPX embeds PXJS, but PXJS itself has no dependency on PSPX.

> **Status:** PXJS is under active development. It is tested on host with ASan, UBSan, GC stress, out-of-memory injection, and the Test262 conformance suite, in the PPSSPP emulator, and **on a real PSP-1000**.

## ECMAScript support

PXJS targets **strict-mode ECMAScript 2023**.

Current Test262 baseline:

```text
Applicable ES2023 tests: 34,460
Passing:                 34,054
Conformance:              98.8%
```

Deliberate exclusions — including sloppy mode, `eval`, BigInt, Intl, SharedArrayBuffer, and unsupported module-loading scenarios — are tracked separately with explicit reasons and are not silently removed from the results.

See [`docs/compatibility.md`](docs/compatibility.md) for the generated compatibility report and [`docs/engine-audit.md`](docs/engine-audit.md) for the full conformance audit.