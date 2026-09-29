#!/bin/sh
# Builds both engines 32-bit, -O2, no sanitizers, and runs bench/*.js on
# each. Host timings only say which engine is faster at what; the PSP-1000
# numbers come from the probe EBOOT (docs/engine.md).
set -e
QJS=bench/third_party/quickjs
mkdir -p build/bench
make -s SAN= OPT=-O2 OUT=build/bench build/bench/pxjs
if [ ! -f build/bench/qjs_run ]; then
    gcc -m32 -O2 -w -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION=\"$(cat $QJS/VERSION)\" -I$QJS tools/qjs_run.c \
        $QJS/quickjs.c $QJS/libregexp.c $QJS/libunicode.c $QJS/cutils.c $QJS/dtoa.c -lm -o build/bench/qjs_run
fi
for f in bench/*.js; do
    echo "== $f: PXJS"
    build/bench/pxjs --stats "$f"
    echo "== $f: QuickJS"
    build/bench/qjs_run "$f"
done
