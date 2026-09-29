#!/usr/bin/env python3
"""Writes docs/compatibility.md from a Test262 run (tools/test262.py).

    python3 tools/compat_report.py [build/test262/test262-results.json] > ../docs/compatibility.md

The tiers are decided here, by hand, with a reason each; the numbers next
to them come from the results file, so the page cannot drift from what was
measured."""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Tier A: expected to conform. Each entry: (label, [Test262 path prefixes]).
TIER_A = [
    ("Core language: expressions, statements, functions, scoping", ["language/expressions", "language/statements",
                                                                     "language/function-code", "language/block-scope",
                                                                     "language/identifiers", "language/literals",
                                                                     "language/types", "language/arguments-object",
                                                                     "language/computed-property-names",
                                                                     "language/destructuring", "language/rest-parameters",
                                                                     "language/asi", "language/keywords",
                                                                     "language/reserved-words", "language/punctuators",
                                                                     "language/white-space", "language/line-terminators",
                                                                     "language/comments", "language/directive-prologue",
                                                                     "language/future-reserved-words",
                                                                     "language/identifier-resolution", "language/global-code",
                                                                     "language/source-text", "language/statementList"]),
    ("Modules (one module, host-provided imports)", ["language/module-code", "language/import", "language/export"]),
    ("Object, Function, Reflect, Symbol", ["built-ins/Object", "built-ins/Function", "built-ins/Reflect",
                                           "built-ins/Symbol", "built-ins/ThrowTypeError"]),
    ("Array and iteration", ["built-ins/Array", "built-ins/ArrayIteratorPrototype", "built-ins/Iterator"]),
    ("String, Number, Boolean, Math, global functions", ["built-ins/String", "built-ins/StringIteratorPrototype",
                                                         "built-ins/Number", "built-ins/Boolean", "built-ins/Math",
                                                         "built-ins/parseInt", "built-ins/parseFloat", "built-ins/isNaN",
                                                         "built-ins/isFinite", "built-ins/decodeURI",
                                                         "built-ins/decodeURIComponent", "built-ins/encodeURI",
                                                         "built-ins/encodeURIComponent", "built-ins/global",
                                                         "built-ins/NaN", "built-ins/Infinity", "built-ins/undefined"]),
    ("Errors", ["built-ins/Error", "built-ins/NativeErrors", "built-ins/AggregateError"]),
    ("JSON", ["built-ins/JSON"]),
    ("Promise, async functions, generators", ["built-ins/Promise", "built-ins/AsyncFunction",
                                              "built-ins/AsyncGeneratorFunction", "built-ins/AsyncGeneratorPrototype",
                                              "built-ins/AsyncFromSyncIteratorPrototype",
                                              "built-ins/AsyncIteratorPrototype", "built-ins/GeneratorFunction",
                                              "built-ins/GeneratorPrototype"]),
    ("Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry", ["built-ins/Map", "built-ins/Set",
                                                                    "built-ins/WeakMap", "built-ins/WeakSet",
                                                                    "built-ins/WeakRef", "built-ins/FinalizationRegistry",
                                                                    "built-ins/MapIteratorPrototype",
                                                                    "built-ins/SetIteratorPrototype"]),
    ("RegExp", ["built-ins/RegExp", "built-ins/RegExpStringIteratorPrototype"]),
    ("ArrayBuffer, typed arrays, DataView", ["built-ins/ArrayBuffer", "built-ins/TypedArray",
                                             "built-ins/TypedArrayConstructors", "built-ins/DataView"]),
    ("Date", ["built-ins/Date"]),
    ("Proxy", ["built-ins/Proxy"]),
]

TIER_C = [
    ("Sloppy mode (non-strict code), `with`", "PSPX runs bundled ES modules, which are strict. PXJS treats all code as "
     "strict, so one set of semantics is implemented and tested, and the runtime is smaller."),
    ("`eval`, `new Function`, other dynamic code", "No code is generated at run time: smaller, safer, and it leaves room "
     "for ahead-of-time compilation. `typeof eval` is \"undefined\"; the Function constructors throw."),
    ("BigInt, BigInt64Array, BigUint64Array", "Arbitrary-precision integers would add a numeric type to every "
     "operator path and a bignum library, for little use in PSPX apps. It could come back as an optional build."),
    ("Intl (ECMA-402)", "Locale data would be several megabytes (ICU); the PSP has 32 MB in total."),
    ("SharedArrayBuffer, Atomics", "PXJS runs one thread; there is no shared memory to coordinate."),
    ("Module graphs, `import()`", "PSPX bundles an app into one module at build time; the engine runs that module "
     "and imports only host-provided modules."),
    ("Proper tail calls", "Not implemented by V8 or SpiderMonkey either; deep recursion is bounded by the frame limit."),
    ("Most of Annex B", "Annex B is required only of web browsers. PXJS keeps the parts real code relies on "
     "(see the Annex B section) and leaves out the rest."),
]

# Annex B, feature by feature: (feature, provided?, why)
ANNEX_B = [
    ("`__proto__: v` in object literals", True, "`{__proto__: null}` is the usual way to make a dictionary object."),
    ("`Object.prototype.__proto__`", True, "Still used by libraries; one accessor pair."),
    ("`String.prototype.substr`", True, "Common in existing code."),
    ("`trimLeft` / `trimRight`", True, "Aliases of `trimStart` / `trimEnd`: no cost."),
    ("RegExp pattern leniency (`/]/`, `/a{/`, a `\\1` before its group)", True,
     "Patterns written for browsers depend on it."),
    ("`Date.prototype.toGMTString`", True, "The same function as `toUTCString`."),
    ("HTML-like comments (`<!--`, `-->`)", False, "Only meaningful in HTML `<script>` elements."),
    ("Sloppy-mode function semantics (block functions in `if`, labelled functions, `arguments` aliasing)", False,
     "All code is strict."),
    ("`escape` / `unescape`", False, "Superseded by `encodeURIComponent`."),
    ("`__defineGetter__` and friends", False, "Superseded by `Object.defineProperty`."),
    ("String HTML methods (`anchor`, `bold`, ...)", False, "Generate HTML markup; nothing uses them."),
    ("`getYear` / `setYear`", False, "Two-digit years; use `getFullYear`."),
    ("`RegExp.prototype.compile`, `RegExp.$1` and other legacy statics", False,
     "Global mutable state updated by every match: a cost on every RegExp call."),
]


def load(path):
    return json.load(open(path, encoding="utf-8"))


def counts(tests, prefixes):
    c = {}
    for rel, r in tests.items():
        if r.get("edition", 2023) > 2023 or rel.startswith("annexB/"):
            continue
        if any(rel == p or rel.startswith(p + "/") for p in prefixes):
            c[r["status"]] = c.get(r["status"], 0) + 1
    return c


def rate(c):
    passed = c.get("PASS", 0) + c.get("UNEXPECTED_PASS", 0)
    run = passed + c.get("FAIL", 0) + c.get("EXPECTED_FAIL", 0) + c.get("TIMEOUT", 0) + c.get("CRASH", 0)
    return passed, run


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/test262/test262-results.json")
    data = load(path)
    tests, summary = data["tests"], data["summary"]
    b = summary["baseline"]
    p, run = rate(b)
    out = []
    w = out.append
    w("# PXJS compatibility")
    w("")
    w("PXJS implements the practical ECMAScript 2023 language and standard library: what modern applications "
      "use, as strict-mode code, with the exclusions listed below made on purpose for the PSP.")
    w("")
    w(f"Measured with Test262 (commit `{summary['test262_commit'][:12]}`), ES2023 features and earlier, "
      f"counting only tests that apply to PXJS's design: **{p} of {run} pass ({100.0 * p / max(run, 1):.1f}%)**.")
    w("This page is generated by `pxjs/tools/compat_report.py` from `scripts/test262.sh` results; "
      "do not edit it by hand.")
    w("")
    w("| Status | Tests |")
    w("|---|---:|")
    for s in ["PASS", "FAIL", "EXPECTED_FAIL", "UNEXPECTED_PASS", "TIMEOUT", "CRASH", "UNSUPPORTED_DESIGN",
              "HARNESS_UNSUPPORTED", "IRRELEVANT"]:
        w(f"| {s} | {b.get(s, 0)} |")
    w("")
    w("## Tier A: expected to conform")
    w("")
    w("| Area | Pass | Applicable | Rate |")
    w("|---|---:|---:|---:|")
    for label, prefixes in TIER_A:
        c = counts(tests, prefixes)
        p, run = rate(c)
        w(f"| {label} | {p} | {run} | {100.0 * p / run:.1f}% |" if run else f"| {label} | — | 0 | — |")
    w("")
    w("Remaining failures in Tier A are bugs or known deviations (Tier B, below), not design choices.")
    w("")
    w("## Tier B: supported with known deviations")
    w("")
    exp = {}
    for rel, r in tests.items():
        if r["status"] in ("EXPECTED_FAIL",):
            exp.setdefault(r.get("reason", "?"), []).append(rel)
    if exp:
        w("| Deviation | Tests |")
        w("|---|---:|")
        for reason, rels in sorted(exp.items(), key=lambda kv: -len(kv[1])):
            w(f"| {reason} | {len(rels)} |")
    else:
        w("None recorded yet.")
    w("")
    w("## Tier C: not supported, by design")
    w("")
    w("| Feature | Why |")
    w("|---|---|")
    for label, why in TIER_C:
        w(f"| {label} | {why} |")
    design = {}
    for rel, r in tests.items():
        if r["status"] == "UNSUPPORTED_DESIGN":
            design[r.get("reason", "?")] = design.get(r.get("reason", "?"), 0) + 1
    w("")
    w("Test262 tests set aside for these reasons (not counted above):")
    w("")
    w("| Reason | Tests |")
    w("|---|---:|")
    for reason, n in sorted(design.items(), key=lambda kv: -kv[1]):
        w(f"| {reason} | {n} |")
    w("")
    w("## Tier D: later editions (ES2024+) implemented as extensions")
    w("")
    w("These do not count towards the ES2023 baseline.")
    w("")
    feats = {}
    for rel, r in tests.items():
        if r.get("edition", 2023) > 2023:
            for f in r.get("features", []):
                if f in LATER:
                    st = feats.setdefault(f, [0, 0])
                    if r["status"] in ("PASS", "UNEXPECTED_PASS"):
                        st[0] += 1
                    if r["status"] in ("PASS", "UNEXPECTED_PASS", "FAIL", "EXPECTED_FAIL", "TIMEOUT", "CRASH"):
                        st[1] += 1
    w("| Feature | Edition | Pass | Run | Status |")
    w("|---|---|---:|---:|---|")
    for f, (pp, rr) in sorted(feats.items(), key=lambda kv: (LATER[kv[0]], kv[0])):
        if not pp or not rr:
            continue
        ed = "proposal" if LATER[f] > 3000 else f"ES{LATER[f]}"
        st = "implemented" if pp >= 0.9 * rr else "partial" if pp >= 0.5 * rr else "not implemented"
        w(f"| {f} | {ed} | {pp} | {rr} | {st} |")
    w("")
    w("\"Implemented\" means at least 90% of the feature's tests pass, \"partial\" at least half. Features "
      "whose tests pass only by accident (a syntax error expected either way, say) show as not implemented; "
      "features with no passing test are left out.")
    w("")
    w("## Annex B (web browser compatibility)")
    w("")
    w("ECMA-262 requires Annex B only of web browsers. It is tested separately and does not count towards the "
      "baseline.")
    w("")
    w("| Feature | In PXJS | Why |")
    w("|---|---|---|")
    for feat, yes, why in ANNEX_B:
        w(f"| {feat} | {'yes' if yes else 'no'} | {why} |")
    ab = {}
    for rel, r in tests.items():
        if rel.startswith("annexB/"):
            ab[r["status"]] = ab.get(r["status"], 0) + 1
    p, run = rate(ab)
    w("")
    w(f"Test262 `annexB/`: {p} of {run} run tests pass; {ab.get('UNSUPPORTED_DESIGN', 0)} are sloppy-mode only.")
    w("")
    w("## Harness limits")
    w("")
    w(f"{b.get('HARNESS_UNSUPPORTED', 0)} tests need host capabilities the Test262 runner does not provide "
      "(a second realm, agents); they say nothing about PXJS either way.")
    print("\n".join(out))


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test262 import LATER_FEATURES as LATER  # noqa: E402

if __name__ == "__main__":
    main()
