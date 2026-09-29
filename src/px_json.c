/* JSON.parse, JSON.stringify, and px_inspect (the console.log format).
 *
 * All three treat their input as untrusted: nesting depth is bounded (the
 * C stack on a PSP thread is small), cycles are detected, and output is
 * bounded by the heap limit rather than growing without check. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define JSON_MAX_DEPTH 256

/* ============================================================ UTF-8 output */

typedef struct Out {
    char  *d;
    size_t n, cap, limit;
    int    oom, full;
} Out;

static void out_bytes(Out *o, const char *s, size_t n) {
    if (o->oom || o->full) return;
    if (o->limit && o->n + n > o->limit) {
        n       = o->limit > o->n ? o->limit - o->n : 0;
        o->full = 1;
    }
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        char  *nd;
        while (cap < o->n + n + 1) cap *= 2;
        nd = (char *)realloc(o->d, cap);
        if (!nd) {
            o->oom = 1;
            return;
        }
        o->d   = nd;
        o->cap = cap;
    }
    memcpy(o->d + o->n, s, n);
    o->n += n;
    o->d[o->n] = '\0';
}

static void out_str(Out *o, const char *s) { out_bytes(o, s, strlen(s)); }

static void out_cp(Out *o, uint32_t c) {
    char b[4];
    if (c < 0x80) b[0] = (char)c, out_bytes(o, b, 1);
    else if (c < 0x800) b[0] = (char)(0xC0 | (c >> 6)), b[1] = (char)(0x80 | (c & 0x3F)), out_bytes(o, b, 2);
    else if (c < 0x10000)
        b[0] = (char)(0xE0 | (c >> 12)), b[1] = (char)(0x80 | ((c >> 6) & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)),
        out_bytes(o, b, 3);
    else
        b[0] = (char)(0xF0 | (c >> 18)), b[1] = (char)(0x80 | ((c >> 12) & 0x3F)),
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), out_bytes(o, b, 4);
}

/* A JS string, escaped for JSON (quote '"') or for display (quote '\''),
 * or raw (quote 0). */
static int out_js_string(PxVM *vm, Out *o, PxValue v, char quote) {
    PxString *s = px_str_flat(vm, v);
    uint32_t  i;
    if (!s) return -1;
    if (quote) out_bytes(o, &quote, 1);
    for (i = 0; i < s->len && !o->full; i++) {
        uint32_t c = px_str_at(s, i);
        char     esc[8];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
            uint32_t d = px_str_at(s, i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                out_cp(o, 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00));
                i++;
                continue;
            }
        }
        if (!quote) {
            out_cp(o, (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c);
            continue;
        }
        if (c == (uint32_t)quote || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            out_bytes(o, esc, 2);
        } else if (c == '\n') out_str(o, "\\n");
        else if (c == '\r') out_str(o, "\\r");
        else if (c == '\t') out_str(o, "\\t");
        else if (c == '\b') out_str(o, "\\b");
        else if (c == '\f') out_str(o, "\\f");
        else if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
            snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
            out_str(o, esc);
        } else out_cp(o, c);
    }
    if (quote) out_bytes(o, &quote, 1);
    return 0;
}

static int out_number(PxVM *vm, Out *o, double d) {
    PxValue s;
    if (d == 0 && signbit(d)) {
        out_str(o, "-0");
        return 0;
    }
    s = px_number_to_string(vm, d, 10);
    if (s == PX_EXCEPTION) return -1;
    return out_js_string(vm, o, s, 0);
}

/* ============================================================ stringify */

/* The output: Latin-1 code units until a character above U+00FF comes,
 * then UTF-16. One buffer, doubled as it fills, and one string made from
 * it at the end (no UTF-8 round trip, no string per fragment). */
typedef struct SB {
    uint8_t *d; /* uint8_t[cap], or uint16_t[cap] once wide */
    uint32_t n, cap;
    int      wide;
    int      err; /* 1: out of memory, 2: longer than a string can be */
} SB;

static int sb_reserve(SB *b, uint32_t more) {
    uint32_t need = b->n + more, cap;
    uint8_t *nd;
    if (b->err) return -1;
    if (need <= b->cap) return 0;
    if (need < b->n || need > (1u << 28)) {
        b->err = 2;
        return -1;
    }
    cap = b->cap ? b->cap * 2 : 256;
    while (cap < need) cap *= 2;
    nd = (uint8_t *)realloc(b->d, (size_t)cap << b->wide);
    if (!nd) {
        b->err = 1;
        return -1;
    }
    b->d   = nd;
    b->cap = cap;
    return 0;
}

static int sb_widen(SB *b) {
    uint32_t  cap = b->cap ? b->cap : 256, i;
    uint16_t *w;
    if (b->err) return -1;
    w = (uint16_t *)malloc((size_t)cap * 2u);
    if (!w) {
        b->err = 1;
        return -1;
    }
    for (i = 0; i < b->n; i++) w[i] = b->d[i];
    free(b->d);
    b->d    = (uint8_t *)w;
    b->cap  = cap;
    b->wide = 1;
    return 0;
}

/* Latin-1 units (ASCII text included) */
static void sb_l1(SB *b, const uint8_t *s, uint32_t len) {
    uint32_t i;
    if (sb_reserve(b, len) < 0) return;
    if (!b->wide) memcpy(b->d + b->n, s, len);
    else
        for (i = 0; i < len; i++) ((uint16_t *)b->d)[b->n + i] = s[i];
    b->n += len;
}

#define sb_lit(b, s) sb_l1((b), (const uint8_t *)(s), (uint32_t)(sizeof(s) - 1))

static void sb_unit(SB *b, uint16_t c) {
    if (c > 0xFF && !b->wide && sb_widen(b) < 0) return;
    if (sb_reserve(b, 1) < 0) return;
    if (b->wide) ((uint16_t *)b->d)[b->n++] = c;
    else b->d[b->n++] = (uint8_t)c;
}

/* The escape for c: '"', '\\' or a control character; or \uXXXX for a
 * lone surrogate (well-formed JSON.stringify) */
static void sb_escape(SB *b, uint16_t c) {
    static const char hex[] = "0123456789abcdef";
    char e[6];
    switch (c) {
    case '"': sb_lit(b, "\\\""); return;
    case '\\': sb_lit(b, "\\\\"); return;
    case '\b': sb_lit(b, "\\b"); return;
    case '\f': sb_lit(b, "\\f"); return;
    case '\n': sb_lit(b, "\\n"); return;
    case '\r': sb_lit(b, "\\r"); return;
    case '\t': sb_lit(b, "\\t"); return;
    }
    e[0] = '\\';
    e[1] = 'u';
    e[2] = hex[(c >> 12) & 15];
    e[3] = hex[(c >> 8) & 15];
    e[4] = hex[(c >> 4) & 15];
    e[5] = hex[c & 15];
    sb_l1(b, (const uint8_t *)e, 6);
}

/* QuoteJSONString: runs of plain characters are copied at once */
static int sb_json_string(PxVM *vm, SB *b, PxValue v) {
    PxString *s = px_str_flat(vm, v);
    uint32_t  i, run;
    if (!s) return -1;
    sb_unit(b, '"');
    if (!s->wide) {
        const uint8_t *c = px_str_l1(s);
        for (i = 0; i < s->len;) {
            run = i;
            while (i < s->len && c[i] >= 0x20 && c[i] != '"' && c[i] != '\\') i++;
            if (i > run) sb_l1(b, c + run, i - run);
            if (i < s->len) sb_escape(b, c[i++]);
        }
    } else {
        const uint16_t *c = px_str_u16(s);
        for (i = 0; i < s->len; i++) {
            uint16_t ch = c[i];
            if (ch >= 0xD800 && ch <= 0xDFFF) {
                if (ch <= 0xDBFF && i + 1 < s->len && c[i + 1] >= 0xDC00 && c[i + 1] <= 0xDFFF) {
                    sb_unit(b, ch);
                    sb_unit(b, c[++i]);
                } else {
                    sb_escape(b, ch);
                }
            } else if (ch < 0x20 || ch == '"' || ch == '\\') {
                sb_escape(b, ch);
            } else {
                sb_unit(b, ch);
            }
        }
    }
    sb_unit(b, '"');
    return 0;
}

/* a finite number, written without making a string */
static void sb_number(SB *b, PxValue v) {
    char buf[40];
    if (px_is_smi(v)) {
        sb_l1(b, (const uint8_t *)buf, (uint32_t)px_itoa(px_smi(v), buf));
    } else {
        double d = px_num(v);
        if (d == 0) sb_lit(b, "0"); /* -0 too */
        else sb_l1(b, (const uint8_t *)buf, (uint32_t)px_fmt_number(d, buf, sizeof buf));
    }
}

typedef struct JCtx {
    PxVM    *vm;
    SB       out;
    PxValue  replacer;  /* function or undefined */
    PxValue  to_json;   /* the key "toJSON" (rooted) */
    PxVec   *allow;     /* allowlist of keys, or NULL */
    uint32_t nallow;
    uint16_t indent[10];
    int      nindent;
    PxValue  stack[JSON_MAX_DEPTH];
    int      depth;
} JCtx;

static int json_value(JCtx *c, PxValue holder, PxValue key, PxIdx idx, PxValue v, int *wrote);

static void newline(JCtx *c) {
    int i, j;
    if (!c->nindent) return;
    sb_unit(&c->out, '\n');
    for (i = 0; i < c->depth; i++)
        for (j = 0; j < c->nindent; j++) sb_unit(&c->out, c->indent[j]);
}

static int json_object(JCtx *c, PxValue v) {
    PxVM    *vm = c->vm;
    PxVec   *keys;
    PxValue  kv;
    uint32_t n, i;
    int      first = 1, is_arr = px_is_array(vm, v);

    if (is_arr < 0) return -1;
    if (c->depth >= JSON_MAX_DEPTH) {
        px_throw_error(vm, PX_RANGE_ERROR, "JSON.stringify: nesting too deep");
        return -1;
    }
    for (i = 0; i < (uint32_t)c->depth; i++)
        if (c->stack[i] == v) {
            px_throw_error(vm, PX_TYPE_ERROR, "JSON.stringify: cyclic object value");
            return -1;
        }
    c->stack[c->depth++] = v;
    sb_unit(&c->out, is_arr ? '[' : '{');
    if (is_arr) {
        PxIdx len, k;
        if (px_length_of(vm, v, &len) < 0) return -1;
        for (k = 0; k < len; k++) {
            PxValue item = 0;
            int     wrote;
            /* a dense element directly; holes, sparse arrays and Proxies
             * the general way. Checked each time: toJSON may change v. */
            if (px_type_of(v) == PX_T_ARRAY) {
                PxArray *a = (PxArray *)px_ptr(v);
                if (a->elems && k < a->length && k < a->elems->cap && a->elems->items[k] != PX_HOLE)
                    item = a->elems->items[k];
            }
            if (!item) item = px_get_index(vm, v, k);
            if (item == PX_EXCEPTION) return -1;
            if (!first) sb_unit(&c->out, ',');
            first = 0;
            newline(c);
            PX_ROOT(vm, item);
            if (json_value(c, v, 0, k, item, &wrote) < 0) {
                px_pop_roots(vm, 1);
                return -1;
            }
            px_pop_roots(vm, 1);
            if (!wrote) sb_lit(&c->out, "null");
        }
    } else {
        if (c->allow) {
            keys = c->allow;
            n    = c->nallow;
        } else {
            keys = px_own_keys(vm, v, 1, &n);
            if (!keys) return -1;
        }
        kv = px_from_ptr(keys);
        PX_ROOT(vm, kv);
        for (i = 0; i < n; i++) {
            PxValue  k = keys->items[i], item, ik;
            uint32_t mark = c->out.n;
            int      wrote;
            ik = px_intern(vm, k);
            if (ik == PX_EXCEPTION) goto fail;
            item = px_get(vm, v, ik);
            if (item == PX_EXCEPTION) goto fail;
            if (!first) sb_unit(&c->out, ',');
            newline(c);
            if (sb_json_string(vm, &c->out, k) < 0) goto fail;
            if (c->nindent) sb_lit(&c->out, ": ");
            else sb_unit(&c->out, ':');
            PX_ROOT(vm, item);
            if (json_value(c, v, k, 0, item, &wrote) < 0) {
                px_pop_roots(vm, 1);
                goto fail;
            }
            px_pop_roots(vm, 1);
            if (!wrote) {
                /* undefined/function values: the whole member is dropped */
                c->out.n = mark;
                continue;
            }
            first = 0;
        }
        px_pop_roots(vm, 1);
    }
    c->depth--;
    if (!first) newline(c);
    sb_unit(&c->out, is_arr ? ']' : '}');
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

/* SerializeJSONProperty. key 0: the array index idx, made a string only
 * if toJSON or the replacer needs it. */
static int json_value(JCtx *c, PxValue holder, PxValue key, PxIdx idx, PxValue v, int *wrote) {
    PxVM *vm = c->vm;
    int   r  = 0;
    *wrote   = 1;
    PX_ROOT(vm, holder);
    PX_ROOT(vm, key);
    PX_ROOT(vm, v);
    if (px_is_obj(v)) {
        PxValue f = px_get(vm, v, c->to_json);
        if (f == PX_EXCEPTION) goto fail;
        if (px_is_callable(f)) {
            if (!key && (key = px_number_to_string(vm, (double)idx, 10)) == PX_EXCEPTION) goto fail;
            v = px_call(vm, f, v, 1, &key);
            if (v == PX_EXCEPTION) goto fail;
        }
    }
    if (c->replacer != PX_UNDEFINED) {
        PxValue args[2];
        if (!key && (key = px_number_to_string(vm, (double)idx, 10)) == PX_EXCEPTION) goto fail;
        args[0] = key;
        args[1] = v;
        v       = px_call(vm, c->replacer, holder, 2, args);
        if (v == PX_EXCEPTION) goto fail;
    }
    if (px_is_obj(v) && px_type_of(v) == PX_T_BOXED) {
        /* Number and String objects convert as their methods say; Boolean
         * objects give their value */
        PxValue pv = ((PxBoxed *)px_ptr(v))->value;
        if (px_is_num(pv)) {
            double d;
            if (px_to_number(vm, v, &d) < 0) goto fail;
            v = px_number(vm, d);
        } else if (px_is_str(pv)) {
            v = px_to_string(vm, v);
        } else if (pv == PX_TRUE || pv == PX_FALSE) {
            v = pv;
        }
        if (v == PX_EXCEPTION) goto fail;
    }
    if (v == PX_NULL) sb_lit(&c->out, "null");
    else if (v == PX_TRUE) sb_lit(&c->out, "true");
    else if (v == PX_FALSE) sb_lit(&c->out, "false");
    else if (px_is_num(v)) {
        if (px_is_smi(v) || isfinite(px_num(v))) sb_number(&c->out, v);
        else sb_lit(&c->out, "null");
    } else if (px_is_str(v)) r = sb_json_string(vm, &c->out, v);
    else if (px_is_obj(v) && !px_is_callable(v)) r = json_object(c, v);
    else *wrote = 0; /* undefined, functions, symbols */
    px_pop_roots(vm, 3);
    if (c->out.err) {
        if (c->out.err == 2) px_throw_error(vm, PX_RANGE_ERROR, "JSON.stringify: result too long");
        else px_throw_oom(vm);
        return -1;
    }
    return r;
fail:
    px_pop_roots(vm, 3);
    return -1;
}

PxValue px_json_stringify(PxVM *vm, PxValue v, PxValue replacer, PxValue space) {
    JCtx    c;
    PxValue r, holder, allowv = 0;
    int     wrote;

    memset(&c, 0, sizeof c);
    c.vm       = vm;
    c.replacer = px_is_callable(replacer) ? replacer : PX_UNDEFINED;
    PX_ROOT(vm, v);
    PX_ROOT(vm, allowv);
    PX_ROOT(vm, space);
    PX_ROOT(vm, c.to_json);
    c.to_json = px_intern_cstr(vm, "toJSON");
    if (c.to_json == PX_EXCEPTION) goto fail;
    if (c.replacer == PX_UNDEFINED && px_is_obj(replacer)) {
        /* an array of property names: strings, numbers and their objects, once each */
        PxIdx len, k;
        int   arr = px_is_array(vm, replacer);
        if (arr < 0) goto fail;
        if (arr) {
            PX_ROOT(vm, replacer);
            if (px_length_of(vm, replacer, &len) < 0) goto fail1;
            if (len > 0xFFFFFF) {
                px_throw_error(vm, PX_RANGE_ERROR, "JSON.stringify: replacer list too long");
                goto fail1;
            }
            c.allow = px_vec_new(vm, (uint32_t)len + 1);
            if (!c.allow) goto fail1;
            allowv = px_from_ptr(c.allow);
            for (k = 0; k < len; k++) {
                PxValue  item = px_get_index(vm, replacer, k);
                uint32_t j;
                if (item == PX_EXCEPTION) goto fail1;
                if (px_is_num(item) ||
                    (px_is_obj(item) && px_type_of(item) == PX_T_BOXED &&
                     (px_is_num(((PxBoxed *)px_ptr(item))->value) || px_is_str(((PxBoxed *)px_ptr(item))->value))))
                    item = px_to_string(vm, item);
                if (item == PX_EXCEPTION) goto fail1;
                if (!px_is_str(item)) continue;
                c.allow = (PxVec *)px_ptr(allowv);
                for (j = 0; j < c.nallow; j++) {
                    int eq = px_str_eq(vm, c.allow->items[j], item);
                    if (eq < 0) goto fail1;
                    if (eq) break;
                }
                if (j == c.nallow) c.allow->items[c.nallow++] = item;
            }
            px_pop_roots(vm, 1);
        }
    }
    if (px_is_obj(space) && px_type_of(space) == PX_T_BOXED) {
        PxValue pv = ((PxBoxed *)px_ptr(space))->value;
        if (px_is_num(pv)) {
            double d;
            space = px_to_number(vm, space, &d) < 0 ? PX_EXCEPTION : px_number(vm, d);
        } else if (px_is_str(pv)) {
            space = px_to_string(vm, space);
        }
        if (space == PX_EXCEPTION) goto fail;
    }
    if (px_is_num(space)) {
        double n = px_num(space);
        int    i;
        if (n > 10) n = 10;
        for (i = 0; i < (int)n; i++) c.indent[c.nindent++] = ' ';
    } else if (px_is_str(space)) {
        PxString *fs = px_str_flat(vm, space);
        uint32_t  i;
        if (!fs) goto fail;
        for (i = 0; i < fs->len && i < 10; i++) c.indent[c.nindent++] = px_str_at(fs, i);
    }
    holder = px_object_new(vm);
    if (holder == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, holder);
    if (px_define(vm, holder, vm->atom[PX_ATOM_empty], v, PX_ATTR_DEFAULT) < 0 ||
        json_value(&c, holder, vm->atom[PX_ATOM_empty], 0, v, &wrote) < 0) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    px_pop_roots(vm, 5);
    if (!wrote) {
        free(c.out.d);
        return PX_UNDEFINED;
    }
    r = c.out.wide ? px_str_new_u16(vm, (const uint16_t *)c.out.d, c.out.n)
                   : px_str_new_l1(vm, c.out.d, c.out.n);
    free(c.out.d);
    return r;
fail1:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 4);
    free(c.out.d);
    return PX_EXCEPTION;
}

static PxValue json_stringify(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    return px_json_stringify(vm, px_arg(argc, argv, 0), px_arg(argc, argv, 1), px_arg(argc, argv, 2));
}

/* ============================================================ parse */

typedef struct JParse {
    PxVM           *vm;
    const uint8_t  *b8;  /* the text, Latin-1 ... */
    const uint16_t *b16; /* ... or UTF-16 (the string stays rooted) */
    int             wide;
    uint32_t        len, pos;
    int             depth;
    uint16_t       *buf; /* strings with escapes are unescaped here */
    uint32_t        cap;
} JParse;

#define JCH(p, i) ((p)->wide ? (p)->b16[i] : (p)->b8[i])

static PxValue perr(JParse *p, const char *what) {
    return px_throw_error(p->vm, PX_SYNTAX_ERROR, "JSON.parse: %s at position %u", what, (unsigned)p->pos);
}

static inline uint16_t peek(JParse *p) { return p->pos < p->len ? JCH(p, p->pos) : 0; }

static inline void skip_ws(JParse *p) {
    while (p->pos < p->len) {
        uint16_t c = JCH(p, p->pos);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static const void *text_at(JParse *p, uint32_t i) {
    return p->wide ? (const void *)(p->b16 + i) : (const void *)(p->b8 + i);
}

static PxValue parse_value(JParse *p);

/* A string, or with as_key its property key. Most JSON strings have no
 * escapes: those are made straight from the text (a key usually without
 * allocating at all: it is interned already). The rest are unescaped into
 * p->buf. */
static PxValue parse_string(JParse *p, int as_key) {
    uint32_t start = ++p->pos, i = start, n;
    if (!p->wide) {
        const uint8_t *b = p->b8;
        while (i < p->len && b[i] != '"' && b[i] != '\\' && b[i] >= 0x20) i++;
    } else {
        const uint16_t *b = p->b16;
        while (i < p->len && b[i] != '"' && b[i] != '\\' && b[i] >= 0x20) i++;
    }
    if (i < p->len && JCH(p, i) == '"') {
        p->pos = i + 1;
        if (as_key) return px_intern_chars(p->vm, text_at(p, start), p->wide, i - start);
        return p->wide ? px_str_new_u16(p->vm, p->b16 + start, i - start) : px_str_new_l1(p->vm, p->b8 + start, i - start);
    }
    /* the slow path, from the first escape (or the error) on */
    n = i - start;
    if (n > p->cap) {
        uint16_t *nb = (uint16_t *)realloc(p->buf, (n + 64) * sizeof(uint16_t));
        if (!nb) return px_throw_oom(p->vm);
        p->buf = nb;
        p->cap = n + 64;
    }
    for (i = 0; i < n; i++) p->buf[i] = JCH(p, start + i);
    p->pos = start + n;
    for (;;) {
        uint16_t c;
        if (p->pos >= p->len) return perr(p, "unterminated string");
        c = JCH(p, p->pos);
        p->pos++;
        if (c == '"') break;
        if (c < 0x20) return perr(p, "control character in string");
        if (c == '\\') {
            uint16_t e;
            if (p->pos >= p->len) return perr(p, "unterminated string");
            e = JCH(p, p->pos);
            p->pos++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int k;
                c = 0;
                for (k = 0; k < 4; k++) {
                    uint16_t h = peek(p);
                    int      d = (h >= '0' && h <= '9') ? h - '0'
                                 : ((h | 0x20) >= 'a' && (h | 0x20) <= 'f') ? (h | 0x20) - 'a' + 10 : -1;
                    if (d < 0) return perr(p, "invalid \\u escape");
                    c = (uint16_t)(c * 16 + d);
                    p->pos++;
                }
                break;
            }
            default: return perr(p, "invalid escape");
            }
        }
        if (n == p->cap) {
            uint32_t  cap = p->cap ? p->cap * 2 : 64;
            uint16_t *nb  = (uint16_t *)realloc(p->buf, cap * sizeof(uint16_t));
            if (!nb) return px_throw_oom(p->vm);
            p->buf = nb;
            p->cap = cap;
        }
        p->buf[n++] = c;
    }
    if (as_key) return px_intern_chars(p->vm, p->buf, 1, n);
    return px_str_new_u16(p->vm, p->buf, n);
}

static PxValue parse_number(JParse *p) {
    uint32_t start = p->pos, digits = 0;
    int      neg = 0, integer = 1;
    int32_t  v   = 0;
    if (peek(p) == '-') {
        neg = 1;
        p->pos++;
    }
    if (peek(p) == '0') {
        p->pos++;
        digits = 1;
    } else if (peek(p) >= '1' && peek(p) <= '9') {
        /* up to 9 digits accumulate exactly: below the SMI limit */
        while (peek(p) >= '0' && peek(p) <= '9') {
            if (++digits <= 9) v = v * 10 + (peek(p) - '0');
            p->pos++;
        }
    } else {
        return perr(p, "invalid number");
    }
    if (peek(p) == '.') {
        integer = 0;
        p->pos++;
        if (!(peek(p) >= '0' && peek(p) <= '9')) return perr(p, "invalid number");
        while (peek(p) >= '0' && peek(p) <= '9') p->pos++;
    }
    if ((peek(p) | 0x20) == 'e') {
        integer = 0;
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') p->pos++;
        if (!(peek(p) >= '0' && peek(p) <= '9')) return perr(p, "invalid number");
        while (peek(p) >= '0' && peek(p) <= '9') p->pos++;
    }
    /* integers: no double arithmetic (software on the PSP), no boxing; -0 is a double */
    if (integer && digits <= 9 && !(neg && v == 0)) return px_from_smi(neg ? -v : v);
    return px_number(p->vm, px_decimal_to_double(text_at(p, start), p->wide, p->pos - start));
}

static int literal(JParse *p, const char *word) {
    size_t i, n = strlen(word);
    for (i = 0; i < n; i++)
        if (p->pos + i >= p->len || JCH(p, p->pos + (uint32_t)i) != (uint16_t)word[i]) return 0;
    p->pos += (uint32_t)n;
    return 1;
}

static PxValue parse_value(JParse *p) {
    PxVM    *vm = p->vm;
    uint16_t c;
    skip_ws(p);
    c = peek(p);
    if (c == '"') return parse_string(p, 0);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (c == 't' && literal(p, "true")) return PX_TRUE;
    if (c == 'f' && literal(p, "false")) return PX_FALSE;
    if (c == 'n' && literal(p, "null")) return PX_NULL;
    if (c == '[' || c == '{') {
        PxValue container;
        int     is_arr = c == '[';
        if (++p->depth > JSON_MAX_DEPTH) return px_throw_error(vm, PX_RANGE_ERROR, "JSON.parse: nesting too deep");
        p->pos++;
        container = is_arr ? px_array_new(vm, 0) : px_object_new(vm);
        if (container == PX_EXCEPTION) return container;
        PX_ROOT(vm, container);
        skip_ws(p);
        if (peek(p) == (is_arr ? ']' : '}')) {
            p->pos++;
        } else {
            for (;;) {
                PxValue key = PX_UNDEFINED, v;
                if (!is_arr) {
                    skip_ws(p);
                    if (peek(p) != '"') {
                        perr(p, "expected a property name");
                        goto fail;
                    }
                    key = parse_string(p, 1);
                    if (key == PX_EXCEPTION) goto fail;
                    skip_ws(p);
                    if (peek(p) != ':') {
                        perr(p, "expected ':'");
                        goto fail;
                    }
                    p->pos++;
                }
                PX_ROOT(vm, key);
                v = parse_value(p);
                if (v == PX_EXCEPTION) {
                    px_pop_roots(vm, 1);
                    goto fail;
                }
                if (is_arr ? px_array_push(vm, container, v) < 0
                           : px_define(vm, container, key, v, PX_ATTR_DEFAULT) < 0) {
                    px_pop_roots(vm, 1);
                    goto fail;
                }
                px_pop_roots(vm, 1);
                skip_ws(p);
                c = peek(p);
                p->pos++;
                if (c == ',') continue;
                if (c == (is_arr ? ']' : '}')) break;
                p->pos--;
                perr(p, is_arr ? "expected ',' or ']'" : "expected ',' or '}'");
                goto fail;
            }
        }
        p->depth--;
        px_pop_roots(vm, 1);
        return container;
    fail:
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    return perr(p, p->pos >= p->len ? "unexpected end of input" : "unexpected character");
}

/* InternalizeJSONProperty: the reviver sees every value bottom-up; what it
 * returns replaces the value (CreateDataProperty, so a refusal is ignored),
 * undefined deletes it. Nesting recurses, bounded by JSON_MAX_DEPTH. */
static int revive_member(PxVM *vm, PxValue reviver, PxValue obj, PxValue key, int depth);

static PxValue revive(PxVM *vm, PxValue reviver, PxValue holder, PxValue key, int depth) {
    PxValue v, args[2], ik;
    int     arr;
    if (depth > JSON_MAX_DEPTH) return px_throw_error(vm, PX_RANGE_ERROR, "JSON.parse: nesting too deep");
    PX_ROOT(vm, holder);
    PX_ROOT(vm, key);
    ik = px_intern(vm, key);
    if (ik == PX_EXCEPTION) goto fail;
    v = px_get(vm, holder, ik);
    if (v == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, v);
    arr = px_is_array(vm, v);
    if (arr < 0) goto fail1;
    if (arr) {
        PxIdx len, i;
        if (px_length_of(vm, v, &len) < 0) goto fail1;
        for (i = 0; i < len; i++) {
            PxValue k = px_number_to_string(vm, (double)i, 10);
            if (k == PX_EXCEPTION || revive_member(vm, reviver, v, k, depth) < 0) goto fail1;
        }
    } else if (px_is_obj(v)) {
        PxVec   *keys;
        PxValue  kv;
        uint32_t n, i;
        keys = px_own_keys(vm, v, PX_KEYS_ENUMERABLE, &n);
        if (!keys) goto fail1;
        kv = px_from_ptr(keys);
        PX_ROOT(vm, kv);
        for (i = 0; i < n; i++)
            if (revive_member(vm, reviver, v, keys->items[i], depth) < 0) {
                px_pop_roots(vm, 1);
                goto fail1;
            }
        px_pop_roots(vm, 1);
    }
    args[0] = key;
    args[1] = v;
    v       = px_call(vm, reviver, holder, 2, args);
    px_pop_roots(vm, 3);
    return v;
fail1:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

static int revive_member(PxVM *vm, PxValue reviver, PxValue obj, PxValue key, int depth) {
    PxValue nv, k;
    int     r;
    PX_ROOT(vm, key);
    nv = revive(vm, reviver, obj, key, depth + 1);
    if (nv == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return -1;
    }
    PX_ROOT(vm, nv);
    k = px_intern(vm, key);
    r = k == PX_EXCEPTION ? -1 : nv == PX_UNDEFINED ? px_delete(vm, obj, k) : px_create_data_property(vm, obj, k, nv);
    px_pop_roots(vm, 2);
    return r < 0 ? -1 : 0;
}

static PxValue json_parse(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    JParse    p;
    PxString *fs;
    PxValue   text = px_to_string(vm, px_arg(argc, argv, 0)), r;
    (void)t;
    if (text == PX_EXCEPTION) return text;
    PX_ROOT(vm, text);
    memset(&p, 0, sizeof p);
    p.vm = vm;
    fs   = px_str_flat(vm, text);
    if (!fs) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    text  = px_from_ptr(fs); /* the flat copy stays rooted: the parser reads its characters */
    p.wide = fs->wide;
    p.len  = fs->len;
    p.b8   = px_str_l1(fs);
    p.b16  = px_str_u16(fs);
    r = parse_value(&p);
    if (r != PX_EXCEPTION) {
        skip_ws(&p);
        if (p.pos < p.len) r = perr(&p, "unexpected data after the value");
    }
    free(p.buf);
    if (r != PX_EXCEPTION && px_is_callable(px_arg(argc, argv, 1))) {
        PxValue holder;
        PX_ROOT(vm, r);
        holder = px_object_new(vm);
        if (holder == PX_EXCEPTION || px_define(vm, holder, vm->atom[PX_ATOM_empty], r, PX_ATTR_DEFAULT) < 0) {
            px_pop_roots(vm, 2);
            return PX_EXCEPTION;
        }
        r = revive(vm, argv[1], holder, vm->atom[PX_ATOM_empty], 0);
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 1);
    return r;
}

/* ============================================================ inspect */

#define INSPECT_DEPTH 3

typedef struct ICtx {
    PxVM   *vm;
    Out     out;
    PxValue seen[INSPECT_DEPTH + 2];
    int     depth;
} ICtx;

static int is_identifier_key(PxVM *vm, PxValue k) {
    PxString *s = px_str_flat(vm, k);
    uint32_t  i;
    if (!s || s->len == 0) return 0;
    for (i = 0; i < s->len; i++) {
        uint16_t c = px_str_at(s, i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
              (i > 0 && c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

static void inspect(ICtx *c, PxValue v, int top);

static void inspect_fn_name(ICtx *c, PxValue v, const char *kind) {
    PxValue n = px_get(c->vm, v, c->vm->atom[PX_ATOM_name]);
    out_str(&c->out, "[");
    out_str(&c->out, kind);
    if (px_is_str(n) && px_str_len(n) > 0) {
        out_str(&c->out, ": ");
        out_js_string(c->vm, &c->out, n, 0);
    } else {
        out_str(&c->out, " (anonymous)");
    }
    out_str(&c->out, "]");
}

static void inspect_object(ICtx *c, PxValue v) {
    PxVM    *vm = c->vm;
    PxVec   *keys;
    PxValue  kv;
    uint32_t n, i;
    int      is_arr = px_type_of(v) == PX_T_ARRAY, any = 0, k;

    for (k = 0; k < c->depth; k++)
        if (c->seen[k] == v) {
            out_str(&c->out, "[Circular]");
            return;
        }
    if (c->depth >= INSPECT_DEPTH) {
        out_str(&c->out, is_arr ? "[Array]" : "[Object]");
        return;
    }
    c->seen[c->depth++] = v;
    PX_ROOT(vm, v);
    if (is_arr) {
        PxArray *a = (PxArray *)px_ptr(v);
        uint32_t holes = 0;
        out_str(&c->out, "[");
        for (i = 0; i < a->length && !c->out.full; i++) {
            PxValue item;
            if (i >= 100) {
                char more[40];
                snprintf(more, sizeof more, ", ... %u more items", (unsigned)(a->length - i));
                out_str(&c->out, more);
                break;
            }
            if (!px_has_own(vm, v, px_from_smi((int32_t)i))) {
                holes++;
                continue;
            }
            if (any || holes) out_str(&c->out, ",");
            out_str(&c->out, " ");
            if (holes) {
                char h[40];
                snprintf(h, sizeof h, "<%u empty item%s>, ", (unsigned)holes, holes > 1 ? "s" : "");
                out_str(&c->out, h);
                holes = 0;
            }
            item = px_get_index(vm, v, i);
            if (item == PX_EXCEPTION) {
                vm->exception = PX_UNDEFINED;
                out_str(&c->out, "?");
            } else {
                inspect(c, item, 0);
            }
            any = 1;
        }
        if (holes) {
            char h[40];
            snprintf(h, sizeof h, "%s <%u empty item%s>", any ? "," : "", (unsigned)holes, holes > 1 ? "s" : "");
            out_str(&c->out, h);
            any = 1;
        }
        out_str(&c->out, any ? " ]" : "]");
    } else {
        keys = px_own_keys(vm, v, 1, &n);
        if (!keys) {
            vm->exception = PX_UNDEFINED;
            out_str(&c->out, "{?}");
        } else {
            kv = px_from_ptr(keys);
            PX_ROOT(vm, kv);
            out_str(&c->out, "{");
            for (i = 0; i < n && !c->out.full; i++) {
                PxValue key = px_intern(vm, keys->items[i]), item;
                if (key == PX_EXCEPTION) break;
                item = px_get(vm, v, key);
                if (item == PX_EXCEPTION) {
                    vm->exception = PX_UNDEFINED;
                    continue;
                }
                out_str(&c->out, any ? ", " : " ");
                if (is_identifier_key(vm, keys->items[i])) out_js_string(vm, &c->out, keys->items[i], 0);
                else out_js_string(vm, &c->out, keys->items[i], '\'');
                out_str(&c->out, ": ");
                inspect(c, item, 0);
                any = 1;
            }
            out_str(&c->out, any ? " }" : "}");
            px_pop_roots(vm, 1);
        }
    }
    px_pop_roots(vm, 1);
    c->depth--;
}

static void inspect(ICtx *c, PxValue v, int top) {
    PxVM *vm = c->vm;
    if (v == PX_UNDEFINED) out_str(&c->out, "undefined");
    else if (v == PX_NULL) out_str(&c->out, "null");
    else if (v == PX_TRUE) out_str(&c->out, "true");
    else if (v == PX_FALSE) out_str(&c->out, "false");
    else if (px_is_num(v)) out_number(vm, &c->out, px_num(v));
    else if (px_is_str(v)) out_js_string(vm, &c->out, v, top ? 0 : '\'');
    else if (px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL) {
        PxValue d = ((PxSymbol *)px_ptr(v))->description;
        out_str(&c->out, "Symbol(");
        if (px_is_str(d)) out_js_string(vm, &c->out, d, 0);
        out_str(&c->out, ")");
    } else if (px_is_callable(v)) {
        inspect_fn_name(c, v, "Function");
    } else if (px_is_obj(v) && px_type_of(v) == PX_T_ERROR) {
        PxValue name = px_get(vm, v, vm->atom[PX_ATOM_name]);
        PxValue msg  = px_get(vm, v, vm->atom[PX_ATOM_message]);
        PxValue stk  = px_get(vm, v, vm->atom[PX_ATOM_stack]);
        if (!top) out_str(&c->out, "[");
        if (px_is_str(name)) out_js_string(vm, &c->out, name, 0);
        else out_str(&c->out, "Error");
        if (px_is_str(msg) && px_str_len(msg)) {
            out_str(&c->out, ": ");
            out_js_string(vm, &c->out, msg, 0);
        }
        if (!top) out_str(&c->out, "]");
        else if (px_is_str(stk) && px_str_len(stk)) {
            out_str(&c->out, "\n");
            out_js_string(vm, &c->out, stk, 0);
            while (c->out.n > 0 && c->out.d[c->out.n - 1] == '\n') c->out.d[--c->out.n] = '\0';
        }
        if (name == PX_EXCEPTION || msg == PX_EXCEPTION || stk == PX_EXCEPTION) vm->exception = PX_UNDEFINED;
    } else if (px_is_obj(v) && px_type_of(v) == PX_T_BOXED) {
        PxValue inner = ((PxBoxed *)px_ptr(v))->value;
        out_str(&c->out, px_is_str(inner) ? "[String: " : px_is_num(inner) ? "[Number: " : "[Boolean: ");
        inspect(c, inner, 0);
        out_str(&c->out, "]");
    } else if (px_is_obj(v)) {
        inspect_object(c, v);
    } else {
        out_str(&c->out, "?");
    }
}

int px_inspect(PxVM *vm, PxValue v, char *dst, size_t cap) {
    ICtx   c;
    size_t n;
    memset(&c, 0, sizeof c);
    c.vm        = vm;
    c.out.limit = cap > 4 ? cap - 4 : cap;
    inspect(&c, v, 1);
    n = c.out.n;
    if (cap) {
        size_t m = n < cap - 1 ? n : cap - 1;
        if (c.out.d) memcpy(dst, c.out.d, m);
        dst[m] = '\0';
        if (c.out.full && cap > 4) {
            /* "…" on a character boundary */
            while (m > 0 && ((unsigned char)dst[m - 1] & 0xC0) == 0x80) m--;
            if (m > 0 && ((unsigned char)dst[m - 1] & 0x80)) m--;
            if (m + 4 > cap) m = cap - 4;
            memcpy(dst + m, "\xE2\x80\xA6", 4);
        }
    }
    free(c.out.d);
    return (int)n;
}

/* ============================================================ setup */

int px_json_init(PxVM *vm) {
    static const PxFnDef fns[] = {{"parse", json_parse, 2, 0}, {"stringify", json_stringify, 3, 0}};
    PxObject *o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), vm->protos[PX_PROTO_OBJECT]);
    PxValue   ov;
    if (!o) return -1;
    ov = px_from_ptr(o);
    PX_ROOT(vm, ov);
    if (px_def_value(vm, vm->global, "JSON", ov, PX_ATTR_HIDDEN) < 0 || px_def_fns(vm, ov, fns, PX_COUNTOF(fns)) < 0 ||
        px_def_tag(vm, ov, "JSON") < 0) {
        px_pop_roots(vm, 1);
        return -1;
    }
    px_pop_roots(vm, 1);
    return 0;
}
