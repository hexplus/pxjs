#!/bin/sh
# Runs every tests/js/*.js twice: normally, and with a garbage collection
# before every allocation (--gc-stress), which turns a missing GC root
# anywhere in the engine into an immediate ASan report instead of a rare
# corruption. A test passes when it exits 0 (no uncaught exception).
PXJS=${1:-build/host/pxjs}
pass=0
fail=0
for f in tests/js/*.js; do
    for mode in "" "--gc-stress"; do
        if out=$("$PXJS" $mode "$f" 2>&1); then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL $f $mode"
            echo "$out" | tail -15 | sed 's/^/    /'
        fi
    done
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
