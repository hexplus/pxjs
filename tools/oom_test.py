#!/usr/bin/env python3
"""Allocation-failure injection over the JS suite.

    python3 tools/oom_test.py [--pxjs build/host/pxjs] [--points 300] [-j N] [tests/js/x.js ...]

For each script: count its heap allocations, then run it again once per
chosen allocation number N with `pxjs --fail-alloc N`: allocation #N fails
as out of memory. The script may then fail (an uncaught out-of-memory error
is fine) but the process must not crash, trip a sanitizer, hang, or leave the
VM broken (pxjs runs a check script and a collection afterwards). Chosen
points: the first 100 allocations (engine set-up paths) and the rest spread
evenly over the run.
"""

import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, timeout=60):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def points(total, n):
    pts = set(range(1, min(total, 100) + 1))
    if total > 100:
        step = max(1, (total - 100) // max(1, n - 100))
        pts.update(range(101, total + 1, step))
    return sorted(pts)


def check(args, script, n):
    code, out, err = run([args.pxjs, "--fail-alloc", str(n), script])
    bad = code is None or code < 0 or "Sanitizer" in err or "runtime error:" in err or "is broken" in err
    return None if not bad else f"{os.path.basename(script)} #{n}: " + (err.strip().splitlines() or ["?"])[-1][:300]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pxjs", default=os.path.join(ROOT, "build/host/pxjs"))
    ap.add_argument("--points", type=int, default=300)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("scripts", nargs="*")
    args = ap.parse_args()
    scripts = args.scripts or sorted(glob.glob(os.path.join(ROOT, "tests/js/*.js")))
    failures, runs = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        futs = []
        for s in scripts:
            code, out, err = run([args.pxjs, "--count-allocs", s])
            m = re.search(r"allocations: (\d+)", out)
            if not m:
                print(f"{s}: cannot count allocations ({err.strip()[:200]})")
                continue
            total = int(m.group(1))
            pts = points(total, args.points)
            runs += len(pts)
            print(f"{os.path.basename(s)}: {total} allocations, {len(pts)} failure points", flush=True)
            futs += [ex.submit(check, args, s, n) for n in pts]
        for f in concurrent.futures.as_completed(futs):
            r = f.result()
            if r:
                failures.append(r)
                print("FAIL", r, flush=True)
    print(f"{runs} runs, {len(failures)} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
