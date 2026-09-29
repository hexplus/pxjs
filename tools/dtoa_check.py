#!/usr/bin/env python3
"""Checks src/px_dtoa.c against exact references computed with Python's
decimal module and float repr (shortest round-trip digits).

    python3 tools/dtoa_check.py [count]      (in the host build container)

Builds tools/dtoa_check.c, feeds it random and edge-case doubles and decimal
strings, and compares every answer. Exit status 1 on any difference."""

import os
import random
import struct
import subprocess
import sys
from decimal import Decimal, ROUND_HALF_UP, getcontext

getcontext().prec = 2000
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def from_bits(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def shortest(x):
    """digits, n with x ~ 0.digits * 10^n"""
    t = Decimal(repr(x)).as_tuple()
    ds = "".join(map(str, t.digits)).lstrip("0") or "0"
    exp = t.exponent + (len("".join(map(str, t.digits))) - len(ds)) * 0
    # value = int(digits) * 10^exponent
    full = "".join(map(str, t.digits))
    k = len(full) + t.exponent
    ds = full.lstrip("0")
    k -= len(full) - len(ds)
    ds = ds.rstrip("0")
    return ds, k


def js_to_string(x):
    if x == 0:
        return "0"
    if x < 0:
        return "-" + js_to_string(-x)
    ds, n = shortest(x)
    k = len(ds)
    if k <= n <= 21:
        return ds + "0" * (n - k)
    if 0 < n <= 21:
        return ds[:n] + "." + ds[n:]
    if -6 < n <= 0:
        return "0." + "0" * (-n) + ds
    e = n - 1
    return ds[0] + ("." + ds[1:] if k > 1 else "") + "e" + ("+" if e >= 0 else "-") + str(abs(e))


def to_fixed(x, f):
    if x == 0:
        x = 0.0
    d = Decimal(x).quantize(Decimal(1).scaleb(-f), rounding=ROUND_HALF_UP)
    s = format(d, "f")
    if x < 0 and not s.startswith("-"):
        s = "-" + s
    return s


def exp_digits(x, f):
    """n (f+1 digits), e: x ~ n * 10^(e-f), ties up"""
    d = Decimal(x)
    e = d.adjusted()
    n = (d.scaleb(f - e)).quantize(Decimal(1), rounding=ROUND_HALF_UP)
    if n >= 10 ** (f + 1):
        e += 1
        n = (d.scaleb(f - e)).quantize(Decimal(1), rounding=ROUND_HALF_UP)
    return str(int(n)), e


def fmt_exp(ds, e):
    return ds[0] + ("." + ds[1:] if len(ds) > 1 else "") + "e" + ("+" if e >= 0 else "-") + str(abs(e))


def to_exponential(x, f):
    sign = "-" if x < 0 else ""
    x = abs(x)
    if x == 0:
        return sign + fmt_exp("0" * (1 if f < 0 else f + 1), 0)
    if f < 0:
        ds, n = shortest(x)
        return sign + fmt_exp(ds, n - 1)
    ds, e = exp_digits(x, f)
    return sign + fmt_exp(ds, e)


def to_precision(x, p):
    sign = "-" if x < 0 else ""
    x = abs(x)
    if x == 0:
        ds, e = "0" * p, 0
    else:
        ds, e = exp_digits(x, p - 1)
    if e < -6 or e >= p:
        return sign + fmt_exp(ds, e)
    if e == p - 1:
        return sign + ds
    if e >= 0:
        return sign + ds[:e + 1] + "." + ds[e + 1:]
    return sign + "0." + "0" * (-(e + 1)) + ds


def doubles(count, rnd):
    out = [5e-324, 1e-323, 2.2250738585072014e-308, 2.225073858507201e-308, 1.7976931348623157e308, 1e21, 1e21 - 65536,
           9007199254740992.0, 9007199254740993.0, 0.1, 0.2, 0.3, 1 / 3, 2 / 3, 123e-20, 1e-7, 1e-6, 123456789012345680000.0,
           0.5, 1.5, 2.5, 1.25, 1.005, 1.45, 8.345, 1.0000000000000002, 4.35, 0.000001, 2 ** -1074 * 3, 1e22, 1e23, 5e-7,
           299792458.0, 1.7976931348623155e308]
    for e in range(-1074, 1024, 7):
        out += [2.0 ** e, float.fromhex("0x1.fffffffffffffp%d" % e) if e < 1023 else 1.0]
    for k in range(-325, 309):
        v = float("1e%d" % k)
        if v and v != float("inf"):
            out += [v, from_bits(bits(v) + 1), from_bits(bits(v) - 1)]
    while len(out) < count:
        r = rnd.random()
        if r < 0.5:
            b = rnd.getrandbits(63)
            v = from_bits(b)
            if v != v or v in (float("inf"), float("-inf")):
                continue
        elif r < 0.7:
            v = rnd.randint(0, 10 ** rnd.randint(1, 8)) / 10 ** rnd.randint(0, 8)
        elif r < 0.85:
            v = rnd.uniform(0, 1000)
        else:
            v = float(rnd.randint(0, 2 ** 64)) * rnd.choice([1, 0.5, 0.25, 2 ** -60, 2 ** 40])
        out.append(v if rnd.random() < 0.8 else -v)
    return out


def decimals(count, rnd):
    out = ["0", "1", "0.1", "1e400", "1e-400", "0" * 500 + "1", "1" + "0" * 400, "0." + "0" * 500 + "1",
           "2.4703282292062327e-324", "2.4703282292062328e-324", "179769313486231580793728971405301e292",
           "9007199254740993", "9007199254740993.0000000000000000000000000001", "123456789012345678901234567890"]
    while len(out) < count:
        r = rnd.random()
        if r < 0.4:
            ds = str(rnd.randint(1, 10 ** rnd.randint(1, 25)))
            e = rnd.randint(-340, 320)
            out.append(ds + "e" + str(e))
        elif r < 0.6:
            ip = str(rnd.randint(0, 10 ** rnd.randint(0, 12)))
            fp = str(rnd.randint(0, 10 ** rnd.randint(0, 12)))
            out.append(ip + "." + fp)
        elif r < 0.9:  # near a halfway point between two doubles
            v = from_bits(rnd.getrandbits(62) + (1 << 61) if rnd.random() < 0.5 else rnd.getrandbits(63))
            if v != v or v == float("inf") or v == 0:
                continue
            w = from_bits(bits(v) + 1)
            if w == float("inf"):
                continue
            mid = (Decimal(v) + Decimal(w)) / 2
            s = format(mid, "e")
            m, e = s.split("e")
            out.append(m + "e" + e)
            ulp = Decimal(1).scaleb(-(len(m.replace(".", "").lstrip("0")) + 5))
            out.append(format(mid + (Decimal(w) - Decimal(v)) * ulp, "e"))
            out.append(format(mid - (Decimal(w) - Decimal(v)) * ulp, "e"))
        else:  # long inputs
            out.append(str(rnd.randint(1, 9)) + "".join(rnd.choice("0123456789") for _ in range(rnd.randint(700, 900))) +
                       "e" + str(rnd.randint(-1200, -300)))
    return [s.replace("E", "e").replace("+", "") for s in out]


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    rnd = random.Random(20260929)
    exe = "/tmp/dtoa_check"
    subprocess.check_call(["gcc", "-m32", "-msse2", "-mfpmath=sse", "-O2", "-std=gnu11", "-fno-strict-aliasing", "-Iinclude", "-Isrc",
                           "tools/dtoa_check.c", "src/px_dtoa.c", "-lm", "-o", exe], cwd=ROOT)
    cases = []
    for v in doubles(count, rnd):
        cases.append(("s %016x" % bits(v), js_to_string(v)))
        if abs(v) < 1e21:
            f = rnd.choice([0, 1, 2, 3, 5, 10, 20, rnd.randint(0, 100)])
            cases.append(("f %d %016x" % (f, bits(v)), to_fixed(v, f)))
        f = rnd.choice([-1, 0, 1, 2, 5, 16, 20, rnd.randint(0, 100)])
        cases.append(("e %d %016x" % (f, bits(v)), to_exponential(v, f)))
        p = rnd.choice([1, 2, 3, 6, 17, 21, rnd.randint(1, 100)])
        cases.append(("p %d %016x" % (p, bits(v)), to_precision(v, p)))
    for s in decimals(count // 2, rnd):
        cases.append(("d " + s, "%016x" % bits(float(s))))
    inp = "\n".join(c[0] for c in cases) + "\n"
    got = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()
    bad = [(c, g) for c, g in zip(cases, got) if c[1] != g]
    if len(got) != len(cases):
        print(f"driver answered {len(got)} of {len(cases)}")
        return 1
    for (q, want), g in bad[:30]:
        print(f"{q}: want {want} got {g}")
    print(f"{len(cases)} cases, {len(bad)} differences")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
