/* ArrayBuffer, the typed arrays, DataView, TextEncoder and TextDecoder.
 *
 * Buffers live in the JS heap (a PxBytes cell), so binary data counts
 * against the same bounded heap as everything else. Every element access
 * goes through memcpy: the Allegrex faults on unaligned halfword and word
 * loads, and a DataView offset or a typed array over a sliced buffer can
 * be anything.
 *
 * A detached buffer (ArrayBuffer.prototype.transfer, the test host's
 * $262.detachArrayBuffer) has no data cell and length 0. Views keep their
 * fields; px_typed_length() reports 0 for them, and every element path
 * (here and in px_object.c) goes through it or checks detached(). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

/* Typed array elements are in host order; DataView byte-swaps for
 * big-endian access on the assumption that the host is little-endian. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "px_typed.c assumes a little-endian host (the PSP's Allegrex is one)"
#endif

#define ARG(i) px_arg(argc, argv, (i))
#define MAGIC  (vm->native_magic)

#define MAX_BUFFER (16u * 1024u * 1024u)

static const char *const k_names[PX_TA_KINDS] = {"Int8Array",   "Uint8Array",  "Uint8ClampedArray",
                                                 "Int16Array",  "Uint16Array", "Int32Array",
                                                 "Uint32Array", "Float32Array", "Float64Array"};
static const uint8_t k_size[PX_TA_KINDS] = {1, 1, 1, 2, 2, 4, 4, 4, 8};

static int is_a(PxValue v, PxType type) { return px_is_obj(v) && px_type_of(v) == type; }

/* ------------------------------------------------------------ buffers */

static int      detached(PxValue buffer) { return ((PxArrayBuffer *)px_ptr(buffer))->data == NULL; }
static uint8_t *buf_data(PxValue buffer) { return ((PxArrayBuffer *)px_ptr(buffer))->data->data; }

/* An ArrayBuffer owning `data` (rooted by the caller). */
static PxValue buffer_wrap(PxVM *vm, PxBytes *data) {
    PxArrayBuffer *ab;
    ab = (PxArrayBuffer *)px_obj_new(vm, PX_T_ARRAYBUFFER, sizeof(PxArrayBuffer), vm->protos[PX_PROTO_ARRAYBUFFER]);
    if (!ab) return PX_EXCEPTION;
    ab->data = data;
    ab->len  = data->len;
    return px_from_ptr(ab);
}

static PxValue buffer_new(PxVM *vm, PxIdx len) {
    PxBytes *b;
    PxValue  bv, r;
    if (len > MAX_BUFFER) return px_throw_error(vm, PX_RANGE_ERROR, "array buffer too large");
    b = px_bytes_new(vm, NULL, (uint32_t)len);
    if (!b) return PX_EXCEPTION;
    memset(b->data, 0, (size_t)len);
    bv = px_from_ptr(b);
    PX_ROOT(vm, bv);
    r = buffer_wrap(vm, b);
    px_pop_roots(vm, 1);
    return r;
}

int px_array_buffer_detach(PxValue buffer) {
    PxArrayBuffer *ab;
    if (!is_a(buffer, PX_T_ARRAYBUFFER)) return -1;
    ab       = (PxArrayBuffer *)px_ptr(buffer);
    ab->data = NULL;
    ab->len  = 0;
    return 0;
}

/* ------------------------------------------------------------ elements */

/* An element's number, from host-order bytes. */
static double load_num(const uint8_t *p, int kind) {
    switch (kind) {
    case PX_TA_INT8: return (int8_t)p[0];
    case PX_TA_UINT8:
    case PX_TA_UINT8C: return p[0];
    case PX_TA_INT16: {
        int16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    case PX_TA_UINT16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    case PX_TA_INT32: {
        int32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    case PX_TA_UINT32: {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    case PX_TA_FLOAT32: {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    default: {
        double v;
        memcpy(&v, p, 8);
        return v;
    }
    }
}

/* ...as a value; the small integer kinds without a double */
static PxValue load(PxVM *vm, const uint8_t *p, int kind) {
    switch (kind) {
    case PX_TA_INT8: return px_from_smi((int8_t)p[0]);
    case PX_TA_UINT8:
    case PX_TA_UINT8C: return px_from_smi(p[0]);
    case PX_TA_INT16: {
        int16_t v;
        memcpy(&v, p, 2);
        return px_from_smi(v);
    }
    case PX_TA_UINT16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return px_from_smi(v);
    }
    case PX_TA_INT32: {
        int32_t v;
        memcpy(&v, p, 4);
        return px_int(vm, v);
    }
    default: return px_number(vm, load_num(p, kind));
    }
}

static void store_bits(uint8_t *p, int size, uint32_t u) {
    if (size == 1) p[0] = (uint8_t)u;
    else if (size == 2) {
        uint16_t v = (uint16_t)u;
        memcpy(p, &v, 2);
    } else memcpy(p, &u, 4);
}

/* ToUint8Clamp: rounds half to even */
static uint8_t clamp_u8(double d) {
    double f;
    if (!(d > 0)) return 0; /* NaN too */
    if (d >= 255) return 255;
    f = floor(d);
    d -= f;
    return (uint8_t)(d > 0.5 || (d == 0.5 && ((int)f & 1)) ? f + 1 : f);
}

/* The conversions of SetValueInBuffer: ToInt32 & co truncate modulo 2^n,
 * Float32 rounds to nearest (a C conversion does exactly that). */
static void store_num(uint8_t *p, int kind, double d) {
    switch (kind) {
    case PX_TA_UINT8C: p[0] = clamp_u8(d); break;
    case PX_TA_FLOAT32: {
        float v = (float)d;
        memcpy(p, &v, 4);
        break;
    }
    case PX_TA_FLOAT64: memcpy(p, &d, 8); break;
    default: store_bits(p, k_size[kind], (uint32_t)px_dbl_to_int32(d)); break;
    }
}

/* ...for a small integer: no double on the integer kinds */
static void store_int(uint8_t *p, int kind, int32_t i) {
    if (kind == PX_TA_UINT8C) p[0] = (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
    else if (kind >= PX_TA_FLOAT32) store_num(p, kind, i);
    else store_bits(p, k_size[kind], (uint32_t)i);
}

static uint8_t *elem(PxTyped *t, uint32_t i) { return buf_data(t->buffer) + t->offset + i * k_size[t->kind]; }

/* i < px_typed_length(t) */
PxValue px_typed_get(PxVM *vm, PxTyped *t, uint32_t i) { return load(vm, elem(t, i), (int)t->kind); }

/* TypedArraySetElement: the value is converted whatever the index; an
 * index out of bounds (by then) stores nothing. */
int px_typed_set(PxVM *vm, PxValue ta, uint32_t i, PxValue v) {
    PxTyped *t;
    double   d = 0;
    if (!px_is_smi(v)) {
        PX_ROOT(vm, ta);
        if (px_to_number(vm, v, &d) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
        px_pop_roots(vm, 1);
    }
    t = (PxTyped *)px_ptr(ta);
    if (i >= px_typed_length(t)) return 0;
    if (px_is_smi(v)) store_int(elem(t, i), (int)t->kind, px_smi(v));
    else store_num(elem(t, i), (int)t->kind, d);
    return 0;
}

/* ------------------------------------------------------------ keys */

/* Whether a property key of a typed array is a CanonicalNumericIndexString
 * (10.4.5): 1 yes, 0 no, -1 exception. Small-integer keys are; a string
 * key that is one never names an element, since every valid index is a
 * small integer (buffers are far below 2^30 elements). */
int px_typed_numeric_key(PxVM *vm, PxValue ta, PxValue key) {
    PxString *s;
    PxValue   n;
    uint16_t  c;
    int       r;
    if (px_is_smi(key)) return 1;
    if (!px_is_str(key)) return 0; /* a symbol */
    s = px_str_flat(vm, key);
    if (!s) return -1;
    if (!s->len) return 0;
    /* only these can start ToString of a number: skip the conversion for names */
    c = px_str_at(s, 0);
    if (!(c >= '0' && c <= '9') && c != '-' && c != 'I' && c != 'N') return 0;
    if (s->len == 2 && c == '-' && px_str_at(s, 1) == '0') return 1;
    PX_ROOT(vm, ta);
    PX_ROOT(vm, key);
    n = px_number_to_string(vm, px_string_to_number(vm, key), 10);
    r = n == PX_EXCEPTION ? -1 : px_str_eq(vm, n, key);
    px_pop_roots(vm, 2);
    return r;
}

/* ------------------------------------------------------------ conversions */

/* ToIndex: an integer in [0, 2^53 - 1], else a RangeError. */
static int to_index(PxVM *vm, PxValue v, PxIdx *out) {
    double d = 0;
    if (px_is_smi(v) && px_smi(v) >= 0) {
        *out = px_smi(v);
        return 0;
    }
    if (v != PX_UNDEFINED && px_to_number(vm, v, &d) < 0) return -1;
    d = isnan(d) ? 0 : trunc(d);
    if (d < 0 || d > 9007199254740991.0) {
        px_throw_error(vm, PX_RANGE_ERROR, "invalid length or offset");
        return -1;
    }
    *out = (PxIdx)d;
    return 0;
}

/* ToIntegerOrInfinity, clamped to [lo, hi] (the infinities with it) */
static int to_int_clamp(PxVM *vm, PxValue v, PxIdx lo, PxIdx hi, PxIdx *out) {
    double d;
    if (px_is_smi(v)) {
        PxIdx i = px_smi(v);
        *out    = i < lo ? lo : i > hi ? hi : i;
        return 0;
    }
    if (px_to_number(vm, v, &d) < 0) return -1;
    d    = isnan(d) ? 0 : trunc(d);
    *out = d < (double)lo ? lo : d > (double)hi ? hi : (PxIdx)d;
    return 0;
}

/* A relative index argument: negative counts from the end; the result is
 * in [0, len], `dflt` for undefined. */
static int rel_index(PxVM *vm, PxValue v, PxIdx len, PxIdx dflt, PxIdx *out) {
    PxIdx i;
    if (v == PX_UNDEFINED) {
        *out = dflt;
        return 0;
    }
    if (to_int_clamp(vm, v, -len - 1, len, &i) < 0) return -1;
    *out = i >= 0 ? i : i + len < 0 ? 0 : i + len;
    return 0;
}

/* SpeciesConstructor(o, dflt) */
static PxValue species_ctor(PxVM *vm, PxValue o, PxValue dflt) {
    PxValue c = px_get(vm, o, vm->atom[PX_ATOM_constructor]), s;
    if (c == PX_EXCEPTION) return c;
    if (c == PX_UNDEFINED) return dflt;
    if (!px_is_obj(c)) return px_throw_error(vm, PX_TYPE_ERROR, "the constructor property is not an object");
    PX_ROOT(vm, c);
    s = px_get(vm, c, vm->sym_species);
    px_pop_roots(vm, 1);
    if (s == PX_EXCEPTION) return s;
    if (s == PX_UNDEFINED || s == PX_NULL) return dflt;
    if (!px_is_constructor(s)) return px_throw_error(vm, PX_TYPE_ERROR, "[Symbol.species] is not a constructor");
    return s;
}

/* IterableToList(obj, method): an Array of what the iterator produces */
static PxValue iterable_to_list(PxVM *vm, PxValue obj, PxValue method) {
    PxValue it, next = PX_UNDEFINED, list = PX_UNDEFINED, r, v;
    if (!px_is_callable(method)) return px_throw_error(vm, PX_TYPE_ERROR, "[Symbol.iterator] is not a function");
    it = px_call(vm, method, obj, 0, NULL);
    if (it == PX_EXCEPTION) return it;
    if (!px_is_obj(it)) return px_throw_error(vm, PX_TYPE_ERROR, "the iterator is not an object");
    PX_ROOT(vm, it);
    PX_ROOT(vm, next);
    PX_ROOT(vm, list);
    next = px_get(vm, it, vm->atom[PX_ATOM_next]);
    if (next == PX_EXCEPTION || (list = px_array_new(vm, 0)) == PX_EXCEPTION) goto fail;
    for (;;) {
        r = px_call(vm, next, it, 0, NULL);
        if (r == PX_EXCEPTION) goto fail;
        if (!px_is_obj(r)) {
            px_throw_error(vm, PX_TYPE_ERROR, "the iterator result is not an object");
            goto fail;
        }
        PX_ROOT(vm, r);
        v = px_get(vm, r, vm->atom[PX_ATOM_done]);
        if (v != PX_EXCEPTION && !px_truthy(v)) v = px_get(vm, r, vm->atom[PX_ATOM_value]);
        else if (v != PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            break;
        }
        px_pop_roots(vm, 1);
        if (v == PX_EXCEPTION || px_array_push(vm, list, v) < 0) goto fail;
    }
    px_pop_roots(vm, 3);
    return list;
fail:
    px_pop_roots(vm, 3);
    return PX_EXCEPTION;
}

/* ------------------------------------------------------------ ArrayBuffer */

static PxValue ab_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx len;
    if (vm->native_new_target == PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "ArrayBuffer must be called with new");
    if (to_index(vm, ARG(0), &len) < 0) return PX_EXCEPTION;
    return buffer_new(vm, len);
}

static PxArrayBuffer *this_ab(PxVM *vm, PxValue t) {
    if (is_a(t, PX_T_ARRAYBUFFER)) return (PxArrayBuffer *)px_ptr(t);
    px_throw_error(vm, PX_TYPE_ERROR, "this is not an ArrayBuffer");
    return NULL;
}

/* byteLength (0), detached (1) */
static PxValue abp_info(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxArrayBuffer *ab = this_ab(vm, t);
    (void)argc;
    (void)argv;
    if (!ab) return PX_EXCEPTION;
    return MAGIC ? px_bool(!ab->data) : px_int(vm, (int32_t)ab->len);
}

static PxValue abp_slice(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxArrayBuffer *ab = this_ab(vm, t), *nab;
    PxIdx          len, a, b;
    PxValue        c, nb;
    if (!ab) return PX_EXCEPTION;
    if (!ab->data) return px_throw_error(vm, PX_TYPE_ERROR, "the ArrayBuffer is detached");
    len = ab->len;
    if (rel_index(vm, ARG(0), len, 0, &a) < 0 || rel_index(vm, ARG(1), len, len, &b) < 0) return PX_EXCEPTION;
    if (b < a) b = a;
    c = species_ctor(vm, t, vm->ctors[PX_PROTO_ARRAYBUFFER]);
    if (c == PX_EXCEPTION) return c;
    if (c == vm->ctors[PX_PROTO_ARRAYBUFFER]) {
        nb = buffer_new(vm, b - a);
    } else {
        PxValue n = px_from_smi((int32_t)(b - a));
        nb        = px_construct(vm, c, 1, &n);
    }
    if (nb == PX_EXCEPTION) return nb;
    if (!is_a(nb, PX_T_ARRAYBUFFER) || detached(nb) || nb == t)
        return px_throw_error(vm, PX_TYPE_ERROR, "the species constructor did not make a new ArrayBuffer");
    nab = (PxArrayBuffer *)px_ptr(nb);
    if (nab->len < b - a)
        return px_throw_error(vm, PX_TYPE_ERROR, "the species constructor made a too small ArrayBuffer");
    if (detached(t)) return px_throw_error(vm, PX_TYPE_ERROR, "the ArrayBuffer is detached");
    memcpy(buf_data(nb), buf_data(t) + a, (size_t)(b - a));
    return nb;
}

/* transfer / transferToFixedLength (ES2024; all buffers are fixed-length):
 * the same length hands the data cell over without a copy. */
static PxValue abp_transfer(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxArrayBuffer *ab = this_ab(vm, t);
    PxIdx          len;
    PxValue        nb;
    if (!ab) return PX_EXCEPTION;
    len = ab->len;
    if (ARG(0) != PX_UNDEFINED && to_index(vm, argv[0], &len) < 0) return PX_EXCEPTION;
    ab = (PxArrayBuffer *)px_ptr(t);
    if (!ab->data) return px_throw_error(vm, PX_TYPE_ERROR, "the ArrayBuffer is detached");
    if (len == ab->len) {
        nb = buffer_wrap(vm, ab->data);
    } else {
        nb = buffer_new(vm, len);
        if (nb != PX_EXCEPTION) {
            ab = (PxArrayBuffer *)px_ptr(t);
            memcpy(buf_data(nb), ab->data->data, (size_t)(len < ab->len ? len : ab->len));
        }
    }
    if (nb != PX_EXCEPTION) px_array_buffer_detach(t);
    return nb;
}

static PxValue ab_is_view(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)t;
    return px_bool(is_a(ARG(0), PX_T_TYPEDARRAY) || is_a(ARG(0), PX_T_DATAVIEW));
}

/* get [Symbol.species]() { return this } */
static PxValue species_getter(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return t;
}

/* ------------------------------------------------------------ creating typed arrays */

static PxValue view_new(PxVM *vm, PxType type, int kind, PxValue buffer, uint32_t offset, uint32_t length) {
    PxTyped *t;
    PxValue  proto = type == PX_T_DATAVIEW ? vm->protos[PX_PROTO_DATAVIEW] : vm->protos[PX_PROTO_TA_FIRST + kind];
    PX_ROOT(vm, buffer);
    t = (PxTyped *)px_obj_new(vm, type, sizeof(PxTyped), proto);
    px_pop_roots(vm, 1);
    if (!t) return PX_EXCEPTION;
    t->buffer = buffer;
    t->offset = offset;
    t->length = length;
    t->kind   = (uint32_t)kind;
    return px_from_ptr(t);
}

/* A typed array of `kind` over a new zeroed buffer. */
static PxValue ta_alloc(PxVM *vm, int kind, PxIdx len) {
    PxValue buffer;
    if (len > MAX_BUFFER / k_size[kind]) return px_throw_error(vm, PX_RANGE_ERROR, "typed array too large");
    buffer = buffer_new(vm, len * k_size[kind]);
    if (buffer == PX_EXCEPTION) return buffer;
    return view_new(vm, PX_T_TYPEDARRAY, kind, buffer, 0, (uint32_t)len);
}

/* ValidateTypedArray: a typed array whose buffer is not detached */
static PxTyped *valid_ta(PxVM *vm, PxValue t) {
    if (!is_a(t, PX_T_TYPEDARRAY)) {
        px_throw_error(vm, PX_TYPE_ERROR, "this is not a typed array");
        return NULL;
    }
    if (detached(((PxTyped *)px_ptr(t))->buffer)) {
        px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
        return NULL;
    }
    return (PxTyped *)px_ptr(t);
}

/* TypedArrayCreateFromConstructor: new c(...args) must make a usable typed
 * array, with room for args[0] elements when that is a length. */
static PxValue ta_create(PxVM *vm, PxValue c, int argc, PxValue *argv) {
    PxValue  r = px_construct(vm, c, argc, argv);
    PxTyped *t;
    if (r == PX_EXCEPTION) return r;
    if (!(t = valid_ta(vm, r))) return PX_EXCEPTION;
    if (argc == 1 && px_is_num(argv[0]) && (double)t->length < px_num(argv[0]))
        return px_throw_error(vm, PX_TYPE_ERROR, "the constructor made a too short typed array");
    return r;
}

/* TypedArraySpeciesCreate(exemplar, [len]) */
static PxValue ta_species_len(PxVM *vm, PxValue exemplar, uint32_t len) {
    int     kind = (int)((PxTyped *)px_ptr(exemplar))->kind;
    PxValue c    = species_ctor(vm, exemplar, vm->ctors[PX_PROTO_TA_FIRST + kind]), n;
    if (c == PX_EXCEPTION) return c;
    /* the intrinsic constructor would do exactly this, unobservably */
    if (c == vm->ctors[PX_PROTO_TA_FIRST + kind]) return ta_alloc(vm, kind, len);
    n = px_from_smi((int32_t)len);
    return ta_create(vm, c, 1, &n);
}

/* new XArray(source): an iterable's values, else an array-like's elements */
static PxValue ta_from_object(PxVM *vm, int kind, PxValue src) {
    PxValue m, out = PX_UNDEFINED;
    PxIdx   len, i;
    PX_ROOT(vm, src);
    PX_ROOT(vm, out);
    m = px_get(vm, src, vm->sym_iterator);
    if (m == PX_EXCEPTION) goto fail;
    if (m != PX_UNDEFINED && m != PX_NULL && (src = iterable_to_list(vm, src, m)) == PX_EXCEPTION) goto fail;
    if (px_length_of(vm, src, &len) < 0 || (out = ta_alloc(vm, kind, len)) == PX_EXCEPTION) goto fail;
    for (i = 0; i < len; i++) {
        PxValue v = px_get_index(vm, src, i);
        if (v == PX_EXCEPTION || px_typed_set(vm, out, (uint32_t)i, v) < 0) goto fail;
    }
    px_pop_roots(vm, 2);
    return out;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

/* new XArray(), (length), (typedArray), (object), (buffer[, byteOffset[, length]]).
 * The prototype is the intrinsic one: for subclasses and Reflect.construct
 * the VM gives the result NewTarget's (see adopt_prototype in px_vm.c). */
static PxValue ta_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     kind = MAGIC, size = k_size[kind];
    PxValue a    = ARG(0), out;
    PxIdx   len, off, blen;
    if (vm->native_new_target == PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "%s must be called with new", k_names[kind]);
    if (!px_is_obj(a)) {
        if (to_index(vm, a, &len) < 0) return PX_EXCEPTION;
        return ta_alloc(vm, kind, len);
    }
    switch (px_type_of(a)) {
    case PX_T_ARRAYBUFFER:
        if (to_index(vm, ARG(1), &off) < 0) return PX_EXCEPTION;
        if (off % size)
            return px_throw_error(vm, PX_RANGE_ERROR, "the offset of a %s must be a multiple of %d", k_names[kind], size);
        if (ARG(2) != PX_UNDEFINED && to_index(vm, argv[2], &len) < 0) return PX_EXCEPTION;
        if (detached(a)) return px_throw_error(vm, PX_TYPE_ERROR, "the buffer is detached");
        blen = ((PxArrayBuffer *)px_ptr(a))->len;
        if (ARG(2) == PX_UNDEFINED) {
            if (blen % size)
                return px_throw_error(vm, PX_RANGE_ERROR, "the length of a %s's buffer must be a multiple of %d",
                                      k_names[kind], size);
            if (off > blen) return px_throw_error(vm, PX_RANGE_ERROR, "the offset is outside the buffer");
            len = (blen - off) / size;
        } else if (off + len * size > blen) {
            return px_throw_error(vm, PX_RANGE_ERROR, "the typed array does not fit in the buffer");
        }
        return view_new(vm, PX_T_TYPEDARRAY, kind, a, (uint32_t)off, (uint32_t)len);
    case PX_T_TYPEDARRAY: {
        PxTyped *s = (PxTyped *)px_ptr(a), *d;
        uint32_t i, n;
        if (detached(s->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the source's buffer is detached");
        n = s->length;
        PX_ROOT(vm, a);
        out = ta_alloc(vm, kind, n);
        px_pop_roots(vm, 1);
        if (out == PX_EXCEPTION || n == 0) return out;
        s = (PxTyped *)px_ptr(a);
        d = (PxTyped *)px_ptr(out);
        if (s->kind == d->kind) memcpy(elem(d, 0), elem(s, 0), (size_t)n * size);
        else
            for (i = 0; i < n; i++) store_num(elem(d, i), kind, load_num(elem(s, i), (int)s->kind));
        return out;
    }
    default: return ta_from_object(vm, kind, a);
    }
}

/* %TypedArray% itself cannot be constructed */
static PxValue ta_abstract(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_throw_error(vm, PX_TYPE_ERROR, "TypedArray is abstract: use Int8Array, Uint8Array...");
}

/* %TypedArray%.from(source[, mapfn[, thisArg]]) */
static PxValue ta_from(PxVM *vm, PxValue c, int argc, PxValue *argv) {
    PxValue src = ARG(0), fn = ARG(1), m, out = PX_UNDEFINED, n;
    PxIdx   len, i;
    if (!px_is_constructor(c)) return px_throw_error(vm, PX_TYPE_ERROR, "from: this is not a constructor");
    if (fn != PX_UNDEFINED && !px_is_callable(fn))
        return px_throw_error(vm, PX_TYPE_ERROR, "from: the map function is not callable");
    PX_ROOT(vm, src);
    PX_ROOT(vm, out);
    m = px_get(vm, src, vm->sym_iterator);
    if (m == PX_EXCEPTION) goto fail;
    src = m != PX_UNDEFINED && m != PX_NULL ? iterable_to_list(vm, src, m) : px_to_object(vm, src);
    if (src == PX_EXCEPTION || px_length_of(vm, src, &len) < 0) goto fail;
    n = px_idx_value(vm, len);
    if (n == PX_EXCEPTION || (out = ta_create(vm, c, 1, &n)) == PX_EXCEPTION) goto fail;
    for (i = 0; i < len; i++) {
        PxValue v = px_get_index(vm, src, i);
        if (v != PX_EXCEPTION && fn != PX_UNDEFINED) {
            PxValue a[2];
            a[0] = v;
            a[1] = px_from_smi((int32_t)i);
            v    = px_call(vm, fn, ARG(2), 2, a);
        }
        if (v == PX_EXCEPTION || px_typed_set(vm, out, (uint32_t)i, v) < 0) goto fail;
    }
    px_pop_roots(vm, 2);
    return out;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

/* %TypedArray%.of(...items) */
static PxValue ta_of(PxVM *vm, PxValue c, int argc, PxValue *argv) {
    PxValue n = px_from_smi(argc), out;
    int     i;
    if (!px_is_constructor(c)) return px_throw_error(vm, PX_TYPE_ERROR, "of: this is not a constructor");
    out = ta_create(vm, c, 1, &n);
    if (out == PX_EXCEPTION) return out;
    PX_ROOT(vm, out);
    for (i = 0; i < argc; i++)
        if (px_typed_set(vm, out, (uint32_t)i, argv[i]) < 0) {
            px_pop_roots(vm, 1);
            return PX_EXCEPTION;
        }
    px_pop_roots(vm, 1);
    return out;
}

/* ------------------------------------------------------------ embedding API */

PxValue px_new_uint8_clamped_array(PxVM *vm, uint32_t len, uint8_t **data) {
    PxValue out = ta_alloc(vm, PX_TA_UINT8C, len);
    if (out != PX_EXCEPTION && data) *data = elem((PxTyped *)px_ptr(out), 0);
    return out;
}

PxValue px_new_array_buffer(PxVM *vm, uint32_t len, uint8_t **data) {
    PxValue out = buffer_new(vm, len);
    if (out != PX_EXCEPTION && data) *data = buf_data(out);
    return out;
}

int px_array_buffer_bytes(PxValue v, uint8_t **data, size_t *len) {
    if (!is_a(v, PX_T_ARRAYBUFFER)) return -1;
    if (detached(v)) {
        *data = NULL;
        *len  = 0;
        return 0;
    }
    *data = buf_data(v);
    *len  = ((PxArrayBuffer *)px_ptr(v))->len;
    return 0;
}

int px_typed_array_bytes(PxValue v, uint8_t **data, size_t *len) {
    PxTyped *t;
    if (!is_a(v, PX_T_TYPEDARRAY) && !is_a(v, PX_T_DATAVIEW)) return -1;
    t = (PxTyped *)px_ptr(v);
    if (detached(t->buffer)) {
        *data = NULL;
        *len  = 0;
        return 0;
    }
    *data = buf_data(t->buffer) + t->offset;
    *len  = px_type_of(v) == PX_T_DATAVIEW ? t->length : (size_t)t->length * k_size[t->kind];
    return 0;
}

/* ------------------------------------------------------------ %TypedArray%.prototype */

/* length (0), byteLength (1), byteOffset (2), buffer (3); + 4: DataView's */
static PxValue view_info(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      dv = MAGIC & 4, what = MAGIC & 3;
    PxTyped *v;
    (void)argc;
    (void)argv;
    if (!is_a(t, dv ? PX_T_DATAVIEW : PX_T_TYPEDARRAY))
        return px_throw_error(vm, PX_TYPE_ERROR, "this is not a %s", dv ? "DataView" : "typed array");
    v = (PxTyped *)px_ptr(t);
    if (what == 3) return v->buffer;
    if (detached(v->buffer))
        return dv ? px_throw_error(vm, PX_TYPE_ERROR, "the DataView's buffer is detached") : px_from_smi(0);
    return px_int(vm, (int32_t)(what == 2 ? v->offset : what == 1 && !dv ? v->length * k_size[v->kind] : v->length));
}

static PxValue tap_to_string_tag(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (!is_a(t, PX_T_TYPEDARRAY)) return PX_UNDEFINED;
    return px_str_from_cstr(vm, k_names[((PxTyped *)px_ptr(t))->kind]);
}

/* keys, values, entries: magic = PX_IT_ARRAY_* */
static PxValue tap_iter(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (!valid_ta(vm, t)) return PX_EXCEPTION;
    return px_make_iterobj(vm, t, MAGIC);
}

static PxValue tap_at(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t);
    PxIdx    len, k;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if (to_int_clamp(vm, ARG(0), -len - 1, len, &k) < 0) return PX_EXCEPTION;
    if (k < 0) k += len;
    ta = (PxTyped *)px_ptr(t);
    return k >= 0 && k < px_typed_length(ta) ? px_typed_get(vm, ta, (uint32_t)k) : PX_UNDEFINED;
}

/* The element k of t, or undefined once a callback has detached its buffer */
static PxValue get_or_undefined(PxVM *vm, PxValue t, uint32_t k) {
    PxTyped *ta = (PxTyped *)px_ptr(t);
    return k < px_typed_length(ta) ? px_typed_get(vm, ta, k) : PX_UNDEFINED;
}

enum { IT_FOREACH, IT_EVERY, IT_SOME, IT_FIND, IT_FINDINDEX, IT_FINDLAST, IT_FINDLASTINDEX, IT_MAP, IT_FILTER };

/* The callback methods: forEach, every, some, find(Last)(Index), map, filter */
static PxValue tap_iterate(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      mode = MAGIC, back = mode == IT_FINDLAST || mode == IT_FINDLASTINDEX;
    PxTyped *ta   = valid_ta(vm, t);
    PxValue  fn = ARG(0), out = PX_UNDEFINED, r = PX_UNDEFINED;
    uint32_t len, i;
    if (!ta) return PX_EXCEPTION;
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "the callback is not a function");
    len = ta->length;
    PX_ROOT(vm, out);
    if (mode == IT_MAP) out = ta_species_len(vm, t, len);
    else if (mode == IT_FILTER) out = px_array_new(vm, 0);
    if (out == PX_EXCEPTION) goto fail;
    switch (mode) {
    case IT_EVERY: r = PX_TRUE; break;
    case IT_SOME: r = PX_FALSE; break;
    case IT_FINDINDEX:
    case IT_FINDLASTINDEX: r = px_from_smi(-1); break;
    default: break;
    }
    for (i = 0; i < len; i++) {
        uint32_t k = back ? len - 1 - i : i;
        PxValue  a[3], res;
        int      stop = 0;
        a[0] = get_or_undefined(vm, t, k);
        if (a[0] == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, a[0]);
        a[1] = px_from_smi((int32_t)k);
        a[2] = t;
        res  = px_call(vm, fn, ARG(1), 3, a);
        if (res == PX_EXCEPTION) goto fail1;
        switch (mode) {
        case IT_EVERY:
            if ((stop = !px_truthy(res))) r = PX_FALSE;
            break;
        case IT_SOME:
            if ((stop = px_truthy(res))) r = PX_TRUE;
            break;
        case IT_FIND:
        case IT_FINDLAST:
            if ((stop = px_truthy(res))) r = a[0];
            break;
        case IT_FINDINDEX:
        case IT_FINDLASTINDEX:
            if ((stop = px_truthy(res))) r = a[1];
            break;
        case IT_MAP:
            if (px_typed_set(vm, out, k, res) < 0) goto fail1;
            break;
        case IT_FILTER:
            if (px_truthy(res) && px_array_push(vm, out, a[0]) < 0) goto fail1;
            break;
        default: break;
        }
        px_pop_roots(vm, 1);
        if (stop) break;
    }
    if (mode == IT_MAP) r = out;
    if (mode == IT_FILTER) {
        uint32_t n = ((PxArray *)px_ptr(out))->length;
        r          = ta_species_len(vm, t, n);
        if (r == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, r);
        for (i = 0; i < n; i++)
            if (px_typed_set(vm, r, i, ((PxArray *)px_ptr(out))->elems->items[i]) < 0) goto fail1;
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 1);
    return r;
fail1:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* reduce (0), reduceRight (1) */
static PxValue tap_reduce(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      right = MAGIC;
    PxTyped *ta    = valid_ta(vm, t);
    PxValue  fn = ARG(0), acc;
    uint32_t len, i = 0;
    if (!ta) return PX_EXCEPTION;
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "the callback is not a function");
    len = ta->length;
    if (argc >= 2) {
        acc = argv[1];
    } else {
        if (len == 0) return px_throw_error(vm, PX_TYPE_ERROR, "reduce of an empty typed array with no initial value");
        acc = px_typed_get(vm, ta, right ? len - 1 : 0);
        if (acc == PX_EXCEPTION) return acc;
        i = 1;
    }
    PX_ROOT(vm, acc);
    for (; i < len; i++) {
        uint32_t k = right ? len - 1 - i : i;
        PxValue  a[4];
        a[0] = acc;
        a[1] = get_or_undefined(vm, t, k);
        if (a[1] == PX_EXCEPTION) break;
        a[2] = px_from_smi((int32_t)k);
        a[3] = t;
        acc  = px_call(vm, fn, PX_UNDEFINED, 4, a);
        if (acc == PX_EXCEPTION) break;
    }
    px_pop_roots(vm, 1);
    return acc;
}

/* indexOf (0), lastIndexOf (1), includes (2): elements are numbers, so
 * only a number can match -- or undefined, for includes, at indices a
 * detached buffer no longer has */
static PxValue tap_index_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      mode = MAGIC;
    PxTyped *ta   = valid_ta(vm, t);
    PxValue  x    = ARG(0);
    PxIdx    len, k, cur;
    double   d;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if (len == 0) return mode == 2 ? PX_FALSE : px_from_smi(-1);
    if (mode == 1) {
        k = len - 1;
        if (argc > 1 && to_int_clamp(vm, argv[1], -len - 1, len - 1, &k) < 0) return PX_EXCEPTION;
        if (k < 0) k += len;
    } else {
        if (to_int_clamp(vm, ARG(1), -len, len, &k) < 0) return PX_EXCEPTION;
        if (k < 0) k += len;
    }
    ta  = (PxTyped *)px_ptr(t);
    cur = px_typed_length(ta);
    if (mode == 2 && x == PX_UNDEFINED) return px_bool((k > cur ? k : cur) < len);
    if (!px_is_num(x)) return mode == 2 ? PX_FALSE : px_from_smi(-1);
    d = px_num(x);
    if (mode == 1) {
        for (; k >= 0; k--)
            if (k < cur && load_num(elem(ta, (uint32_t)k), (int)ta->kind) == d) return px_from_smi((int32_t)k);
        return px_from_smi(-1);
    }
    for (; k < len && k < cur; k++) {
        double e = load_num(elem(ta, (uint32_t)k), (int)ta->kind);
        if (e == d || (mode == 2 && isnan(d) && isnan(e))) return mode == 2 ? PX_TRUE : px_from_smi((int32_t)k);
    }
    return mode == 2 ? PX_FALSE : px_from_smi(-1);
}

/* join (0), toLocaleString (1: each element's toLocaleString(), comma-separated) */
static PxValue tap_join(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      locale = MAGIC;
    PxTyped *ta     = valid_ta(vm, t);
    PxValue  sep, r;
    uint32_t len, k;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    sep = locale || ARG(0) == PX_UNDEFINED ? px_str_from_cstr(vm, ",") : px_to_string(vm, argv[0]);
    if (sep == PX_EXCEPTION) return sep;
    PX_ROOT(vm, sep);
    r = vm->atom[PX_ATOM_empty];
    PX_ROOT(vm, r);
    for (k = 0; k < len; k++) {
        PxValue v;
        if (k > 0 && (r = px_str_concat(vm, r, sep)) == PX_EXCEPTION) goto fail;
        v = get_or_undefined(vm, t, k);
        if (v == PX_UNDEFINED) continue;
        if (v != PX_EXCEPTION && locale) {
            PxValue m = px_get(vm, v, px_intern_cstr(vm, "toLocaleString"));
            v         = m == PX_EXCEPTION ? m : px_call(vm, m, v, 0, NULL);
        }
        if (v != PX_EXCEPTION) v = px_to_string(vm, v);
        if (v == PX_EXCEPTION || (r = px_str_concat(vm, r, v)) == PX_EXCEPTION) goto fail;
    }
    px_pop_roots(vm, 2);
    return r;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

static void swap_elems(uint8_t *a, uint8_t *b, int size) {
    uint8_t tmp[8];
    memcpy(tmp, a, (size_t)size);
    memcpy(a, b, (size_t)size);
    memcpy(b, tmp, (size_t)size);
}

static PxValue tap_reverse(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t);
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!ta) return PX_EXCEPTION;
    for (i = 0, n = ta->length; i + 1 < n - i; i++) swap_elems(elem(ta, i), elem(ta, n - 1 - i), k_size[ta->kind]);
    return t;
}

static PxValue tap_to_reversed(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t), *d;
    PxValue  out;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!ta) return PX_EXCEPTION;
    n   = ta->length;
    out = ta_alloc(vm, (int)ta->kind, n);
    if (out == PX_EXCEPTION) return out;
    ta = (PxTyped *)px_ptr(t);
    d  = (PxTyped *)px_ptr(out);
    for (i = 0; i < n; i++) memcpy(elem(d, i), elem(ta, n - 1 - i), k_size[ta->kind]);
    return out;
}

static PxValue tap_fill(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t);
    PxValue  v  = ARG(0);
    double   d  = 0;
    PxIdx    len, a, b;
    uint8_t  tmp[8];
    int      size;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if ((!px_is_smi(v) && px_to_number(vm, v, &d) < 0) || rel_index(vm, ARG(1), len, 0, &a) < 0 ||
        rel_index(vm, ARG(2), len, len, &b) < 0)
        return PX_EXCEPTION;
    ta = (PxTyped *)px_ptr(t);
    if (detached(ta->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
    if (px_is_smi(v)) store_int(tmp, (int)ta->kind, px_smi(v));
    else store_num(tmp, (int)ta->kind, d);
    size = k_size[ta->kind];
    for (; a < b; a++) memcpy(elem(ta, (uint32_t)a), tmp, (size_t)size);
    return t;
}

static PxValue tap_copy_within(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t);
    PxIdx    len, to, from, end, n;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if (rel_index(vm, ARG(0), len, 0, &to) < 0 || rel_index(vm, ARG(1), len, 0, &from) < 0 ||
        rel_index(vm, ARG(2), len, len, &end) < 0)
        return PX_EXCEPTION;
    n = end - from < len - to ? end - from : len - to;
    if (n > 0) {
        ta = (PxTyped *)px_ptr(t);
        if (detached(ta->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
        memmove(elem(ta, (uint32_t)to), elem(ta, (uint32_t)from), (size_t)n * k_size[ta->kind]);
    }
    return t;
}

/* set(source[, offset]) */
static PxValue tap_set(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  src = ARG(0);
    PxTyped *d;
    PxIdx    off, len, i;
    if (!is_a(t, PX_T_TYPEDARRAY)) return px_throw_error(vm, PX_TYPE_ERROR, "this is not a typed array");
    if (to_int_clamp(vm, ARG(1), -1, (PxIdx)1 << 53, &off) < 0) return PX_EXCEPTION;
    if (off < 0) return px_throw_error(vm, PX_RANGE_ERROR, "the offset must not be negative");
    d = (PxTyped *)px_ptr(t);
    if (detached(d->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
    if (is_a(src, PX_T_TYPEDARRAY)) {
        PxTyped *s = (PxTyped *)px_ptr(src);
        uint32_t n = s->length, size = k_size[s->kind];
        if (detached(s->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the source's buffer is detached");
        if (off + n > d->length) return px_throw_error(vm, PX_RANGE_ERROR, "the source does not fit at this offset");
        if (s->kind == d->kind) {
            memmove(elem(d, (uint32_t)off), elem(s, 0), (size_t)n * size);
        } else if (s->buffer == d->buffer && n > 0) {
            /* overlapping views of one buffer: convert from a copy */
            PxBytes *copy;
            PX_ROOT(vm, src);
            copy = px_bytes_new(vm, elem(s, 0), n * size);
            px_pop_roots(vm, 1);
            if (!copy) return PX_EXCEPTION;
            s = (PxTyped *)px_ptr(src);
            d = (PxTyped *)px_ptr(t);
            for (i = 0; i < n; i++)
                store_num(elem(d, (uint32_t)(off + i)), (int)d->kind, load_num(copy->data + i * size, (int)s->kind));
        } else {
            for (i = 0; i < n; i++)
                store_num(elem(d, (uint32_t)(off + i)), (int)d->kind, load_num(elem(s, (uint32_t)i), (int)s->kind));
        }
        return PX_UNDEFINED;
    }
    src = px_to_object(vm, src);
    if (src == PX_EXCEPTION) return src;
    PX_ROOT(vm, src);
    if (px_length_of(vm, src, &len) < 0) goto fail;
    if (off + len > ((PxTyped *)px_ptr(t))->length) {
        px_throw_error(vm, PX_RANGE_ERROR, "the source does not fit at this offset");
        goto fail;
    }
    for (i = 0; i < len; i++) {
        PxValue v = px_get_index(vm, src, i);
        if (v == PX_EXCEPTION || px_typed_set(vm, t, (uint32_t)(off + i), v) < 0) goto fail;
    }
    px_pop_roots(vm, 1);
    return PX_UNDEFINED;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue tap_slice(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t), *d;
    PxIdx    len, k, end, n;
    PxValue  out;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if (rel_index(vm, ARG(0), len, 0, &k) < 0 || rel_index(vm, ARG(1), len, len, &end) < 0) return PX_EXCEPTION;
    n   = end > k ? end - k : 0;
    out = ta_species_len(vm, t, (uint32_t)n);
    if (out == PX_EXCEPTION || n == 0) return out;
    ta = (PxTyped *)px_ptr(t);
    d  = (PxTyped *)px_ptr(out);
    if (detached(ta->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
    if (ta->kind != d->kind) {
        PxIdx i;
        for (i = 0; i < n; i++)
            store_num(elem(d, (uint32_t)i), (int)d->kind, load_num(elem(ta, (uint32_t)(k + i)), (int)ta->kind));
    } else {
        /* byte by byte, forwards, as the spec copies: the species
         * constructor may have made a view of the same buffer */
        uint8_t *s = elem(ta, (uint32_t)k), *p = elem(d, 0);
        size_t   b, nb = (size_t)n * k_size[ta->kind];
        for (b = 0; b < nb; b++) p[b] = s[b];
    }
    return out;
}

static PxValue tap_subarray(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta;
    PxIdx    len, a, b;
    PxValue  c, args[3];
    if (!is_a(t, PX_T_TYPEDARRAY)) return px_throw_error(vm, PX_TYPE_ERROR, "this is not a typed array");
    len = ((PxTyped *)px_ptr(t))->length;
    if (rel_index(vm, ARG(0), len, 0, &a) < 0 || rel_index(vm, ARG(1), len, len, &b) < 0) return PX_EXCEPTION;
    ta = (PxTyped *)px_ptr(t);
    c  = species_ctor(vm, t, vm->ctors[PX_PROTO_TA_FIRST + ta->kind]);
    if (c == PX_EXCEPTION) return c;
    ta      = (PxTyped *)px_ptr(t);
    args[0] = ta->buffer;
    args[1] = px_from_smi((int32_t)(ta->offset + a * k_size[ta->kind]));
    args[2] = px_from_smi((int32_t)(b > a ? b - a : 0));
    return ta_create(vm, c, 3, args);
}

/* sort's default order: numeric, -0 before +0, NaN last */
static int num_before(double a, double b) {
    if (isnan(a)) return 0;
    if (isnan(b)) return 1;
    if (a != b) return a < b;
    return signbit(a) && !signbit(b);
}

/* A comparator's verdict that b sorts before a; *err on an exception. */
static int cmp_before(PxVM *vm, PxValue cmp, PxValue b, PxValue a, int *err) {
    PxValue args[2], r;
    double  d;
    args[0] = b;
    args[1] = a;
    r       = px_call(vm, cmp, PX_UNDEFINED, 2, args);
    if (px_is_smi(r)) return px_smi(r) < 0;
    if (r == PX_EXCEPTION || px_to_number(vm, r, &d) < 0) {
        *err = 1;
        return 0;
    }
    return d < 0; /* NaN counts as +0 */
}

/* A stable bottom-up merge sort (no recursion) of n numbers, or with a
 * comparator of n values: v and tmp each hold n. */
static int merge_sort(PxVM *vm, PxValue cmp, double *nv, double *ntmp, PxValue *v, PxValue *tmp, uint32_t n) {
    uint32_t w, i;
    int      err = 0;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            uint32_t mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n, a = i, b = mid, k = i;
            while (a < mid && b < hi) {
                if (nv) ntmp[k++] = num_before(nv[b], nv[a]) ? nv[b++] : nv[a++];
                else {
                    tmp[k++] = cmp_before(vm, cmp, v[b], v[a], &err) ? v[b++] : v[a++];
                    if (err) return -1;
                }
            }
            for (; k < hi; k++) {
                uint32_t from = a < mid ? a++ : b++; /* one run is used up: copy the other */
                if (nv) ntmp[k] = nv[from];
                else tmp[k] = v[from];
            }
        }
        if (nv) memcpy(nv, ntmp, n * sizeof *nv);
        else memcpy(v, tmp, n * sizeof *v);
    }
    return 0;
}

/* Sorts the n elements of src into dst (the same array for sort, a new
 * one of the same kind for toSorted). */
static int ta_sort(PxVM *vm, PxValue src, PxValue dst, uint32_t n, PxValue cmp) {
    PxTyped *s = (PxTyped *)px_ptr(src), *d;
    uint32_t i;
    int      r;
    if (cmp == PX_UNDEFINED) {
        /* no user code runs: sort the numbers themselves */
        double *nv = (double *)malloc(2 * (size_t)n * sizeof(double) + 1);
        if (!nv) {
            px_throw_oom(vm);
            return -1;
        }
        for (i = 0; i < n; i++) nv[i] = load_num(elem(s, i), (int)s->kind);
        merge_sort(vm, cmp, nv, nv + n, NULL, NULL, n);
        d = (PxTyped *)px_ptr(dst);
        for (i = 0; i < n; i++) store_num(elem(d, i), (int)d->kind, nv[i]);
        free(nv);
        return 0;
    }
    {
        PxVec  *vec = px_vec_new(vm, 2 * n);
        PxValue vv;
        if (!vec) return -1;
        vv = px_from_ptr(vec);
        PX_ROOT(vm, vv);
        for (i = 0; i < n; i++)
            if ((vec->items[i] = px_typed_get(vm, (PxTyped *)px_ptr(src), i)) == PX_EXCEPTION) {
                px_pop_roots(vm, 1);
                return -1;
            }
        r = merge_sort(vm, cmp, NULL, NULL, vec->items, vec->items + n, n);
        /* a comparator may have detached the buffer: then nothing is stored */
        for (i = 0; i < n && r == 0; i++) r = px_typed_set(vm, dst, i, vec->items[i]);
        px_pop_roots(vm, 1);
        return r;
    }
}

/* sort (0), toSorted (1) */
static PxValue tap_sort(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  cmp = ARG(0), out = t;
    PxTyped *ta;
    if (cmp != PX_UNDEFINED && !px_is_callable(cmp))
        return px_throw_error(vm, PX_TYPE_ERROR, "the comparator is not a function");
    if (!(ta = valid_ta(vm, t))) return PX_EXCEPTION;
    if (MAGIC == 1 && (out = ta_alloc(vm, (int)ta->kind, ta->length)) == PX_EXCEPTION) return out;
    PX_ROOT(vm, out);
    if (ta_sort(vm, t, out, ((PxTyped *)px_ptr(t))->length, cmp) < 0) out = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return out;
}

/* with(index, value): a copy with one element replaced */
static PxValue tap_with(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxTyped *ta = valid_ta(vm, t), *d;
    PxValue  v  = ARG(1), out;
    PxIdx    len, k;
    double   num = 0;
    if (!ta) return PX_EXCEPTION;
    len = ta->length;
    if (to_int_clamp(vm, ARG(0), -len - 1, len, &k) < 0 || (!px_is_smi(v) && px_to_number(vm, v, &num) < 0))
        return PX_EXCEPTION;
    if (k < 0) k += len;
    if (k < 0 || k >= px_typed_length((PxTyped *)px_ptr(t)))
        return px_throw_error(vm, PX_RANGE_ERROR, "index out of range");
    out = ta_alloc(vm, (int)((PxTyped *)px_ptr(t))->kind, len);
    if (out == PX_EXCEPTION) return out;
    ta = (PxTyped *)px_ptr(t);
    d  = (PxTyped *)px_ptr(out);
    memcpy(elem(d, 0), elem(ta, 0), (size_t)len * k_size[ta->kind]);
    if (px_is_smi(v)) store_int(elem(d, (uint32_t)k), (int)d->kind, px_smi(v));
    else store_num(elem(d, (uint32_t)k), (int)d->kind, num);
    return out;
}

/* ------------------------------------------------------------ DataView */

static PxValue dv_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue b = ARG(0);
    PxIdx   off, len, blen;
    if (vm->native_new_target == PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "DataView must be called with new");
    if (!is_a(b, PX_T_ARRAYBUFFER)) return px_throw_error(vm, PX_TYPE_ERROR, "DataView needs an ArrayBuffer");
    if (to_index(vm, ARG(1), &off) < 0) return PX_EXCEPTION;
    if (detached(b)) return px_throw_error(vm, PX_TYPE_ERROR, "the buffer is detached");
    blen = ((PxArrayBuffer *)px_ptr(b))->len;
    if (off > blen) return px_throw_error(vm, PX_RANGE_ERROR, "the offset is outside the buffer");
    len = blen - off;
    if (ARG(2) != PX_UNDEFINED) {
        if (to_index(vm, argv[2], &len) < 0) return PX_EXCEPTION;
        if (off + len > blen) return px_throw_error(vm, PX_RANGE_ERROR, "the DataView does not fit in the buffer");
    }
    if (detached(b)) return px_throw_error(vm, PX_TYPE_ERROR, "the buffer is detached");
    return view_new(vm, PX_T_DATAVIEW, 0, b, (uint32_t)off, (uint32_t)len);
}

/* get<Type>(offset[, littleEndian]) / set<Type>(offset, value[, littleEndian]):
 * magic = kind | 0x100 for set */
static PxValue dvp_access(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      kind = MAGIC & 0xFF, set = MAGIC >> 8, size = k_size[kind], little, k;
    PxValue  v    = ARG(1);
    PxIdx    off;
    uint8_t  tmp[8], *p;
    PxTyped *dv;
    double   d = 0;
    if (!is_a(t, PX_T_DATAVIEW)) return px_throw_error(vm, PX_TYPE_ERROR, "this is not a DataView");
    if (to_index(vm, ARG(0), &off) < 0) return PX_EXCEPTION;
    if (set && !px_is_smi(v) && px_to_number(vm, v, &d) < 0) return PX_EXCEPTION;
    little = px_truthy(ARG(set ? 2 : 1));
    dv     = (PxTyped *)px_ptr(t);
    if (detached(dv->buffer)) return px_throw_error(vm, PX_TYPE_ERROR, "the DataView's buffer is detached");
    if (off + size > dv->length) return px_throw_error(vm, PX_RANGE_ERROR, "the offset is outside the DataView");
    p = buf_data(dv->buffer) + dv->offset + off;
    /* tmp holds the value in host (little-endian) order */
    if (set) {
        if (px_is_smi(v)) store_int(tmp, kind, px_smi(v));
        else store_num(tmp, kind, d);
        for (k = 0; k < size; k++) p[k] = tmp[little ? k : size - 1 - k];
        return PX_UNDEFINED;
    }
    for (k = 0; k < size; k++) tmp[k] = p[little ? k : size - 1 - k];
    return load(vm, tmp, kind);
}

/* ------------------------------------------------------------ text */

static PxValue te_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (vm->native_new_target == PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "TextEncoder must be called with new");
    return t;
}

static PxValue tep_encode(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = ARG(0) == PX_UNDEFINED ? vm->atom[PX_ATOM_empty] : px_to_string(vm, ARG(0)), buffer;
    size_t  n;
    (void)t;
    if (s == PX_EXCEPTION) return s;
    PX_ROOT(vm, s);
    n = px_str_to_utf8(vm, s, NULL, 0);
    if (n > MAX_BUFFER) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_RANGE_ERROR, "string too long to encode");
    }
    buffer = buffer_new(vm, (uint32_t)n + 1);
    if (buffer == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return buffer;
    }
    px_str_to_utf8(vm, s, (char *)buf_data(buffer), n + 1);
    ((PxArrayBuffer *)px_ptr(buffer))->len = (uint32_t)n; /* drop the NUL */
    px_pop_roots(vm, 1);
    return view_new(vm, PX_T_TYPEDARRAY, PX_TA_UINT8, buffer, 0, (uint32_t)n);
}

static PxValue td_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char label[16] = "utf-8";
    if (vm->native_new_target == PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "TextDecoder must be called with new");
    if (ARG(0) != PX_UNDEFINED) px_to_utf8(vm, argv[0], label, sizeof label);
    if (strcmp(label, "utf-8") != 0 && strcmp(label, "utf8") != 0 && strcmp(label, "UTF-8") != 0)
        return px_throw_error(vm, PX_RANGE_ERROR, "TextDecoder supports utf-8 only");
    return t;
}

static PxValue tdp_decode(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  src = ARG(0);
    uint8_t *p;
    size_t   n;
    uint32_t skip = 0;
    (void)t;
    if (src == PX_UNDEFINED) return vm->atom[PX_ATOM_empty];
    if (is_a(src, PX_T_ARRAYBUFFER)) {
        n = ((PxArrayBuffer *)px_ptr(src))->len;
        p = n ? buf_data(src) : NULL;
    } else if (px_typed_array_bytes(src, &p, &n) < 0) {
        return px_throw_error(vm, PX_TYPE_ERROR, "decode expects an ArrayBuffer or a view");
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) skip = 3; /* BOM */
    return px_str_from_utf8(vm, n ? (const char *)p + skip : "", n - skip);
}

/* ------------------------------------------------------------ setup */

/* An accessor property with a getter only; its function is named "get <name>". */
static int def_getter(PxVM *vm, PxValue obj, PxValue key, const char *name, PxNativeFn fn, int magic) {
    char    fname[40];
    PxValue g;
    int     r;
    snprintf(fname, sizeof fname, "get %s", name);
    PX_ROOT(vm, obj);
    PX_ROOT(vm, key);
    g = px_make_native(vm, fn, fname, 0, magic);
    if (g != PX_EXCEPTION) ((PxObject *)px_ptr(g))->flags |= PX_OBJ_NOT_CTOR;
    r = g == PX_EXCEPTION ? -1 : px_define_accessor(vm, obj, key, g, PX_UNDEFINED, PX_ATTR_CONFIGURABLE);
    px_pop_roots(vm, 2);
    return r;
}

static int def_named_getter(PxVM *vm, PxValue obj, const char *name, PxNativeFn fn, int magic) {
    PxValue k;
    PX_ROOT(vm, obj);
    k = px_intern_cstr(vm, name);
    px_pop_roots(vm, 1);
    return k == PX_EXCEPTION ? -1 : def_getter(vm, obj, k, name, fn, magic);
}

static int make_ctor(PxVM *vm, const char *name, PxNativeFn fn, int length, int magic, int proto_index) {
    PxValue ctor = px_make_native(vm, fn, name, length, magic);
    if (ctor == PX_EXCEPTION) return -1;
    vm->ctors[proto_index] = ctor;
    if (px_def_value(vm, vm->global, name, ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_value(vm, ctor, "prototype", vm->protos[proto_index], 0) < 0 ||
        px_define(vm, vm->protos[proto_index], vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0)
        return -1;
    return 0;
}

int px_typed_init(PxVM *vm) {
    static const PxFnDef ab_fns[] = {{"slice", abp_slice, 2, 0},
                                     {"transfer", abp_transfer, 0, 0},
                                     {"transferToFixedLength", abp_transfer, 0, 0}};
    static const PxFnDef ab_st[]  = {{"isView", ab_is_view, 1, 0}};
    static const PxFnDef ta_st[]  = {{"from", ta_from, 1, 0}, {"of", ta_of, 0, 0}};
    static const PxFnDef ta_fns[] = {
        {"at", tap_at, 1, 0},
        {"copyWithin", tap_copy_within, 2, 0},
        {"entries", tap_iter, 0, PX_IT_ARRAY_ENTRIES},
        {"every", tap_iterate, 1, IT_EVERY},
        {"fill", tap_fill, 1, 0},
        {"filter", tap_iterate, 1, IT_FILTER},
        {"find", tap_iterate, 1, IT_FIND},
        {"findIndex", tap_iterate, 1, IT_FINDINDEX},
        {"findLast", tap_iterate, 1, IT_FINDLAST},
        {"findLastIndex", tap_iterate, 1, IT_FINDLASTINDEX},
        {"forEach", tap_iterate, 1, IT_FOREACH},
        {"includes", tap_index_of, 1, 2},
        {"indexOf", tap_index_of, 1, 0},
        {"join", tap_join, 1, 0},
        {"keys", tap_iter, 0, PX_IT_ARRAY_KEYS},
        {"lastIndexOf", tap_index_of, 1, 1},
        {"map", tap_iterate, 1, IT_MAP},
        {"reduce", tap_reduce, 1, 0},
        {"reduceRight", tap_reduce, 1, 1},
        {"reverse", tap_reverse, 0, 0},
        {"set", tap_set, 1, 0},
        {"slice", tap_slice, 2, 0},
        {"some", tap_iterate, 1, IT_SOME},
        {"sort", tap_sort, 1, 0},
        {"subarray", tap_subarray, 2, 0},
        {"toLocaleString", tap_join, 0, 1},
        {"toReversed", tap_to_reversed, 0, 0},
        {"toSorted", tap_sort, 1, 1},
        {"values", tap_iter, 0, PX_IT_ARRAY_VALUES},
        {"with", tap_with, 2, 0},
    };
    static const char *const view_getters[] = {"length", "byteLength", "byteOffset", "buffer"};
    static const char *const dv_types[]     = {"Int8", "Uint8", NULL, "Int16", "Uint16", "Int32", "Uint32",
                                               "Float32", "Float64"};
    PxValue abp = vm->protos[PX_PROTO_ARRAYBUFFER], tap = vm->protos[PX_PROTO_TYPEDARRAY];
    PxValue dvp = vm->protos[PX_PROTO_DATAVIEW], ta, f;
    int     i;

    /* ArrayBuffer */
    if (make_ctor(vm, "ArrayBuffer", ab_ctor, 1, 0, PX_PROTO_ARRAYBUFFER) < 0 ||
        px_def_fns(vm, abp, ab_fns, PX_COUNTOF(ab_fns)) < 0 ||
        px_def_fns(vm, vm->ctors[PX_PROTO_ARRAYBUFFER], ab_st, PX_COUNTOF(ab_st)) < 0 ||
        def_named_getter(vm, abp, "byteLength", abp_info, 0) < 0 ||
        def_named_getter(vm, abp, "detached", abp_info, 1) < 0 || px_def_tag(vm, abp, "ArrayBuffer") < 0 ||
        def_getter(vm, vm->ctors[PX_PROTO_ARRAYBUFFER], vm->sym_species, "[Symbol.species]", species_getter, 0) < 0)
        return -1;

    /* %TypedArray%: the constructors' prototype, holding from, of and the
     * shared prototype methods */
    ta = px_make_native(vm, ta_abstract, "TypedArray", 0, 0);
    if (ta == PX_EXCEPTION) return -1;
    PX_ROOT(vm, ta);
    if (px_def_value(vm, ta, "prototype", tap, 0) < 0 ||
        px_define(vm, tap, vm->atom[PX_ATOM_constructor], ta, PX_ATTR_HIDDEN) < 0 ||
        px_def_fns(vm, ta, ta_st, PX_COUNTOF(ta_st)) < 0 ||
        def_getter(vm, ta, vm->sym_species, "[Symbol.species]", species_getter, 0) < 0 ||
        px_def_fns(vm, tap, ta_fns, PX_COUNTOF(ta_fns)) < 0)
        goto fail;
    for (i = 0; i < 4; i++)
        if (def_named_getter(vm, tap, view_getters[i], view_info, i) < 0) goto fail;
    /* toString is Array.prototype.toString itself, [Symbol.iterator] values */
    f = px_get(vm, vm->protos[PX_PROTO_ARRAY], vm->atom[PX_ATOM_toString]);
    if (f == PX_EXCEPTION || px_define(vm, tap, vm->atom[PX_ATOM_toString], f, PX_ATTR_HIDDEN) < 0) goto fail;
    f = px_get(vm, tap, px_intern_cstr(vm, "values"));
    if (f == PX_EXCEPTION || px_define(vm, tap, vm->sym_iterator, f, PX_ATTR_HIDDEN) < 0 ||
        def_getter(vm, tap, vm->sym_to_string_tag, "[Symbol.toStringTag]", tap_to_string_tag, 0) < 0)
        goto fail;
    for (i = 0; i < PX_TA_KINDS; i++) {
        PxValue bpe = px_from_smi(k_size[i]);
        if (px_set_proto(vm, vm->protos[PX_PROTO_TA_FIRST + i], tap) < 0 ||
            make_ctor(vm, k_names[i], ta_ctor, 3, i, PX_PROTO_TA_FIRST + i) < 0 ||
            px_set_proto(vm, vm->ctors[PX_PROTO_TA_FIRST + i], ta) < 0 ||
            px_def_value(vm, vm->ctors[PX_PROTO_TA_FIRST + i], "BYTES_PER_ELEMENT", bpe, 0) < 0 ||
            px_def_value(vm, vm->protos[PX_PROTO_TA_FIRST + i], "BYTES_PER_ELEMENT", bpe, 0) < 0)
            goto fail;
    }
    px_pop_roots(vm, 1);

    /* DataView */
    if (make_ctor(vm, "DataView", dv_ctor, 1, 0, PX_PROTO_DATAVIEW) < 0 || px_def_tag(vm, dvp, "DataView") < 0)
        return -1;
    for (i = 1; i < 4; i++)
        if (def_named_getter(vm, dvp, view_getters[i], view_info, i | 4) < 0) return -1;
    for (i = 0; i < PX_TA_KINDS; i++) {
        char    name[24];
        PxValue g, s;
        if (!dv_types[i]) continue;
        snprintf(name, sizeof name, "get%s", dv_types[i]);
        g = px_make_native(vm, dvp_access, name, 1, i);
        if (g == PX_EXCEPTION) return -1;
        ((PxObject *)px_ptr(g))->flags |= PX_OBJ_NOT_CTOR;
        if (px_def_value(vm, dvp, name, g, PX_ATTR_HIDDEN) < 0) return -1;
        snprintf(name, sizeof name, "set%s", dv_types[i]);
        s = px_make_native(vm, dvp_access, name, 2, i | 0x100);
        if (s == PX_EXCEPTION) return -1;
        ((PxObject *)px_ptr(s))->flags |= PX_OBJ_NOT_CTOR;
        if (px_def_value(vm, dvp, name, s, PX_ATTR_HIDDEN) < 0) return -1;
    }

    /* TextEncoder / TextDecoder (UTF-8) */
    {
        static const PxFnDef te_fns[] = {{"encode", tep_encode, 1, 0}};
        static const PxFnDef td_fns[] = {{"decode", tdp_decode, 1, 0}};
        PxObject *tp = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), vm->protos[PX_PROTO_OBJECT]);
        PxValue   tpv, ctor;
        if (!tp) return -1;
        tpv = px_from_ptr(tp);
        PX_ROOT(vm, tpv);
        ctor = px_make_native(vm, te_ctor, "TextEncoder", 0, 0);
        if (ctor == PX_EXCEPTION || px_def_value(vm, ctor, "prototype", tpv, 0) < 0 ||
            px_def_value(vm, vm->global, "TextEncoder", ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, tpv, te_fns, 1) < 0 ||
            px_def_value(vm, tpv, "encoding", px_str_from_cstr(vm, "utf-8"), 0) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
        px_pop_roots(vm, 1);
        tp = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), vm->protos[PX_PROTO_OBJECT]);
        if (!tp) return -1;
        tpv = px_from_ptr(tp);
        PX_ROOT(vm, tpv);
        ctor = px_make_native(vm, td_ctor, "TextDecoder", 0, 0);
        if (ctor == PX_EXCEPTION || px_def_value(vm, ctor, "prototype", tpv, 0) < 0 ||
            px_def_value(vm, vm->global, "TextDecoder", ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, tpv, td_fns, 1) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
        px_pop_roots(vm, 1);
    }
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}
