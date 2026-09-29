# PXJS

**PXJS is a compact JavaScript engine for memory-constrained 32-bit systems, originally built for [PSPX](https://github.com/hexplus/pspx) and the Sony PSP.**

It is written from scratch in C and targets the PSP-1000 as its reference platform: a 333 MHz in-order MIPS CPU with 32 MB of RAM and no hardware double-precision floating point.

PXJS currently passes **98.8% of its applicable ECMAScript 2023 Test262 baseline** — **34,054 of 34,460 tests** on the pinned Test262 commit `7ab7fafa`.

It is a standalone C library with no dependencies beyond libc and libm. PXJS builds for the PSP using PSPDEV and for 32-bit host systems for testing. PSPX embeds PXJS, but PXJS has no dependency on PSPX.

> **Status:** tested on host with ASan, UBSan, GC stress and out-of-memory injection, and tested in PPSSPP. **Real PSP-1000 hardware validation is still pending.**

## ECMAScript support

PXJS targets **strict-mode ECMAScript 2023**.

Current Test262 baseline:

```text
Applicable ES2023 tests: 34,460
Passing:                 34,054
Conformance:              98.8%
```

Deliberate exclusions such as sloppy mode, `eval`, BigInt, Intl, SharedArrayBuffer and unsupported module-graph behavior are classified separately and do not silently disappear from the test results.

See [`docs/compatibility.md`](docs/compatibility.md) for the generated compatibility report and [`docs/engine-audit.md`](docs/engine-audit.md) for the full conformance audit.