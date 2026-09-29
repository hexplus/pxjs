/* Web-platform functions apps expect to find: structuredClone, atob, btoa.
 *
 * They are HTML, not ECMAScript, but PSPX has no browser around the engine
 * to provide them, and code written for the web uses them freely.
 *
 * structuredClone follows the HTML structured-clone algorithm for the types
 * PXJS has: primitives, plain objects and arrays (own enumerable string
 * keys), Date, RegExp, Map, Set, Error, boxed primitives, ArrayBuffer and
 * its views. Cycles and shared references are preserved. Functions,
 * symbols, promises, generators, WeakMap/WeakSet throw a DataCloneError
 * (an Error named so; there is no DOMException). Transfer lists are not
 * supported: everything is copied. */

#include <stdlib.h>
#include <string.h>

#include "px_internal.h"

#define ARG(i)          px_arg(argc, argv, (i))
#define CLONE_MAX_DEPTH 200 /* C recursion; bounds the stack on the PSP */

static PxValue clone_error(PxVM *vm, const char *what) {
    PxValue e = px_throw_error(vm, PX_ERROR, "%s could not be cloned", what), n;
    PxValue ex = vm->exception;
    (void)e;
    PX_ROOT(vm, ex);
    n = px_str_from_cstr(vm, "DataCloneError");
    if (n != PX_EXCEPTION) px_define(vm, ex, vm->atom[PX_ATOM_name], n, PX_ATTR_HIDDEN);
    px_pop_roots(vm, 1);
    vm->exception = ex;
    return PX_EXCEPTION;
}

static PxValue clone(PxVM *vm, PxValue v, PxValue memo, int depth);

/* Copies own enumerable string-keyed properties of src onto dst. */
static int clone_props(PxVM *vm, PxValue src, PxValue dst, PxValue memo, int depth, int skip_indices) {
    PxVec   *keys;
    PxValue  kv;
    uint32_t n, i;
    keys = px_own_keys(vm, src, 1, &n);
    if (!keys) return -1;
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    for (i = 0; i < n; i++) {
        PxValue key = keys->items[i], k, v;
        if (!px_is_str(key) && !px_is_smi(key)) continue; /* symbols are not copied */
        if (skip_indices && px_is_smi(key)) continue;
        k = px_intern(vm, key);
        if (k == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, k);
        v = px_get(vm, src, k);
        if (v != PX_EXCEPTION) v = clone(vm, v, memo, depth + 1);
        if (v == PX_EXCEPTION || px_define(vm, dst, k, v, PX_ATTR_DEFAULT) < 0) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 1);
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

static PxValue construct1(PxVM *vm, int proto_index, int argc, PxValue *argv) {
    PxValue c = vm->ctors[proto_index];
    return px_construct_nt(vm, c, argc, argv, c);
}

static PxValue clone(PxVM *vm, PxValue v, PxValue memo, int depth) {
    PxValue out = PX_UNDEFINED, prev;
    PxType  type;

    if (!px_is_ptr(v)) return v; /* SMIs, undefined, null, booleans */
    type = px_type_of(v);
    if (type == PX_T_NUMBER || type == PX_T_STRING || type == PX_T_ROPE) return v;
    if (type == PX_T_SYMBOL) return clone_error(vm, "a Symbol");
    if (!px_is_obj(v)) return clone_error(vm, "an internal value");
    if (depth > CLONE_MAX_DEPTH) return px_throw_error(vm, PX_RANGE_ERROR, "structuredClone: nesting too deep");
    prev = px_map_lookup(vm, memo, v);
    if (prev != PX_HOLE) return prev;

    PX_ROOT(vm, v);
    PX_ROOT(vm, out);
    switch (type) {
    case PX_T_OBJECT:
        out = px_object_new(vm);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        if (clone_props(vm, v, out, memo, depth, 0) < 0) goto fail;
        break;
    case PX_T_ARRAY: {
        PxIdx len, i;
        out = px_array_new(vm, 0);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        if (px_length_of(vm, v, &len) < 0) goto fail;
        for (i = 0; i < len; i++) {
            PxValue e;
            if (i <= PX_SMI_MAX && !px_has_own(vm, v, px_from_smi((int32_t)i))) continue; /* holes stay holes */
            e = px_get_index(vm, v, i);
            if (e == PX_EXCEPTION) goto fail;
            e = clone(vm, e, memo, depth + 1);
            if (e == PX_EXCEPTION || px_set_index(vm, out, i, e) < 0) goto fail;
        }
        {
            PxValue l = px_idx_value(vm, len);
            if (l == PX_EXCEPTION || px_set(vm, out, vm->atom[PX_ATOM_length], l) < 0) goto fail;
        }
        if (clone_props(vm, v, out, memo, depth, 1) < 0) goto fail;
        break;
    }
    case PX_T_DATE: {
        PxValue t = px_number(vm, ((PxDate *)px_ptr(v))->time);
        if (t == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, t);
        out = construct1(vm, PX_PROTO_DATE, 1, &t);
        px_pop_roots(vm, 1);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        break;
    }
    case PX_T_REGEXP: {
        PxValue a[2];
        a[0] = px_get(vm, v, px_intern_cstr(vm, "source"));
        if (a[0] == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, a[0]);
        a[1] = px_get(vm, v, px_intern_cstr(vm, "flags"));
        if (a[1] == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        PX_ROOT(vm, a[1]);
        out = construct1(vm, PX_PROTO_REGEXP, 2, a);
        px_pop_roots(vm, 2);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        break;
    }
    case PX_T_MAP: {
        PxMap   *m    = (PxMap *)px_ptr(v);
        int      kind = (int)m->kind;
        uint32_t i;
        if (kind != PX_MAP_MAP && kind != PX_MAP_SET) {
            px_pop_roots(vm, 2);
            return clone_error(vm, kind == PX_MAP_WEAKMAP ? "a WeakMap" : "a WeakSet");
        }
        out = px_map_create(vm, kind);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        for (i = 0; i < ((PxMap *)px_ptr(v))->used; i++) {
            PxValue k = ((PxMap *)px_ptr(v))->entries->items[2 * i], val;
            if (k == PX_HOLE) continue;
            k = clone(vm, k, memo, depth + 1);
            if (k == PX_EXCEPTION) goto fail;
            PX_ROOT(vm, k);
            val = kind == PX_MAP_SET ? k : clone(vm, ((PxMap *)px_ptr(v))->entries->items[2 * i + 1], memo, depth + 1);
            if (val == PX_EXCEPTION || px_map_put(vm, out, k, val) < 0) {
                px_pop_roots(vm, 1);
                goto fail;
            }
            px_pop_roots(vm, 1);
        }
        break;
    }
    case PX_T_ERROR: {
        /* the same error type (by name, as HTML does), message, cause */
        static const char *const names[] = {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError"};
        PxValue     name = px_get(vm, v, vm->atom[PX_ATOM_name]), msg, k;
        PxErrorType type_ = PX_ERROR;
        char        buf[24];
        int         i;
        if (name == PX_EXCEPTION) goto fail;
        if (px_is_str(name)) {
            px_to_utf8(vm, name, buf, sizeof buf);
            for (i = 0; i < 5; i++)
                if (strcmp(buf, names[i]) == 0) type_ = (PxErrorType)i;
        }
        msg = px_get(vm, v, vm->atom[PX_ATOM_message]);
        if (msg == PX_EXCEPTION) goto fail;
        if (msg != PX_UNDEFINED && (msg = px_to_string(vm, msg)) == PX_EXCEPTION) goto fail;
        out = px_error_new(vm, type_, msg);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        k = px_intern_cstr(vm, "cause");
        if (k == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, k);
        if (px_has_own(vm, v, k)) {
            PxValue c = px_get(vm, v, k);
            if (c != PX_EXCEPTION) c = clone(vm, c, memo, depth + 1);
            if (c == PX_EXCEPTION || px_define(vm, out, k, c, PX_ATTR_HIDDEN) < 0) {
                px_pop_roots(vm, 1);
                goto fail;
            }
        }
        px_pop_roots(vm, 1);
        break;
    }
    case PX_T_BOXED: {
        PxValue pv = ((PxBoxed *)px_ptr(v))->value;
        if (px_is_ptr(pv) && px_type_of(pv) == PX_T_SYMBOL) {
            px_pop_roots(vm, 2);
            return clone_error(vm, "a Symbol object");
        }
        out = px_to_object(vm, pv);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        break;
    }
    case PX_T_ARRAYBUFFER: {
        PxValue slice = px_get(vm, v, px_intern_cstr(vm, "slice")), zero = px_from_smi(0);
        if (slice == PX_EXCEPTION) goto fail;
        out = px_call(vm, slice, v, 1, &zero);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        break;
    }
    case PX_T_TYPEDARRAY:
    case PX_T_DATAVIEW: {
        PxTyped *t = (PxTyped *)px_ptr(v);
        PxValue  a[3];
        a[0] = clone(vm, t->buffer, memo, depth + 1);
        if (a[0] == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, a[0]);
        t    = (PxTyped *)px_ptr(v);
        a[1] = px_int(vm, (int32_t)t->offset);
        a[2] = px_int(vm, (int32_t)t->length);
        out  = construct1(vm, type == PX_T_DATAVIEW ? PX_PROTO_DATAVIEW : PX_PROTO_TA_FIRST + (int)t->kind, 3, a);
        px_pop_roots(vm, 1);
        if (out == PX_EXCEPTION || px_map_put(vm, memo, v, out) < 0) goto fail;
        break;
    }
    case PX_T_CLOSURE:
    case PX_T_NATIVE:
    case PX_T_BOUND:
        px_pop_roots(vm, 2);
        return clone_error(vm, "a function");
    default:
        px_pop_roots(vm, 2);
        return clone_error(vm, "this object");
    }
    px_pop_roots(vm, 2);
    return out;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

static PxValue structured_clone(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue memo, r;
    (void)t;
    if (argc > 1 && px_is_obj(argv[1])) {
        PxValue tr = px_get(vm, argv[1], px_intern_cstr(vm, "transfer"));
        if (tr == PX_EXCEPTION) return tr;
        if (tr != PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "structuredClone: transfer is not supported");
    }
    memo = px_map_create(vm, PX_MAP_MAP);
    if (memo == PX_EXCEPTION) return memo;
    PX_ROOT(vm, memo);
    r = clone(vm, ARG(0), memo, 0);
    px_pop_roots(vm, 1);
    return r;
}

/* ------------------------------------------------------------ base64 */

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static PxValue invalid_char(PxVM *vm, const char *fn) {
    PxValue ex, n;
    px_throw_error(vm, PX_ERROR, "%s: the string contains characters outside of the Latin1 range", fn);
    ex = vm->exception;
    PX_ROOT(vm, ex);
    n = px_str_from_cstr(vm, "InvalidCharacterError");
    if (n != PX_EXCEPTION) px_define(vm, ex, vm->atom[PX_ATOM_name], n, PX_ATTR_HIDDEN);
    px_pop_roots(vm, 1);
    vm->exception = ex;
    return PX_EXCEPTION;
}

static PxValue btoa_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   sv, out;
    PxString *s, *o;
    uint32_t  i, n, olen;
    (void)t;
    sv = px_to_string(vm, ARG(0));
    if (sv == PX_EXCEPTION) return sv;
    PX_ROOT(vm, sv);
    s = px_str_flat(vm, sv);
    if (!s) goto fail;
    for (i = 0; i < s->len; i++)
        if (px_str_at(s, i) > 0xFF) {
            px_pop_roots(vm, 1);
            return invalid_char(vm, "btoa");
        }
    n    = s->len;
    olen = (n + 2) / 3 * 4;
    out  = px_str_new_l1(vm, NULL, olen);
    if (out == PX_EXCEPTION) goto fail;
    s = px_str_flat(vm, sv);
    o = (PxString *)px_ptr(out);
    for (i = 0; i < n; i += 3) {
        uint32_t b = (uint32_t)px_str_at(s, i) << 16, j = i / 3 * 4;
        if (i + 1 < n) b |= (uint32_t)px_str_at(s, i + 1) << 8;
        if (i + 2 < n) b |= px_str_at(s, i + 2);
        px_str_l1(o)[j]     = (uint8_t)k_b64[(b >> 18) & 63];
        px_str_l1(o)[j + 1] = (uint8_t)k_b64[(b >> 12) & 63];
        px_str_l1(o)[j + 2] = (uint8_t)(i + 1 < n ? k_b64[(b >> 6) & 63] : '=');
        px_str_l1(o)[j + 3] = (uint8_t)(i + 2 < n ? k_b64[b & 63] : '=');
    }
    px_pop_roots(vm, 1);
    return out;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static int b64_value(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* HTML's forgiving-base64 decode. */
static PxValue atob_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   sv, out;
    PxString *s;
    uint8_t  *buf;
    uint32_t  i, n = 0, bits = 0, nbits = 0, len = 0;
    (void)t;
    sv = px_to_string(vm, ARG(0));
    if (sv == PX_EXCEPTION) return sv;
    PX_ROOT(vm, sv);
    s = px_str_flat(vm, sv);
    if (!s) goto fail;
    buf = (uint8_t *)malloc(s->len * 3 / 4 + 3);
    if (!buf) {
        px_pop_roots(vm, 1);
        return px_throw_oom(vm);
    }
    /* count the data characters (ASCII whitespace ignored), check '=' use */
    for (i = 0; i < s->len; i++) {
        uint16_t c = px_str_at(s, i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') continue;
        if (c == '=') break;
        if (b64_value(c) < 0) goto bad;
        n++;
    }
    for (; i < s->len; i++) { /* at most two '=', then only whitespace */
        uint16_t c = px_str_at(s, i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') continue;
        if (c != '=') goto bad;
        len++;
    }
    if (len > 2 || (len && (n + len) % 4) || n % 4 == 1) goto bad;
    len = 0;
    for (i = 0; i < s->len; i++) {
        int d = b64_value(px_str_at(s, i));
        if (d < 0) continue;
        bits = bits << 6 | (uint32_t)d;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            buf[len++] = (uint8_t)(bits >> nbits);
        }
    }
    out = px_str_new_l1(vm, buf, len);
    free(buf);
    px_pop_roots(vm, 1);
    return out;
bad:
    free(buf);
    px_pop_roots(vm, 1);
    {
        PxValue ex, nm;
        px_throw_error(vm, PX_ERROR, "atob: the string to be decoded is not correctly encoded");
        ex = vm->exception;
        PX_ROOT(vm, ex);
        nm = px_str_from_cstr(vm, "InvalidCharacterError");
        if (nm != PX_EXCEPTION) px_define(vm, ex, vm->atom[PX_ATOM_name], nm, PX_ATTR_HIDDEN);
        px_pop_roots(vm, 1);
        vm->exception = ex;
    }
    return PX_EXCEPTION;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

int px_web_init(PxVM *vm) {
    static const PxFnDef fns[] = {
        {"structuredClone", structured_clone, 1, 0},
        {"atob", atob_fn, 1, 0},
        {"btoa", btoa_fn, 1, 0},
    };
    return px_def_fns(vm, vm->global, fns, PX_COUNTOF(fns));
}
