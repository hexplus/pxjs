/* The standard library: Object, Function, Array, String, Number, Boolean,
 * Math, Error and the global functions. JSON and console formatting live
 * in px_json.c.
 *
 * Every method is generic where the spec makes it generic (Array methods
 * work on array-likes), with fast paths for the dense arrays apps
 * actually use. Callbacks (map, filter, sort comparators) are called
 * through px_call, which is the one place native code re-enters JS. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))
#define MAGIC (vm->native_magic)

PxValue px_arg(int argc, PxValue *argv, int i) { return i < argc ? argv[i] : PX_UNDEFINED; }

int px_def_value(PxVM *vm, PxValue obj, const char *name, PxValue v, uint32_t attrs) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, v);
    k = px_intern_cstr(vm, name);
    r = k == PX_EXCEPTION ? -1 : px_define(vm, obj, k, v, attrs);
    px_pop_roots(vm, 2);
    return r;
}

int px_def_fns(PxVM *vm, PxValue obj, const PxFnDef *defs, int n) {
    int i;
    PX_ROOT(vm, obj);
    for (i = 0; i < n; i++) {
        PxValue f = px_make_native(vm, defs[i].fn, defs[i].name, defs[i].length, defs[i].magic);
        if (f == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            return -1;
        }
        /* built-in methods and functions are never constructors */
        ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
        if (px_def_value(vm, obj, defs[i].name, f, PX_ATTR_HIDDEN) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
    }
    px_pop_roots(vm, 1);
    return 0;
}

int px_length_of(PxVM *vm, PxValue obj, PxIdx *out) {
    PxValue l;
    double  d;
    if (px_is_obj(obj) && px_type_of(obj) == PX_T_ARRAY) {
        *out = ((PxArray *)px_ptr(obj))->length;
        return 0;
    }
    l = px_get(vm, obj, vm->atom[PX_ATOM_length]);
    if (px_is_smi(l)) {
        *out = px_smi(l) > 0 ? px_smi(l) : 0;
        return 0;
    }
    if (l == PX_EXCEPTION || px_to_number(vm, l, &d) < 0) return -1;
    if (isnan(d) || d <= 0) d = 0;
    if (d > 9007199254740991.0) d = 9007199254740991.0;
    *out = (PxIdx)d; /* truncates: ToLength */
    return 0;
}

static PxValue index_key(PxVM *vm, PxIdx i) {
    if (i >= 0 && i <= PX_SMI_MAX) return px_from_smi((int32_t)i);
    {
        PxValue s = px_number_to_string(vm, (double)i, 10);
        return s == PX_EXCEPTION ? s : px_intern(vm, s);
    }
}

PxValue px_get_index(PxVM *vm, PxValue obj, PxIdx i) {
    PxValue k;
    if (px_is_obj(obj) && px_type_of(obj) == PX_T_ARRAY && i >= 0 && i < 0x7FFFFFFF) {
        PxArray *a = (PxArray *)px_ptr(obj);
        uint32_t u = (uint32_t)i;
        if (a->elems && u < a->length && u < a->elems->cap && a->elems->items[u] != PX_HOLE) return a->elems->items[u];
    }
    PX_ROOT(vm, obj);
    k = index_key(vm, i);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    return px_get(vm, obj, k);
}

int px_set_index(PxVM *vm, PxValue obj, PxIdx i, PxValue v) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, v);
    k = index_key(vm, i);
    r = k == PX_EXCEPTION ? -1 : px_set(vm, obj, k, v);
    px_pop_roots(vm, 2);
    return r;
}

static int has_index(PxVM *vm, PxValue obj, PxIdx i) {
    PxValue k;
    if (px_is_obj(obj) && px_type_of(obj) == PX_T_ARRAY && i >= 0 && i < 0x7FFFFFFF) {
        /* a present element of a dense array: no key, no lookup */
        PxArray *a = (PxArray *)px_ptr(obj);
        uint32_t u = (uint32_t)i;
        if (a->elems && u < a->length && u < a->elems->cap && a->elems->items[u] != PX_HOLE) return 1;
    }
    PX_ROOT(vm, obj);
    k = index_key(vm, i);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return -1;
    return px_has(vm, obj, k);
}

/* DeletePropertyOrThrow on an index */
static int delete_index(PxVM *vm, PxValue obj, PxIdx i) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    k = index_key(vm, i);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return -1;
    r = px_delete(vm, obj, k);
    if (r == 0) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot delete array index %lld", (long long)i);
        return -1;
    }
    return r < 0 ? -1 : 0;
}

static int set_length(PxVM *vm, PxValue obj, PxIdx len) {
    PxValue n = px_idx_value(vm, len);
    if (n == PX_EXCEPTION) return -1;
    return px_set(vm, obj, vm->atom[PX_ATOM_length], n);
}

/* Relative index arguments (slice, splice, at...): negative counts from
 * the end, clamped to [0, len]. */
static int rel_index(PxVM *vm, PxValue v, PxIdx len, PxIdx dflt, PxIdx *out) {
    double d;
    if (v == PX_UNDEFINED) {
        *out = dflt;
        return 0;
    }
    if (px_is_smi(v)) {
        PxIdx i = px_smi(v);
        if (i < 0) i = i + len < 0 ? 0 : i + len;
        else if (i > len) i = len;
        *out = i;
        return 0;
    }
    if (px_to_number(vm, v, &d) < 0) return -1;
    if (isnan(d)) d = 0;
    d = trunc(d);
    if (d < 0) d = d + (double)len < 0 ? 0 : d + (double)len;
    else if (d > (double)len) d = (double)len;
    *out = (PxIdx)d;
    return 0;
}

static PxValue require_callable(PxVM *vm, PxValue f, const char *what) {
    if (!px_is_callable(f)) return px_throw_error(vm, PX_TYPE_ERROR, "%s: callback is not a function", what);
    return f;
}

/* ============================================================ Object */

/* A constructor called with new.target other than itself makes its object
 * from newTarget.prototype (OrdinaryCreateFromConstructor); `fallback` is
 * the intrinsic default prototype. */
static PxValue proto_from_ctor(PxVM *vm, PxValue new_target, PxValue fallback) {
    PxValue p;
    if (!px_is_obj(new_target)) return fallback;
    PX_ROOT(vm, fallback);
    p = px_get(vm, new_target, vm->atom[PX_ATOM_prototype]);
    px_pop_roots(vm, 1);
    if (p == PX_EXCEPTION) return p;
    return px_is_obj(p) ? p : fallback;
}

static PxValue new_with_proto(PxVM *vm, PxType type, size_t bytes, PxValue new_target, int proto_index) {
    PxValue   proto = proto_from_ctor(vm, new_target, vm->protos[proto_index]);
    PxObject *o;
    if (proto == PX_EXCEPTION) return proto;
    o = px_obj_new(vm, type, bytes, proto);
    return o ? px_from_ptr(o) : PX_EXCEPTION;
}

static PxValue obj_ctor(PxVM *vm, PxValue this_val, int argc, PxValue *argv) {
    PxValue v = ARG(0), nt = vm->native_new_target;
    (void)this_val;
    if (nt != PX_UNDEFINED && nt != vm->native_callee)
        return new_with_proto(vm, PX_T_OBJECT, sizeof(PxObject), nt, PX_PROTO_OBJECT);
    if (v == PX_UNDEFINED || v == PX_NULL) return px_object_new(vm);
    return px_to_object(vm, v);
}

/* The keys of an array made from a key vector: strings as they are. */
static PxValue keys_to_array(PxVM *vm, PxVec *keys, uint32_t n) {
    PxValue  kv = px_from_ptr(keys), arr;
    uint32_t i;
    PX_ROOT(vm, kv);
    arr = px_array_new(vm, n);
    if (arr != PX_EXCEPTION) {
        PxArray *a = (PxArray *)px_ptr(arr);
        keys       = (PxVec *)px_ptr(kv);
        for (i = 0; i < n; i++) a->elems->items[i] = keys->items[i];
        a->length = n;
    }
    px_pop_roots(vm, 1);
    return arr;
}

/* Object.keys (0), values (1), entries (2): EnumerableOwnProperties. */
static PxValue keys_array(PxVM *vm, PxValue obj, int mode) {
    PxVec   *keys;
    PxValue  kv, arr;
    uint32_t n, i;
    PxDesc   d;
    obj = px_to_object(vm, obj);
    if (obj == PX_EXCEPTION) return obj;
    PX_ROOT(vm, obj);
    /* keys alone run no code between listing and checking: filter at once */
    keys = px_own_keys(vm, obj, mode == 0 ? PX_KEYS_ENUMERABLE : 0, &n);
    if (!keys) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    if (mode == 0) {
        px_pop_roots(vm, 1);
        return keys_to_array(vm, keys, n);
    }
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    arr = px_array_new(vm, 0);
    if (arr == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, arr);
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    for (i = 0; i < n; i++) {
        PxValue item = ((PxVec *)px_ptr(kv))->items[i], key = px_intern(vm, item), v;
        int     r;
        if (key == PX_EXCEPTION) goto fail2;
        r = px_get_own_property(vm, obj, key, &d);
        if (r < 0) goto fail2;
        if (!r || !(d.attrs & PX_ATTR_ENUMERABLE)) continue;
        v = px_get(vm, obj, key);
        if (v == PX_EXCEPTION) goto fail2;
        if (mode == 2) {
            PxValue pair;
            d.value = v;
            pair    = px_array_new(vm, 2);
            if (pair == PX_EXCEPTION) goto fail2;
            ((PxArray *)px_ptr(pair))->elems->items[0] = ((PxVec *)px_ptr(kv))->items[i];
            ((PxArray *)px_ptr(pair))->elems->items[1] = d.value;
            ((PxArray *)px_ptr(pair))->length          = 2;
            v                                           = pair;
        }
        if (px_array_push(vm, arr, v) < 0) goto fail2;
    }
    px_pop_roots(vm, 6);
    return arr;
fail2:
    px_pop_roots(vm, 4);
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

static PxValue obj_keys(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    return keys_array(vm, ARG(0), MAGIC);
}

/* CopyDataProperties' core for Object.assign: every enumerable own
 * property (symbols too) of `src`, by [[Get]], onto `target` by [[Set]]. */
static int assign_from(PxVM *vm, PxValue target, PxValue src) {
    PxVec   *keys;
    PxValue  kv;
    PxDesc   d;
    uint32_t n, j;
    int      r = -1;
    src = px_to_object(vm, src);
    if (src == PX_EXCEPTION) return -1;
    PX_ROOT(vm, src);
    keys = px_own_keys(vm, src, PX_KEYS_SYMBOLS, &n);
    if (!keys) {
        px_pop_roots(vm, 1);
        return -1;
    }
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    for (j = 0; j < n; j++) {
        PxValue k = px_intern(vm, ((PxVec *)px_ptr(kv))->items[j]), v;
        int     f;
        if (k == PX_EXCEPTION) goto out;
        f = px_get_own_property(vm, src, k, &d);
        if (f < 0) goto out;
        if (!f || !(d.attrs & PX_ATTR_ENUMERABLE)) continue;
        v = px_get(vm, src, k);
        if (v == PX_EXCEPTION || px_set(vm, target, k, v) < 0) goto out;
    }
    r = 0;
out:
    px_pop_roots(vm, 5);
    return r;
}

static PxValue obj_assign(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue target = px_to_object(vm, ARG(0));
    int     i;
    (void)t;
    if (target == PX_EXCEPTION) return target;
    PX_ROOT(vm, target);
    for (i = 1; i < argc; i++) {
        if (argv[i] == PX_UNDEFINED || argv[i] == PX_NULL) continue;
        if (assign_from(vm, target, argv[i]) < 0) {
            px_pop_roots(vm, 1);
            return PX_EXCEPTION;
        }
    }
    px_pop_roots(vm, 1);
    return target;
}

/* ObjectDefineProperties: every descriptor is read first, then all are
 * defined, as the spec orders it. */
static PxValue define_properties(PxVM *vm, PxValue o, PxValue props) {
    PxVec   *keys;
    PxValue  kv, listv = PX_UNDEFINED;
    PxDesc   d;
    uint32_t n, i, m = 0;
    props = px_to_object(vm, props);
    if (props == PX_EXCEPTION) return props;
    PX_ROOT(vm, o);
    PX_ROOT(vm, props);
    PX_ROOT(vm, listv);
    keys = px_own_keys(vm, props, PX_KEYS_SYMBOLS, &n);
    if (!keys) goto fail;
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    {
        /* (key, value, get, set, has | attrs << 8) per descriptor */
        PxVec *list = px_vec_new(vm, n * 5 + 1);
        if (!list) goto fail2;
        listv = px_from_ptr(list);
    }
    for (i = 0; i < n; i++) {
        PxValue k = px_intern(vm, ((PxVec *)px_ptr(kv))->items[i]), dobj;
        PxVec  *list;
        int     r;
        if (k == PX_EXCEPTION) goto fail2;
        ((PxVec *)px_ptr(kv))->items[i] = k; /* keeps it alive */
        r = px_get_own_property(vm, props, k, &d);
        if (r < 0) goto fail2;
        if (!r || !(d.attrs & PX_ATTR_ENUMERABLE)) continue;
        dobj = px_get(vm, props, k);
        if (dobj == PX_EXCEPTION || px_to_property_descriptor(vm, dobj, &d) < 0) goto fail2;
        list                  = (PxVec *)px_ptr(listv);
        list->items[m * 5]     = k;
        list->items[m * 5 + 1] = d.value;
        list->items[m * 5 + 2] = d.get;
        list->items[m * 5 + 3] = d.set;
        list->items[m * 5 + 4] = px_from_smi((int32_t)(d.has | d.attrs << 8));
        m++;
    }
    for (i = 0; i < m; i++) {
        PxVec  *list = (PxVec *)px_ptr(listv);
        uint32_t bits = (uint32_t)px_smi(list->items[i * 5 + 4]);
        d.value       = list->items[i * 5 + 1];
        d.get         = list->items[i * 5 + 2];
        d.set         = list->items[i * 5 + 3];
        d.has         = bits & 0xFF;
        d.attrs       = bits >> 8;
        if (px_define_or_throw(vm, o, list->items[i * 5], &d) < 0) goto fail2;
    }
    px_pop_roots(vm, 7);
    return o;
fail2:
    px_pop_roots(vm, 4);
fail:
    px_pop_roots(vm, 3);
    return PX_EXCEPTION;
}

static PxValue obj_create(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   proto = ARG(0), ov;
    PxObject *o;
    (void)t;
    if (proto != PX_NULL && !px_is_obj(proto))
        return px_throw_error(vm, PX_TYPE_ERROR, "Object.create: prototype must be an object or null");
    o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), proto);
    if (!o) return PX_EXCEPTION;
    ov = px_from_ptr(o);
    return ARG(1) == PX_UNDEFINED ? ov : define_properties(vm, ov, ARG(1));
}

static PxValue obj_define_properties(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (!px_is_obj(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "Object.defineProperties called on a non-object");
    return define_properties(vm, ARG(0), ARG(1));
}

static PxValue obj_get_proto(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = px_to_object(vm, ARG(0));
    (void)t;
    return o == PX_EXCEPTION ? o : px_proto_of(vm, o);
}

static PxValue obj_set_proto(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0), proto = ARG(1);
    (void)t;
    if (o == PX_UNDEFINED || o == PX_NULL) return px_throw_error(vm, PX_TYPE_ERROR, "Object.setPrototypeOf called on null or undefined");
    if (proto != PX_NULL && !px_is_obj(proto)) return px_throw_error(vm, PX_TYPE_ERROR, "prototype must be an object or null");
    if (!px_is_obj(o)) return o;
    PX_ROOT(vm, o);
    if (px_set_proto(vm, o, proto) < 0) o = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return o;
}

/* SetIntegrityLevel: sealed (1) or frozen (2). 1 / 0 / -1. */
static int set_integrity(PxVM *vm, PxValue o, int level) {
    PxVec   *keys;
    PxValue  kv;
    PxDesc   d;
    uint32_t n, i;
    int      r = px_prevent_extensions(vm, o);
    if (r <= 0) return r;
    PX_ROOT(vm, o);
    keys = px_own_keys(vm, o, PX_KEYS_SYMBOLS, &n);
    if (!keys) {
        px_pop_roots(vm, 1);
        return -1;
    }
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    r = 1;
    for (i = 0; i < n && r > 0; i++) {
        PxValue k = px_intern(vm, ((PxVec *)px_ptr(kv))->items[i]);
        if (k == PX_EXCEPTION) {
            r = -1;
            break;
        }
        d.has   = PX_DESC_CONFIGURABLE;
        d.attrs = 0;
        if (level == 2) {
            int f = px_get_own_property(vm, o, k, &d);
            if (f < 0) {
                r = -1;
                break;
            }
            if (!f) continue;
            d.has   = PX_DESC_IS_ACCESSOR(&d) ? PX_DESC_CONFIGURABLE : PX_DESC_CONFIGURABLE | PX_DESC_WRITABLE;
            d.attrs = 0;
        }
        if (px_define_or_throw(vm, o, k, &d) < 0) r = -1;
    }
    px_pop_roots(vm, 5);
    return r;
}

/* Object.freeze (2) / seal (1) / preventExtensions (0) */
static PxValue obj_freeze(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0);
    int     r;
    (void)t;
    if (!px_is_obj(o)) return o;
    PX_ROOT(vm, o);
    r = MAGIC == 0 ? px_prevent_extensions(vm, o) : set_integrity(vm, o, MAGIC);
    px_pop_roots(vm, 1);
    if (r < 0) return PX_EXCEPTION;
    if (r == 0) return px_throw_error(vm, PX_TYPE_ERROR, "cannot prevent extensions");
    return o;
}

/* Object.isFrozen (2) / isSealed (1) / isExtensible (0): TestIntegrityLevel */
static PxValue obj_is_frozen(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  o = ARG(0), kv;
    PxVec   *keys;
    PxDesc   d;
    uint32_t n, i;
    int      r;
    (void)t;
    if (!px_is_obj(o)) return px_bool(MAGIC != 0);
    r = px_is_extensible(vm, o);
    if (r < 0) return PX_EXCEPTION;
    if (MAGIC == 0) return px_bool(r);
    if (r) return PX_FALSE;
    PX_ROOT(vm, o);
    keys = px_own_keys(vm, o, PX_KEYS_SYMBOLS, &n);
    if (!keys) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    r = 1;
    for (i = 0; i < n && r > 0; i++) {
        PxValue k = px_intern(vm, ((PxVec *)px_ptr(kv))->items[i]);
        int     f = k == PX_EXCEPTION ? -1 : px_get_own_property(vm, o, k, &d);
        if (f < 0) r = -1;
        else if (f && ((d.attrs & PX_ATTR_CONFIGURABLE) ||
                       (MAGIC == 2 && PX_DESC_IS_DATA(&d) && (d.attrs & PX_ATTR_WRITABLE))))
            r = 0;
    }
    px_pop_roots(vm, 5);
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

static PxValue obj_define_property(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0), k;
    PxDesc  d;
    int     r;
    (void)t;
    if (!px_is_obj(o)) return px_throw_error(vm, PX_TYPE_ERROR, "Object.defineProperty called on a non-object");
    PX_ROOT(vm, o);
    k = px_intern(vm, ARG(1));
    if (k == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return k;
    }
    PX_ROOT(vm, k);
    PX_ROOT_DESC(vm, d);
    r = px_to_property_descriptor(vm, ARG(2), &d);
    if (r == 0) r = MAGIC ? px_define_own_property(vm, o, k, &d) : px_define_or_throw(vm, o, k, &d);
    px_pop_roots(vm, 5);
    if (r < 0) return PX_EXCEPTION;
    /* Reflect.defineProperty (magic 1) answers with a boolean */
    return MAGIC ? px_bool(r) : o;
}

/* Own string keys (0: getOwnPropertyNames) or symbols (1: getOwnPropertySymbols). */
static PxValue obj_get_own_names(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  o = px_to_object(vm, ARG(0)), r;
    PxVec   *keys;
    uint32_t n;
    (void)t;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    keys = px_own_keys(vm, o, MAGIC ? PX_KEYS_SYMBOLS | PX_KEYS_NO_STRINGS : 0, &n);
    r    = keys ? keys_to_array(vm, keys, n) : PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return r;
}

/* Object.fromEntries: AddEntriesFromIterable with CreateDataProperty. */
static PxValue obj_from_entries(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue src = ARG(0), o, iter, item;
    int     r;
    (void)t;
    if (src == PX_UNDEFINED || src == PX_NULL)
        return px_throw_error(vm, PX_TYPE_ERROR, "Object.fromEntries: expected an iterable of pairs");
    o = px_object_new(vm);
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    iter = px_get_iterator(vm, src);
    if (iter == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, iter);
    while ((r = px_iterator_step(vm, iter, &item)) > 0) {
        PxValue k, v;
        PX_ROOT(vm, item);
        if (!px_is_obj(item)) {
            px_throw_error(vm, PX_TYPE_ERROR, "Object.fromEntries: an entry is not an object");
            goto close;
        }
        k = px_get_index(vm, item, 0);
        if (k == PX_EXCEPTION) goto close;
        PX_ROOT(vm, k);
        v = px_get_index(vm, item, 1);
        if (v == PX_EXCEPTION) goto close1;
        PX_ROOT(vm, v);
        k = px_intern(vm, k);
        if (k == PX_EXCEPTION || px_create_data_property(vm, o, k, v) < 0) {
            px_pop_roots(vm, 1);
            goto close1;
        }
        px_pop_roots(vm, 3);
        continue;
    close1:
        px_pop_roots(vm, 1);
    close:
        px_pop_roots(vm, 1);
        {
            /* IfAbruptCloseIterator: close, keep the first exception */
            PxValue exc = vm->exception;
            PX_ROOT(vm, exc);
            px_iterator_close(vm, iter);
            vm->exception = exc;
            px_pop_roots(vm, 1);
        }
        goto fail1;
    }
    if (r < 0) goto fail1;
    px_pop_roots(vm, 2);
    return o;
fail1:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue obj_is(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    return px_bool(px_same_value(vm, ARG(0), ARG(1)));
}

static PxValue has_own_of(PxVM *vm, PxValue o, PxValue k) {
    int r;
    PX_ROOT(vm, k);
    r = px_has_own(vm, o, k);
    px_pop_roots(vm, 1);
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

/* Object.prototype.hasOwnProperty: the key first, then ToObject(this) */
static PxValue objp_has_own(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue k;
    PX_ROOT(vm, t);
    k = px_intern(vm, ARG(0));
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    PX_ROOT(vm, k);
    t = px_to_object(vm, t);
    px_pop_roots(vm, 1);
    return t == PX_EXCEPTION ? t : has_own_of(vm, t, k);
}

/* Object.hasOwn: ToObject first, then the key */
static PxValue obj_has_own(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = px_to_object(vm, ARG(0)), k;
    (void)t;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    k = px_intern(vm, ARG(1));
    px_pop_roots(vm, 1);
    return k == PX_EXCEPTION ? k : has_own_of(vm, o, k);
}

static PxValue objp_is_proto_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue cur = ARG(0);
    if (!px_is_obj(cur)) return PX_FALSE;
    t = px_to_object(vm, t);
    if (t == PX_EXCEPTION) return t;
    PX_ROOT(vm, t);
    for (;;) {
        cur = px_proto_of(vm, cur);
        if (cur == PX_EXCEPTION || !px_is_obj(cur) || cur == t) break;
    }
    px_pop_roots(vm, 1);
    return cur == PX_EXCEPTION ? cur : px_bool(cur == t);
}

static PxValue objp_prop_enumerable(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue k;
    PxDesc  d;
    int     r;
    PX_ROOT(vm, t);
    k = px_intern(vm, ARG(0));
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    PX_ROOT(vm, k);
    t = px_to_object(vm, t);
    if (t == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return t;
    }
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    r = px_get_own_property(vm, t, k, &d);
    px_pop_roots(vm, 4);
    return r < 0 ? PX_EXCEPTION : px_bool(r > 0 && (d.attrs & PX_ATTR_ENUMERABLE));
}


static PxValue objp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    const char *tag = "Object";
    char        buf[80];
    PxValue     o, st;
    int         arr;
    (void)argc;
    (void)argv;
    if (t == PX_UNDEFINED) return px_str_from_cstr(vm, "[object Undefined]");
    if (t == PX_NULL) return px_str_from_cstr(vm, "[object Null]");
    o = px_to_object(vm, t);
    if (o == PX_EXCEPTION) return o;
    arr = px_is_array(vm, o);
    if (arr < 0) return PX_EXCEPTION;
    if (arr) tag = "Array";
    else if (px_is_callable(o)) tag = "Function";
    else {
        switch (px_type_of(o)) {
        case PX_T_ERROR: tag = "Error"; break;
        case PX_T_DATE: tag = "Date"; break;
        case PX_T_REGEXP: tag = "RegExp"; break;
        case PX_T_BOXED: {
            PxValue pv = ((PxBoxed *)px_ptr(o))->value;
            if (px_is_num(pv)) tag = "Number";
            else if (px_is_str(pv)) tag = "String";
            else if (pv == PX_TRUE || pv == PX_FALSE) tag = "Boolean";
            break;
        }
        default: break;
        }
    }
    PX_ROOT(vm, o);
    st = px_get(vm, o, vm->sym_to_string_tag);
    px_pop_roots(vm, 1);
    if (st == PX_EXCEPTION) return st;
    if (px_is_str(st)) {
        PxValue open, r;
        PX_ROOT(vm, st);
        open = px_str_from_cstr(vm, "[object ");
        r    = open == PX_EXCEPTION ? open : px_str_concat(vm, open, st);
        if (r != PX_EXCEPTION) {
            PX_ROOT(vm, r);
            open = px_str_from_cstr(vm, "]");
            r    = open == PX_EXCEPTION ? open : px_str_concat(vm, r, open);
            px_pop_roots(vm, 1);
        }
        px_pop_roots(vm, 1);
        return r;
    }
    snprintf(buf, sizeof buf, "[object %s]", tag);
    return px_str_from_cstr(vm, buf);
}

static PxValue objp_to_locale_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue f;
    (void)argc;
    (void)argv;
    PX_ROOT(vm, t);
    f = px_get(vm, t, vm->atom[PX_ATOM_toString]);
    px_pop_roots(vm, 1);
    if (f == PX_EXCEPTION) return f;
    return px_call(vm, f, t, 0, NULL);
}

static PxValue objp_value_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    return px_to_object(vm, t);
}

/* Object.prototype.__proto__ (Annex B): get (0) and set (1) */
static PxValue objp_proto(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue proto = ARG(0);
    int     r;
    if (MAGIC == 0) {
        t = px_to_object(vm, t);
        return t == PX_EXCEPTION ? t : px_proto_of(vm, t);
    }
    if (t == PX_UNDEFINED || t == PX_NULL) return px_throw_error(vm, PX_TYPE_ERROR, "__proto__ set on null or undefined");
    if ((proto != PX_NULL && !px_is_obj(proto)) || !px_is_obj(t)) return PX_UNDEFINED;
    r = px_set_proto_ok(vm, t, proto);
    if (r < 0) return PX_EXCEPTION;
    if (r == 0) return px_throw_error(vm, PX_TYPE_ERROR, "cannot set the prototype");
    return PX_UNDEFINED;
}

static const PxFnDef k_object_fns[] = {
    {"keys", obj_keys, 1, 0},
    {"values", obj_keys, 1, 1},
    {"entries", obj_keys, 1, 2},
    {"assign", obj_assign, 2, 0},
    {"create", obj_create, 2, 0},
    {"getPrototypeOf", obj_get_proto, 1, 0},
    {"setPrototypeOf", obj_set_proto, 2, 0},
    {"freeze", obj_freeze, 1, 2},
    {"seal", obj_freeze, 1, 1},
    {"preventExtensions", obj_freeze, 1, 0},
    {"isFrozen", obj_is_frozen, 1, 2},
    {"isSealed", obj_is_frozen, 1, 1},
    {"isExtensible", obj_is_frozen, 1, 0},
    {"defineProperty", obj_define_property, 3, 0},
    {"defineProperties", obj_define_properties, 2, 0},
    {"getOwnPropertyNames", obj_get_own_names, 1, 0},
    {"getOwnPropertySymbols", obj_get_own_names, 1, 1},
    {"fromEntries", obj_from_entries, 1, 0},
    {"is", obj_is, 2, 0},
    {"hasOwn", obj_has_own, 2, 0},
};

static const PxFnDef k_object_proto_fns[] = {
    {"hasOwnProperty", objp_has_own, 1, 0},
    {"isPrototypeOf", objp_is_proto_of, 1, 0},
    {"propertyIsEnumerable", objp_prop_enumerable, 1, 0},
    {"toString", objp_to_string, 0, 0},
    {"toLocaleString", objp_to_locale_string, 0, 0},
    {"valueOf", objp_value_of, 0, 0},
};

/* Calls fn (or, with construct, constructs it with new.target nt) with
 * the elements of the array-like `list` as arguments
 * (CreateListFromArrayLike). The elements are gathered on the VM stack,
 * where they are roots, and handed over without an allocation between. */
static PxValue call_with_list(PxVM *vm, PxValue fn, PxValue this_val, PxValue list, int construct, PxValue nt) {
    PxValue  args[16], *buf = args, *base, r;
    PxIdx    len, i;
    if (!px_is_obj(list)) return px_throw_error(vm, PX_TYPE_ERROR, "the argument list must be an object");
    PX_ROOT(vm, fn);
    PX_ROOT(vm, this_val);
    PX_ROOT(vm, list);
    PX_ROOT(vm, nt);
    if (px_length_of(vm, list, &len) < 0) goto fail;
    base = vm->sp;
    if (len > 65535 || base + (size_t)len + 16 >= vm->stack_end) {
        px_throw_error(vm, PX_RANGE_ERROR, "too many arguments");
        goto fail;
    }
    for (i = 0; i < len; i++) {
        PxValue v = px_get_index(vm, list, i);
        if (v == PX_EXCEPTION) {
            vm->sp = base;
            goto fail;
        }
        *vm->sp++ = v;
    }
    vm->sp = base;
    /* px_call copies argv above vm->sp: move the values out of the way */
    if (len > PX_COUNTOF(args)) {
        buf = (PxValue *)malloc((size_t)len * sizeof(PxValue));
        if (!buf) {
            px_pop_roots(vm, 4);
            return px_throw_oom(vm);
        }
    }
    memcpy(buf, base, (size_t)len * sizeof(PxValue));
    r = construct ? px_construct_nt(vm, fn, (int)len, buf, nt) : px_call(vm, fn, this_val, (int)len, buf);
    if (buf != args) free(buf);
    px_pop_roots(vm, 4);
    return r;
fail:
    px_pop_roots(vm, 4);
    return PX_EXCEPTION;
}

/* ============================================================ Function */

static PxValue fn_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_throw_error(vm, PX_TYPE_ERROR, "the Function constructor is not supported (no runtime code generation)");
}

/* Function.prototype itself: callable, returns undefined. */
static PxValue fn_proto_call(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)t;
    (void)argc;
    (void)argv;
    return PX_UNDEFINED;
}

/* %ThrowTypeError%: the poisoned caller/arguments accessors */
static PxValue throw_type_error(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_throw_error(vm, PX_TYPE_ERROR, "'caller', 'callee' and 'arguments' may not be accessed in strict mode");
}

/* %ThrowTypeError% (frozen, its length and name not configurable) and
 * Function.prototype's caller and arguments accessors made of it. */
static int def_throw_type_error(PxVM *vm) {
    PxValue f = px_make_native(vm, throw_type_error, "", 0, 0), fp = vm->protos[PX_PROTO_FUNCTION], k;
    if (f == PX_EXCEPTION) return -1;
    vm->throw_type_error = f;
    ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
    if (px_define(vm, f, vm->atom[PX_ATOM_length], px_from_smi(0), 0) < 0 ||
        px_define(vm, f, vm->atom[PX_ATOM_name], vm->atom[PX_ATOM_empty], 0) < 0)
        return -1;
    ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_EXTENSIBLE;
    k = px_intern_cstr(vm, "caller");
    if (k == PX_EXCEPTION || px_define_accessor(vm, fp, k, f, f, PX_ATTR_CONFIGURABLE) < 0) return -1;
    k = px_intern_cstr(vm, "arguments");
    return k == PX_EXCEPTION ? -1 : px_define_accessor(vm, fp, k, f, f, PX_ATTR_CONFIGURABLE);
}

static PxValue fnp_call(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    if (!px_is_callable(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Function.prototype.call: not a function");
    return px_call(vm, t, ARG(0), argc > 1 ? argc - 1 : 0, argv + 1);
}

static PxValue fnp_apply(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    if (!px_is_callable(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Function.prototype.apply: not a function");
    if (ARG(1) == PX_UNDEFINED || ARG(1) == PX_NULL) return px_call(vm, t, ARG(0), 0, NULL);
    return call_with_list(vm, t, ARG(0), ARG(1), 0, PX_UNDEFINED);
}

static PxValue fnp_bind(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxBound *b;
    PxVec   *args = NULL;
    PxValue  av = 0, bv, proto, len, name;
    double   l = 0;
    int      i, r;
    if (!px_is_callable(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Function.prototype.bind: not a function");
    PX_ROOT(vm, t);
    proto = px_proto_of(vm, t);
    if (proto == PX_EXCEPTION) goto fail0;
    PX_ROOT(vm, proto);
    if (argc > 1) {
        args = px_vec_new(vm, (uint32_t)(argc - 1));
        if (!args) goto fail1;
        for (i = 1; i < argc; i++) args->items[i - 1] = argv[i];
        av = px_from_ptr(args);
    }
    PX_ROOT(vm, av);
    b = (PxBound *)px_obj_new(vm, PX_T_BOUND, sizeof(PxBound), proto);
    if (!b) goto fail2;
    b->target   = t;
    b->this_val = ARG(0);
    b->args     = args;
    b->nargs    = argc > 1 ? (uint32_t)(argc - 1) : 0;
    bv          = px_from_ptr(b);
    PX_ROOT(vm, bv);
    /* length: the target's own length, less the bound arguments */
    r = px_has_own(vm, t, vm->atom[PX_ATOM_length]);
    if (r < 0) goto fail3;
    if (r) {
        len = px_get(vm, t, vm->atom[PX_ATOM_length]);
        if (len == PX_EXCEPTION) goto fail3;
        if (px_is_num(len)) {
            l = px_num(len);
            if (l != l) l = 0;
            else if (l != INFINITY) {
                l = l < 0 ? ceil(l) : floor(l);
                l = l > (double)b->nargs ? l - (double)b->nargs : 0;
            }
        }
    }
    len = px_number(vm, l);
    if (len == PX_EXCEPTION || px_define(vm, bv, vm->atom[PX_ATOM_length], len, PX_ATTR_CONFIGURABLE) < 0) goto fail3;
    /* name: "bound " + the target's name */
    name = px_get(vm, t, vm->atom[PX_ATOM_name]);
    if (name == PX_EXCEPTION) goto fail3;
    if (!px_is_str(name)) name = vm->atom[PX_ATOM_empty];
    PX_ROOT(vm, name);
    {
        PxValue pre = px_str_from_cstr(vm, "bound ");
        name        = pre == PX_EXCEPTION ? pre : px_str_concat(vm, pre, name);
    }
    px_pop_roots(vm, 1);
    if (name == PX_EXCEPTION || px_define(vm, bv, vm->atom[PX_ATOM_name], name, PX_ATTR_CONFIGURABLE) < 0) goto fail3;
    px_pop_roots(vm, 4);
    return bv;
fail3:
    px_pop_roots(vm, 1);
fail2:
    px_pop_roots(vm, 1);
fail1:
    px_pop_roots(vm, 1);
fail0:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* A name NativeFunction syntax accepts as it is: an identifier, maybe
 * after "get " or "set ", or a [Symbol.x] name. */
static int plain_fn_name(const char *s) {
    if (!strncmp(s, "get ", 4) || !strncmp(s, "set ", 4)) s += 4;
    if (!strncmp(s, "[Symbol.", 8)) return 1;
    if (!(*s == '_' || *s == '$' || ((*s | 32) >= 'a' && (*s | 32) <= 'z'))) return 0;
    for (s++; *s; s++)
        if (!(*s == '_' || *s == '$' || (*s >= '0' && *s <= '9') || ((*s | 32) >= 'a' && (*s | 32) <= 'z'))) return 0;
    return 1;
}

/* No source text is kept (it would cost the whole source in memory on the
 * PSP), so every function prints in the NativeFunction form the spec
 * allows for functions without one. */
static PxValue fnp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char    buf[112], name[72] = "";
    PxValue n = PX_UNDEFINED;
    (void)argc;
    (void)argv;
    if (!px_is_callable(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Function.prototype.toString: not a function");
    switch (px_type_of(t)) {
    case PX_T_CLOSURE: n = ((PxClosure *)px_ptr(t))->proto->name; break;
    case PX_T_NATIVE: n = ((PxNative *)px_ptr(t))->name; break;
    default: break;
    }
    if (px_is_str(n)) px_str_to_utf8(vm, n, name, sizeof name);
    if (strlen(name) >= sizeof name - 1 || !plain_fn_name(name)) name[0] = 0;
    snprintf(buf, sizeof buf, "function %s() { [native code] }", name);
    return px_str_from_cstr(vm, buf);
}

/* Function.prototype[Symbol.hasInstance]: OrdinaryHasInstance */
static PxValue fnp_has_instance(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue v = ARG(0), proto, cur;
    if (!px_is_callable(t)) return PX_FALSE;
    while (px_type_of(t) == PX_T_BOUND) t = ((PxBound *)px_ptr(t))->target;
    if (!px_is_obj(v)) return PX_FALSE;
    PX_ROOT(vm, v);
    proto = px_get(vm, t, vm->atom[PX_ATOM_prototype]);
    px_pop_roots(vm, 1);
    if (proto == PX_EXCEPTION) return proto;
    if (!px_is_obj(proto)) return px_throw_error(vm, PX_TYPE_ERROR, "function has no prototype object");
    PX_ROOT(vm, proto);
    for (cur = v;;) {
        cur = px_proto_of(vm, cur);
        if (cur == PX_EXCEPTION || !px_is_obj(cur) || cur == proto) break;
    }
    px_pop_roots(vm, 1);
    return cur == PX_EXCEPTION ? cur : px_bool(cur == proto);
}

static const PxFnDef k_function_proto_fns[] = {
    {"apply", fnp_apply, 2, 0},
    {"bind", fnp_bind, 1, 0},
    {"call", fnp_call, 1, 0},
    {"toString", fnp_to_string, 0, 0},
};

/* ============================================================ Array */

#define MAX_SAFE_LEN 9007199254740991LL /* 2^53 - 1 */

/* A growable UTF-16 buffer for building strings in C. */
typedef struct SBuf {
    uint16_t *d;
    uint32_t  n, cap;
    int       oom;
} SBuf;

static void sb_put(SBuf *b, uint16_t c) {
    if (b->oom) return;
    if (b->n == b->cap) {
        uint32_t  cap = b->cap ? b->cap * 2 : 64;
        uint16_t *n   = (uint16_t *)realloc(b->d, cap * sizeof(uint16_t));
        if (!n || cap > (1u << 28)) {
            b->oom = 1;
            return;
        }
        b->d   = n;
        b->cap = cap;
    }
    b->d[b->n++] = c;
}

static int sb_put_str(PxVM *vm, SBuf *b, PxValue s) {
    PxString *f = px_str_flat(vm, s);
    uint32_t  i;
    if (!f) return -1;
    for (i = 0; i < f->len; i++) sb_put(b, px_str_at(f, i));
    return 0;
}

static PxValue sb_finish(PxVM *vm, SBuf *b) {
    PxValue r;
    if (b->oom) {
        free(b->d);
        return px_throw_error(vm, PX_RANGE_ERROR, "string too long");
    }
    r = px_str_new_u16(vm, b->d, b->n);
    free(b->d);
    return r;
}

/* CreateDataPropertyOrThrow(obj, i, v) */
static int create_index(PxVM *vm, PxValue obj, PxIdx i, PxValue v) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, v);
    k = index_key(vm, i);
    r = k == PX_EXCEPTION ? -1 : px_create_data_property(vm, obj, k, v);
    px_pop_roots(vm, 2);
    if (r == 0) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot define array index %lld", (long long)i);
        return -1;
    }
    return r < 0 ? -1 : 0;
}

/* An element of an array made here, not yet seen by any code (no setter,
 * no attributes to respect): stored directly. */
static int put_fresh(PxVM *vm, PxValue a, PxIdx i, PxValue v) {
    if (i <= PX_SMI_MAX) return px_define(vm, a, px_from_smi((int32_t)i), v, PX_ATTR_DEFAULT);
    return create_index(vm, a, i, v);
}

/* ArrayCreate(len) */
static PxValue array_create(PxVM *vm, PxIdx len) {
    PxValue a;
    if (len > 0xFFFFFFFFLL) return px_throw_error(vm, PX_RANGE_ERROR, "invalid array length");
    a = px_array_new(vm, len < 1024 ? (uint32_t)len : 0);
    if (a != PX_EXCEPTION) px_array_set_length(vm, (PxArray *)px_ptr(a), (uint32_t)len);
    return a;
}

/* ArraySpeciesCreate(o, len). *fresh: the result is an ordinary array
 * made here (put_fresh may fill it). */
static PxValue species_create(PxVM *vm, PxValue o, PxIdx len, int *fresh) {
    PxValue c, n, args[1];
    int     arr = px_is_array(vm, o);
    *fresh      = 0;
    if (arr < 0) return PX_EXCEPTION;
    if (arr) {
        c = px_get(vm, o, vm->atom[PX_ATOM_constructor]);
        if (c == PX_EXCEPTION) return c;
        if (px_is_obj(c)) {
            c = px_get(vm, c, vm->sym_species);
            if (c == PX_EXCEPTION) return c;
            if (c == PX_NULL) c = PX_UNDEFINED;
        }
        if (c != PX_UNDEFINED && c != vm->ctors[PX_PROTO_ARRAY]) {
            if (!px_is_constructor(c)) return px_throw_error(vm, PX_TYPE_ERROR, "the array species is not a constructor");
            PX_ROOT(vm, c);
            n = px_idx_value(vm, len);
            px_pop_roots(vm, 1);
            if (n == PX_EXCEPTION) return n;
            args[0] = n;
            return px_construct(vm, c, 1, args);
        }
    }
    *fresh = 1;
    return array_create(vm, len);
}

static int set_ret(PxVM *vm, PxValue a, PxIdx i, PxValue v, int fresh) {
    return fresh ? put_fresh(vm, a, i, v) : create_index(vm, a, i, v);
}

/* The Array constructor, as a function or with new. */
static PxValue arr_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue a, nt = vm->native_new_target;
    int     i;
    (void)t;
    if (nt == PX_UNDEFINED) nt = vm->native_callee;
    if (argc == 1) {
        if (px_is_num(argv[0])) {
            double   d = px_num(argv[0]);
            uint32_t n = (uint32_t)d;
            if (!(d >= 0 && d <= 4294967295.0) || (double)n != d) return px_throw_error(vm, PX_RANGE_ERROR, "invalid array length");
            a = array_create(vm, n);
        } else {
            a = array_create(vm, 0);
            if (a != PX_EXCEPTION) {
                PX_ROOT(vm, a);
                if (px_array_push(vm, a, argv[0]) < 0) a = PX_EXCEPTION;
                px_pop_roots(vm, 1);
            }
        }
    } else {
        a = px_array_new(vm, (uint32_t)argc);
        if (a == PX_EXCEPTION) return a;
        PX_ROOT(vm, a);
        for (i = 0; i < argc; i++)
            if (px_array_push(vm, a, argv[i]) < 0) {
                a = PX_EXCEPTION;
                break;
            }
        px_pop_roots(vm, 1);
    }
    if (a != PX_EXCEPTION && nt != vm->ctors[PX_PROTO_ARRAY]) {
        /* a subclass: the prototype comes from new.target */
        PxValue p;
        PX_ROOT(vm, a);
        p = proto_from_ctor(vm, nt, vm->protos[PX_PROTO_ARRAY]);
        if (p == PX_EXCEPTION || px_set_proto(vm, a, p) < 0) a = PX_EXCEPTION;
        px_pop_roots(vm, 1);
    }
    return a;
}

static PxValue arr_is_array(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int r = px_is_array(vm, ARG(0));
    (void)t;
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

/* new C(len) when C (the `this` of Array.of/from) is a constructor, else ArrayCreate */
static PxValue construct_this_or_array(PxVM *vm, PxValue c, int with_len, PxIdx len, int *fresh) {
    PxValue args[1];
    *fresh = 0;
    if (px_is_constructor(c) && c != vm->ctors[PX_PROTO_ARRAY]) {
        if (!with_len) return px_construct(vm, c, 0, NULL);
        args[0] = px_idx_value(vm, len);
        if (args[0] == PX_EXCEPTION) return args[0];
        return px_construct(vm, c, 1, args);
    }
    *fresh = 1;
    return array_create(vm, with_len ? len : 0);
}

static PxValue arr_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue a;
    int     i, fresh;
    a = construct_this_or_array(vm, t, 1, argc, &fresh);
    if (a == PX_EXCEPTION) return a;
    PX_ROOT(vm, a);
    for (i = 0; i < argc; i++)
        if (set_ret(vm, a, i, argv[i], fresh) < 0) goto fail;
    if (set_length(vm, a, argc) < 0) goto fail;
    px_pop_roots(vm, 1);
    return a;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arr_from(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue items = ARG(0), fn = ARG(1), this_arg = ARG(2), a = PX_UNDEFINED, m, rec = PX_UNDEFINED, v;
    PxIdx   k = 0, len;
    int     fresh, r;
    if (fn != PX_UNDEFINED && !px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "Array.from: the map function is not callable");
    if (items == PX_UNDEFINED || items == PX_NULL) return px_throw_error(vm, PX_TYPE_ERROR, "Array.from: expected an iterable or array-like");
    PX_ROOT(vm, a);
    PX_ROOT(vm, rec);
    m = px_get(vm, items, vm->sym_iterator);
    if (m == PX_EXCEPTION) goto fail;
    if (m != PX_UNDEFINED && m != PX_NULL) {
        /* the iterable path: GetIteratorFromMethod */
        PxValue it, next;
        PxVec  *rv;
        if (!px_is_callable(m)) {
            px_throw_error(vm, PX_TYPE_ERROR, "Array.from: [Symbol.iterator] is not a function");
            goto fail;
        }
        PX_ROOT(vm, m);
        a = construct_this_or_array(vm, t, 0, 0, &fresh);
        it = a == PX_EXCEPTION ? a : px_call(vm, m, items, 0, NULL);
        px_pop_roots(vm, 1);
        if (it == PX_EXCEPTION) goto fail;
        if (!px_is_obj(it)) {
            px_throw_error(vm, PX_TYPE_ERROR, "Array.from: the iterator is not an object");
            goto fail;
        }
        PX_ROOT(vm, it);
        next = px_get(vm, it, vm->atom[PX_ATOM_next]);
        if (next == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        PX_ROOT(vm, next);
        rv = px_vec_new(vm, 2);
        px_pop_roots(vm, 2);
        if (!rv) goto fail;
        rv->items[0] = it;
        rv->items[1] = next;
        rec          = px_from_ptr(rv);
        for (;;) {
            r = px_record_step(vm, rec, &v);
            if (r < 0) goto fail;
            if (r == 0) break;
            if (fn != PX_UNDEFINED) {
                PxValue args[2];
                args[0] = v;
                args[1] = px_idx_value(vm, k);
                if (args[1] == PX_EXCEPTION) goto close;
                v = px_call(vm, fn, this_arg, 2, args);
                if (v == PX_EXCEPTION) goto close;
            }
            if (set_ret(vm, a, k, v, fresh) < 0) goto close;
            k++;
        }
        if (set_length(vm, a, k) < 0) goto fail;
        px_pop_roots(vm, 2);
        return a;
    close:
        px_iterator_close_throw(vm, ((PxVec *)px_ptr(rec))->items[0]);
        goto fail;
    }
    /* the array-like path */
    items = px_to_object(vm, items);
    if (items == PX_EXCEPTION) goto fail;
    rec = items; /* keeps it rooted */
    if (px_length_of(vm, items, &len) < 0) goto fail;
    a = construct_this_or_array(vm, t, 1, len, &fresh);
    if (a == PX_EXCEPTION) goto fail;
    for (k = 0; k < len; k++) {
        v = px_get_index(vm, rec, k);
        if (v == PX_EXCEPTION) goto fail;
        if (fn != PX_UNDEFINED) {
            PxValue args[2];
            PX_ROOT(vm, v);
            args[0] = v;
            args[1] = px_idx_value(vm, k);
            v       = args[1] == PX_EXCEPTION ? args[1] : px_call(vm, fn, this_arg, 2, args);
            px_pop_roots(vm, 1);
            if (v == PX_EXCEPTION) goto fail;
        }
        if (set_ret(vm, a, k, v, fresh) < 0) goto fail;
    }
    if (set_length(vm, a, len) < 0) goto fail;
    px_pop_roots(vm, 2);
    return a;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

/* get Array[Symbol.species]() { return this } */
static PxValue arr_species(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return t;
}

/* The `this` of an Array.prototype method, as an object, and its length. */
#define THIS_OBJ()                                                                                   \
    PxValue o = px_to_object(vm, t);                                                                 \
    PxIdx   len;                                                                                     \
    if (o == PX_EXCEPTION) return o;                                                                 \
    PX_ROOT(vm, o);                                                                                  \
    if (px_length_of(vm, o, &len) < 0) {                                                             \
        px_pop_roots(vm, 1);                                                                         \
        return PX_EXCEPTION;                                                                         \
    }

static PxValue too_long(PxVM *vm) {
    return px_throw_error(vm, PX_TYPE_ERROR, "the array would be longer than 2^53 - 1");
}

/* 1 if o is an array whose next elements can be appended directly: its
 * own length writable, extensible, and Array.prototype and Object.prototype
 * without index properties to set through. */
static int plain_append(PxVM *vm, PxValue o) {
    PxObject *ob, *ap;
    uint32_t  i;
    if (px_type_of(o) != PX_T_ARRAY) return 0;
    ob = (PxObject *)px_ptr(o);
    if (ob->flags & (PX_OBJ_NOT_EXTENSIBLE | PX_OBJ_LENGTH_RO)) return 0;
    ap = (PxObject *)px_ptr(vm->protos[PX_PROTO_ARRAY]);
    if (!ob->shape->proto || ob->shape->proto != ap || ap->shape->proto != (PxObject *)px_ptr(vm->protos[PX_PROTO_OBJECT]))
        return 0;
    i = ((PxArray *)ob)->length;
    return ((PxArray *)ap)->length == 0 && (i > (uint32_t)PX_SMI_MAX ||
                                            (!px_own_slot(vm, ap, px_from_smi((int32_t)i), NULL) &&
                                             !px_own_slot(vm, ap->shape->proto, px_from_smi((int32_t)i), NULL)));
}

static PxValue arrp_push(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int i;
    THIS_OBJ();
    if (len + argc > MAX_SAFE_LEN) {
        px_pop_roots(vm, 1);
        return too_long(vm);
    }
    for (i = 0; i < argc; i++) {
        if (plain_append(vm, o) && len + i < 0xFFFFFFFFLL) {
            if (px_array_push(vm, o, argv[i]) < 0) goto fail;
        } else if (px_set_index(vm, o, len + i, argv[i]) < 0) goto fail;
    }
    if (set_length(vm, o, len + argc) < 0) goto fail;
    px_pop_roots(vm, 1);
    return px_idx_value(vm, len + argc);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_pop(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue v;
    (void)argc;
    (void)argv;
    {
        THIS_OBJ();
        if (len == 0) {
            if (set_length(vm, o, 0) < 0) goto fail;
            px_pop_roots(vm, 1);
            return PX_UNDEFINED;
        }
        v = px_get_index(vm, o, len - 1);
        if (v == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, v);
        if (delete_index(vm, o, len - 1) < 0 || set_length(vm, o, len - 1) < 0) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        px_pop_roots(vm, 2);
        return v;
    fail:
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
}

/* 1 if o is a dense array whose elements [0, n) are all present and plain,
 * so that moving them is a memmove: no holes, no getters, no attributes. */
static int dense_prefix(PxValue o, PxIdx n) {
    PxArray *a;
    uint32_t i;
    if (px_type_of(o) != PX_T_ARRAY) return 0;
    a = (PxArray *)px_ptr(o);
    if (n > a->length || !a->elems || n > a->elems->cap || (a->obj.flags & PX_OBJ_NOT_EXTENSIBLE)) return 0;
    for (i = 0; i < (uint32_t)n; i++)
        if (a->elems->items[i] == PX_HOLE) return 0;
    return 1;
}

/* Moves [from, from+count) to `to` (overlapping ok), holes preserved:
 * the loops of shift, unshift, splice and copyWithin. */
static int move_range(PxVM *vm, PxValue o, PxIdx from, PxIdx to, PxIdx count) {
    PxIdx k;
    if (dense_prefix(o, (from > to ? from : to) + count)) {
        PxArray *a = (PxArray *)px_ptr(o);
        memmove(&a->elems->items[(uint32_t)to], &a->elems->items[(uint32_t)from], (size_t)count * sizeof(PxValue));
        return 0;
    }
    for (k = 0; k < count; k++) {
        PxIdx src = to < from ? from + k : from + count - 1 - k, dst = to < from ? to + k : to + count - 1 - k;
        int   h   = has_index(vm, o, src);
        if (h < 0) return -1;
        if (h) {
            PxValue v = px_get_index(vm, o, src);
            if (v == PX_EXCEPTION || px_set_index(vm, o, dst, v) < 0) return -1;
        } else if (delete_index(vm, o, dst) < 0) return -1;
    }
    return 0;
}

static PxValue arrp_shift(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue v;
    (void)argc;
    (void)argv;
    {
        THIS_OBJ();
        if (len == 0) {
            if (set_length(vm, o, 0) < 0) goto fail;
            px_pop_roots(vm, 1);
            return PX_UNDEFINED;
        }
        v = px_get_index(vm, o, 0);
        if (v == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, v);
        if (move_range(vm, o, 1, 0, len - 1) < 0 || delete_index(vm, o, len - 1) < 0 || set_length(vm, o, len - 1) < 0) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        px_pop_roots(vm, 2);
        return v;
    fail:
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
}

static PxValue arrp_unshift(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int i;
    THIS_OBJ();
    if (argc > 0) {
        if (len + argc > MAX_SAFE_LEN) {
            px_pop_roots(vm, 1);
            return too_long(vm);
        }
        if (plain_append(vm, o) && dense_prefix(o, len) && len + argc <= 0xFFFFFFFFLL) {
            /* grow in place, then slide the elements up */
            for (i = 0; i < argc; i++)
                if (px_array_push(vm, o, PX_UNDEFINED) < 0) goto fail;
            if (move_range(vm, o, 0, argc, len) < 0) goto fail;
        } else if (move_range(vm, o, 0, argc, len) < 0) goto fail;
        for (i = 0; i < argc; i++)
            if (px_set_index(vm, o, i, argv[i]) < 0) goto fail;
    }
    if (set_length(vm, o, len + argc) < 0) goto fail;
    px_pop_roots(vm, 1);
    return px_idx_value(vm, len + argc);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_slice(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue out;
    PxIdx   start, end, k, n = 0;
    int     fresh;
    THIS_OBJ();
    if (rel_index(vm, ARG(0), len, 0, &start) < 0 || rel_index(vm, ARG(1), len, len, &end) < 0) goto fail;
    out = species_create(vm, o, end > start ? end - start : 0, &fresh);
    if (out == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, out);
    for (k = start; k < end; k++, n++) {
        int h = has_index(vm, o, k);
        if (h < 0) goto fail2;
        if (h) {
            PxValue v = px_get_index(vm, o, k);
            if (v == PX_EXCEPTION || set_ret(vm, out, n, v, fresh) < 0) goto fail2;
        }
    }
    if (set_length(vm, out, n) < 0) goto fail2;
    px_pop_roots(vm, 2);
    return out;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* ToIntegerOrInfinity(v) clamped to [0, hi] (splice's deleteCount, ...) */
static int clamp_count(PxVM *vm, PxValue v, PxIdx hi, PxIdx *out) {
    double d;
    if (px_to_number(vm, v, &d) < 0) return -1;
    if (isnan(d) || d < 0) d = 0;
    *out = d > (double)hi ? hi : (PxIdx)d;
    return 0;
}

static PxValue arrp_splice(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue removed;
    PxIdx   start, del, k, ins = argc > 2 ? argc - 2 : 0;
    int     fresh;
    THIS_OBJ();
    if (rel_index(vm, ARG(0), len, 0, &start) < 0) goto fail;
    if (argc == 0) del = 0;
    else if (argc == 1) del = len - start;
    else if (clamp_count(vm, argv[1], len - start, &del) < 0) goto fail;
    if (len + ins - del > MAX_SAFE_LEN) {
        px_pop_roots(vm, 1);
        return too_long(vm);
    }
    removed = species_create(vm, o, del, &fresh);
    if (removed == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, removed);
    for (k = 0; k < del; k++) {
        int h = has_index(vm, o, start + k);
        if (h < 0) goto fail2;
        if (h) {
            PxValue v = px_get_index(vm, o, start + k);
            if (v == PX_EXCEPTION || set_ret(vm, removed, k, v, fresh) < 0) goto fail2;
        }
    }
    if (set_length(vm, removed, del) < 0) goto fail2;
    if (ins < del) {
        if (move_range(vm, o, start + del, start + ins, len - start - del) < 0) goto fail2;
        for (k = len; k > len - del + ins; k--)
            if (delete_index(vm, o, k - 1) < 0) goto fail2;
    } else if (ins > del) {
        if (move_range(vm, o, start + del, start + ins, len - start - del) < 0) goto fail2;
    }
    for (k = 0; k < ins; k++)
        if (px_set_index(vm, o, start + k, argv[2 + (int)k]) < 0) goto fail2;
    if (set_length(vm, o, len - del + ins) < 0) goto fail2;
    px_pop_roots(vm, 2);
    return removed;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* IsConcatSpreadable */
static int spreadable(PxVM *vm, PxValue v) {
    PxValue s;
    if (!px_is_obj(v)) return 0;
    PX_ROOT(vm, v);
    s = px_get(vm, v, vm->sym_is_concat_spreadable);
    px_pop_roots(vm, 1);
    if (s == PX_EXCEPTION) return -1;
    if (s != PX_UNDEFINED) return px_truthy(s);
    return px_is_array(vm, v);
}

static PxValue arrp_concat(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue out;
    PxIdx   n = 0;
    int     i, fresh;
    t = px_to_object(vm, t);
    if (t == PX_EXCEPTION) return t;
    PX_ROOT(vm, t);
    out = species_create(vm, t, 0, &fresh);
    if (out == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return out;
    }
    PX_ROOT(vm, out);
    for (i = -1; i < argc; i++) {
        PxValue item = i < 0 ? t : argv[i];
        int     sp   = spreadable(vm, item);
        if (sp < 0) goto fail;
        if (sp) {
            PxIdx len, k;
            if (px_length_of(vm, item, &len) < 0) goto fail;
            if (n + len > MAX_SAFE_LEN) {
                too_long(vm);
                goto fail;
            }
            for (k = 0; k < len; k++) {
                int h = has_index(vm, item, k);
                if (h < 0) goto fail;
                if (h) {
                    PxValue v = px_get_index(vm, item, k);
                    if (v == PX_EXCEPTION || set_ret(vm, out, n + k, v, fresh) < 0) goto fail;
                }
            }
            n += len;
        } else {
            if (n >= MAX_SAFE_LEN) {
                too_long(vm);
                goto fail;
            }
            if (set_ret(vm, out, n, item, fresh) < 0) goto fail;
            n++;
        }
    }
    if (set_length(vm, out, n) < 0) goto fail;
    px_pop_roots(vm, 2);
    return out;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

/* join (0), toLocaleString (1) */
static PxValue arrp_join(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue sep = MAGIC ? PX_UNDEFINED : ARG(0);
    SBuf    b   = {0};
    PxIdx   k;
    THIS_OBJ();
    if (sep == PX_UNDEFINED) sep = px_str_from_cstr(vm, ",");
    else sep = px_to_string(vm, sep);
    if (sep == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, sep);
    for (k = 0; k < len; k++) {
        PxValue v;
        if (k > 0 && sb_put_str(vm, &b, sep) < 0) goto fail2;
        v = px_get_index(vm, o, k);
        if (v == PX_EXCEPTION) goto fail2;
        if (v == PX_UNDEFINED || v == PX_NULL) continue;
        if (MAGIC) {
            /* Invoke(element, "toLocaleString") */
            PxValue f;
            PX_ROOT(vm, v);
            f = px_get(vm, v, px_intern_cstr(vm, "toLocaleString"));
            if (f != PX_EXCEPTION && !px_is_callable(f)) f = px_throw_error(vm, PX_TYPE_ERROR, "toLocaleString is not a function");
            v = f == PX_EXCEPTION ? f : px_call(vm, f, v, 0, NULL);
            px_pop_roots(vm, 1);
            if (v == PX_EXCEPTION) goto fail2;
        }
        v = px_to_string(vm, v);
        if (v == PX_EXCEPTION || sb_put_str(vm, &b, v) < 0) goto fail2;
    }
    px_pop_roots(vm, 2);
    return sb_finish(vm, &b);
fail2:
    px_pop_roots(vm, 1);
fail:
    free(b.d);
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = px_to_object(vm, t), f;
    (void)argc;
    (void)argv;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    f = px_get(vm, o, px_intern_cstr(vm, "join"));
    px_pop_roots(vm, 1);
    if (f == PX_EXCEPTION) return f;
    return px_is_callable(f) ? px_call(vm, f, o, 0, NULL) : objp_to_string(vm, o, 0, NULL);
}

static PxValue arrp_reverse(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx lo, hi;
    (void)argc;
    (void)argv;
    {
        THIS_OBJ();
        for (lo = 0, hi = len - 1; lo < hi; lo++, hi--) {
            PxValue a = PX_UNDEFINED, b = PX_UNDEFINED;
            int     ha, hb;
            PX_ROOT(vm, a);
            PX_ROOT(vm, b);
            if ((ha = has_index(vm, o, lo)) < 0 || (ha && (a = px_get_index(vm, o, lo)) == PX_EXCEPTION) ||
                (hb = has_index(vm, o, hi)) < 0 || (hb && (b = px_get_index(vm, o, hi)) == PX_EXCEPTION) ||
                (hb ? px_set_index(vm, o, lo, b) : ha ? delete_index(vm, o, lo) : 0) < 0 ||
                (ha ? px_set_index(vm, o, hi, a) : hb ? delete_index(vm, o, hi) : 0) < 0) {
                px_pop_roots(vm, 2);
                goto fail;
            }
            px_pop_roots(vm, 2);
        }
        px_pop_roots(vm, 1);
        return o;
    fail:
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
}

/* The start index of indexOf/includes: fromIndex relative to len, clamped
 * to [0, len]; lastIndexOf: to [-1, len - 1]. */
static int from_index(PxVM *vm, PxValue v, PxIdx len, int last, PxIdx *out) {
    double d;
    if (px_to_number(vm, v, &d) < 0) return -1;
    d = isnan(d) ? 0 : trunc(d);
    if (last) {
        if (d < 0) d += (double)len;
        *out = d < 0 ? -1 : d >= (double)len ? len - 1 : (PxIdx)d;
    } else {
        if (d < 0) d += (double)len;
        *out = d < 0 ? 0 : d > (double)len ? len : (PxIdx)d;
    }
    return 0;
}

/* indexOf (0), lastIndexOf (1), includes (2). */
static PxValue arrp_index_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue x = ARG(0);
    PxIdx   k, from;
    int     mode = MAGIC;
    THIS_OBJ();
    if (len == 0) {
        px_pop_roots(vm, 1);
        return mode == 2 ? PX_FALSE : px_from_smi(-1);
    }
    if (mode == 1) from = len - 1;
    else from = 0;
    if (argc > 1 && from_index(vm, argv[1], len, mode == 1, &from) < 0) goto fail;
    PX_ROOT(vm, x);
    for (k = from; mode == 1 ? k >= 0 : k < len; k += mode == 1 ? -1 : 1) {
        PxValue v;
        if (mode != 2) {
            int h = has_index(vm, o, k);
            if (h < 0) goto fail2;
            if (!h) continue;
        }
        v = px_get_index(vm, o, k);
        if (v == PX_EXCEPTION) goto fail2;
        if (mode == 2 ? px_same_value_zero(vm, v, x) : px_strict_equals(vm, v, x)) {
            px_pop_roots(vm, 2);
            return mode == 2 ? PX_TRUE : px_idx_value(vm, k);
        }
    }
    px_pop_roots(vm, 2);
    return mode == 2 ? PX_FALSE : px_from_smi(-1);
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* The callback family. magic: */
enum { IT_FOREACH, IT_MAP, IT_FILTER, IT_SOME, IT_EVERY, IT_FIND, IT_FINDINDEX, IT_FINDLAST, IT_FINDLASTINDEX };

static PxValue arrp_iterate(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue fn = ARG(0), this_arg = ARG(1), out = PX_UNDEFINED;
    PxIdx   k, n = 0;
    int     mode = MAGIC, backwards = mode == IT_FINDLAST || mode == IT_FINDLASTINDEX, fresh = 0;
    THIS_OBJ();
    if (require_callable(vm, fn, "Array iteration") == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, out);
    if (mode == IT_MAP || mode == IT_FILTER) {
        out = species_create(vm, o, mode == IT_MAP ? len : 0, &fresh);
        if (out == PX_EXCEPTION) goto fail2;
    }
    for (k = backwards ? len - 1 : 0; backwards ? k >= 0 : k < len; k += backwards ? -1 : 1) {
        PxValue args[3], r;
        if (mode <= IT_EVERY) { /* these skip holes; the find family does not */
            int h = has_index(vm, o, k);
            if (h < 0) goto fail2;
            if (!h) continue;
        }
        args[0] = px_get_index(vm, o, k);
        if (args[0] == PX_EXCEPTION) goto fail2;
        PX_ROOT(vm, args[0]);
        args[1] = px_idx_value(vm, k);
        if (args[1] == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail2;
        }
        args[2] = o;
        r       = px_call(vm, fn, this_arg, 3, args);
        if (r == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail2;
        }
        switch (mode) {
        case IT_MAP:
            if (set_ret(vm, out, k, r, fresh) < 0) {
                px_pop_roots(vm, 1);
                goto fail2;
            }
            break;
        case IT_FILTER:
            if (px_truthy(r) && set_ret(vm, out, n++, args[0], fresh) < 0) {
                px_pop_roots(vm, 1);
                goto fail2;
            }
            break;
        case IT_SOME:
            if (px_truthy(r)) {
                px_pop_roots(vm, 3);
                return PX_TRUE;
            }
            break;
        case IT_EVERY:
            if (!px_truthy(r)) {
                px_pop_roots(vm, 3);
                return PX_FALSE;
            }
            break;
        case IT_FIND:
        case IT_FINDLAST:
            if (px_truthy(r)) {
                PxValue v = args[0];
                px_pop_roots(vm, 3);
                return v;
            }
            break;
        case IT_FINDINDEX:
        case IT_FINDLASTINDEX:
            if (px_truthy(r)) {
                px_pop_roots(vm, 3);
                return px_idx_value(vm, k);
            }
            break;
        default: break;
        }
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 2);
    switch (mode) {
    case IT_MAP:
    case IT_FILTER: return out;
    case IT_SOME: return PX_FALSE;
    case IT_EVERY: return PX_TRUE;
    case IT_FINDINDEX:
    case IT_FINDLASTINDEX: return px_from_smi(-1);
    default: return PX_UNDEFINED;
    }
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_reduce(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue fn = ARG(0), acc = PX_UNDEFINED;
    PxIdx   k;
    int     right = MAGIC, have = argc > 1;
    THIS_OBJ();
    if (require_callable(vm, fn, "reduce") == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, acc);
    if (have) acc = argv[1];
    for (k = right ? len - 1 : 0; right ? k >= 0 : k < len; k += right ? -1 : 1) {
        PxValue args[4];
        int     h = has_index(vm, o, k);
        if (h < 0) goto fail2;
        if (!h) continue;
        if (!have) {
            acc = px_get_index(vm, o, k);
            if (acc == PX_EXCEPTION) goto fail2;
            have = 1;
            continue;
        }
        args[0] = acc;
        args[1] = px_get_index(vm, o, k);
        if (args[1] == PX_EXCEPTION) goto fail2;
        PX_ROOT(vm, args[1]);
        args[2] = px_idx_value(vm, k);
        args[3] = o;
        if (args[2] == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail2;
        }
        acc = px_call(vm, fn, PX_UNDEFINED, 4, args);
        px_pop_roots(vm, 1);
        if (acc == PX_EXCEPTION) goto fail2;
    }
    px_pop_roots(vm, 2);
    if (!have) return px_throw_error(vm, PX_TYPE_ERROR, "reduce of an empty array with no initial value");
    return acc;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_fill(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx start, end, k;
    THIS_OBJ();
    if (rel_index(vm, ARG(1), len, 0, &start) < 0 || rel_index(vm, ARG(2), len, len, &end) < 0) goto fail;
    for (k = start; k < end; k++)
        if (px_set_index(vm, o, k, ARG(0)) < 0) goto fail;
    px_pop_roots(vm, 1);
    return o;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue arrp_at(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d;
    THIS_OBJ();
    if (px_to_number(vm, ARG(0), &d) < 0) goto fail;
    d = isnan(d) ? 0 : trunc(d);
    if (d < 0) d += (double)len;
    if (d < 0 || d >= (double)len) {
        px_pop_roots(vm, 1);
        return PX_UNDEFINED;
    }
    {
        PxValue v = px_get_index(vm, o, (PxIdx)d);
        px_pop_roots(vm, 1);
        return v;
    }
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* FlattenIntoArray. Nesting recurses in C, bounded like native calls. */
static int flatten_into(PxVM *vm, PxValue out, PxIdx *n, PxValue src, PxIdx len, double depth, PxValue fn,
                        PxValue this_arg, int fresh) {
    PxIdx k;
    for (k = 0; k < len; k++) {
        PxValue v;
        int     h = has_index(vm, src, k), sp = 0;
        if (h < 0) return -1;
        if (!h) continue;
        v = px_get_index(vm, src, k);
        if (v == PX_EXCEPTION) return -1;
        if (fn != PX_UNDEFINED) {
            PxValue args[3];
            PX_ROOT(vm, v);
            args[0] = v;
            args[1] = px_idx_value(vm, k);
            args[2] = src;
            v       = args[1] == PX_EXCEPTION ? args[1] : px_call(vm, fn, this_arg, 3, args);
            px_pop_roots(vm, 1);
            if (v == PX_EXCEPTION) return -1;
        }
        if (depth > 0) {
            sp = px_is_array(vm, v);
            if (sp < 0) return -1;
        }
        if (sp) {
            PxIdx sub;
            int   r;
            if (vm->native_depth >= vm->max_native_depth) {
                px_throw_error(vm, PX_RANGE_ERROR, "flat: nesting too deep");
                return -1;
            }
            PX_ROOT(vm, v);
            r = px_length_of(vm, v, &sub);
            if (r == 0) {
                vm->native_depth++;
                r = flatten_into(vm, out, n, v, sub, depth - 1, PX_UNDEFINED, PX_UNDEFINED, fresh);
                vm->native_depth--;
            }
            px_pop_roots(vm, 1);
            if (r < 0) return -1;
        } else {
            if (*n >= MAX_SAFE_LEN) {
                too_long(vm);
                return -1;
            }
            if (set_ret(vm, out, (*n)++, v, fresh) < 0) return -1;
        }
    }
    return 0;
}

/* flat (0), flatMap (1) */
static PxValue arrp_flat(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue out, fn = PX_UNDEFINED;
    double  depth = 1;
    PxIdx   n     = 0;
    int     fresh;
    THIS_OBJ();
    if (MAGIC == 1) {
        fn = ARG(0);
        if (require_callable(vm, fn, "flatMap") == PX_EXCEPTION) goto fail;
    } else if (ARG(0) != PX_UNDEFINED) {
        if (px_to_number(vm, ARG(0), &depth) < 0) goto fail;
        depth = isnan(depth) || depth < 0 ? 0 : trunc(depth);
    }
    out = species_create(vm, o, 0, &fresh);
    if (out == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, out);
    if (flatten_into(vm, out, &n, o, len, depth, fn, ARG(1), fresh) < 0) goto fail2;
    px_pop_roots(vm, 2);
    return out;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* sort: SortIndexedProperties with a stable merge sort. Holes and
 * undefined are set aside; the default order compares strings. */
typedef struct SortCtx {
    PxVM   *vm;
    PxValue cmp;
    int     err;
} SortCtx;

static int sort_less(SortCtx *s, PxValue a, PxValue b) {
    PxVM *vm = s->vm;
    if (s->err) return 0;
    if (s->cmp != PX_UNDEFINED) {
        PxValue args[2] = {a, b}, r;
        double  d;
        r = px_call(vm, s->cmp, PX_UNDEFINED, 2, args);
        if (px_is_smi(r)) return px_smi(r) < 0;
        if (r == PX_EXCEPTION || px_to_number(vm, r, &d) < 0) {
            s->err = 1;
            return 0;
        }
        return d < 0;
    }
    if (px_is_smi(a) && px_is_smi(b)) {
        /* compare as decimal strings without making them */
        char x[16], y[16];
        px_itoa(px_smi(a), x);
        px_itoa(px_smi(b), y);
        return strcmp(x, y) < 0;
    }
    {
        PxValue sa, sb;
        int     c;
        PX_ROOT(vm, b);
        sa = px_to_string(vm, a);
        if (sa == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            s->err = 1;
            return 0;
        }
        PX_ROOT(vm, sa);
        sb = px_to_string(vm, b);
        if (sb == PX_EXCEPTION || px_str_cmp(vm, sa, sb, &c) < 0) {
            px_pop_roots(vm, 2);
            s->err = 1;
            return 0;
        }
        px_pop_roots(vm, 2);
        return c < 0;
    }
}

static void merge_sort(SortCtx *s, PxValue *v, PxValue *tmp, uint32_t n) {
    uint32_t width, i;
    /* bottom-up: no recursion */
    for (width = 1; width < n && !s->err; width *= 2) {
        for (i = 0; i < n && !s->err; i += 2 * width) {
            uint32_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                if (sort_less(s, v[b], v[a])) tmp[k++] = v[b++];
                else tmp[k++] = v[a++];
                if (s->err) return;
            }
            while (a < mid) tmp[k++] = v[a++];
            while (b < hi) tmp[k++] = v[b++];
        }
        memcpy(v, tmp, n * sizeof(PxValue));
    }
}

/* Reads o[0, len) (holes skipped, undefined counted), sorts, and returns
 * the sorted values in a vec (rooted in *vv); *n values, *undefs undefined. */
static int sort_values(PxVM *vm, PxValue o, PxIdx len, PxValue cmp, int skip_holes, PxValue *vv, uint32_t *n,
                       uint32_t *undefs) {
    PxVec  *vals, *tmp;
    PxValue tv;
    PxIdx   k;
    SortCtx s;
    *n = *undefs = 0;
    if (len > (1 << 24)) {
        px_throw_error(vm, PX_RANGE_ERROR, "sort: array too long");
        return -1;
    }
    vals = px_vec_new(vm, (uint32_t)len + 1);
    if (!vals) return -1;
    *vv = px_from_ptr(vals);
    for (k = 0; k < len; k++) {
        PxValue v;
        if (skip_holes) {
            int h = has_index(vm, o, k);
            if (h < 0) return -1;
            if (!h) continue;
        }
        v = px_get_index(vm, o, k);
        if (v == PX_EXCEPTION) return -1;
        if (v == PX_UNDEFINED) (*undefs)++;
        else ((PxVec *)px_ptr(*vv))->items[(*n)++] = v;
    }
    tmp = px_vec_new(vm, *n + 1);
    if (!tmp) return -1;
    tv = px_from_ptr(tmp);
    PX_ROOT(vm, tv);
    s.vm  = vm;
    s.cmp = cmp;
    s.err = 0;
    merge_sort(&s, ((PxVec *)px_ptr(*vv))->items, tmp->items, *n);
    px_pop_roots(vm, 1);
    return s.err ? -1 : 0;
}

static PxValue arrp_sort(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  cmp = ARG(0), vv = PX_UNDEFINED;
    PxIdx    k;
    uint32_t n, undefs;
    if (cmp != PX_UNDEFINED && !px_is_callable(cmp))
        return px_throw_error(vm, PX_TYPE_ERROR, "sort: the comparator must be a function");
    {
        THIS_OBJ();
        PX_ROOT(vm, vv);
        if (sort_values(vm, o, len, cmp, 1, &vv, &n, &undefs) < 0) goto fail;
        for (k = 0; k < n; k++)
            if (px_set_index(vm, o, k, ((PxVec *)px_ptr(vv))->items[(uint32_t)k]) < 0) goto fail;
        for (; k < (PxIdx)n + undefs; k++)
            if (px_set_index(vm, o, k, PX_UNDEFINED) < 0) goto fail;
        for (; k < len; k++)
            if (delete_index(vm, o, k) < 0) goto fail;
        px_pop_roots(vm, 2);
        return o;
    fail:
        px_pop_roots(vm, 2);
        return PX_EXCEPTION;
    }
}

static const PxFnDef k_array_fns[] = {
    {"isArray", arr_is_array, 1, 0},
    {"of", arr_of, 0, 0},
    {"from", arr_from, 1, 0},
};

static const PxFnDef k_array_proto_fns[] = {
    {"at", arrp_at, 1, 0},
    {"concat", arrp_concat, 1, 0},
    {"every", arrp_iterate, 1, IT_EVERY},
    {"fill", arrp_fill, 1, 0},
    {"filter", arrp_iterate, 1, IT_FILTER},
    {"find", arrp_iterate, 1, IT_FIND},
    {"findIndex", arrp_iterate, 1, IT_FINDINDEX},
    {"findLast", arrp_iterate, 1, IT_FINDLAST},
    {"findLastIndex", arrp_iterate, 1, IT_FINDLASTINDEX},
    {"flat", arrp_flat, 0, 0},
    {"flatMap", arrp_flat, 1, 1},
    {"forEach", arrp_iterate, 1, IT_FOREACH},
    {"includes", arrp_index_of, 1, 2},
    {"indexOf", arrp_index_of, 1, 0},
    {"join", arrp_join, 1, 0},
    {"lastIndexOf", arrp_index_of, 1, 1},
    {"map", arrp_iterate, 1, IT_MAP},
    {"pop", arrp_pop, 0, 0},
    {"push", arrp_push, 1, 0},
    {"reduce", arrp_reduce, 1, 0},
    {"reduceRight", arrp_reduce, 1, 1},
    {"reverse", arrp_reverse, 0, 0},
    {"shift", arrp_shift, 0, 0},
    {"slice", arrp_slice, 2, 0},
    {"some", arrp_iterate, 1, IT_SOME},
    {"sort", arrp_sort, 1, 0},
    {"splice", arrp_splice, 2, 0},
    {"toLocaleString", arrp_join, 0, 1},
    {"toString", arrp_to_string, 0, 0},
    {"unshift", arrp_unshift, 1, 0},
};

/* ============================================================ String */

static PxValue this_string(PxVM *vm, PxValue t) {
    if (px_is_str(t)) return t;
    if (t == PX_UNDEFINED || t == PX_NULL)
        return px_throw_error(vm, PX_TYPE_ERROR, "String.prototype method called on null or undefined");
    return px_to_string(vm, t);
}

#define THIS_STR()                                                                                   \
    PxValue   sv = this_string(vm, t);                                                               \
    PxString *s;                                                                                     \
    if (sv == PX_EXCEPTION) return sv;                                                               \
    PX_ROOT(vm, sv);                                                                                 \
    s = px_str_flat(vm, sv);                                                                         \
    if (!s) {                                                                                        \
        px_pop_roots(vm, 1);                                                                         \
        return PX_EXCEPTION;                                                                         \
    }

#define RESTR() (s = px_str_flat(vm, sv)) /* after an allocation, re-read (cheap: already flat) */

/* A String/Number/Boolean object for `new`, its prototype from new.target. */
static PxValue box_for_new(PxVM *vm, PxValue value, int proto_index) {
    PxValue o;
    if (value == PX_EXCEPTION) return value;
    PX_ROOT(vm, value);
    o = new_with_proto(vm, PX_T_BOXED, sizeof(PxBoxed), vm->native_new_target, proto_index);
    px_pop_roots(vm, 1);
    if (o != PX_EXCEPTION) ((PxBoxed *)px_ptr(o))->value = value;
    return o;
}

static PxValue str_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (vm->native_new_target != PX_UNDEFINED)
        return box_for_new(vm, argc == 0 ? vm->atom[PX_ATOM_empty] : px_to_string(vm, argv[0]), PX_PROTO_STRING);
    if (argc == 0) return vm->atom[PX_ATOM_empty];
    if (px_is_ptr(argv[0]) && px_type_of(argv[0]) == PX_T_SYMBOL) {
        /* String(symbol) is allowed: "Symbol(desc)" */
        PxValue d = ((PxSymbol *)px_ptr(argv[0]))->description, r, open;
        open = px_str_from_cstr(vm, "Symbol(");
        if (open == PX_EXCEPTION) return open;
        PX_ROOT(vm, open);
        r = px_str_concat(vm, open, px_is_str(d) ? d : vm->atom[PX_ATOM_empty]);
        px_pop_roots(vm, 1);
        if (r == PX_EXCEPTION) return r;
        PX_ROOT(vm, r);
        open = px_str_from_cstr(vm, ")");
        px_pop_roots(vm, 1);
        return open == PX_EXCEPTION ? open : px_str_concat(vm, r, open);
    }
    return px_to_string(vm, argv[0]);
}

static PxValue str_from_char_code(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    SBuf b = {0};
    int  i;
    (void)t;
    for (i = 0; i < argc; i++) {
        double d;
        if (px_to_number(vm, argv[i], &d) < 0) {
            free(b.d);
            return PX_EXCEPTION;
        }
        if (MAGIC == 1) {
            uint32_t cp;
            if (d < 0 || d > 0x10FFFF || d != floor(d)) {
                free(b.d);
                return px_throw_error(vm, PX_RANGE_ERROR, "invalid code point");
            }
            cp = (uint32_t)d;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                sb_put(&b, (uint16_t)(0xD800 + (cp >> 10)));
                sb_put(&b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
                continue;
            }
            sb_put(&b, (uint16_t)cp);
        } else {
            int32_t x;
            if (px_to_int32(vm, argv[i], &x) < 0) {
                free(b.d);
                return PX_EXCEPTION;
            }
            sb_put(&b, (uint16_t)x);
        }
    }
    return sb_finish(vm, &b);
}

static PxValue strp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (px_is_str(t)) return t;
    if (px_is_obj(t) && px_type_of(t) == PX_T_BOXED && px_is_str(((PxBoxed *)px_ptr(t))->value))
        return ((PxBoxed *)px_ptr(t))->value;
    return px_throw_error(vm, PX_TYPE_ERROR, "String.prototype.toString called on a non-string");
}

/* charAt (0), charCodeAt (1), codePointAt (2), at (3) */
static PxValue strp_char_at(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx    d;
    uint16_t c;
    THIS_STR();
    if (px_is_smi(ARG(0))) {
        d = px_smi(ARG(0));
    } else {
        double x = 0;
        if (px_to_number(vm, ARG(0), &x) < 0) {
            px_pop_roots(vm, 1);
            return PX_EXCEPTION;
        }
        x = isnan(x) ? 0 : trunc(x);
        d = x < -4294967296.0 ? -4294967296LL : x > 4294967296.0 ? 4294967296LL : (PxIdx)x;
    }
    px_pop_roots(vm, 1);
    RESTR();
    if (MAGIC == 3 && d < 0) d += s->len;
    if (d < 0 || d >= s->len) return MAGIC == 0 ? vm->atom[PX_ATOM_empty] : MAGIC == 1 ? px_number(vm, NAN) : PX_UNDEFINED;
    c = px_str_at(s, (uint32_t)d);
    if (MAGIC == 1) return px_from_smi(c);
    if (MAGIC == 2) {
        if (c >= 0xD800 && c <= 0xDBFF && d + 1 < s->len) {
            uint16_t lo = px_str_at(s, (uint32_t)d + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) return px_from_smi(0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00));
        }
        return px_from_smi(c);
    }
    return px_str_new_u16(vm, &c, 1);
}

static int64_t str_find(const PxString *s, const PxString *p, int64_t from, int backwards) {
    int64_t i;
    if (p->len > s->len) return -1;
    if (backwards) {
        if (from > (int64_t)(s->len - p->len)) from = s->len - p->len;
        for (i = from; i >= 0; i--) {
            uint32_t j;
            for (j = 0; j < p->len && px_str_at(s, (uint32_t)i + j) == px_str_at(p, j); j++) {}
            if (j == p->len) return i;
        }
        return -1;
    }
    for (i = from < 0 ? 0 : from; i + p->len <= s->len; i++) {
        uint32_t j;
        for (j = 0; j < p->len && px_str_at(s, (uint32_t)i + j) == px_str_at(p, j); j++) {}
        if (j == p->len) return i;
    }
    return -1;
}

/* IsRegExp: 1, 0, or -1 on exception */
static int is_regexp_arg(PxVM *vm, PxValue v) {
    PxValue m;
    if (!px_is_obj(v)) return 0;
    PX_ROOT(vm, v);
    m = px_get(vm, v, vm->sym_match);
    px_pop_roots(vm, 1);
    if (m == PX_EXCEPTION) return -1;
    if (m != PX_UNDEFINED) return px_truthy(m);
    return px_is_regexp(v);
}

/* indexOf (0), lastIndexOf (1), includes (2), startsWith (3), endsWith (4) */
static PxValue strp_index_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   pv;
    PxString *p;
    double    pos;
    int64_t   r;
    THIS_STR();
    if (MAGIC >= 2) {
        /* includes, startsWith, endsWith refuse a RegExp (IsRegExp) */
        int re = is_regexp_arg(vm, ARG(0));
        if (re != 0) {
            if (re > 0) px_throw_error(vm, PX_TYPE_ERROR, "the search string must not be a RegExp");
            goto fail;
        }
    }
    pv = px_to_string(vm, ARG(0));
    if (pv == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, pv);
    p = px_str_flat(vm, pv);
    if (!p) goto fail2;
    RESTR();
    if (MAGIC == 1) {
        pos = s->len;
        if (ARG(1) != PX_UNDEFINED) {
            if (px_to_number(vm, argv[1], &pos) < 0) goto fail2;
            if (isnan(pos)) pos = s->len;
        }
    } else if (MAGIC == 4) {
        pos = s->len;
        if (ARG(1) != PX_UNDEFINED && px_to_number(vm, argv[1], &pos) < 0) goto fail2;
    } else {
        pos = 0;
        if (ARG(1) != PX_UNDEFINED && px_to_number(vm, argv[1], &pos) < 0) goto fail2;
    }
    if (isnan(pos)) pos = 0;
    pos = trunc(pos);
    if (pos < 0) pos = 0;
    if (pos > s->len) pos = s->len;
    RESTR();
    p = px_str_flat(vm, pv);
    px_pop_roots(vm, 2);
    switch (MAGIC) {
    case 3: {
        uint32_t j;
        if (pos + p->len > s->len) return PX_FALSE;
        for (j = 0; j < p->len; j++)
            if (px_str_at(s, (uint32_t)pos + j) != px_str_at(p, j)) return PX_FALSE;
        return PX_TRUE;
    }
    case 4: {
        uint32_t j;
        int64_t  start = (int64_t)pos - p->len;
        if (start < 0) return PX_FALSE;
        for (j = 0; j < p->len; j++)
            if (px_str_at(s, (uint32_t)start + j) != px_str_at(p, j)) return PX_FALSE;
        return PX_TRUE;
    }
    default:
        r = str_find(s, p, (int64_t)pos, MAGIC == 1);
        if (MAGIC == 2) return px_bool(r >= 0);
        return px_idx_value(vm, r);
    }
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* ToIntegerOrInfinity(v) clamped to [lo, hi] (NaN and undefined: 0). */
static int clamp_index(PxVM *vm, PxValue v, PxIdx lo, PxIdx hi, PxIdx *out) {
    double d;
    if (px_is_smi(v)) {
        PxIdx i = px_smi(v);
        *out    = i < lo ? lo : i > hi ? hi : i;
        return 0;
    }
    if (px_to_number(vm, v, &d) < 0) return -1;
    if (isnan(d)) d = 0;
    *out = d < (double)lo ? lo : d > (double)hi ? hi : (PxIdx)d;
    return 0;
}

/* slice (0), substring (1), substr (2) */
static PxValue strp_slice(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx a, b, len;
    THIS_STR();
    len = s->len;
    if (MAGIC == 0) {
        if (rel_index(vm, ARG(0), len, 0, &a) < 0 || rel_index(vm, ARG(1), len, len, &b) < 0) goto fail;
    } else if (MAGIC == 1) {
        PxIdx x, y = len;
        if (clamp_index(vm, ARG(0), 0, len, &x) < 0) goto fail;
        if (ARG(1) != PX_UNDEFINED && clamp_index(vm, ARG(1), 0, len, &y) < 0) goto fail;
        a = x < y ? x : y;
        b = x < y ? y : x;
    } else {
        PxIdx n = len;
        if (rel_index(vm, ARG(0), len, 0, &a) < 0) goto fail;
        if (ARG(1) != PX_UNDEFINED && clamp_index(vm, ARG(1), 0, len - a, &n) < 0) goto fail;
        if (n > len - a) n = len - a;
        b = a + n;
    }
    px_pop_roots(vm, 1);
    if (b <= a) return vm->atom[PX_ATOM_empty];
    return px_str_slice(vm, sv, (uint32_t)a, (uint32_t)b);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* Case mapping for Latin-1 and the common Latin Extended/Greek/Cyrillic
 * blocks. Characters outside those tables keep their case. */
static uint16_t to_upper(uint16_t c) {
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (uint16_t)(c - 32);
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : (uint16_t)(c - 1);
        return (c & 1) ? (uint16_t)(c - 1) : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (uint16_t)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (uint16_t)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (uint16_t)(c - 80);
    return c;
}

static uint16_t to_lower(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? (uint16_t)(c + 1) : c;
        return (c & 1) ? c : (uint16_t)(c + 1);
    }
    if (c >= 0x391 && c <= 0x3A9) return (uint16_t)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (uint16_t)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (uint16_t)(c + 80);
    return c;
}

static PxValue strp_case(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    SBuf     b = {0};
    uint32_t i;
    (void)argc;
    (void)argv;
    {
        THIS_STR();
        if (!s->wide) {
            /* Latin-1 in, Latin-1 out -- except ß (-> SS), µ and ÿ, whose
             * capitals are outside Latin-1: those take the general path. */
            const uint8_t *src = px_str_l1(s);
            for (i = 0; i < s->len; i++)
                if (MAGIC && (src[i] == 0xDF || src[i] == 0xB5 || src[i] == 0xFF)) break;
            if (i == s->len) {
                PxValue   r = px_str_new_l1(vm, NULL, s->len);
                PxString *d;
                if (r == PX_EXCEPTION) {
                    px_pop_roots(vm, 1);
                    return r;
                }
                RESTR();
                d = (PxString *)px_ptr(r);
                for (i = 0; i < s->len; i++)
                    px_str_l1(d)[i] = (uint8_t)(MAGIC ? to_upper(px_str_l1(s)[i]) : to_lower(px_str_l1(s)[i]));
                px_pop_roots(vm, 1);
                return r;
            }
        }
        for (i = 0; i < s->len; i++) {
            uint16_t c = px_str_at(s, i);
            if (MAGIC && c == 0xDF) { /* ß -> SS */
                sb_put(&b, 'S');
                sb_put(&b, 'S');
                continue;
            }
            sb_put(&b, MAGIC ? to_upper(c) : to_lower(c));
        }
        px_pop_roots(vm, 1);
    }
    return sb_finish(vm, &b);
}

static int is_ws(uint16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}

/* trim (0), trimStart (1), trimEnd (2) */
static PxValue strp_trim(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    uint32_t a = 0, b;
    (void)argc;
    (void)argv;
    {
        THIS_STR();
        b = s->len;
        if (MAGIC != 2)
            while (a < b && is_ws(px_str_at(s, a))) a++;
        if (MAGIC != 1)
            while (b > a && is_ws(px_str_at(s, b - 1))) b--;
        px_pop_roots(vm, 1);
        return px_str_slice(vm, sv, a, b);
    }
}

/* padStart (0), padEnd (1) */
static PxValue strp_pad(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double    target;
    PxValue   fill;
    PxString *f;
    SBuf      b = {0};
    uint32_t  i, need;
    THIS_STR();
    if (px_to_number(vm, ARG(0), &target) < 0) goto fail;
    if (isnan(target) || target <= s->len) {
        px_pop_roots(vm, 1);
        return sv;
    }
    if (target > (1 << 26)) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_RANGE_ERROR, "invalid string length");
    }
    fill = ARG(1) == PX_UNDEFINED ? px_str_from_cstr(vm, " ") : px_to_string(vm, ARG(1));
    if (fill == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, fill);
    f = px_str_flat(vm, fill);
    if (!f) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    RESTR();
    if (f->len == 0) {
        px_pop_roots(vm, 2);
        return sv;
    }
    need = (uint32_t)target - s->len;
    if (MAGIC == 1)
        for (i = 0; i < s->len; i++) sb_put(&b, px_str_at(s, i));
    for (i = 0; i < need; i++) sb_put(&b, px_str_at(f, i % f->len));
    if (MAGIC == 0)
        for (i = 0; i < s->len; i++) sb_put(&b, px_str_at(s, i));
    px_pop_roots(vm, 2);
    return sb_finish(vm, &b);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue strp_repeat(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double   n;
    SBuf     b = {0};
    uint32_t i, k;
    THIS_STR();
    if (px_to_number(vm, ARG(0), &n) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    if (isnan(n)) n = 0;
    if (n < 0 || isinf(n)) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_RANGE_ERROR, "invalid count value");
    }
    if (n * s->len > (1 << 26)) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_RANGE_ERROR, "invalid string length");
    }
    for (k = 0; k < (uint32_t)n; k++)
        for (i = 0; i < s->len; i++) sb_put(&b, px_str_at(s, i));
    px_pop_roots(vm, 1);
    return sb_finish(vm, &b);
}

static PxValue strp_concat(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue r = this_string(vm, t);
    int     i;
    if (r == PX_EXCEPTION) return r;
    PX_ROOT(vm, r);
    for (i = 0; i < argc; i++) {
        PxValue s = px_to_string(vm, argv[i]);
        if (s == PX_EXCEPTION) goto fail;
        r = px_str_concat(vm, r, s);
        if (r == PX_EXCEPTION) goto fail;
    }
    px_pop_roots(vm, 1);
    return r;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* split, replace (0) / replaceAll (1), and below match, matchAll, search:
 * px_regexp.c, which also handles string patterns. */
static PxValue strp_split(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    return px_string_regexp_method(vm, t, argc, argv, PX_SM_SPLIT);
}

static PxValue strp_replace(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    return px_string_regexp_method(vm, t, argc, argv, MAGIC ? PX_SM_REPLACE_ALL : PX_SM_REPLACE);
}

static PxValue strp_locale_compare(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o;
    int     c;
    THIS_STR();
    o = px_to_string(vm, ARG(0));
    if (o == PX_EXCEPTION || px_str_cmp(vm, sv, o, &c) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    return px_from_smi(c);
}

/* The form is checked, but composition tables are not shipped (they would
 * cost tens of kilobytes): the string comes back as it is. */
static PxValue strp_normalize(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = this_string(vm, t), f;
    char    form[8];
    if (s == PX_EXCEPTION || ARG(0) == PX_UNDEFINED) return s;
    PX_ROOT(vm, s);
    f = px_to_string(vm, ARG(0));
    px_pop_roots(vm, 1);
    if (f == PX_EXCEPTION) return f;
    if (px_str_len(f) > 4 || (px_str_to_utf8(vm, f, form, sizeof form),
                              strcmp(form, "NFC") && strcmp(form, "NFD") && strcmp(form, "NFKC") && strcmp(form, "NFKD")))
        return px_throw_error(vm, PX_RANGE_ERROR, "the normalization form must be NFC, NFD, NFKC or NFKD");
    return s;
}

/* String.raw(strings, ...substitutions): the raw strings, interleaved. */
static PxValue str_raw(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue raw, out = vm->atom[PX_ATOM_empty], k;
    PxIdx   len, i;
    (void)t;
    if (!px_is_obj(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "String.raw: expected a template strings object");
    k = px_intern_cstr(vm, "raw");
    if (k == PX_EXCEPTION) return k;
    raw = px_get(vm, ARG(0), k);
    if (raw == PX_EXCEPTION) return raw;
    if (!px_is_obj(raw)) return px_throw_error(vm, PX_TYPE_ERROR, "String.raw: .raw is not an object");
    PX_ROOT(vm, raw);
    PX_ROOT(vm, out);
    if (px_length_of(vm, raw, &len) < 0) goto fail;
    for (i = 0; i < len; i++) {
        PxValue s = px_get_index(vm, raw, i);
        if (s == PX_EXCEPTION || (s = px_to_string(vm, s)) == PX_EXCEPTION) goto fail;
        out = px_str_concat(vm, out, s);
        if (out == PX_EXCEPTION) goto fail;
        if (i + 1 < len && i + 1 < argc) {
            s = px_to_string(vm, argv[i + 1]);
            if (s == PX_EXCEPTION || (out = px_str_concat(vm, out, s)) == PX_EXCEPTION) goto fail;
        }
    }
    px_pop_roots(vm, 2);
    return out;
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

static const PxFnDef k_string_fns[] = {
    {"fromCharCode", str_from_char_code, 1, 0},
    {"fromCodePoint", str_from_char_code, 1, 1},
    {"raw", str_raw, 1, 0},
};

static const PxFnDef k_string_proto_fns[] = {
    {"toString", strp_to_string, 0, 0},
    {"valueOf", strp_to_string, 0, 0},
    {"charAt", strp_char_at, 1, 0},
    {"charCodeAt", strp_char_at, 1, 1},
    {"codePointAt", strp_char_at, 1, 2},
    {"at", strp_char_at, 1, 3},
    {"indexOf", strp_index_of, 1, 0},
    {"lastIndexOf", strp_index_of, 1, 1},
    {"includes", strp_index_of, 1, 2},
    {"startsWith", strp_index_of, 1, 3},
    {"endsWith", strp_index_of, 1, 4},
    {"slice", strp_slice, 2, 0},
    {"substring", strp_slice, 2, 1},
    {"substr", strp_slice, 2, 2},
    {"toLowerCase", strp_case, 0, 0},
    {"toUpperCase", strp_case, 0, 1},
    {"toLocaleLowerCase", strp_case, 0, 0},
    {"toLocaleUpperCase", strp_case, 0, 1},
    {"trim", strp_trim, 0, 0},
    {"trimStart", strp_trim, 0, 1},
    {"trimEnd", strp_trim, 0, 2},
    {"padStart", strp_pad, 1, 0},
    {"padEnd", strp_pad, 1, 1},
    {"repeat", strp_repeat, 1, 0},
    {"concat", strp_concat, 1, 0},
    {"split", strp_split, 2, 0},
    {"replace", strp_replace, 2, 0},
    {"replaceAll", strp_replace, 2, 1},
    {"localeCompare", strp_locale_compare, 1, 0},
    {"normalize", strp_normalize, 0, 0},
};

/* match (0), matchAll (1), search (2) */
static PxValue strp_match(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    static const int which[] = {PX_SM_MATCH, PX_SM_MATCH_ALL, PX_SM_SEARCH};
    return px_string_regexp_method(vm, t, argc, argv, which[MAGIC]);
}

/* ============================================================ more Object */

/* Object.getOwnPropertyDescriptor (ToObject first) and
 * Reflect.getOwnPropertyDescriptor (magic 1: the target must be an object). */
static PxValue obj_get_own_descriptor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0), k, r;
    (void)t;
    if (MAGIC) {
        if (!px_is_obj(o)) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect.getOwnPropertyDescriptor called on a non-object");
    } else {
        o = px_to_object(vm, o);
        if (o == PX_EXCEPTION) return o;
    }
    PX_ROOT(vm, o);
    k = px_intern(vm, ARG(1));
    if (k == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return k;
    }
    PX_ROOT(vm, k);
    r = px_descriptor_of(vm, o, k);
    px_pop_roots(vm, 2);
    return r;
}

static PxValue obj_get_own_descriptors(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue  o = px_to_object(vm, ARG(0)), out, kv;
    PxVec   *keys;
    uint32_t n, i;
    (void)t;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    out = px_object_new(vm);
    if (out == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, out);
    keys = px_own_keys(vm, o, PX_KEYS_SYMBOLS, &n);
    if (!keys) goto fail2;
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    for (i = 0; i < n; i++) {
        PxValue k = px_intern(vm, ((PxVec *)px_ptr(kv))->items[i]), d;
        if (k == PX_EXCEPTION) goto fail3;
        ((PxVec *)px_ptr(kv))->items[i] = k;
        d = px_descriptor_of(vm, o, k);
        if (d == PX_EXCEPTION) goto fail3;
        if (d != PX_UNDEFINED && px_create_data_property(vm, out, ((PxVec *)px_ptr(kv))->items[i], d) < 0) goto fail3;
    }
    px_pop_roots(vm, 3);
    return out;
fail3:
    px_pop_roots(vm, 1);
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue obj_group_by(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue out, fn = ARG(1), iter, item;
    double  k = 0;
    int     r;
    (void)t;
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "groupBy: callback is not a function");
    {
        PxObject *o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), PX_NULL);
        if (!o) return PX_EXCEPTION;
        out = px_from_ptr(o);
    }
    PX_ROOT(vm, out);
    iter = px_get_iterator(vm, ARG(0));
    if (iter == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, iter);
    while ((r = px_iterator_step(vm, iter, &item)) > 0) {
        PxValue args[2], key, group;
        PX_ROOT(vm, item);
        args[0] = item;
        args[1] = px_number(vm, k++);
        key     = px_call(vm, fn, PX_UNDEFINED, 2, args);
        if (key == PX_EXCEPTION) goto fail3;
        key = px_intern(vm, key);
        if (key == PX_EXCEPTION) goto fail3;
        PX_ROOT(vm, key);
        group = px_get(vm, out, key);
        if (group == PX_EXCEPTION) goto fail4;
        if (group == PX_UNDEFINED) {
            group = px_array_new(vm, 0);
            if (group == PX_EXCEPTION || px_define(vm, out, key, group, PX_ATTR_DEFAULT) < 0) goto fail4;
        }
        if (px_array_push(vm, group, item) < 0) goto fail4;
        px_pop_roots(vm, 2);
        continue;
    fail4:
        px_pop_roots(vm, 1);
    fail3:
        px_pop_roots(vm, 1);
        goto fail2;
    }
    if (r < 0) goto fail2;
    px_pop_roots(vm, 2);
    return out;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* ============================================================ Reflect */

static PxValue reflect_apply(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (!px_is_callable(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect.apply: the target is not a function");
    return call_with_list(vm, ARG(0), ARG(1), ARG(2), 0, PX_UNDEFINED);
}

static PxValue reflect_construct(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue nt = argc > 2 ? argv[2] : ARG(0);
    (void)t;
    if (!px_is_constructor(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect.construct: the target is not a constructor");
    if (!px_is_constructor(nt)) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect.construct: newTarget is not a constructor");
    return call_with_list(vm, ARG(0), PX_UNDEFINED, ARG(1), 1, nt);
}

/* The target (an object) and the key of a Reflect method; -1 on exception. */
static int reflect_target_key(PxVM *vm, int argc, PxValue *argv, PxValue *key) {
    if (!px_is_obj(ARG(0))) {
        px_throw_error(vm, PX_TYPE_ERROR, "Reflect method called on a non-object");
        return -1;
    }
    PX_ROOT(vm, argv[0]);
    *key = px_intern(vm, ARG(1));
    px_pop_roots(vm, 1);
    return *key == PX_EXCEPTION ? -1 : 0;
}

/* get (0), has (1), set (2), deleteProperty (3) */
static PxValue reflect_prop(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0), k;
    int     r;
    (void)t;
    if (reflect_target_key(vm, argc, argv, &k) < 0) return PX_EXCEPTION;
    PX_ROOT(vm, k);
    switch (MAGIC) {
    case 0: r = -2; break;
    case 1: r = px_has(vm, o, k); break;
    case 2: r = px_set_recv(vm, o, k, ARG(2), argc > 3 ? argv[3] : o); break;
    default: r = px_delete(vm, o, k); break;
    }
    if (r == -2) {
        PxValue v = px_get_recv(vm, o, k, argc > 2 ? argv[2] : o);
        px_pop_roots(vm, 1);
        return v;
    }
    px_pop_roots(vm, 1);
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

/* getPrototypeOf (0), isExtensible (1), preventExtensions (2), ownKeys (3) */
static PxValue reflect_obj(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = ARG(0);
    int     r;
    (void)t;
    if (!px_is_obj(o)) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect method called on a non-object");
    switch (MAGIC) {
    case 0: return px_proto_of(vm, o);
    case 1: r = px_is_extensible(vm, o); break;
    case 2: r = px_prevent_extensions(vm, o); break;
    default: {
        uint32_t n;
        PxVec   *keys = px_own_keys(vm, o, PX_KEYS_SYMBOLS, &n);
        return keys ? keys_to_array(vm, keys, n) : PX_EXCEPTION;
    }
    }
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

static PxValue reflect_set_proto(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int r;
    (void)t;
    if (!px_is_obj(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "Reflect.setPrototypeOf called on a non-object");
    if (ARG(1) != PX_NULL && !px_is_obj(ARG(1))) return px_throw_error(vm, PX_TYPE_ERROR, "prototype must be an object or null");
    r = px_set_proto_ok(vm, ARG(0), ARG(1));
    return r < 0 ? PX_EXCEPTION : px_bool(r);
}

static const PxFnDef k_reflect_fns[] = {
    {"apply", reflect_apply, 3, 0},
    {"construct", reflect_construct, 2, 0},
    {"defineProperty", obj_define_property, 3, 1},
    {"deleteProperty", reflect_prop, 2, 3},
    {"get", reflect_prop, 2, 0},
    {"getOwnPropertyDescriptor", obj_get_own_descriptor, 2, 1},
    {"getPrototypeOf", reflect_obj, 1, 0},
    {"has", reflect_prop, 2, 1},
    {"isExtensible", reflect_obj, 1, 1},
    {"ownKeys", reflect_obj, 1, 3},
    {"preventExtensions", reflect_obj, 1, 2},
    {"set", reflect_prop, 3, 2},
    {"setPrototypeOf", reflect_set_proto, 2, 0},
};

/* ============================================================ more Array */

/* toReversed (0), toSorted (1), with (2), toSpliced (3): a new array, the
 * receiver untouched, holes read through. */
static PxValue arrp_copying(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue copy, vv = PX_UNDEFINED;
    PxIdx   i, start = 0, skip = 0, at = 0, n, ins = argc > 2 ? argc - 2 : 0;
    int     mode = MAGIC;
    if (mode == 1 && ARG(0) != PX_UNDEFINED && !px_is_callable(ARG(0)))
        return px_throw_error(vm, PX_TYPE_ERROR, "toSorted: the comparator must be a function");
    {
        THIS_OBJ();
        PX_ROOT(vm, vv);
        n = len;
        if (mode == 2) {
            double d;
            if (px_to_number(vm, ARG(0), &d) < 0) goto fail;
            d = isnan(d) ? 0 : trunc(d);
            if (d < 0) d += (double)len;
            if (d < 0 || d >= (double)len) {
                px_throw_error(vm, PX_RANGE_ERROR, "with: index out of range");
                goto fail;
            }
            at = (PxIdx)d;
        } else if (mode == 3) {
            if (rel_index(vm, ARG(0), len, 0, &start) < 0) goto fail;
            if (argc == 0) skip = 0;
            else if (argc == 1) skip = len - start;
            else if (clamp_count(vm, argv[1], len - start, &skip) < 0) goto fail;
            n = len + ins - skip;
            if (n > MAX_SAFE_LEN) {
                too_long(vm);
                goto fail;
            }
        }
        copy = array_create(vm, n);
        if (copy == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, copy);
        if (mode == 1) {
            uint32_t cnt, undefs;
            if (sort_values(vm, o, len, ARG(0), 0, &vv, &cnt, &undefs) < 0) goto fail2;
            for (i = 0; i < cnt; i++)
                if (put_fresh(vm, copy, i, ((PxVec *)px_ptr(vv))->items[(uint32_t)i]) < 0) goto fail2;
            for (; i < len; i++)
                if (put_fresh(vm, copy, i, PX_UNDEFINED) < 0) goto fail2;
        } else {
            for (i = 0; i < n; i++) {
                PxValue v;
                if (mode == 0) v = px_get_index(vm, o, len - 1 - i);
                else if (mode == 2) v = i == at ? ARG(1) : px_get_index(vm, o, i);
                else if (i < start) v = px_get_index(vm, o, i);
                else if (i < start + ins) v = argv[2 + (int)(i - start)];
                else v = px_get_index(vm, o, i - ins + skip);
                if (v == PX_EXCEPTION || put_fresh(vm, copy, i, v) < 0) goto fail2;
            }
        }
        px_pop_roots(vm, 3);
        return copy;
    fail2:
        px_pop_roots(vm, 1);
    fail:
        px_pop_roots(vm, 2);
        return PX_EXCEPTION;
    }
}

/* copyWithin(target, start, end) */
static PxValue arrp_copy_within(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIdx to, from, end, count;
    THIS_OBJ();
    if (rel_index(vm, ARG(0), len, 0, &to) < 0 || rel_index(vm, ARG(1), len, 0, &from) < 0 ||
        rel_index(vm, ARG(2), len, len, &end) < 0)
        goto fail;
    count = end - from;
    if (count > len - to) count = len - to;
    if (count > 0 && move_range(vm, o, from, to, count) < 0) goto fail;
    px_pop_roots(vm, 1);
    return o;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static const PxFnDef k_extra_array_proto_fns[] = {
    {"copyWithin", arrp_copy_within, 2, 0},
    {"toReversed", arrp_copying, 0, 0},
    {"toSorted", arrp_copying, 1, 1},
    {"toSpliced", arrp_copying, 2, 3},
    {"with", arrp_copying, 2, 2},
};

static const PxFnDef k_extra_object_fns[] = {
    {"getOwnPropertyDescriptor", obj_get_own_descriptor, 2, 0},
    {"getOwnPropertyDescriptors", obj_get_own_descriptors, 1, 0},
    {"groupBy", obj_group_by, 2, 0},
};

static const PxFnDef k_extra_string_proto_fns[] = {
    {"match", strp_match, 1, 0},
    {"matchAll", strp_match, 1, 1},
    {"search", strp_match, 1, 2},
};

/* ============================================================ Number, Boolean */

static PxValue num_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double  d = 0;
    PxValue n;
    (void)t;
    if (argc > 0 && px_to_number(vm, argv[0], &d) < 0) return PX_EXCEPTION;
    n = px_number(vm, d);
    return vm->native_new_target == PX_UNDEFINED ? n : box_for_new(vm, n, PX_PROTO_NUMBER);
}

static int this_number(PxVM *vm, PxValue t, double *d) {
    if (px_is_num(t)) {
        *d = px_num(t);
        return 0;
    }
    if (px_is_obj(t) && px_type_of(t) == PX_T_BOXED && px_is_num(((PxBoxed *)px_ptr(t))->value)) {
        *d = px_num(((PxBoxed *)px_ptr(t))->value);
        return 0;
    }
    px_throw_error(vm, PX_TYPE_ERROR, "Number.prototype method called on a non-number");
    return -1;
}

static PxValue nump_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d, r = 10;
    if (this_number(vm, t, &d) < 0) return PX_EXCEPTION;
    if (ARG(0) != PX_UNDEFINED && px_to_number(vm, ARG(0), &r) < 0) return PX_EXCEPTION;
    if (r < 2 || r > 36 || r != floor(r)) return px_throw_error(vm, PX_RANGE_ERROR, "radix must be between 2 and 36");
    return px_number_to_string(vm, d, (int)r);
}

/* toFixed (0), toPrecision (1), toExponential (2) */
static PxValue nump_to_fixed(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d, p = 0;
    char   buf[160];
    int    lo = MAGIC == 1 ? 1 : 0;
    if (this_number(vm, t, &d) < 0) return PX_EXCEPTION;
    if (ARG(0) == PX_UNDEFINED && MAGIC == 1) return px_number_to_string(vm, d, 10);
    if (px_to_number(vm, ARG(0), &p) < 0) return PX_EXCEPTION;
    p = isnan(p) ? 0 : trunc(p); /* ToIntegerOrInfinity */
    /* toFixed checks the range before the value; the others after */
    if (MAGIC == 0 && (p < lo || p > 100)) return px_throw_error(vm, PX_RANGE_ERROR, "digits out of range");
    if (!isfinite(d)) return px_number_to_string(vm, d, 10);
    if (p < lo || p > 100) return px_throw_error(vm, PX_RANGE_ERROR, "precision out of range");
    if (MAGIC == 0) {
        if (fabs(d) >= 1e21) return px_number_to_string(vm, d, 10);
        px_fmt_fixed(d, (int)p, buf, sizeof buf);
    } else if (MAGIC == 2) {
        px_fmt_exponential(d, ARG(0) == PX_UNDEFINED ? -1 : (int)p, buf, sizeof buf);
    } else {
        px_fmt_precision(d, (int)p, buf, sizeof buf);
    }
    return px_str_from_cstr(vm, buf);
}

static PxValue nump_value_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d;
    (void)argc;
    (void)argv;
    if (this_number(vm, t, &d) < 0) return PX_EXCEPTION;
    return px_number(vm, d);
}

/* isInteger (0), isSafeInteger (1), isFinite (2), isNaN (3) */
static PxValue num_is(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d;
    (void)vm;
    (void)t;
    if (!px_is_num(ARG(0))) return PX_FALSE;
    d = px_num(ARG(0));
    switch (MAGIC) {
    case 0: return px_bool(isfinite(d) && d == floor(d));
    case 1: return px_bool(isfinite(d) && d == floor(d) && fabs(d) <= 9007199254740991.0);
    case 2: return px_bool(isfinite(d));
    default: return px_bool(isnan(d));
    }
}

static PxValue bool_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue b = px_bool(px_truthy(ARG(0)));
    (void)t;
    return vm->native_new_target == PX_UNDEFINED ? b : box_for_new(vm, b, PX_PROTO_BOOLEAN);
}

static PxValue boolp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue v = t;
    (void)argc;
    (void)argv;
    if (px_is_obj(t) && px_type_of(t) == PX_T_BOXED) v = ((PxBoxed *)px_ptr(t))->value;
    if (v != PX_TRUE && v != PX_FALSE) return px_throw_error(vm, PX_TYPE_ERROR, "not a boolean");
    return MAGIC ? v : px_str_from_cstr(vm, v == PX_TRUE ? "true" : "false");
}

/* ============================================================ globals */

static PxValue g_parse_float(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   s = px_to_string(vm, ARG(0));
    PxString *f;
    uint32_t  i = 0;
    (void)t;
    if (s == PX_EXCEPTION) return s;
    f = px_str_flat(vm, s);
    if (!f) return PX_EXCEPTION;
    while (i < f->len && is_ws(px_str_at(f, i))) i++;
    /* the longest prefix that is a decimal literal */
    {
        uint32_t start = i, j = i, digits = 0;
        if (j < f->len && (px_str_at(f, j) == '+' || px_str_at(f, j) == '-')) j++;
        if (j + 8 <= f->len) {
            static const char inf[] = "Infinity";
            uint32_t          k;
            for (k = 0; k < 8 && px_str_at(f, j + k) == (uint16_t)inf[k]; k++) {}
            if (k == 8) return px_number(vm, px_str_at(f, start) == '-' ? -INFINITY : INFINITY);
        }
        while (j < f->len && px_str_at(f, j) >= '0' && px_str_at(f, j) <= '9') j++, digits++;
        if (j < f->len && px_str_at(f, j) == '.') {
            j++;
            while (j < f->len && px_str_at(f, j) >= '0' && px_str_at(f, j) <= '9') j++, digits++;
        }
        if (!digits) return px_number(vm, NAN);
        if (j < f->len && (px_str_at(f, j) | 0x20) == 'e') {
            uint32_t k = j + 1, ed = 0;
            if (k < f->len && (px_str_at(f, k) == '+' || px_str_at(f, k) == '-')) k++;
            while (k < f->len && px_str_at(f, k) >= '0' && px_str_at(f, k) <= '9') k++, ed++;
            if (ed) j = k;
        }
        return px_number(vm, px_decimal_to_double(f->wide ? (const void *)(px_str_u16(f) + start)
                                                          : (const void *)(px_str_l1(f) + start),
                                                  f->wide, j - start));
    }
}

static PxValue g_parse_int(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue   s = px_to_string(vm, ARG(0));
    PxString *f;
    int32_t   radix;
    double    r = 0;
    uint32_t  i = 0, start;
    int       neg = 0, shift;
    (void)t;
    if (s == PX_EXCEPTION) return s;
    PX_ROOT(vm, s);
    if (px_to_int32(vm, ARG(1), &radix) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    f = px_str_flat(vm, s);
    if (!f) return PX_EXCEPTION;
    while (i < f->len && is_ws(px_str_at(f, i))) i++;
    if (i < f->len && (px_str_at(f, i) == '+' || px_str_at(f, i) == '-')) neg = px_str_at(f, i++) == '-';
    if (radix != 0 && (radix < 2 || radix > 36)) return px_number(vm, NAN);
    if ((radix == 0 || radix == 16) && i + 1 < f->len && px_str_at(f, i) == '0' && (px_str_at(f, i + 1) | 0x20) == 'x') {
        i += 2;
        radix = 16;
    }
    if (radix == 0) radix = 10;
    for (start = i; i < f->len; i++) {
        uint16_t c = px_str_at(f, i);
        int      d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'z' ? (c | 0x20) - 'a' + 10 : 99;
        if (d >= radix) break;
    }
    if (i == start) return px_number(vm, NAN);
    /* radix 10 and the powers of two are exact (correctly rounded) */
    for (shift = 1; shift <= 5 && (1 << shift) != radix; shift++) {}
    if (radix == 10)
        r = px_decimal_to_double(f->wide ? (const void *)(px_str_u16(f) + start) : (const void *)(px_str_l1(f) + start),
                                 f->wide, i - start);
    else if (shift <= 5)
        r = px_radix2_to_double(f->wide ? (const void *)(px_str_u16(f) + start) : (const void *)(px_str_l1(f) + start),
                                f->wide, i - start, shift);
    else
        for (; start < i; start++) {
            uint16_t c = px_str_at(f, start) | 0x20;
            r          = r * radix + (c <= '9' ? c - '0' : c - 'a' + 10);
        }
    return px_number(vm, neg ? -r : r);
}

static PxValue g_is_nan(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d;
    (void)t;
    if (px_to_number(vm, ARG(0), &d) < 0) return PX_EXCEPTION;
    return px_bool(MAGIC ? isfinite(d) : isnan(d));
}

static int hex_digit(uint16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The byte %XY at s[k] (k at the '%'), or -1. */
static int pct_byte(const PxString *s, uint32_t k) {
    int h, l;
    if (k + 2 >= s->len || px_str_at(s, k) != '%') return -1;
    h = hex_digit(px_str_at(s, k + 1));
    l = hex_digit(px_str_at(s, k + 2));
    return h < 0 || l < 0 ? -1 : h * 16 + l;
}

/* encodeURIComponent (0) / encodeURI (1) / decodeURIComponent (2) /
 * decodeURI (3): Encode and Decode (ECMA-262 19.2.6) over UTF-16. */
static PxValue g_uri(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    static const char hex[] = "0123456789ABCDEF", reserved[] = ";/?:@&=+$,#";
    PxValue           sv = px_to_string(vm, ARG(0));
    PxString         *s;
    SBuf              b = {0};
    uint32_t          k;
    (void)t;
    if (sv == PX_EXCEPTION) return sv;
    PX_ROOT(vm, sv);
    s = px_str_flat(vm, sv);
    px_pop_roots(vm, 1);
    if (!s) return PX_EXCEPTION;
    for (k = 0; k < s->len; k++) {
        uint16_t c = px_str_at(s, k);
        uint32_t cp;
        if (MAGIC <= 1) {
            uint8_t oct[4];
            int     n, j;
            if ((c < 128 && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) || (c >= '0' && c <= '9') ||
                (c && c < 128 && strchr("-_.!~*'()", c)) || (MAGIC == 1 && c && c < 128 && strchr(reserved, c))) {
                sb_put(&b, c);
                continue;
            }
            cp = c;
            if (c >= 0xDC00 && c <= 0xDFFF) goto bad;
            if (c >= 0xD800 && c <= 0xDBFF) {
                uint16_t lo = k + 1 < s->len ? px_str_at(s, k + 1) : 0;
                if (lo < 0xDC00 || lo > 0xDFFF) goto bad;
                cp = 0x10000 + ((uint32_t)(c - 0xD800) << 10) + (lo - 0xDC00);
                k++;
            }
            if (cp < 0x80) oct[0] = (uint8_t)cp, n = 1;
            else if (cp < 0x800) oct[0] = (uint8_t)(0xC0 | cp >> 6), oct[1] = (uint8_t)(0x80 | (cp & 63)), n = 2;
            else if (cp < 0x10000)
                oct[0] = (uint8_t)(0xE0 | cp >> 12), oct[1] = (uint8_t)(0x80 | (cp >> 6 & 63)),
                oct[2] = (uint8_t)(0x80 | (cp & 63)), n = 3;
            else
                oct[0] = (uint8_t)(0xF0 | cp >> 18), oct[1] = (uint8_t)(0x80 | (cp >> 12 & 63)),
                oct[2] = (uint8_t)(0x80 | (cp >> 6 & 63)), oct[3] = (uint8_t)(0x80 | (cp & 63)), n = 4;
            for (j = 0; j < n; j++) {
                sb_put(&b, '%');
                sb_put(&b, (uint16_t)hex[oct[j] >> 4]);
                sb_put(&b, (uint16_t)hex[oct[j] & 15]);
            }
        } else {
            int      byte, n, j;
            uint32_t start = k;
            if (c != '%') {
                sb_put(&b, c);
                continue;
            }
            byte = pct_byte(s, k);
            if (byte < 0) goto bad;
            k += 2;
            if (byte < 0x80) {
                /* decodeURI keeps the escapes of reserved characters */
                if (MAGIC == 3 && strchr(reserved, byte) && byte) {
                    for (j = (int)start; j <= (int)k; j++) sb_put(&b, px_str_at(s, (uint32_t)j));
                } else {
                    sb_put(&b, (uint16_t)byte);
                }
                continue;
            }
            n = (byte & 0xE0) == 0xC0 ? 2 : (byte & 0xF0) == 0xE0 ? 3 : (byte & 0xF8) == 0xF0 ? 4 : 0;
            if (!n) goto bad;
            cp = (uint32_t)byte & (0x7F >> n);
            for (j = 1; j < n; j++) {
                int cb = pct_byte(s, k + 1);
                if (cb < 0 || (cb & 0xC0) != 0x80) goto bad;
                cp = cp << 6 | (uint32_t)(cb & 63);
                k += 3;
            }
            /* no overlong forms, surrogates or values past U+10FFFF */
            if (cp < (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                goto bad;
            if (cp >= 0x10000) {
                sb_put(&b, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
                sb_put(&b, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
            } else {
                sb_put(&b, (uint16_t)cp);
            }
        }
    }
    return sb_finish(vm, &b);
bad:
    free(b.d);
    return px_throw_error(vm, PX_URI_ERROR, MAGIC <= 1 ? "URI malformed: a lone surrogate" : "URI malformed");
}

static const PxFnDef k_global_fns[] = {
    {"parseInt", g_parse_int, 2, 0},
    {"parseFloat", g_parse_float, 1, 0},
    {"isNaN", g_is_nan, 1, 0},
    {"isFinite", g_is_nan, 1, 1},
    {"encodeURIComponent", g_uri, 1, 0},
    {"encodeURI", g_uri, 1, 1},
    {"decodeURIComponent", g_uri, 1, 2},
    {"decodeURI", g_uri, 1, 3},
};

/* ============================================================ Math */

static uint32_t g_rng[4] = {0x9E3779B9u, 0x243F6A88u, 0xB7E15162u, 0x12345678u};

/* xoshiro128**: small, fast on a 32-bit core, good enough for Math.random. */
static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static uint32_t rng_next(void) {
    uint32_t r = rotl(g_rng[1] * 5, 7) * 9, t = g_rng[1] << 9;
    g_rng[2] ^= g_rng[0];
    g_rng[3] ^= g_rng[1];
    g_rng[1] ^= g_rng[2];
    g_rng[0] ^= g_rng[3];
    g_rng[2] ^= t;
    g_rng[3] = rotl(g_rng[3], 11);
    return r;
}

enum {
    M_ABS, M_FLOOR, M_CEIL, M_ROUND, M_TRUNC, M_SIGN, M_SQRT, M_CBRT, M_EXP, M_EXPM1, M_LOG, M_LOG1P, M_LOG2, M_LOG10,
    M_SIN, M_COS, M_TAN, M_ASIN, M_ACOS, M_ATAN, M_SINH, M_COSH, M_TANH, M_FROUND, M_ASINH, M_ACOSH, M_ATANH
};

static PxValue math_1(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double d, r;
    (void)t;
    if (px_is_smi(ARG(0))) {
        int32_t i = px_smi(ARG(0));
        switch (MAGIC) {
        case M_FLOOR:
        case M_CEIL:
        case M_ROUND:
        case M_TRUNC: return ARG(0);
        case M_ABS: return px_int(vm, i < 0 ? -i : i);
        default: break;
        }
    }
    if (px_to_number(vm, ARG(0), &d) < 0) return PX_EXCEPTION;
    switch (MAGIC) {
    case M_ABS: r = fabs(d); break;
    case M_FLOOR: r = floor(d); break;
    case M_CEIL: r = ceil(d); break;
    case M_ROUND:
        /* not floor(d + 0.5): that sum rounds for 0.49999999999999994 and above 2^52 */
        r = floor(d);
        if (d - r >= 0.5) r += 1;
        if (r == 0 && d < 0) r = -0.0;
        break;
    case M_TRUNC: r = trunc(d); break;
    case M_SIGN: r = d > 0 ? 1 : d < 0 ? -1 : d; break;
    case M_SQRT: r = sqrt(d); break;
    case M_CBRT: r = cbrt(d); break;
    case M_EXP: r = exp(d); break;
    case M_EXPM1: r = expm1(d); break;
    case M_LOG: r = log(d); break;
    case M_LOG1P: r = log1p(d); break;
    case M_LOG2: r = log2(d); break;
    case M_LOG10: r = log10(d); break;
    case M_SIN: r = sin(d); break;
    case M_COS: r = cos(d); break;
    case M_TAN: r = tan(d); break;
    case M_ASIN: r = asin(d); break;
    case M_ACOS: r = acos(d); break;
    case M_ATAN: r = atan(d); break;
    case M_SINH: r = sinh(d); break;
    case M_COSH: r = cosh(d); break;
    case M_TANH: r = tanh(d); break;
    case M_FROUND: r = (double)(float)d; break;
    case M_ASINH: r = asinh(d); break;
    case M_ACOSH: r = acosh(d); break;
    case M_ATANH: r = atanh(d); break;
    default: r = NAN; break;
    }
    return px_number(vm, r);
}

static PxValue math_minmax(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double r = MAGIC ? -INFINITY : INFINITY;
    int    i;
    (void)t;
    for (i = 0; i < argc; i++) {
        double d;
        if (px_to_number(vm, argv[i], &d) < 0) return PX_EXCEPTION;
        if (isnan(d)) r = NAN;
        else if (!isnan(r)) {
            if (MAGIC ? (d > r || (d == 0 && r == 0 && !signbit(d))) : (d < r || (d == 0 && r == 0 && signbit(d)))) r = d;
        }
    }
    return px_number(vm, r);
}

static PxValue math_2(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double a, b;
    (void)t;
    if (px_to_number(vm, ARG(0), &a) < 0 || px_to_number(vm, ARG(1), &b) < 0) return PX_EXCEPTION;
    switch (MAGIC) {
    case 0: return px_number(vm, (isnan(b) || (fabs(a) == 1 && isinf(b))) ? NAN : pow(a, b));
    case 1: return px_number(vm, atan2(a, b));
    default: {
        int32_t x, y;
        if (px_to_int32(vm, ARG(0), &x) < 0 || px_to_int32(vm, ARG(1), &y) < 0) return PX_EXCEPTION;
        return px_int(vm, (int32_t)((uint32_t)x * (uint32_t)y));
    }
    }
}

/* Every argument is coerced first; then Infinity wins over NaN. The sum
 * is scaled by the largest value so that squares neither overflow nor
 * underflow. */
static PxValue math_hypot(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double  buf[8], *v = buf, s = 0, m = 0;
    int     i, inf = 0, nan = 0;
    PxValue r;
    (void)t;
    if (argc > 8 && !(v = (double *)malloc((size_t)argc * sizeof(double)))) return px_throw_oom(vm);
    for (i = 0; i < argc; i++) {
        if (px_to_number(vm, argv[i], &v[i]) < 0) {
            if (v != buf) free(v);
            return PX_EXCEPTION;
        }
        v[i] = fabs(v[i]);
        if (isinf(v[i])) inf = 1;
        else if (isnan(v[i])) nan = 1;
        else if (v[i] > m) m = v[i];
    }
    if (!inf && !nan && m > 0)
        for (i = 0; i < argc; i++) s += (v[i] / m) * (v[i] / m);
    r = px_number(vm, inf ? INFINITY : nan ? NAN : m > 0 ? m * sqrt(s) : 0);
    if (v != buf) free(v);
    return r;
}

static PxValue math_random(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    uint32_t hi, lo;
    (void)t;
    (void)argc;
    (void)argv;
    hi = rng_next() >> 5;
    lo = rng_next() >> 6;
    return px_number(vm, (hi * 67108864.0 + lo) / 9007199254740992.0);
}

static PxValue math_clz32(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    uint32_t x;
    int      n = 0;
    (void)t;
    if (px_to_uint32(vm, ARG(0), &x) < 0) return PX_EXCEPTION;
    if (x == 0) return px_from_smi(32);
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return px_from_smi(n);
}

static const PxFnDef k_math_fns[] = {
    {"abs", math_1, 1, M_ABS},     {"floor", math_1, 1, M_FLOOR}, {"ceil", math_1, 1, M_CEIL},
    {"round", math_1, 1, M_ROUND}, {"trunc", math_1, 1, M_TRUNC}, {"sign", math_1, 1, M_SIGN},
    {"sqrt", math_1, 1, M_SQRT},   {"cbrt", math_1, 1, M_CBRT},   {"exp", math_1, 1, M_EXP},
    {"expm1", math_1, 1, M_EXPM1}, {"log", math_1, 1, M_LOG},     {"log1p", math_1, 1, M_LOG1P},
    {"log2", math_1, 1, M_LOG2},   {"log10", math_1, 1, M_LOG10}, {"sin", math_1, 1, M_SIN},
    {"cos", math_1, 1, M_COS},     {"tan", math_1, 1, M_TAN},     {"asin", math_1, 1, M_ASIN},
    {"acos", math_1, 1, M_ACOS},   {"atan", math_1, 1, M_ATAN},   {"sinh", math_1, 1, M_SINH},
    {"cosh", math_1, 1, M_COSH},   {"tanh", math_1, 1, M_TANH},   {"fround", math_1, 1, M_FROUND},
    {"min", math_minmax, 2, 0},    {"max", math_minmax, 2, 1},    {"pow", math_2, 2, 0},
    {"atan2", math_2, 2, 1},       {"imul", math_2, 2, 2},        {"hypot", math_hypot, 2, 0},
    {"random", math_random, 0, 0}, {"clz32", math_clz32, 1, 0},  {"asinh", math_1, 1, M_ASINH},
    {"acosh", math_1, 1, M_ACOSH}, {"atanh", math_1, 1, M_ATANH},
};

/* ============================================================ Error */

/* Error and the NativeErrors (magic: PxErrorType); AggregateError takes
 * (errors, message, options). Order as the spec has it: the object (its
 * prototype from new.target), the message, the cause, then the errors. */
static PxValue error_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     type = MAGIC, agg = type == PX_AGGREGATE_ERROR;
    PxValue nt = vm->native_new_target, e, msg = ARG(agg), options = ARG(agg + 1), k;
    int     r;
    (void)t;
    if (nt == PX_UNDEFINED) nt = vm->native_callee;
    e = new_with_proto(vm, PX_T_ERROR, sizeof(PxObject), nt, PX_PROTO_ERROR + type);
    if (e == PX_EXCEPTION) return e;
    PX_ROOT(vm, e);
    if (msg != PX_UNDEFINED) {
        msg = px_to_string(vm, msg);
        if (msg == PX_EXCEPTION || px_define(vm, e, vm->atom[PX_ATOM_message], msg, PX_ATTR_HIDDEN) < 0) goto fail;
    }
    if (px_is_obj(options)) {
        /* InstallErrorCause */
        k = px_intern_cstr(vm, "cause");
        if (k == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, k);
        r = px_has(vm, options, k);
        if (r > 0) {
            PxValue c = px_get(vm, options, k);
            r         = c == PX_EXCEPTION ? -1 : px_define(vm, e, k, c, PX_ATTR_HIDDEN);
        }
        px_pop_roots(vm, 1);
        if (r < 0) goto fail;
    }
    if (agg) {
        /* the errors: IterableToList, as an array */
        PxValue errs = px_array_new(vm, 0), rec, item;
        if (errs == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, errs);
        rec = px_iter_record(vm, ARG(0), 0);
        if (rec == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        PX_ROOT(vm, rec);
        while ((r = px_record_step(vm, rec, &item)) > 0)
            if (px_array_push(vm, errs, item) < 0) {
                r = -1;
                break;
            }
        if (r == 0) r = px_def_value(vm, e, "errors", errs, PX_ATTR_HIDDEN);
        px_pop_roots(vm, 2);
        if (r < 0) goto fail;
    }
    px_capture_stack(vm, e);
    px_pop_roots(vm, 1);
    return e;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue errorp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue name, msg, r, sep;
    (void)argc;
    (void)argv;
    if (!px_is_obj(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Error.prototype.toString called on a non-object");
    PX_ROOT(vm, t);
    name = px_get(vm, t, vm->atom[PX_ATOM_name]);
    if (name == PX_EXCEPTION) goto fail;
    name = name == PX_UNDEFINED ? vm->atom[PX_ATOM_Error] : px_to_string(vm, name);
    if (name == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, name);
    msg = px_get(vm, t, vm->atom[PX_ATOM_message]);
    if (msg == PX_EXCEPTION) goto fail2;
    msg = msg == PX_UNDEFINED ? vm->atom[PX_ATOM_empty] : px_to_string(vm, msg);
    if (msg == PX_EXCEPTION) goto fail2;
    if (px_str_len(msg) == 0) {
        px_pop_roots(vm, 2);
        return name;
    }
    if (px_str_len(name) == 0) {
        px_pop_roots(vm, 2);
        return msg;
    }
    PX_ROOT(vm, msg);
    sep = px_str_from_cstr(vm, ": ");
    if (sep == PX_EXCEPTION) goto fail3;
    r = px_str_concat(vm, name, sep);
    if (r == PX_EXCEPTION) goto fail3;
    r = px_str_concat(vm, r, msg);
    px_pop_roots(vm, 3);
    return r;
fail3:
    px_pop_roots(vm, 1);
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* ============================================================ setup */

static PxValue new_plain(PxVM *vm, PxValue proto) {
    PxObject *o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), proto);
    return o ? px_from_ptr(o) : PX_EXCEPTION;
}

/* Function.prototype is itself a function, Array.prototype an array, and
 * String/Number/Boolean.prototype wrap "", 0 and false (ECMA-262 20-23). */
static PxValue new_proto(PxVM *vm, int index) {
    PxValue   op = vm->protos[PX_PROTO_OBJECT];
    PxObject *o;
    switch (index) {
    case PX_PROTO_FUNCTION:
        o = px_obj_new(vm, PX_T_NATIVE, sizeof(PxNative), op);
        if (o) {
            PxNative *n = (PxNative *)o;
            n->fn       = fn_proto_call;
            n->name     = vm->atom[PX_ATOM_empty];
            n->data     = PX_UNDEFINED;
            o->flags |= PX_OBJ_NOT_CTOR;
        }
        break;
    case PX_PROTO_ARRAY: o = px_obj_new(vm, PX_T_ARRAY, sizeof(PxArray), op); break;
    case PX_PROTO_STRING:
    case PX_PROTO_NUMBER:
    case PX_PROTO_BOOLEAN:
        o = px_obj_new(vm, PX_T_BOXED, sizeof(PxBoxed), op);
        if (o)
            ((PxBoxed *)o)->value = index == PX_PROTO_STRING   ? vm->atom[PX_ATOM_empty]
                                    : index == PX_PROTO_NUMBER ? px_from_smi(0)
                                                               : PX_FALSE;
        break;
    default: return new_plain(vm, op);
    }
    return o ? px_from_ptr(o) : PX_EXCEPTION;
}

/* A built-in accessor property; attrs as px_define_accessor takes them. */
static int def_accessor(PxVM *vm, PxValue obj, const char *name, PxNativeFn fn, int magic_get, int magic_set,
                        uint32_t attrs) {
    char    buf[64];
    PxValue g, st = PX_UNDEFINED, k;
    int     r = -1;
    PX_ROOT(vm, obj);
    snprintf(buf, sizeof buf, "get %s", name);
    g = px_make_native(vm, fn, buf, 0, magic_get);
    if (g == PX_EXCEPTION) goto out;
    ((PxObject *)px_ptr(g))->flags |= PX_OBJ_NOT_CTOR;
    PX_ROOT(vm, g);
    PX_ROOT(vm, st);
    if (magic_set >= 0) {
        snprintf(buf, sizeof buf, "set %s", name);
        st = px_make_native(vm, fn, buf, 1, magic_set);
        if (st == PX_EXCEPTION) goto out2;
        ((PxObject *)px_ptr(st))->flags |= PX_OBJ_NOT_CTOR;
    }
    k = px_intern_cstr(vm, name);
    if (k != PX_EXCEPTION) r = px_define_accessor(vm, obj, k, g, st, attrs);
out2:
    px_pop_roots(vm, 2);
out:
    px_pop_roots(vm, 1);
    return r;
}

/* A native constructor: global[name] = ctor, ctor.prototype = proto,
 * proto.constructor = ctor. */
static int def_ctor(PxVM *vm, const char *name, PxNativeFn fn, int length, int magic, int proto_index) {
    PxValue c = px_make_native(vm, fn, name, length, magic);
    if (c == PX_EXCEPTION) return -1;
    PX_ROOT(vm, c);
    vm->ctors[proto_index] = c;
    if (px_def_value(vm, c, "prototype", vm->protos[proto_index], 0) < 0 ||
        px_define(vm, vm->protos[proto_index], vm->atom[PX_ATOM_constructor], c, PX_ATTR_HIDDEN) < 0 ||
        px_def_value(vm, vm->global, name, c, PX_ATTR_HIDDEN) < 0) {
        px_pop_roots(vm, 1);
        return -1;
    }
    px_pop_roots(vm, 1);
    return 0;
}

static const char *const k_error_names[PX_ERROR_TYPES] = {"Error",       "TypeError",      "RangeError",
                                                           "ReferenceError", "SyntaxError", "InternalError",
                                                           "AggregateError", "EvalError",   "URIError"};

#define TRY(x) do { if ((x) < 0) return -1; } while (0)

/* Symbol.prototype[Symbol.toPrimitive]: thisSymbolValue */
static PxValue symp_to_primitive(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (px_is_obj(t) && px_type_of(t) == PX_T_BOXED) t = ((PxBoxed *)px_ptr(t))->value;
    if (px_is_ptr(t) && px_type_of(t) == PX_T_SYMBOL) return t;
    return px_throw_error(vm, PX_TYPE_ERROR, "Symbol.prototype[Symbol.toPrimitive] called on a non-symbol");
}

/* A well-known symbol px_iter.c does not make: Symbol[name], in *slot. */
static int def_well_known(PxVM *vm, PxValue *slot, const char *name) {
    char    buf[40];
    PxValue d;
    snprintf(buf, sizeof buf, "Symbol.%s", name);
    d = px_str_from_cstr(vm, buf);
    if (d == PX_EXCEPTION) return -1;
    *slot = px_symbol_new(vm, d);
    if (*slot == PX_EXCEPTION) return -1;
    return px_def_value(vm, vm->ctors[PX_PROTO_SYMBOL], name, *slot, 0);
}

/* Array[Symbol.species] and Array.prototype[Symbol.unscopables] */
static int array_symbols(PxVM *vm) {
    static const char *const unscopable[] = {"at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast",
                                             "findLastIndex", "flat", "flatMap", "includes", "keys", "toReversed",
                                             "toSorted", "toSpliced", "values"};
    PxValue g = px_make_native(vm, arr_species, "get [Symbol.species]", 0, 0), u;
    int     i, r;
    if (g == PX_EXCEPTION) return -1;
    ((PxObject *)px_ptr(g))->flags |= PX_OBJ_NOT_CTOR;
    if (px_define_accessor(vm, vm->ctors[PX_PROTO_ARRAY], vm->sym_species, g, PX_UNDEFINED, PX_ATTR_CONFIGURABLE) < 0)
        return -1;
    u = new_plain(vm, PX_NULL);
    if (u == PX_EXCEPTION) return -1;
    PX_ROOT(vm, u);
    for (i = 0, r = 0; i < PX_COUNTOF(unscopable) && r == 0; i++) r = px_def_value(vm, u, unscopable[i], PX_TRUE, PX_ATTR_DEFAULT);
    if (r == 0) r = px_define(vm, vm->protos[PX_PROTO_ARRAY], vm->sym_unscopables, u, PX_ATTR_CONFIGURABLE);
    px_pop_roots(vm, 1);
    return r;
}

int px_builtins_init(PxVM *vm) {
    PxValue v;
    int     i;

    /* Prototypes first: every object made after this has a proper one. */
    vm->protos[PX_PROTO_OBJECT] = new_plain(vm, PX_NULL);
    TRY(vm->protos[PX_PROTO_OBJECT] == PX_EXCEPTION ? -1 : 0);
    for (i = PX_PROTO_FUNCTION; i < PX_PROTO_ERROR; i++) {
        vm->protos[i] = new_proto(vm, i);
        TRY(vm->protos[i] == PX_EXCEPTION ? -1 : 0);
    }
    vm->protos[PX_PROTO_ERROR] = new_plain(vm, vm->protos[PX_PROTO_OBJECT]);
    TRY(vm->protos[PX_PROTO_ERROR] == PX_EXCEPTION ? -1 : 0);
    for (i = 1; i < PX_ERROR_TYPES; i++) {
        vm->protos[PX_PROTO_ERROR + i] = new_plain(vm, vm->protos[PX_PROTO_ERROR]);
        TRY(vm->protos[PX_PROTO_ERROR + i] == PX_EXCEPTION ? -1 : 0);
    }
    vm->global = new_plain(vm, vm->protos[PX_PROTO_OBJECT]);
    TRY(vm->global == PX_EXCEPTION ? -1 : 0);

    /* The out-of-memory error exists before it is needed. */
    v = px_str_from_cstr(vm, "out of memory");
    TRY(v == PX_EXCEPTION ? -1 : 0);
    vm->oom_error = px_error_new(vm, PX_INTERNAL_ERROR, v);
    TRY(vm->oom_error == PX_EXCEPTION ? -1 : 0);

    TRY(def_ctor(vm, "Object", obj_ctor, 1, 0, PX_PROTO_OBJECT));
    TRY(px_def_fns(vm, vm->ctors[PX_PROTO_OBJECT], k_object_fns, PX_COUNTOF(k_object_fns)));
    TRY(px_def_fns(vm, vm->ctors[PX_PROTO_OBJECT], k_extra_object_fns, PX_COUNTOF(k_extra_object_fns)));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_OBJECT], k_object_proto_fns, PX_COUNTOF(k_object_proto_fns)));
    TRY(def_accessor(vm, vm->protos[PX_PROTO_OBJECT], "__proto__", objp_proto, 0, 1, PX_ATTR_CONFIGURABLE));

    TRY(def_ctor(vm, "Function", fn_ctor, 1, 0, PX_PROTO_FUNCTION));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_FUNCTION], k_function_proto_fns, PX_COUNTOF(k_function_proto_fns)));
    TRY(def_throw_type_error(vm));

    TRY(def_ctor(vm, "Array", arr_ctor, 1, 0, PX_PROTO_ARRAY));
    TRY(px_def_fns(vm, vm->ctors[PX_PROTO_ARRAY], k_array_fns, PX_COUNTOF(k_array_fns)));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_ARRAY], k_array_proto_fns, PX_COUNTOF(k_array_proto_fns)));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_ARRAY], k_extra_array_proto_fns, PX_COUNTOF(k_extra_array_proto_fns)));

    TRY(def_ctor(vm, "String", str_ctor, 1, 0, PX_PROTO_STRING));
    TRY(px_def_fns(vm, vm->ctors[PX_PROTO_STRING], k_string_fns, PX_COUNTOF(k_string_fns)));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_STRING], k_string_proto_fns, PX_COUNTOF(k_string_proto_fns)));
    TRY(px_def_fns(vm, vm->protos[PX_PROTO_STRING], k_extra_string_proto_fns, PX_COUNTOF(k_extra_string_proto_fns)));

    {
        static const PxFnDef num_fns[] = {
            {"isInteger", num_is, 1, 0}, {"isSafeInteger", num_is, 1, 1}, {"isFinite", num_is, 1, 2},
            {"isNaN", num_is, 1, 3},     {"parseFloat", g_parse_float, 1, 0}, {"parseInt", g_parse_int, 2, 0},
        };
        static const PxFnDef nump_fns[] = {
            {"toString", nump_to_string, 1, 0}, {"toLocaleString", nump_to_string, 0, 0},
            {"toFixed", nump_to_fixed, 1, 0},   {"toPrecision", nump_to_fixed, 1, 1},
            {"toExponential", nump_to_fixed, 1, 2}, {"valueOf", nump_value_of, 0, 0},
        };
        static const struct {
            const char *name;
            double      v;
        } consts[] = {
            {"MAX_SAFE_INTEGER", 9007199254740991.0}, {"MIN_SAFE_INTEGER", -9007199254740991.0},
            {"EPSILON", 2.220446049250313e-16},       {"MAX_VALUE", 1.7976931348623157e308},
            {"MIN_VALUE", 5e-324},                    {"POSITIVE_INFINITY", INFINITY},
            {"NEGATIVE_INFINITY", -INFINITY},         {"NaN", NAN},
        };
        TRY(def_ctor(vm, "Number", num_ctor, 1, 0, PX_PROTO_NUMBER));
        TRY(px_def_fns(vm, vm->ctors[PX_PROTO_NUMBER], num_fns, PX_COUNTOF(num_fns)));
        TRY(px_def_fns(vm, vm->protos[PX_PROTO_NUMBER], nump_fns, PX_COUNTOF(nump_fns)));
        for (i = 0; i < PX_COUNTOF(consts); i++) {
            v = px_number(vm, consts[i].v);
            TRY(v == PX_EXCEPTION ? -1 : 0);
            TRY(px_def_value(vm, vm->ctors[PX_PROTO_NUMBER], consts[i].name, v, 0));
        }
    }
    {
        static const PxFnDef boolp_fns[] = {{"toString", boolp_to_string, 0, 0}, {"valueOf", boolp_to_string, 0, 1}};
        TRY(def_ctor(vm, "Boolean", bool_ctor, 1, 0, PX_PROTO_BOOLEAN));
        TRY(px_def_fns(vm, vm->protos[PX_PROTO_BOOLEAN], boolp_fns, PX_COUNTOF(boolp_fns)));
    }
    for (i = 0; i < PX_ERROR_TYPES; i++) {
        TRY(def_ctor(vm, k_error_names[i], error_ctor, i == PX_AGGREGATE_ERROR ? 2 : 1, i, PX_PROTO_ERROR + i));
        /* the NativeErrors inherit from Error */
        if (i > 0) TRY(px_set_proto(vm, vm->ctors[PX_PROTO_ERROR + i], vm->ctors[PX_PROTO_ERROR]));
        TRY(px_def_value(vm, vm->protos[PX_PROTO_ERROR + i], "name", px_str_from_cstr(vm, k_error_names[i]),
                         PX_ATTR_HIDDEN));
        TRY(px_define(vm, vm->protos[PX_PROTO_ERROR + i], vm->atom[PX_ATOM_message], vm->atom[PX_ATOM_empty],
                      PX_ATTR_HIDDEN));
    }
    {
        static const PxFnDef errp_fns[] = {{"toString", errorp_to_string, 0, 0}};
        TRY(px_def_fns(vm, vm->protos[PX_PROTO_ERROR], errp_fns, PX_COUNTOF(errp_fns)));
    }
    {
        PxValue m = new_plain(vm, vm->protos[PX_PROTO_OBJECT]);
        static const struct {
            const char *name;
            double      v;
        } consts[] = {
            {"PI", 3.141592653589793},  {"E", 2.718281828459045},        {"LN2", 0.6931471805599453},
            {"LN10", 2.302585092994046}, {"LOG2E", 1.4426950408889634},   {"LOG10E", 0.4342944819032518},
            {"SQRT2", 1.4142135623730951}, {"SQRT1_2", 0.7071067811865476},
        };
        TRY(m == PX_EXCEPTION ? -1 : 0);
        PX_ROOT(vm, m);
        if (px_def_value(vm, vm->global, "Math", m, PX_ATTR_HIDDEN) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
        px_pop_roots(vm, 1);
        TRY(px_def_fns(vm, m, k_math_fns, PX_COUNTOF(k_math_fns)));
        for (i = 0; i < PX_COUNTOF(consts); i++) {
            v = px_number(vm, consts[i].v);
            TRY(v == PX_EXCEPTION ? -1 : 0);
            TRY(px_def_value(vm, m, consts[i].name, v, 0));
        }
    }
    TRY(px_def_fns(vm, vm->global, k_global_fns, PX_COUNTOF(k_global_fns)));
    {
        PxObject *r = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), vm->protos[PX_PROTO_OBJECT]);
        PxValue   rv;
        TRY(r ? 0 : -1);
        rv = px_from_ptr(r);
        PX_ROOT(vm, rv);
        if (px_def_value(vm, vm->global, "Reflect", rv, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, rv, k_reflect_fns, PX_COUNTOF(k_reflect_fns)) < 0) {
            px_pop_roots(vm, 1);
            return -1;
        }
        px_pop_roots(vm, 1);
    }
    TRY(px_iter_init(vm));
    TRY(px_promise_init(vm));
    TRY(px_asyncgen_init(vm));
    TRY(px_collections_init(vm));
    TRY(px_date_init(vm));
    TRY(px_regexp_init(vm));
    TRY(px_typed_init(vm));
    TRY(px_web_init(vm));
    TRY(px_proxy_init(vm));
    {
        /* after px_iter_init: the well-known symbols exist */
        PxValue f = px_make_native(vm, fnp_has_instance, "[Symbol.hasInstance]", 1, 0), rv;
        TRY(f == PX_EXCEPTION ? -1 : 0);
        ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
        TRY(px_define(vm, vm->protos[PX_PROTO_FUNCTION], vm->sym_has_instance, f, 0));
        rv = px_get(vm, vm->global, px_intern_cstr(vm, "Reflect"));
        TRY(rv == PX_EXCEPTION ? -1 : px_def_tag(vm, rv, "Reflect"));
        rv = px_get(vm, vm->global, px_intern_cstr(vm, "Math"));
        TRY(rv == PX_EXCEPTION ? -1 : px_def_tag(vm, rv, "Math"));
        TRY(def_well_known(vm, &vm->sym_is_concat_spreadable, "isConcatSpreadable"));
        TRY(def_well_known(vm, &vm->sym_unscopables, "unscopables"));
        TRY(array_symbols(vm));
        f = px_make_native(vm, symp_to_primitive, "[Symbol.toPrimitive]", 1, 0);
        TRY(f == PX_EXCEPTION ? -1 : 0);
        ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
        TRY(px_define(vm, vm->protos[PX_PROTO_SYMBOL], vm->sym_to_primitive, f, PX_ATTR_CONFIGURABLE));
        TRY(px_def_tag(vm, vm->protos[PX_PROTO_SYMBOL], "Symbol"));
    }
    TRY(px_def_value(vm, vm->global, "globalThis", vm->global, PX_ATTR_HIDDEN));
    TRY(px_def_value(vm, vm->global, "undefined", PX_UNDEFINED, 0));
    v = px_number(vm, NAN);
    TRY(v == PX_EXCEPTION ? -1 : 0);
    TRY(px_def_value(vm, vm->global, "NaN", v, 0));
    v = px_number(vm, INFINITY);
    TRY(v == PX_EXCEPTION ? -1 : 0);
    TRY(px_def_value(vm, vm->global, "Infinity", v, 0));
    return px_json_init(vm);
}
