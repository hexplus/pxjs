#!/bin/sh
# Every PXJS check in one run, each step logged, a summary at the end.
# Needs make, gcc with -m32 support (gcc-multilib), python3, and git for
# the first Test262 fetch. Runs from any directory.
#
#   tools/verify.sh            everything (about 5 minutes on a fast machine)
#   tools/verify.sh --quick    without Test262
#
# Steps:
#   1. JS tests (tests/js), ASan + UBSan, normally and under GC stress
#   2. number conversion against exact references (tools/dtoa_check.py)
#   3. out-of-memory injection, sampled (tools/oom_test.py)
#   4. the Web Platform Tests' URL data (tools/url_wpt.py)
#   5. the embedded JavaScript headers match their sources (tools/embed_js.py)
#   6. Test262, ES2023 baseline -> build/test262/ (json + summary.md)
#   7. docs/compatibility.md regenerated from those results
#
# Logs: build/verify/<step>.log. Exit status: non-zero if any step failed.
set -u
cd "$(dirname "$0")/.."

LOGS=build/verify
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
mkdir -p "$LOGS"
SUMMARY=""
FAILED=0

# step <name> <shell command>: runs it, logs it, records PASS/FAIL and the time
step() {
    name=$1
    shift
    printf '%-20s ' "$name"
    t0=$(date +%s)
    if sh -c "$1" >"$LOGS/$name.log" 2>&1; then r=PASS; else r=FAIL; FAILED=1; fi
    dt=$(($(date +%s) - t0))
    echo "$r  (${dt}s)  $(tail -n 1 "$LOGS/$name.log" | cut -c1-80)"
    SUMMARY="$SUMMARY
$(printf '%-20s %s  %4ss' "$name" "$r" "$dt")"
}

step 1-js-tests     'make -s -j$(nproc) && sh tests/run.sh build/host/pxjs'
step 2-number-check 'python3 tools/dtoa_check.py 20000'
step 3-oom-sampled  'make -s -j$(nproc) && python3 tools/oom_test.py --points 60'
step 4-url-wpt      'make -s -j$(nproc) && python3 tools/url_wpt.py'
step 5-embedded-js  'for f in url; do python3 tools/embed_js.py src/js/$f.js k_${f}_js | cmp -s - src/px_${f}_js.h || { echo "src/px_${f}_js.h is out of date: python3 tools/embed_js.py src/js/$f.js k_${f}_js > src/px_${f}_js.h"; exit 1; }; done; echo up to date'
if [ "$QUICK" = 0 ]; then
    step 6-test262    'sh tools/test262.sh'
    step 7-compat-doc 'python3 tools/compat_report.py build/test262/test262-results.json > docs/compatibility.md && sed -n 5p docs/compatibility.md'
fi

echo "
Summary (logs in pxjs/build/verify/):$SUMMARY"
if [ "$QUICK" = 0 ] && [ -f build/test262/summary.md ]; then
    echo
    grep -m1 "ES2023 baseline" build/test262/summary.md
fi
exit $FAILED
