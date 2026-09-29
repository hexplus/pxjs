#!/bin/sh
# Test262 for PXJS: fetches the pinned Test262 commit (once), builds the
# test host, and runs tools/test262.py. Needs make, gcc with -m32 support,
# python3, and git for the first fetch.
#
#   tools/test262.sh                          everything
#   tools/test262.sh --only built-ins/Array language/expressions
#   tools/test262.sh --gc-stress --only built-ins/Promise
#   T262_SAN=1 tools/test262.sh ...           the host with ASan/UBSan (slower)
#   T262_BUILD=build/t262-mine tools/test262.sh --out build/test262-mine ...
#                                             a separate build and result directory
#
# Results: build/test262/test262-results.json and summary.md.
set -eu
cd "$(dirname "$0")/.."

TEST262_COMMIT=7ab7fafa0003f73fc85c1b95d88094d33f7eb8bd
T262=third_party/test262

# a detached checkout of the pinned commit: .git/HEAD holds the hash
if [ "$(cat "$T262/.git/HEAD" 2>/dev/null || true)" != "$TEST262_COMMIT" ]; then
    echo "fetching Test262 $TEST262_COMMIT"
    mkdir -p "$T262"
    git -C "$T262" init -q
    git -C "$T262" remote add origin https://github.com/tc39/test262.git 2>/dev/null || true
    git -C "$T262" fetch -q --depth 1 origin "$TEST262_COMMIT"
    git -C "$T262" checkout -q FETCH_HEAD
fi

if [ "${T262_SAN:-0}" = 1 ]; then
    out=${T262_BUILD:-build/t262san}
    make -s -j"$(nproc)" OUT="$out" OPT='-O2 -g' "$out/test262_host"
else
    out=${T262_BUILD:-build/t262}
    make -s -j"$(nproc)" OUT="$out" SAN= OPT='-O2 -g' "$out/test262_host"
fi
exec python3 tools/test262.py --host "$out/test262_host" "$@"
