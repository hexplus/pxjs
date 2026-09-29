/* Strings: Latin-1 or UTF-16 cells, ropes, interning, and the number <->
 * string conversions JavaScript defines.
 *
 * Most text an app handles on a PSP is Latin-1 (UI labels, JSON keys,
 * Western names), so a string costs one byte per character unless it has
 * to cost two. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define ROPE_MIN_LEN 32u
#define PX_STR_MAX_LEN (1u << 28)

int px_is_str(PxValue v) {
    PxType t;
    if (!px_is_ptr(v)) return 0;
    t = px_type_of(v);
    return t == PX_T_STRING || t == PX_T_ROPE;
}

uint32_t px_str_len(PxValue v) {
    return px_type_of(v) == PX_T_STRING ? ((PxString *)px_ptr(v))->len : ((PxRope *)px_ptr(v))->len;
}

static int str_wide(PxValue v) {
    return px_type_of(v) == PX_T_STRING ? ((PxString *)px_ptr(v))->wide : ((PxRope *)px_ptr(v))->wide;
}


static PxString *str_alloc(PxVM *vm, uint32_t len, int wide) {
    PxString *s;
    if (len > PX_STR_MAX_LEN) {
        px_throw_error(vm, PX_RANGE_ERROR, "string too long");
        return NULL;
    }
    s = (PxString *)px_alloc(vm, PX_T_STRING, sizeof(PxString) + (size_t)len * (wide ? 2u : 1u));
    if (!s) return NULL;
    s->len  = len;
    s->wide = (uint8_t)wide;
    return s;
}

PxValue px_str_new_l1(PxVM *vm, const uint8_t *src, uint32_t len) {
    PxString *s = str_alloc(vm, len, 0);
    if (!s) return PX_EXCEPTION;
    if (len && src) memcpy(px_str_l1(s), src, len); /* NULL src: the caller fills it */
    return px_from_ptr(s);
}

PxValue px_str_new_u16(PxVM *vm, const uint16_t *src, uint32_t len) {
    uint32_t  i;
    int       wide = 0;
    PxString *s;
    for (i = 0; i < len; i++)
        if (src[i] > 0xFF) {
            wide = 1;
            break;
        }
    s = str_alloc(vm, len, wide);
    if (!s) return PX_EXCEPTION;
    if (wide)
        memcpy(px_str_u16(s), src, (size_t)len * 2u);
    else
        for (i = 0; i < len; i++) px_str_l1(s)[i] = (uint8_t)src[i];
    return px_from_ptr(s);
}

/* One code point from UTF-8, U+FFFD for anything malformed (one byte
 * consumed, so loops always advance). */
static size_t utf8_next(const uint8_t *s, size_t len, uint32_t *cp) {
    uint32_t c = s[0], min;
    size_t   n, i;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) n = 2, c &= 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, c &= 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, c &= 0x07, min = 0x10000;
    else {
        *cp = 0xFFFD;
        return 1;
    }
    if (len < n) {
        *cp = 0xFFFD;
        return 1;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = c;
    return n;
}

PxValue px_str_from_utf8(PxVM *vm, const char *src, size_t len) {
    const uint8_t *s = (const uint8_t *)src;
    size_t         i = 0;
    uint32_t       units = 0, cp;
    int            wide = 0;
    PxString      *out;

    while (i < len) {
        i += utf8_next(s + i, len - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
        if (cp > 0xFF) wide = 1;
        if (units > PX_STR_MAX_LEN) return px_throw_error(vm, PX_RANGE_ERROR, "string too long");
    }
    out = str_alloc(vm, units, wide);
    if (!out) return PX_EXCEPTION;
    for (i = 0, units = 0; i < len;) {
        i += utf8_next(s + i, len - i, &cp);
        if (!wide) {
            px_str_l1(out)[units++] = (uint8_t)cp;
        } else if (cp >= 0x10000) {
            cp -= 0x10000;
            px_str_u16(out)[units++] = (uint16_t)(0xD800 + (cp >> 10));
            px_str_u16(out)[units++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            px_str_u16(out)[units++] = (uint16_t)cp;
        }
    }
    return px_from_ptr(out);
}

PxValue px_str_from_cstr(PxVM *vm, const char *s) { return px_str_from_utf8(vm, s, strlen(s)); }

PxValue px_string(PxVM *vm, const char *utf8, size_t len) { return px_str_from_utf8(vm, utf8, len); }

/* ------------------------------------------------------------ ropes */

PxValue px_str_concat(PxVM *vm, PxValue a, PxValue b) {
    uint32_t la = px_str_len(a), lb = px_str_len(b);
    int      wide = str_wide(a) || str_wide(b);

    if (la == 0) return b;
    if (lb == 0) return a;
    if ((uint64_t)la + lb > PX_STR_MAX_LEN) return px_throw_error(vm, PX_RANGE_ERROR, "string too long");
    if (la + lb < ROPE_MIN_LEN && px_type_of(a) == PX_T_STRING && px_type_of(b) == PX_T_STRING) {
        PxString *out;
        uint32_t  i;
        PX_ROOT(vm, a);
        PX_ROOT(vm, b);
        out = str_alloc(vm, la + lb, wide);
        px_pop_roots(vm, 2);
        if (!out) return PX_EXCEPTION;
        {
            PxString *sa = (PxString *)px_ptr(a), *sb = (PxString *)px_ptr(b);
            if (!wide) {
                memcpy(px_str_l1(out), px_str_l1(sa), la);
                memcpy(px_str_l1(out) + la, px_str_l1(sb), lb);
            } else {
                for (i = 0; i < la; i++) px_str_u16(out)[i] = px_str_at(sa, i);
                for (i = 0; i < lb; i++) px_str_u16(out)[la + i] = px_str_at(sb, i);
            }
        }
        return px_from_ptr(out);
    }
    {
        PxRope  *r;
        uint32_t da = px_type_of(a) == PX_T_ROPE ? ((PxRope *)px_ptr(a))->depth : 0;
        uint32_t db = px_type_of(b) == PX_T_ROPE ? ((PxRope *)px_ptr(b))->depth : 0;
        PX_ROOT(vm, a);
        PX_ROOT(vm, b);
        r = (PxRope *)px_alloc(vm, PX_T_ROPE, sizeof(PxRope));
        px_pop_roots(vm, 2);
        if (!r) return PX_EXCEPTION;
        r->len   = la + lb;
        r->left  = a;
        r->right = b;
        r->depth = (da > db ? da : db) + 1;
        r->wide  = (uint8_t)wide;
        return px_from_ptr(r);
    }
}

PxString *px_str_flat(PxVM *vm, PxValue v) {
    PxRope   *root;
    PxString *out;
    PxValue  *stack;
    uint32_t  cap = 32, top = 0, pos;

    if (px_type_of(v) == PX_T_STRING) return (PxString *)px_ptr(v);
    root = (PxRope *)px_ptr(v);
    if (root->right == PX_UNDEFINED) return (PxString *)px_ptr(root->left); /* flattened before */

    PX_ROOT(vm, v);
    out = str_alloc(vm, root->len, root->wide);
    px_pop_roots(vm, 1);
    if (!out) return NULL;
    /* Filled from the end with an explicit stack: `s += x` builds ropes
     * thousands deep on the left, and C recursion over that would blow a
     * PSP thread stack. Pushing left before right keeps that shape at a
     * stack depth of two. */
    stack = (PxValue *)malloc(cap * sizeof(PxValue));
    if (!stack) {
        px_throw_oom(vm);
        return NULL;
    }
    stack[top++] = v;
    pos          = root->len;
    while (top > 0) {
        PxValue n = stack[--top];
        if (px_type_of(n) == PX_T_ROPE) {
            PxRope *r = (PxRope *)px_ptr(n);
            if (r->right == PX_UNDEFINED) {
                n = r->left;
            } else {
                if (top + 2 > cap) {
                    PxValue *grown;
                    cap *= 2;
                    grown = (PxValue *)realloc(stack, cap * sizeof(PxValue));
                    if (!grown) {
                        free(stack);
                        px_throw_oom(vm);
                        return NULL;
                    }
                    stack = grown;
                }
                stack[top++] = r->left;
                stack[top++] = r->right;
                continue;
            }
        }
        {
            PxString *s = (PxString *)px_ptr(n);
            uint32_t  i;
            pos -= s->len;
            if (out->wide)
                for (i = 0; i < s->len; i++) px_str_u16(out)[pos + i] = px_str_at(s, i);
            else
                memcpy(px_str_l1(out) + pos, px_str_l1(s), s->len);
        }
    }
    free(stack);
    /* The rope becomes a forwarder to its flat copy, and its children can
     * be collected. */
    root        = (PxRope *)px_ptr(v);
    root->left  = px_from_ptr(out);
    root->right = PX_UNDEFINED;
    return out;
}

/* ------------------------------------------------------------ comparison */

static uint32_t str_hash(const PxString *s) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i < s->len; i++) {
        h ^= px_str_at(s, i);
        h *= 16777619u;
    }
    return h ? h : 1;
}

static int flat_eq(const PxString *a, const PxString *b) {
    uint32_t i;
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    if (a->hash && b->hash && a->hash != b->hash) return 0;
    if (!a->wide && !b->wide) return memcmp(px_str_l1(a), px_str_l1(b), a->len) == 0;
    for (i = 0; i < a->len; i++)
        if (px_str_at(a, i) != px_str_at(b, i)) return 0;
    return 1;
}

int px_str_eq(PxVM *vm, PxValue a, PxValue b) {
    PxString *fa, *fb;
    if (a == b) return 1;
    if (px_str_len(a) != px_str_len(b)) return 0;
    PX_ROOT(vm, a);
    PX_ROOT(vm, b);
    fa = px_str_flat(vm, a);
    fb = fa ? px_str_flat(vm, b) : NULL;
    px_pop_roots(vm, 2);
    if (!fa || !fb) return -1;
    if (fa->interned && fb->interned) return fa == fb;
    return flat_eq(fa, fb);
}

int px_str_cmp(PxVM *vm, PxValue a, PxValue b, int *out) {
    PxString *fa, *fb;
    uint32_t  i, n;
    PX_ROOT(vm, a);
    PX_ROOT(vm, b);
    fa = px_str_flat(vm, a);
    fb = fa ? px_str_flat(vm, b) : NULL;
    px_pop_roots(vm, 2);
    if (!fa || !fb) return -1;
    n = fa->len < fb->len ? fa->len : fb->len;
    for (i = 0; i < n; i++) {
        uint16_t x = px_str_at(fa, i), y = px_str_at(fb, i);
        if (x != y) {
            *out = x < y ? -1 : 1;
            return 0;
        }
    }
    *out = fa->len < fb->len ? -1 : fa->len > fb->len ? 1 : 0;
    return 0;
}

PxValue px_str_slice(PxVM *vm, PxValue v, uint32_t start, uint32_t end) {
    PxString *s, *out;
    uint32_t  i, len;
    int       wide = 0;
    if (start >= end) return vm->atom[PX_ATOM_empty];
    PX_ROOT(vm, v);
    s = px_str_flat(vm, v);
    if (!s) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    if (start == 0 && end == s->len) {
        px_pop_roots(vm, 1);
        return px_from_ptr(s);
    }
    len = end - start;
    if (s->wide)
        for (i = start; i < end; i++)
            if (px_str_u16(s)[i] > 0xFF) {
                wide = 1;
                break;
            }
    out = str_alloc(vm, len, wide);
    s   = px_str_flat(vm, v); /* still flat; re-read after the allocation */
    px_pop_roots(vm, 1);
    if (!out) return PX_EXCEPTION;
    for (i = 0; i < len; i++) {
        if (wide) px_str_u16(out)[i] = px_str_u16(s)[start + i];
        else px_str_l1(out)[i] = (uint8_t)px_str_at(s, start + i);
    }
    return px_from_ptr(out);
}

/* ------------------------------------------------------------ interning */

static int array_index(const PxString *s, uint32_t *out) {
    uint32_t i, v = 0;
    if (s->len == 0 || s->len > 10) return 0;
    if (s->len > 1 && px_str_at(s, 0) == '0') return 0;
    for (i = 0; i < s->len; i++) {
        uint16_t c = px_str_at(s, i);
        if (c < '0' || c > '9') return 0;
        if (v > (uint32_t)PX_SMI_MAX / 10u) return 0;
        v = v * 10u + (c - '0');
    }
    if (v > (uint32_t)PX_SMI_MAX) return 0;
    *out = v;
    return 1;
}

static int atoms_grow(PxVM *vm) {
    uint32_t   ncap = vm->atoms_cap ? vm->atoms_cap * 2 : 256, i;
    PxString **n    = (PxString **)calloc(ncap, sizeof(PxString *));
    if (!n) return -1;
    for (i = 0; i < vm->atoms_cap; i++) {
        PxString *s = vm->atoms[i];
        uint32_t  j;
        if (!s) continue;
        for (j = s->hash & (ncap - 1); n[j]; j = (j + 1) & (ncap - 1)) {}
        n[j] = s;
    }
    free(vm->atoms);
    vm->atoms     = n;
    vm->atoms_cap = ncap;
    return 0;
}

/* Interns a flat string as a string (never as an index): the unique copy
 * of its text. */
static PxValue intern_string(PxVM *vm, PxString *s) {
    uint32_t idx, j;
    if (s->interned) return px_from_ptr(s);
    if (!s->hash) s->hash = str_hash(s);
    s->is_index = (uint8_t)array_index(s, &idx);
    if ((vm->atoms_count + 1) * 2 > vm->atoms_cap && atoms_grow(vm) != 0) return px_throw_oom(vm);
    for (j = s->hash & (vm->atoms_cap - 1); vm->atoms[j]; j = (j + 1) & (vm->atoms_cap - 1))
        if (flat_eq(vm->atoms[j], s)) return px_from_ptr(vm->atoms[j]);
    s->interned  = 1;
    vm->atoms[j] = s;
    vm->atoms_count++;
    return px_from_ptr(s);
}

/* The key form of a value (ToPropertyKey): array-index strings become
 * small integers ("5" and 5 are the same property), other strings their
 * interned copy, symbols themselves; anything else goes through
 * ToPrimitive (hint string) and ToString first, which can run JS. */
PxValue px_intern(PxVM *vm, PxValue v) {
    PxString *s;
    uint32_t  idx;

    if (px_is_smi(v) && px_smi(v) >= 0) return v;
    if (!px_is_str(v)) {
        if (px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL) return v;
        if (px_is_obj(v)) {
            v = px_to_primitive(vm, v, 1);
            if (v == PX_EXCEPTION || (px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL)) return v;
        }
        v = px_to_string(vm, v);
        if (v == PX_EXCEPTION) return v;
    }
    s = px_str_flat(vm, v);
    if (!s) return PX_EXCEPTION;
    if (s->interned && !s->is_index) return px_from_ptr(s);
    if (array_index(s, &idx)) return px_from_smi((int32_t)idx);
    return intern_string(vm, s);
}

/* A string literal's constant: deduplicated, but always a string. */
PxValue px_intern_literal(PxVM *vm, PxValue v) {
    PxString *s = px_str_flat(vm, v);
    if (!s) return PX_EXCEPTION;
    return intern_string(vm, s);
}

PxValue px_intern_cstr(PxVM *vm, const char *cs) {
    PxValue v = px_str_from_cstr(vm, cs);
    if (v == PX_EXCEPTION) return v;
    return px_intern(vm, v);
}

PxValue px_intern_chars(PxVM *vm, const void *text, int wide, uint32_t len) {
    const uint8_t  *b8  = (const uint8_t *)text;
    const uint16_t *b16 = (const uint16_t *)text;
    uint32_t        h = 2166136261u, i, j, v = 0;
    PxValue         s;
#define CH(k) (wide ? b16[k] : b8[k])
    /* an array index is a SMI key (as array_index decides) */
    if (len > 0 && len <= 10 && !(len > 1 && CH(0) == '0')) {
        for (i = 0; i < len; i++) {
            uint32_t c = CH(i);
            if (c < '0' || c > '9' || v > (uint32_t)PX_SMI_MAX / 10u) break;
            v = v * 10u + (c - '0');
        }
        if (i == len && v <= (uint32_t)PX_SMI_MAX) return px_from_smi((int32_t)v);
    }
    for (i = 0; i < len; i++) { /* str_hash */
        h ^= CH(i);
        h *= 16777619u;
    }
    if (!h) h = 1;
    if (vm->atoms_cap)
        for (j = h & (vm->atoms_cap - 1); vm->atoms[j]; j = (j + 1) & (vm->atoms_cap - 1)) {
            PxString *a = vm->atoms[j];
            if (a->hash != h || a->len != len) continue;
            if (!a->wide && !wide) {
                if (memcmp(px_str_l1(a), b8, len) == 0) return px_from_ptr(a);
            } else {
                for (i = 0; i < len && px_str_at(a, i) == CH(i); i++) {}
                if (i == len) return px_from_ptr(a);
            }
        }
#undef CH
    s = wide ? px_str_new_u16(vm, b16, len) : px_str_new_l1(vm, b8, len);
    if (s == PX_EXCEPTION) return s;
    ((PxString *)px_ptr(s))->hash = h;
    return intern_string(vm, (PxString *)px_ptr(s));
}

/* The table does not keep strings alive: a name no code or object uses
 * any more is dropped from it. Called during GC, after marking. */
void px_atoms_sweep(PxVM *vm) {
    PxString **old = vm->atoms;
    uint32_t   cap = vm->atoms_cap, i;
    if (!old) return;
    vm->atoms = (PxString **)calloc(cap, sizeof(PxString *));
    if (!vm->atoms) {
        /* Cannot rebuild: keep the old table and keep every entry alive by
         * marking it -- too late for this cycle, so the entries of dead
         * strings would dangle. Prevent that by not sweeping them: */
        vm->atoms = old;
        for (i = 0; i < cap; i++)
            if (old[i]) old[i]->hdr |= PX_HDR_MARK;
        return;
    }
    vm->atoms_count = 0;
    for (i = 0; i < cap; i++) {
        PxString *s = old[i];
        uint32_t  j;
        if (!s || !(s->hdr & PX_HDR_MARK)) continue;
        for (j = s->hash & (cap - 1); vm->atoms[j]; j = (j + 1) & (cap - 1)) {}
        vm->atoms[j] = s;
        vm->atoms_count++;
    }
    free(old);
}

/* ------------------------------------------------------------ UTF-8 out */

size_t px_str_to_utf8(PxVM *vm, PxValue v, char *dst, size_t cap) {
    PxString *s = px_str_flat(vm, v);
    size_t    need = 0, wrote = 0;
    int       full = 0;
    uint32_t  i;
    if (!s) {
        if (cap) dst[0] = '\0';
        return 0;
    }
    for (i = 0; i < s->len; i++) {
        uint32_t c = px_str_at(s, i);
        uint8_t  buf[4];
        size_t   n;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
            uint32_t d = px_str_at(s, i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD; /* lone surrogate */
        if (c < 0x80) {
            buf[0] = (uint8_t)c;
            n      = 1;
        } else if (c < 0x800) {
            buf[0] = (uint8_t)(0xC0 | (c >> 6));
            buf[1] = (uint8_t)(0x80 | (c & 0x3F));
            n      = 2;
        } else if (c < 0x10000) {
            buf[0] = (uint8_t)(0xE0 | (c >> 12));
            buf[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            buf[2] = (uint8_t)(0x80 | (c & 0x3F));
            n      = 3;
        } else {
            buf[0] = (uint8_t)(0xF0 | (c >> 18));
            buf[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            buf[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            buf[3] = (uint8_t)(0x80 | (c & 0x3F));
            n      = 4;
        }
        /* Whole characters only: once one does not fit, stop writing. */
        if (!full && cap && wrote + n <= cap - 1) {
            memcpy(dst + wrote, buf, n);
            wrote += n;
        } else {
            full = 1;
        }
        need += n;
    }
    if (cap) dst[wrote] = '\0';
    return need;
}

/* ------------------------------------------------------------ numbers */

/* Decimal digits of an int32 into buf (at least 12 bytes); returns the
 * length. 32-bit division by a constant: a multiply on MIPS, where
 * printf("%lld") would divide 64-bit numbers in software per digit. */
int px_itoa(int32_t v, char *buf) {
    char     tmp[12];
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    int      n = 0, o = 0;
    do {
        tmp[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (v < 0) buf[o++] = '-';
    while (n) buf[o++] = tmp[--n];
    buf[o] = '\0';
    return o;
}

PxValue px_int_to_string(PxVM *vm, int32_t v) {
    char buf[12];
    int  n = px_itoa(v, buf);
    return px_str_new_l1(vm, (const uint8_t *)buf, (uint32_t)n);
}

PxValue px_number_to_string(PxVM *vm, double d, int radix) {
    char    buf[80];
    int32_t i;
    if (radix == 10 && px_dbl_to_smi(d, &i)) return px_int_to_string(vm, i);
    if (isnan(d)) return px_str_from_cstr(vm, "NaN");
    if (isinf(d)) return px_str_from_cstr(vm, d < 0 ? "-Infinity" : "Infinity");
    if (d == 0) return px_str_from_cstr(vm, "0");
    if (radix == 10) {
        px_fmt_number(d, buf, sizeof buf);
        return px_str_from_cstr(vm, buf);
    }
    {
        /* Other radices: exact integer part, up to 20 fractional digits. */
        static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        char   rev[80];
        int    n = 0, o = 0, i;
        double ip = floor(fabs(d)), fp = fabs(d) - ip;
        if (d < 0) buf[o++] = '-';
        if (ip == 0) rev[n++] = '0';
        while (ip >= 1 && n < 70) {
            double q = floor(ip / radix);
            rev[n++] = dig[(int)(ip - q * radix)];
            ip       = q;
        }
        for (i = n - 1; i >= 0; i--) buf[o++] = rev[i];
        if (fp > 0) {
            buf[o++] = '.';
            for (i = 0; i < 20 && fp > 0; i++) {
                int dgt;
                fp *= radix;
                dgt = (int)fp;
                buf[o++] = dig[dgt];
                fp -= dgt;
            }
        }
        buf[o] = '\0';
        return px_str_from_cstr(vm, buf);
    }
}

static int js_space(uint32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}

/* StringToNumber: whitespace, then Infinity, a hex/octal/binary integer or
 * a decimal literal, then whitespace -- and nothing else. */
double px_string_to_number(PxVM *vm, PxValue v) {
    PxString   *s = px_str_flat(vm, v);
    const void *text;
    uint32_t    a = 0, b, p, n;
    if (!s) return NAN;
    b = s->len;
    while (a < b && js_space(px_str_at(s, a))) a++;
    while (b > a && js_space(px_str_at(s, b - 1))) b--;
    if (a == b) return 0;
    n    = b - a;
    text = s->wide ? (const void *)(px_str_u16(s) + a) : (const void *)(px_str_l1(s) + a);
#define C(i) px_str_at(s, a + (i))
#define DIGIT(c) ((c) >= '0' && (c) <= '9')
    p = C(0) == '+' || C(0) == '-';
    if (n - p == 8) {
        static const char inf[] = "Infinity";
        uint32_t          i;
        for (i = 0; i < 8 && C(p + i) == (uint16_t)inf[i]; i++) {}
        if (i == 8) return C(0) == '-' ? -INFINITY : INFINITY;
    }
    if (n > 2 && C(0) == '0') {
        uint16_t x     = C(1) | 0x20;
        int      shift = x == 'x' ? 4 : x == 'o' ? 3 : x == 'b' ? 1 : 0;
        if (shift) {
            for (p = 2; p < n; p++) {
                uint16_t c = C(p), l = c | 0x20;
                int      dv = DIGIT(c) ? c - '0' : (l >= 'a' && l <= 'f') ? l - 'a' + 10 : 99;
                if (dv >= 1 << shift) return NAN;
            }
            return px_radix2_to_double(s->wide ? (const void *)((const uint16_t *)text + 2)
                                               : (const void *)((const uint8_t *)text + 2),
                                       s->wide, n - 2, shift);
        }
    }
    /* [+-] digits [. digits] [(e|E) [+-] digits], with at least one digit
     * in the mantissa */
    {
        uint32_t mant = 0;
        while (p < n && DIGIT(C(p))) p++, mant++;
        if (p < n && C(p) == '.') {
            p++;
            while (p < n && DIGIT(C(p))) p++, mant++;
        }
        if (!mant) return NAN;
        if (p < n && (C(p) == 'e' || C(p) == 'E')) {
            uint32_t ed = 0;
            p++;
            if (p < n && (C(p) == '+' || C(p) == '-')) p++;
            while (p < n && DIGIT(C(p))) p++, ed++;
            if (!ed) return NAN;
        }
        if (p != n) return NAN;
    }
#undef C
#undef DIGIT
    return px_decimal_to_double(text, s->wide, n);
}
