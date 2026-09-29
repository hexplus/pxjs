/* Number <-> decimal text, exactly, without printf.
 *
 * Doubles are software-emulated on the PSP, and libc's printf/strtod pair
 * was the slow part of every number -> string conversion (the shortest
 * round-trip digits were found by printing at precision 1..17 and reading
 * each back). Here:
 *
 *   - shortest digits: Burger & Dybvig's free-format algorithm on exact
 *     big integers (Printing Floating-Point Numbers Quickly and Accurately,
 *     PLDI 1996), which gives the shortest digits that read back as the
 *     same double, the closest such digits, and an even digit on a tie:
 *     what Number::toString asks for;
 *   - fixed precision (toFixed, toExponential, toPrecision): exact digits,
 *     rounded half up (ECMA-262: "if there are two such n, pick the larger
 *     n"), where printf rounds a tie to even;
 *   - decimal -> double: exact integer arithmetic for short inputs, libc's
 *     strtod (correctly rounded in glibc and newlib) for the rest, with
 *     inputs of any length reduced to 800 significant digits plus a sticky
 *     digit, which decides the rounding the same way.
 *
 * The output does not depend on the C library, so it is the same on the
 * host and on the PSP. The big integers need 40 words at most: the extremes
 * are 2^1076 * 10^324 (denormals), 2^1026 (the largest doubles) and
 * (2^54 + 1) * 10^343 (parsing). */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "px_dtoa.h"

/* ------------------------------------------------------------ big integers */

#define BIG_WORDS 44

typedef struct Big {
    int      n; /* words in use; w[n - 1] != 0 unless n == 0 */
    uint32_t w[BIG_WORDS];
} Big;

static void big_set(Big *a, uint64_t v) {
    a->n = 0;
    while (v) {
        a->w[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void big_mul_small(Big *a, uint32_t m) {
    uint64_t c = 0;
    int      i;
    for (i = 0; i < a->n; i++) {
        c += (uint64_t)a->w[i] * m;
        a->w[i] = (uint32_t)c;
        c >>= 32;
    }
    if (c) a->w[a->n++] = (uint32_t)c;
}

static void big_mul_pow10(Big *a, int k) {
    static const uint32_t p10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    for (; k >= 9; k -= 9) big_mul_small(a, 1000000000u);
    if (k) big_mul_small(a, p10[k]);
}

static void big_shl(Big *a, int bits) {
    int words = bits >> 5, s = bits & 31, i;
    if (!a->n) return;
    if (s) {
        uint32_t carry = 0;
        for (i = 0; i < a->n; i++) {
            uint32_t w = a->w[i];
            a->w[i]    = (w << s) | carry;
            carry      = w >> (32 - s);
        }
        if (carry) a->w[a->n++] = carry;
    }
    if (words) {
        memmove(a->w + words, a->w, (size_t)a->n * sizeof a->w[0]);
        memset(a->w, 0, (size_t)words * sizeof a->w[0]);
        a->n += words;
    }
}

static int big_cmp(const Big *a, const Big *b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* compare a + b with c */
static int big_cmp_sum(const Big *a, const Big *b, const Big *c) {
    Big      t;
    uint64_t carry = 0;
    int      i, n = a->n > b->n ? a->n : b->n;
    for (i = 0; i < n; i++) {
        carry += (uint64_t)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0);
        t.w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    t.n = n;
    if (carry) t.w[t.n++] = (uint32_t)carry;
    return big_cmp(&t, c);
}

/* a -= b, a >= b */
static void big_sub(Big *a, const Big *b) {
    int64_t borrow = 0;
    int     i;
    for (i = 0; i < a->n; i++) {
        borrow += (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0);
        a->w[i] = (uint32_t)borrow;
        borrow >>= 32; /* 0 or -1 */
    }
    while (a->n && !a->w[a->n - 1]) a->n--;
}

/* r / s, a digit (r < 10 s); r becomes the remainder */
static int big_digit(Big *r, const Big *s) {
    int d = 0;
    while (big_cmp(r, s) >= 0) {
        big_sub(r, s);
        d++;
    }
    return d;
}

/* Small enough for the uint64_t fast paths: below 2^60, so ten times it
 * (and the sums compared with it) still fit. Numbers from 1 to 2^53 get
 * there: their scaled denominator is below 2^58. */
static int big_small(const Big *a) { return a->n <= 1 || (a->n == 2 && a->w[1] < (1u << 28)); }

static uint64_t big_u64(const Big *a) {
    return a->n == 0 ? 0 : a->n == 1 ? a->w[0] : ((uint64_t)a->w[1] << 32) | a->w[0];
}

/* ------------------------------------------------------------ digits */

static void decompose(double v, uint64_t *f, int *e) {
    uint64_t bits;
    int      be;
    memcpy(&bits, &v, sizeof bits);
    *f = bits & ((1ull << 52) - 1);
    be = (int)((bits >> 52) & 0x7ff);
    if (be) {
        *f |= 1ull << 52;
        *e = be - 1075;
    } else {
        *e = -1074;
    }
}

static int bit_length(uint64_t f) {
    /* clz is one instruction on MIPS32 (and x86); f > 0 */
    uint32_t hi = (uint32_t)(f >> 32);
    return hi ? 64 - __builtin_clz(hi) : 32 - __builtin_clz((uint32_t)f);
}

/* An estimate of ceil(log10(v)) for v = f * 2^e, never too large and at
 * most one too small: ceil(x * log10(2)) for x = floor(log2(v)), in integer
 * arithmetic ((x * 78913) >> 18 is floor(x * log10(2)) for 0 <= x <= 1650,
 * and x * log10(2) is never an integer but for x = 0). Doubles are software
 * routines on the PSP. */
static int estimate_k(uint64_t f, int e) {
    int x = e + bit_length(f) - 1;
    if (x > 0) return (int)(((uint32_t)x * 78913u) >> 18) + 1;
    if (x == 0) return 0;
    return -(int)(((uint32_t)-x * 78913u) >> 18);
}

int px_dtoa_shortest(double v, char *digits, int *kout) {
    Big      r, s, mp, mm;
    uint64_t f;
    int      e, k, n = 0, even;

    decompose(v, &f, &e);
    even = !(f & 1);
    if (e >= 0) {
        big_set(&r, f);
        big_set(&mm, 1);
        big_shl(&mm, e);
        if (f != 1ull << 52) {
            big_shl(&r, e + 1);
            big_set(&s, 2);
            mp = mm;
        } else {
            big_shl(&r, e + 2);
            big_set(&s, 4);
            mp = mm;
            big_shl(&mp, 1);
        }
    } else {
        big_set(&s, 1);
        big_set(&mm, 1);
        if (e == -1074 || f != 1ull << 52) {
            big_set(&r, f << 1);
            big_shl(&s, 1 - e);
            big_set(&mp, 1);
        } else {
            big_set(&r, f << 2);
            big_shl(&s, 2 - e);
            big_set(&mp, 2);
        }
    }
    k = estimate_k(f, e);
    if (k >= 0) {
        big_mul_pow10(&s, k);
    } else {
        big_mul_pow10(&r, -k);
        big_mul_pow10(&mp, -k);
        big_mul_pow10(&mm, -k);
    }
    {
        int c = big_cmp_sum(&r, &mp, &s);
        if (even ? c >= 0 : c > 0) {
            k++;
        } else {
            big_mul_small(&r, 10);
            big_mul_small(&mp, 10);
            big_mul_small(&mm, 10);
        }
    }
    if (big_small(&s)) { /* the same loop in 64-bit integers */
        uint64_t R = big_u64(&r), S = big_u64(&s), P = big_u64(&mp), M = big_u64(&mm);
        for (;;) {
            int d = 0, low, high;
            while (R >= S) {
                R -= S;
                d++;
            }
            low  = even ? R <= M : R < M;
            high = even ? R + P >= S : R + P > S;
            if (!low && !high) {
                digits[n++] = (char)('0' + d);
                R *= 10;
                P *= 10;
                M *= 10;
                continue;
            }
            if (low && high) {
                if (2 * R > S || (2 * R == S && (d & 1))) d++;
            } else if (high) {
                d++;
            }
            digits[n++] = (char)('0' + d);
            *kout = k;
            return n;
        }
    }
    for (;;) {
        int d = big_digit(&r, &s), c1, c2, low, high;
        c1   = big_cmp(&r, &mm);
        c2   = big_cmp_sum(&r, &mp, &s);
        low  = even ? c1 <= 0 : c1 < 0;
        high = even ? c2 >= 0 : c2 > 0;
        if (!low && !high) {
            digits[n++] = (char)('0' + d);
            big_mul_small(&r, 10);
            big_mul_small(&mp, 10);
            big_mul_small(&mm, 10);
            continue;
        }
        if (low && high) {
            int c = big_cmp_sum(&r, &r, &s); /* 2r vs s */
            if (c > 0 || (c == 0 && (d & 1))) d++;
        } else if (high) {
            d++;
        }
        digits[n++] = (char)('0' + d);
        break;
    }
    *kout = k;
    return n;
}

/* The digits of v > 0 rounded half up to `ndigits` digits, or, when frac
 * >= 0, to `frac` digits after the decimal point. v = 0.d1d2... * 10^k.
 * Returns the number of digits written (0 when v rounds to 0 at frac). */
int px_dtoa_exact(double v, int ndigits, int frac, char *digits, int *kout) {
    Big      r, s;
    uint64_t f;
    int      e, k, n, i, up;

    decompose(v, &f, &e);
    big_set(&r, f);
    big_set(&s, 1);
    if (e >= 0) big_shl(&r, e);
    else big_shl(&s, -e);
    k = estimate_k(f, e);
    if (k >= 0) big_mul_pow10(&s, k);
    else big_mul_pow10(&r, -k);
    if (big_cmp(&r, &s) >= 0) {
        big_mul_small(&s, 10);
        k++;
    }
    /* r / s = v / 10^k, in [0.1, 1) */
    if (frac >= 0) ndigits = k + frac;
    if (ndigits < 0) {
        *kout = k;
        return 0;
    }
    if (big_small(&s)) {
        uint64_t R = big_u64(&r), S = big_u64(&s);
        for (n = 0; n < ndigits; n++) {
            int d = 0;
            R *= 10;
            while (R >= S) {
                R -= S;
                d++;
            }
            digits[n] = (char)('0' + d);
        }
        up = 2 * R >= S;
    } else {
        for (n = 0; n < ndigits; n++) {
            big_mul_small(&r, 10);
            digits[n] = (char)('0' + big_digit(&r, &s));
        }
        /* the rest, r / s, decides: half or more rounds up */
        up = big_cmp_sum(&r, &r, &s) >= 0;
    }
    if (up) {
        for (i = n - 1; i >= 0 && digits[i] == '9'; i--) digits[i] = '0';
        if (i >= 0) {
            digits[i]++;
        } else { /* 99.9 -> 100: one digit more in front */
            if (n) digits[0] = '1';
            else digits[n++] = '1';
            k++;
        }
    }
    *kout = k;
    return n;
}

/* ------------------------------------------------------------ formatting */

typedef struct Out {
    char  *p;
    size_t o, cap;
} Out;

static void put(Out *w, char c) {
    if (w->o + 1 < w->cap) w->p[w->o++] = c;
}

static void put_exp(Out *w, int e) {
    char tmp[8];
    int  n = 0;
    put(w, 'e');
    put(w, e < 0 ? '-' : '+');
    if (e < 0) e = -e;
    do {
        tmp[n++] = (char)('0' + e % 10);
        e /= 10;
    } while (e);
    while (n) put(w, tmp[--n]);
}

static size_t finish(Out *w) {
    if (w->cap) w->p[w->o] = '\0';
    return w->o;
}

/* Number::toString(d) for finite d (ECMA-262 6.1.6.1.20) */
size_t px_fmt_number(double d, char *out, size_t cap) {
    Out  w = {out, 0, cap};
    char dg[20];
    int  k, n, i;
    if (d == 0) {
        put(&w, '0');
        return finish(&w);
    }
    if (d < 0) {
        put(&w, '-');
        d = -d;
    }
    k = px_dtoa_shortest(d, dg, &n);
    /* k digits dg, value 0.dg * 10^n */
    if (k <= n && n <= 21) {
        for (i = 0; i < k; i++) put(&w, dg[i]);
        for (i = k; i < n; i++) put(&w, '0');
    } else if (0 < n && n <= 21) {
        for (i = 0; i < n; i++) put(&w, dg[i]);
        put(&w, '.');
        for (i = n; i < k; i++) put(&w, dg[i]);
    } else if (-6 < n && n <= 0) {
        put(&w, '0');
        put(&w, '.');
        for (i = 0; i < -n; i++) put(&w, '0');
        for (i = 0; i < k; i++) put(&w, dg[i]);
    } else {
        put(&w, dg[0]);
        if (k > 1) {
            put(&w, '.');
            for (i = 1; i < k; i++) put(&w, dg[i]);
        }
        put_exp(&w, n - 1);
    }
    return finish(&w);
}

/* Number.prototype.toFixed: finite d, |d| < 1e21, 0 <= f <= 100 */
size_t px_fmt_fixed(double d, int f, char *out, size_t cap) {
    Out  w = {out, 0, cap};
    char dg[140];
    int  k = 0, n = 0, i;
    if (d < 0) {
        put(&w, '-');
        d = -d;
    }
    if (d != 0) n = px_dtoa_exact(d, 0, f, dg, &k);
    /* digit i has the weight 10^(k - 1 - i) */
#define DIGIT(i) ((i) >= 0 && (i) < n ? dg[i] : '0')
    if (k <= 0) put(&w, '0');
    for (i = 0; i < k; i++) put(&w, DIGIT(i));
    if (f > 0) {
        put(&w, '.');
        for (i = 0; i < f; i++) put(&w, DIGIT(k + i));
    }
#undef DIGIT
    return finish(&w);
}

/* Number.prototype.toExponential: finite d; f < 0 when fractionDigits is
 * undefined (as many digits as needed) */
size_t px_fmt_exponential(double d, int f, char *out, size_t cap) {
    Out  w = {out, 0, cap};
    char dg[140];
    int  k = 1, n, i;
    if (d < 0) {
        put(&w, '-');
        d = -d;
    }
    if (d == 0) {
        n = f < 0 ? 1 : f + 1;
        memset(dg, '0', (size_t)n);
    } else if (f < 0) {
        n = px_dtoa_shortest(d, dg, &k);
    } else {
        n = px_dtoa_exact(d, f + 1, -1, dg, &k);
    }
    put(&w, dg[0]);
    if (n > 1) {
        put(&w, '.');
        for (i = 1; i < n; i++) put(&w, dg[i]);
    }
    put_exp(&w, k - 1);
    return finish(&w);
}

/* Number.prototype.toPrecision: finite d, 1 <= p <= 100 */
size_t px_fmt_precision(double d, int p, char *out, size_t cap) {
    Out  w = {out, 0, cap};
    char dg[140];
    int  k = 1, e, i;
    if (d < 0) {
        put(&w, '-');
        d = -d;
    }
    if (d == 0) memset(dg, '0', (size_t)p);
    else px_dtoa_exact(d, p, -1, dg, &k);
    e = k - 1;
    if (e < -6 || e >= p) {
        put(&w, dg[0]);
        if (p > 1) {
            put(&w, '.');
            for (i = 1; i < p; i++) put(&w, dg[i]);
        }
        put_exp(&w, e);
    } else if (e >= 0) {
        for (i = 0; i <= e; i++) put(&w, dg[i]);
        if (e + 1 < p) {
            put(&w, '.');
            for (i = e + 1; i < p; i++) put(&w, dg[i]);
        }
    } else {
        put(&w, '0');
        put(&w, '.');
        for (i = 0; i < -e - 1; i++) put(&w, '0');
        for (i = 0; i < p; i++) put(&w, dg[i]);
    }
    return finish(&w);
}

/* ------------------------------------------------------------ parsing */

#define DEC_MAX_DIGITS 800

static const double p10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                             1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

/* m * 10^s compared with n * 2^b, exactly */
static int cmp_dec_bin(uint64_t m, int s, uint64_t n, int b) {
    Big l, r;
    big_set(&l, m);
    big_set(&r, n);
    if (s >= 0) big_mul_pow10(&l, s);
    else big_mul_pow10(&r, -s);
    if (b >= 0) big_shl(&r, b);
    else big_shl(&l, -b);
    return big_cmp(&l, &r);
}

/* m * 10^s correctly rounded, for m > 0 of up to 19 digits and a result
 * between 1e-324 and 1e310: a double approximation within a few units in
 * the last place, then moved one unit at a time until the exact value lies
 * between the midpoints to its neighbours (a tie goes to the even one). */
static double refine(uint64_t m, int s) {
    double   z = (double)m;
    int      t = s;
    uint64_t bits;
    while (t > 22) z *= 1e22, t -= 22;
    while (t < -22) z /= 1e22, t += 22;
    z = t >= 0 ? z * p10[t] : z / p10[-t];
    if (z == 0) z = 4.9406564584124654e-324;
    if (isinf(z)) z = 1.7976931348623157e308;
    for (;;) {
        uint64_t f;
        int      e, c;
        decompose(z, &f, &e);
        c = cmp_dec_bin(m, s, 2 * f + 1, e - 1); /* the midpoint above */
        memcpy(&bits, &z, sizeof bits);
        if (c > 0 || (c == 0 && (f & 1))) {
            bits++;
            memcpy(&z, &bits, sizeof z);
            if (isinf(z)) return z;
            continue;
        }
        if (f == 1ull << 52 && e > -1074) c = cmp_dec_bin(m, s, 4 * f - 1, e - 2); /* the gap below is half */
        else c = cmp_dec_bin(m, s, 2 * f - 1, e - 1);
        if (c < 0 || (c == 0 && (f & 1))) {
            bits--;
            memcpy(&z, &bits, sizeof z);
            if (z == 0) return z;
            continue;
        }
        return z;
    }
}

/* A decimal literal [+-] digits [. digits] [(e|E) [+-] digits], already
 * checked by the caller, as 8-bit (wide == 0) or 16-bit characters. Numeric
 * separators ('_') are skipped: the lexer has checked where they stand. */
double px_decimal_to_double(const void *text, int wide, size_t len) {
    char     buf[DEC_MAX_DIGITS + 32];
    size_t   i = 0;
    int      neg = 0, nd = 0, sticky = 0, seen_point = 0;
    long     exp10 = 0, e = 0; /* value = 0.digits * 10^exp10 * 10^e */
    uint64_t m = 0;
#define AT(j) (wide ? ((const uint16_t *)text)[j] : ((const uint8_t *)text)[j])

    if (i < len && (AT(i) == '+' || AT(i) == '-')) neg = AT(i++) == '-';
    for (; i < len; i++) {
        unsigned c = AT(i);
        if (c == '_') continue;
        if (c == '.') {
            seen_point = 1;
            continue;
        }
        if (c < '0' || c > '9') break;
        if (nd == 0 && c == '0') { /* leading zeros */
            if (seen_point) exp10--;
            continue;
        }
        if (nd < DEC_MAX_DIGITS) {
            buf[nd++] = (char)c;
            if (nd <= 19) m = m * 10 + (c - '0');
        } else if (c != '0') {
            sticky = 1;
        }
        if (!seen_point) exp10++;
    }
    if (i < len && (AT(i) == 'e' || AT(i) == 'E')) {
        int eneg = 0;
        i++;
        if (i < len && (AT(i) == '+' || AT(i) == '-')) eneg = AT(i++) == '-';
        for (; i < len && ((AT(i) >= '0' && AT(i) <= '9') || AT(i) == '_'); i++)
            if (AT(i) != '_' && e < 100000) e = e * 10 + (AT(i) - '0');
        if (eneg) e = -e;
    }
#undef AT
    if (nd == 0) return neg ? -0.0 : 0.0;
    exp10 += e;
    /* the digits as an integer: m * 10^(exp10 - nd) */
#if FLT_EVAL_METHOD == 0 /* one IEEE double operation: not on x87, which would round twice */
    /* m < 2^53 is exact as a double: one correctly rounded operation */
    if (nd <= 16 && !sticky && m <= 9007199254740992ull) {
        long   s = exp10 - nd;
        double v = (double)m;
        if (s >= 0 && s <= 22) return neg ? -(v * p10[s]) : v * p10[s];
        if (s < 0 && s >= -22) return neg ? -(v / p10[-s]) : v / p10[-s];
    }
#endif
    if (nd <= 19 && !sticky) {
        if (exp10 <= -324) return neg ? -0.0 : 0.0; /* below 1e-324: half the smallest denormal */
        if (exp10 >= 310) return neg ? -INFINITY : INFINITY;
        return neg ? -refine(m, (int)(exp10 - nd)) : refine(m, (int)(exp10 - nd));
    }
    if (exp10 > 400) return neg ? -INFINITY : INFINITY;
    if (exp10 < -400) return neg ? -0.0 : 0.0;
    {
        char *p = buf + nd, tmp[16];
        int   t = 0;
        long  x = exp10 - nd;
        if (sticky) *p++ = '1', x--;
        *p++ = 'e';
        if (x < 0) *p++ = '-', x = -x;
        do {
            tmp[t++] = (char)('0' + x % 10);
            x /= 10;
        } while (x);
        while (t) *p++ = tmp[--t];
        *p = '\0';
    }
    return neg ? -strtod(buf, NULL) : strtod(buf, NULL);
}

/* Hex, octal or binary digits (1 << shift is the radix; checked by the
 * caller, '_' skipped), correctly rounded: the first 61+ significant bits
 * are kept, the rest only as a sticky bit. */
double px_radix2_to_double(const void *text, int wide, size_t len, int shift) {
    uint64_t m = 0;
    int      e = 0, sticky = 0;
    size_t   i;
    for (i = 0; i < len; i++) {
        unsigned c = wide ? ((const uint16_t *)text)[i] : ((const uint8_t *)text)[i], h;
        if (c == '_') continue;
        h = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        if (m >> (64 - shift)) {
            e += shift;
            sticky |= h != 0;
        } else {
            m = (m << shift) | h;
        }
    }
    if (sticky) m |= 1; /* below the rounding position: breaks a tie upwards */
    return e ? ldexp((double)m, e) : (double)m;
}
