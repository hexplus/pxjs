#!/usr/bin/env python3
"""Runs Test262 against PXJS and classifies every test.

    python3 tools/test262.py [--host build/t262/test262_host] [--test262 third_party/test262]
                             [-j N] [--gc-stress] [--only PREFIX ...] [--out build/test262]

Each test runs in a fresh process (tools/test262_host.c). PXJS treats all code
as strict mode code (docs/compatibility.md), so a test runs once: as strict
code, or as a module for module tests, or unmodified for raw tests. Tests that
only make sense in sloppy mode (flags: [noStrict]) are UNSUPPORTED_DESIGN.
Statuses:

    PASS                  every variant behaved as the test expects
    FAIL                  a variant did not
    EXPECTED_FAIL         failed, and tests/test262/expectations.txt says why
    UNEXPECTED_PASS       listed as an expected failure, but passed: update the list
    UNSUPPORTED_DESIGN    needs something PXJS leaves out on purpose (BigInt, Intl,
                          eval, shared memory, ...); not run, or classified by its error
    HARNESS_UNSUPPORTED   needs a host capability this runner lacks (realms, agents)
    IRRELEVANT            a host feature PXJS does not provide (staging proposals)
    TIMEOUT               ran out of time
    CRASH                 the process died (a signal, a sanitizer report)

Results: <out>/test262-results.json (every test) and <out>/summary.md. Tests
using features newer than ES2023 are reported separately (edition > 2023):
they do not count towards the ES2023 baseline.
"""

import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ------------------------------------------------------------------ features

# Left out of PXJS on purpose (docs/compatibility.md explains each).
DESIGN_FEATURES = {
    "BigInt": "BigInt is not implemented (see docs/compatibility.md)",
    "SharedArrayBuffer": "no shared memory: PXJS runs one thread",
    "Atomics": "no shared memory: PXJS runs one thread",
    "Atomics.waitAsync": "no shared memory: PXJS runs one thread",
    "Atomics.pause": "no shared memory: PXJS runs one thread",
    "tail-call-optimization": "proper tail calls are not implemented (as in V8 and SpiderMonkey)",
    "caller": "function.caller/arguments.callee legacy extensions are not provided",
    "IsHTMLDDA": "document.all is a browser feature",
    "dynamic-import": "import() loads modules at run time; PXJS runs one bundled module",
}

# Standardised after ES2023: the edition they arrived in (or a proposal).
LATER_FEATURES = {
    # ES2024
    "array-grouping": 2024, "arraybuffer-transfer": 2024, "resizable-arraybuffer": 2024,
    "promise-with-resolvers": 2024, "regexp-v-flag": 2024, "String.prototype.isWellFormed": 2024,
    "String.prototype.toWellFormed": 2024,
    # ES2025
    "set-methods": 2025, "iterator-helpers": 2025, "promise-try": 2025, "RegExp.escape": 2025,
    "json-modules": 2025, "import-attributes": 2025, "regexp-duplicate-named-groups": 2025,
    "regexp-modifiers": 2025, "Float16Array": 2025,
    # ES2026 and proposals
    "Array.fromAsync": 2026, "explicit-resource-management": 2026, "uint8array-base64": 2026,
    "Error.isError": 2026, "Math.sumPrecise": 2026, "upsert": 2026, "source-phase-imports": 9999,
    "source-phase-imports-module-source": 9999, "import-defer": 9999, "export-defer": 9999,
    "import-text": 9999, "import-bytes": 9999, "decorators": 9999, "Temporal": 9999, "ShadowRealm": 9999,
    "json-parse-with-source": 9999, "iterator-sequencing": 9999, "joint-iteration": 9999,
    "iterator-chunking": 9999, "iterator-includes": 9999, "Iterator.prototype.join": 9999,
    "immutable-arraybuffer": 9999, "canonical-tz": 9999, "legacy-regexp": 9999, "await-dictionary": 9999,
    "error-stack-accessor": 9999, "nonextensible-applies-to-private": 9999,
}

HARNESS_FEATURES = {
    "cross-realm": "needs a second realm ($262.createRealm)",
}

HARNESS_INCLUDES = {}

# ------------------------------------------------------------------ metadata

FRONT = re.compile(r"/\*---(.*?)---\*/", re.S)


def parse_meta(text):
    """The YAML front matter subset Test262 uses: scalars, [lists], - lists, one nested map."""
    m = FRONT.search(text)
    meta = {}
    if not m:
        return meta
    key = None
    for raw in m.group(1).splitlines():
        line = raw.rstrip()
        if not line.strip() or line.strip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        s = line.strip()
        if indent == 0 and ":" in s:
            key, _, val = s.partition(":")
            key, val = key.strip(), val.strip()
            if val.startswith("[") and val.endswith("]"):
                meta[key] = [v.strip() for v in val[1:-1].split(",") if v.strip()]
            elif val in ("", "|", ">"):
                meta[key] = None
            else:
                meta[key] = val
        elif key and s.startswith("- "):
            if not isinstance(meta.get(key), list):
                meta[key] = []
            meta[key].append(s[2:].strip())
        elif key and indent > 0 and ":" in s and key == "negative":
            if not isinstance(meta.get(key), dict):
                meta[key] = {}
            k, _, v = s.partition(":")
            meta[key][k.strip()] = v.strip()
    return meta


# ------------------------------------------------------------------ expectations

def load_expectations(path):
    """Every expectations*.txt in the directory of `path`. Lines: CATEGORY
    pattern reason... (# comments). Every entry needs a reason."""
    rules = []
    folder = os.path.dirname(path)
    if not os.path.isdir(folder):
        return rules
    for name in sorted(os.listdir(folder)):
        if name.startswith("expectations") and name.endswith(".txt"):
            rules += load_expectation_file(os.path.join(folder, name))
    return rules


def load_expectation_file(path):
    rules = []
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            sys.exit(f"{path}:{n}: an entry needs a category, a pattern and a reason")
        cat, pat, reason = parts
        if cat not in ("EXPECTED_FAIL", "UNSUPPORTED_DESIGN", "HARNESS_UNSUPPORTED", "IRRELEVANT"):
            sys.exit(f"{path}:{n}: unknown category {cat}")
        rules.append((cat, pat, reason))
    return rules


def match_rule(rules, rel):
    for cat, pat, reason in rules:
        if rel == pat or rel.startswith(pat.rstrip("/") + "/") or fnmatch.fnmatch(rel, pat):
            return cat, reason
    return None


# ------------------------------------------------------------------ running

def run_variant(args, test_path, includes, mode):
    cmd = [args.host]
    if mode == "strict":
        cmd.append("--strict")
    if mode == "module":
        cmd.append("--module")
    if args.gc_stress:
        cmd.append("--gc-stress")
    cmd += includes + ["--", test_path]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return {"outcome": "timeout", "ms": int((time.time() - t0) * 1000)}
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    res = {"ms": int((time.time() - t0) * 1000), "out": out[-2000:], "err": err[-2000:], "code": p.returncode}
    last = [l for l in out.splitlines() if l.startswith("PXJS-RESULT ")]
    if p.returncode < 0 or (not last and p.returncode != 0) or "AddressSanitizer" in err or "runtime error:" in err:
        res["outcome"] = "crash"
        return res
    words = last[-1].split(" ", 3) if last else ["", "none"]
    res["outcome"] = words[1]
    res["name"] = words[2] if len(words) > 2 else ""
    res["message"] = words[3] if len(words) > 3 else ""
    return res


EVAL_ERRORS = re.compile(r"eval is not defined|code generation from strings|Function constructor|not supported: eval")


def judge(meta, res, is_async):
    """(passed, why)"""
    neg = meta.get("negative")
    if res["outcome"] in ("timeout", "crash"):
        return False, res["outcome"]
    if neg:
        phase, typ = neg.get("phase"), neg.get("type")
        want = "parse-error" if phase in ("parse", "early") else "runtime-error"
        if phase == "resolution":
            want = None
        if want and res["outcome"] != want:
            return False, f"expected a {phase} {typ}, got {res['outcome']} {res.get('name', '')} {res.get('message', '')}"
        if res["outcome"] in ("parse-error", "runtime-error") and res.get("name") != typ:
            return False, f"expected {typ}, got {res.get('name')}: {res.get('message', '')}"
        if res["outcome"] == "ok":
            return False, f"expected {typ}, completed normally"
        return True, ""
    if res["outcome"] != "ok":
        return False, f"{res['outcome']} {res.get('name', '')}: {res.get('message', '')}".strip()
    if is_async:
        if "Test262:AsyncTestComplete" in res.get("out", ""):
            return True, ""
        fail = [l for l in res.get("out", "").splitlines() if "Test262:AsyncTestFailure" in l]
        return False, fail[0] if fail else "async test did not complete"
    return True, ""


def classify(args, rules, rel):
    path = os.path.join(args.test262, "test", rel)
    text = open(path, encoding="utf-8", errors="replace").read()
    meta = parse_meta(text)
    feats = meta.get("features") or []
    flags = meta.get("flags") or []
    incs = meta.get("includes") or []
    edition = max([LATER_FEATURES.get(f, 2023) for f in feats] or [2023])
    base = {"edition": edition, "features": feats}
    if rel.startswith("annexB/"):
        # Annex B is required only of web browsers (ECMA-262 Annex B): run and
        # reported on its own, outside the baseline (docs/compatibility.md)
        base["annexB"] = True

    if rel.startswith("intl402/") or any(f.startswith("Intl") or f.startswith("intl-") for f in feats):
        return dict(base, status="UNSUPPORTED_DESIGN", reason="Intl is not implemented (embedded size)")
    for f in feats:
        if f in DESIGN_FEATURES:
            return dict(base, status="UNSUPPORTED_DESIGN", reason=DESIGN_FEATURES[f])
    if rel.startswith("staging/"):
        return dict(base, status="IRRELEVANT", reason="staging: proposals not yet in the standard")
    for f in feats:
        if f in HARNESS_FEATURES:
            return dict(base, status="HARNESS_UNSUPPORTED", reason=HARNESS_FEATURES[f])
    for i in incs:
        if i in HARNESS_INCLUDES:
            return dict(base, status="HARNESS_UNSUPPORTED", reason=HARNESS_INCLUDES[i])
    if "CanBlockIsFalse" in flags or "CanBlockIsTrue" in flags or "$262.agent" in text:
        return dict(base, status="HARNESS_UNSUPPORTED", reason="needs agents ($262.agent)")
    if "$262.createRealm" in text:
        return dict(base, status="HARNESS_UNSUPPORTED", reason="needs a second realm ($262.createRealm)")
    if "noStrict" in flags:
        return dict(base, status="UNSUPPORTED_DESIGN", reason="sloppy mode: PXJS runs all code as strict mode code")
    rule = match_rule(rules, rel)
    if rule and rule[0] != "EXPECTED_FAIL":
        return dict(base, status=rule[0], reason=rule[1])
    body = FRONT.sub("", text)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    uses_eval = bool(re.search(r"\beval\s*\(|\bFunction\s*\(|\bindirectEval\b|\.constructor\s*\(\s*['\"`]", body))

    is_async = "async" in flags
    harness = os.path.join(args.test262, "harness")
    includes = []
    if "raw" not in flags:
        includes = [os.path.join(harness, "assert.js"), os.path.join(harness, "sta.js")]
        if is_async:
            includes.append(os.path.join(harness, "doneprintHandle.js"))
        includes += [os.path.join(harness, i) for i in incs]
    if "module" in flags:
        modes = ["module"]
    elif "raw" in flags:
        modes = ["sloppy"]  # as written: the engine treats it as strict anyway
    else:
        modes = ["strict"]

    variants = {}
    passed, why = True, ""
    for mode in modes:
        res = run_variant(args, path, includes, mode)
        ok, reason = judge(meta, res, is_async)
        variants[mode] = {"ok": ok, "outcome": res["outcome"], "ms": res.get("ms"), "why": reason}
        if not ok:
            passed, why = False, why or f"[{mode}] {reason}"
            if res["outcome"] in ("timeout", "crash"):
                variants[mode]["detail"] = (res.get("err") or res.get("out") or "")[-1500:]
                status = "TIMEOUT" if res["outcome"] == "timeout" else "CRASH"
                return dict(base, status=status, reason=why, variants=variants)
            break

    if not passed and (uses_eval or EVAL_ERRORS.search(why)):
        return dict(base, status="UNSUPPORTED_DESIGN", reason="uses eval or the Function constructor", variants=variants)
    if not passed and "no module graph" in why:
        return dict(base, status="UNSUPPORTED_DESIGN", reason="imports another source module (PXJS runs one bundled module)", variants=variants)
    if rule and rule[0] == "EXPECTED_FAIL":
        if passed:
            return dict(base, status="UNEXPECTED_PASS", reason=rule[1], variants=variants)
        return dict(base, status="EXPECTED_FAIL", reason=rule[1], detail=why, variants=variants)
    if passed:
        return dict(base, status="PASS", variants=variants)
    return dict(base, status="FAIL", reason=why, variants=variants)


def collect(args):
    tests = []
    base = os.path.join(args.test262, "test")
    for d in ("annexB", "built-ins", "harness", "intl402", "language", "staging"):
        for dirpath, _, files in os.walk(os.path.join(base, d)):
            for f in files:
                if f.endswith(".js") and "_FIXTURE" not in f:
                    rel = os.path.relpath(os.path.join(dirpath, f), base).replace(os.sep, "/")
                    if not args.only or any(rel.startswith(o) for o in args.only):
                        tests.append(rel)
    return sorted(tests)


STATUSES = ["PASS", "FAIL", "EXPECTED_FAIL", "UNEXPECTED_PASS", "UNSUPPORTED_DESIGN", "HARNESS_UNSUPPORTED",
            "IRRELEVANT", "TIMEOUT", "CRASH"]


def group_of(rel):
    parts = rel.split("/")
    if parts[0] == "built-ins" and len(parts) > 2:
        return "/".join(parts[:2])
    if parts[0] == "language" and len(parts) > 2:
        return "/".join(parts[:2])
    return parts[0]


def summarize(results, commit):
    def count(items):
        c = {s: 0 for s in STATUSES}
        for r in items:
            c[r["status"]] += 1
        return c

    baseline = [r for r in results.values() if r["edition"] <= 2023 and not r.get("annexB")]
    later = [r for r in results.values() if r["edition"] > 2023 and not r.get("annexB")]
    annexb = [r for r in results.values() if r.get("annexB")]
    groups = {}
    for rel, r in results.items():
        if r["edition"] <= 2023 and not r.get("annexB"):
            groups.setdefault(group_of(rel), []).append(r)
    return {
        "test262_commit": commit,
        "baseline": count(baseline),
        "later_editions": count(later),
        "annexB": count(annexb),
        "groups": {g: count(v) for g, v in sorted(groups.items())},
    }


def score(c):
    run = c["PASS"] + c["FAIL"] + c["EXPECTED_FAIL"] + c["UNEXPECTED_PASS"] + c["TIMEOUT"] + c["CRASH"]
    return (c["PASS"] + c["UNEXPECTED_PASS"], run)


def write_markdown(summary, path):
    b = summary["baseline"]
    p, run = score(b)
    lines = [
        "# Test262 results",
        "",
        f"Test262 commit `{summary['test262_commit']}`. Generated by `tools/test262.py`.",
        "",
        f"**ES2023 baseline: {p} of {run} applicable tests pass ({100.0 * p / max(run, 1):.1f}%).**",
        "Applicable means run: not left out by design, not needing a host capability the runner lacks.",
        "",
        "| Status | ES2023 baseline | Later editions | Annex B (browsers only) |",
        "|---|---:|---:|---:|",
    ]
    for s in STATUSES:
        lines.append(f"| {s} | {b[s]} | {summary['later_editions'][s]} | {summary['annexB'][s]} |")
    lines += ["", "## By area (ES2023 baseline)", "", "| Area | Pass | Fail | Expected fail | Design | Harness | Timeout | Crash | Pass rate |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for g, c in summary["groups"].items():
        p, run = score(c)
        rate = f"{100.0 * p / run:.1f}%" if run else "—"
        lines.append(f"| {g} | {c['PASS']} | {c['FAIL']} | {c['EXPECTED_FAIL']} | {c['UNSUPPORTED_DESIGN']} | "
                     f"{c['HARNESS_UNSUPPORTED']} | {c['TIMEOUT']} | {c['CRASH']} | {rate} |")
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(ROOT, "build/t262/test262_host"))
    ap.add_argument("--test262", default=os.path.join(ROOT, "third_party/test262"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build/test262"))
    ap.add_argument("--expectations", default=os.path.join(ROOT, "tests/test262/expectations.txt"))
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--gc-stress", action="store_true")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--list", choices=STATUSES, help="print the tests with this status")
    args = ap.parse_args()

    rules = load_expectations(args.expectations)
    tests = collect(args)
    commit = "unknown"
    try:  # a detached checkout of the pinned commit: .git/HEAD holds the hash
        commit = open(os.path.join(args.test262, ".git", "HEAD")).read().strip()
    except OSError:
        pass
    results = {}
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        futs = {ex.submit(classify, args, rules, rel): rel for rel in tests}
        for n, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            rel = futs[fut]
            try:
                results[rel] = fut.result()
            except Exception as e:  # a bug in this script, not in PXJS
                results[rel] = {"status": "CRASH", "reason": f"runner: {e}", "edition": 2023}
            if n % 2000 == 0:
                print(f"  {n}/{len(tests)} ({time.time() - t0:.0f} s)", file=sys.stderr)
    os.makedirs(args.out, exist_ok=True)
    summary = summarize(results, commit)
    json.dump({"summary": summary, "tests": results}, open(os.path.join(args.out, "test262-results.json"), "w"),
              indent=1, sort_keys=True)
    write_markdown(summary, os.path.join(args.out, "summary.md"))
    b = summary["baseline"]
    p, run = score(b)
    print(f"ES2023 baseline: {p}/{run} pass ({100.0 * p / max(run, 1):.1f}%) in {time.time() - t0:.0f} s")
    print("  " + ", ".join(f"{s} {b[s]}" for s in STATUSES if b[s]))
    l = summary["later_editions"]
    print("later editions: " + ", ".join(f"{s} {l[s]}" for s in STATUSES if l[s]))
    a = summary["annexB"]
    print("Annex B: " + ", ".join(f"{s} {a[s]}" for s in STATUSES if a[s]))
    if args.list:
        for rel, r in sorted(results.items()):
            if r["status"] == args.list:
                print(f"{rel}: {r.get('reason', '')}")


if __name__ == "__main__":
    main()
