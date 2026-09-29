#!/usr/bin/env python3
"""The Web Platform Tests' URL data against PXJS's URL and URLSearchParams.

    python3 tools/url_wpt.py [--pxjs build/host/pxjs] [--source]

Runs tests/wpt/url/urltestdata.json (parsing) and setters_tests.json (the
URL setters), pinned copies from web-platform-tests/wpt (see
tests/wpt/url/README.md), through the pxjs runner. --source tests
src/js/url.js directly instead of the engine's built-in URL.

A test listed in tests/wpt/url-expectations.txt is an expected failure,
with the reason there. Exit status 1 on any unexpected failure, and on an
expected failure that now passes (so the list stays true)."""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tests", "wpt", "url")

RUNNER = r"""
const FIELDS = ['href', 'origin', 'protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'];
const fails = [];
let total = 0;
const show = v => JSON.stringify(v);
for (const t of PARSE) {
  if (typeof t === 'string') continue;
  total++;
  const id = 'parse ' + show(t.input) + ' ' + show(t.base);
  const base = t.base === null ? undefined : t.base;
  let url = null, err = null;
  try { url = base === undefined ? new URL(t.input) : new URL(t.input, base); } catch (e) { err = e; }
  if (t.failure) {
    if (url !== null) fails.push([id, 'parsed, expected a failure: ' + url.href]);
    else if (!(err instanceof TypeError)) fails.push([id, 'threw ' + err]);
    else if (URL.canParse(t.input, base) !== false) fails.push([id, 'canParse is true']);
    continue;
  }
  if (url === null) { fails.push([id, 'threw ' + err]); continue; }
  for (const f of FIELDS) {
    if (!(f in t)) continue;
    if (url[f] !== t[f]) { fails.push([id, f + ' is ' + show(url[f]) + ', expected ' + show(t[f])]); break; }
  }
}
for (const prop of Object.keys(SETTERS)) {
  if (prop === 'comment') continue;
  for (const t of SETTERS[prop]) {
    total++;
    const id = 'set ' + prop + ' ' + show(t.href) + ' ' + show(t.new_value);
    let url;
    try { url = new URL(t.href); url[prop] = t.new_value; }
    catch (e) { fails.push([id, 'threw ' + e]); continue; }
    for (const f of Object.keys(t.expected)) {
      if (url[f] !== t.expected[f]) { fails.push([id, f + ' is ' + show(url[f]) + ', expected ' + show(t.expected[f])]); break; }
    }
  }
}
print('TOTAL ' + total);
for (const [id, why] of fails) print('FAIL ' + id + '\t' + why);
"""


def load_expectations(path):
    exp = {}
    if not os.path.exists(path):
        return exp
    reason = ""
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        if line.startswith("#"):
            reason = line[1:].strip()  # the comment above a group gives its reason
            continue
        exp[line] = reason
    return exp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pxjs", default=os.path.join(ROOT, "build", "host", "pxjs"))
    ap.add_argument("--source", action="store_true", help="test src/js/url.js, not the built-in")
    ap.add_argument("--list", action="store_true", help="print every failure with its detail")
    args = ap.parse_args()

    parse = open(os.path.join(DATA, "urltestdata.json"), encoding="utf-8").read()
    setters = open(os.path.join(DATA, "setters_tests.json"), encoding="utf-8").read()
    head = "const PARSE = " + parse + ";\nconst SETTERS = " + setters + ";\n"
    if args.source:
        src = open(os.path.join(ROOT, "src", "js", "url.js"), encoding="utf-8").read()
        head += "const { URL, URLSearchParams } = (" + src.strip() + ")();\n"
    out_dir = os.path.join(ROOT, "build", "url_wpt")
    os.makedirs(out_dir, exist_ok=True)
    script = os.path.join(out_dir, "run.js")
    with open(script, "w", encoding="utf-8") as f:
        f.write(head + RUNNER)
    r = subprocess.run([args.pxjs, script], capture_output=True, text=True, encoding="utf-8", errors="replace")
    total, fails = 0, {}
    for line in r.stdout.splitlines():
        if line.startswith("TOTAL "):
            total = int(line[6:])
        elif line.startswith("FAIL "):
            tid, _, why = line[5:].partition("\t")
            fails[tid] = why
    if r.returncode != 0 or total == 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
        print("the runner failed")
        return 1
    exp = load_expectations(os.path.join(ROOT, "tests", "wpt", "url-expectations.txt"))
    unexpected = [t for t in fails if t not in exp]
    fixed = [t for t in exp if t not in fails]
    for t in unexpected:
        print("FAIL", t, "--", fails[t])
    for t in fixed:
        print("NOW PASSES (remove it from url-expectations.txt):", t)
    if args.list:
        for t in fails:
            if t in exp:
                print("expected", t, "--", fails[t])
    passed = total - len(fails)
    print(f"{passed} of {total} WPT URL tests pass; {len(fails) - len(unexpected)} expected failures, "
          f"{len(unexpected)} unexpected, {len(fixed)} expected but passing")
    return 1 if unexpected or fixed else 0


if __name__ == "__main__":
    sys.exit(main())
