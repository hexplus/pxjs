#!/bin/sh
# Host benchmarks: the realistic workloads (bench/workloads/) and the
# number/JSON micro-benchmark, on an optimised build without sanitizers.
#
#   tools/bench.sh              timings (build/bench/pxjs)
#   tools/bench.sh --profile    the same with the performance counters
#                               (allocations by type, GCs, inline caches,
#                               the most executed instructions) on stderr
#
# Host timings compare versions of PXJS with each other; PSP timings come
# from scripts/bench-psp.sh (PPSSPP) or a PSP-1000.
set -eu
cd "$(dirname "$0")/.."
if [ "${1:-}" = "--profile" ]; then
    out=build/prof
    make -s -j"$(nproc)" OUT=$out SAN= OPT='-O2 -g' PROFILE=1 $out/pxjs
    flags=--profile
else
    out=build/bench
    make -s -j"$(nproc)" OUT=$out SAN= OPT='-O2 -g' $out/pxjs
    flags=
fi
for f in bench/workloads/*.js bench/numconv.js; do
    $out/pxjs $flags "$f"
done
