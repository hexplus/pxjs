/* Map, Set, WeakMap and WeakSet.
 *
 * One insertion-ordered hash table (see PxMap) serves all four. Keys
 * compare with SameValueZero: numbers by value (NaN equals NaN, -0 equals
 * 0), strings by content, everything else by identity.
 *
 * WeakMap, WeakSet, WeakRef and FinalizationRegistry use the same table;
 * the collector treats their keys weakly (ephemerons, see px_heap.c).
 * Their keys may be objects, or symbols not made by Symbol.for. */

#include <math.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))

static uint32_t value_hash(PxVM *vm, PxValue k) {
    /* the same hash as the double path below gives an integer */
    if (px_is_smi(k)) return (uint32_t)px_smi(k) * 2654435761u;
    if (px_is_num(k)) {
        double   d = px_num(k);
        uint32_t w[2];
        if (d == 0) d = 0; /* -0 hashes as 0 */
        if (isnan(d)) return 0x7FF80000u;
        if (d == (double)(int32_t)d) return (uint32_t)(int32_t)d * 2654435761u;
        memcpy(w, &d, sizeof d);
        return (w[0] ^ w[1]) * 2654435761u;
    }
    if (px_is_str(k)) {
        PxString *s = px_str_flat(vm, k);
        uint32_t  h = 2166136261u, i;
        if (!s) return 0;
        if (s->hash) return s->hash;
        for (i = 0; i < s->len; i++) {
            h ^= px_str_at(s, i);
            h *= 16777619u;
        }
        /* strings are immutable: the next lookup with this key is free
         * (the same function as the atom table's, so the field agrees) */
        s->hash = h ? h : 1;
        return s->hash;
    }
    return (uint32_t)k * 2654435761u;
}

static int32_t *map_index(PxMap *m) { return (int32_t *)(void *)m->index->data; }

/* index of the entry for key, or -1 */
static int32_t map_find(PxVM *vm, PxMap *m, PxValue key) {
    uint32_t mask, j;
    if (!m->cap) return -1;
    mask = m->cap * 2 - 1;
    for (j = value_hash(vm, key) & mask;; j = (j + 1) & mask) {
        int32_t e = map_index(m)[j];
        if (e < 0) return -1;
        if (m->entries->items[2 * e] != PX_HOLE && px_same_value_zero(vm, m->entries->items[2 * e], key)) return e;
    }
}

static int map_rehash(PxVM *vm, PxValue mv, uint32_t cap) {
    PxMap   *m = (PxMap *)px_ptr(mv);
    PxVec   *ne;
    PxBytes *ni;
    PxValue  nev;
    uint32_t i, used = 0;
    PX_ROOT(vm, mv);
    ne = px_vec_new(vm, cap * 2);
    if (!ne) goto fail;
    nev = px_from_ptr(ne);
    PX_ROOT(vm, nev);
    ni = px_bytes_new(vm, NULL, cap * 2 * (uint32_t)sizeof(int32_t));
    px_pop_roots(vm, 1);
    if (!ni) goto fail;
    m = (PxMap *)px_ptr(mv);
    memset(ni->data, 0xFF, ni->len);
    for (i = 0; i < m->used; i++) {
        PxValue k = m->entries->items[2 * i];
        if (k == PX_HOLE) continue;
        ne->items[2 * used]     = k;
        ne->items[2 * used + 1] = m->entries->items[2 * i + 1];
        used++;
    }
    m->entries = ne;
    m->index   = ni;
    m->cap     = cap;
    m->used    = used;
    for (i = 0; i < used; i++) {
        uint32_t mask = cap * 2 - 1, j;
        for (j = value_hash(vm, ne->items[2 * i]) & mask; map_index(m)[j] >= 0; j = (j + 1) & mask) {}
        map_index(m)[j] = (int32_t)i;
    }
    px_pop_roots(vm, 1);
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

static int map_set(PxVM *vm, PxValue mv, PxValue key, PxValue value) {
    PxMap   *m = (PxMap *)px_ptr(mv);
    int32_t  e;
    uint32_t mask, j;
    if (px_is_ptr(key) && px_type_of(key) == PX_T_NUMBER && ((PxNumber *)px_ptr(key))->d == 0)
        key = px_from_smi(0); /* -0 is stored as 0 */
    e = map_find(vm, m, key);
    if (e >= 0) {
        m->entries->items[2 * e + 1] = value;
        return 0;
    }
    if (m->used == m->cap) {
        /* a power of two: the index is probed with `& mask` */
        uint32_t cap = 8;
        while (cap < m->count * 2) cap *= 2;
        PX_ROOT(vm, key);
        PX_ROOT(vm, value);
        if (map_rehash(vm, mv, cap) < 0) {
            px_pop_roots(vm, 2);
            return -1;
        }
        px_pop_roots(vm, 2);
        m = (PxMap *)px_ptr(mv);
    }
    m->entries->items[2 * m->used]     = key;
    m->entries->items[2 * m->used + 1] = value;
    mask                               = m->cap * 2 - 1;
    for (j = value_hash(vm, key) & mask; map_index(m)[j] >= 0; j = (j + 1) & mask) {}
    map_index(m)[j] = (int32_t)m->used;
    m->used++;
    m->count++;
    return 0;
}

/* CanBeHeldWeakly: an object, or a symbol not made by Symbol.for */
static int weak_ok(PxVM *vm, PxValue v) {
    if (px_is_obj(v)) return 1;
    return px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL && px_symbol_registered(vm, v) == 0;
}

/* The object a built-in constructor makes for `new.target` (which must be
 * there): newTarget.prototype, or the intrinsic `proto_index`. */
static PxValue new_collection(PxVM *vm, int proto_index, int kind) {
    PxValue nt = vm->native_new_target, proto = vm->protos[proto_index];
    PxMap  *m;
    if (nt == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "the constructor requires 'new'");
    if (nt != vm->native_callee) {
        proto = px_get(vm, nt, vm->atom[PX_ATOM_prototype]);
        if (proto == PX_EXCEPTION) return proto;
        if (!px_is_obj(proto)) proto = vm->protos[proto_index];
    }
    PX_ROOT(vm, proto);
    m = (PxMap *)px_obj_new(vm, PX_T_MAP, sizeof(PxMap), proto);
    px_pop_roots(vm, 1);
    if (!m) return PX_EXCEPTION;
    m->kind = (uint32_t)kind;
    return px_from_ptr(m);
}

/* new Map/Set/WeakMap/WeakSet(iterable): each item goes through the new
 * object's own set/add (which a subclass may replace); an abrupt step
 * closes the iterator. */
static PxValue map_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     kind = vm->native_magic, is_map = kind == PX_MAP_MAP || kind == PX_MAP_WEAKMAP, r;
    PxValue mv, adder = PX_UNDEFINED, rec = PX_UNDEFINED, item, args[2];
    (void)t;
    mv = new_collection(vm, PX_PROTO_MAP + kind, kind);
    if (mv == PX_EXCEPTION || ARG(0) == PX_UNDEFINED || ARG(0) == PX_NULL) return mv;
    PX_ROOT(vm, mv);
    PX_ROOT(vm, adder);
    PX_ROOT(vm, rec);
    adder = px_get(vm, mv, px_intern_cstr(vm, is_map ? "set" : "add"));
    if (adder == PX_EXCEPTION) goto fail;
    if (!px_is_callable(adder)) {
        px_throw_error(vm, PX_TYPE_ERROR, "the collection's %s is not a function", is_map ? "set" : "add");
        goto fail;
    }
    rec = px_iter_record(vm, argv[0], 0);
    if (rec == PX_EXCEPTION) goto fail;
    while ((r = px_record_step(vm, rec, &item)) > 0) {
        args[0] = item;
        if (is_map) {
            if (!px_is_obj(item)) {
                px_throw_error(vm, PX_TYPE_ERROR, "iterator value is not an entry object");
                goto close;
            }
            PX_ROOT(vm, item);
            args[0] = px_get_index(vm, item, 0);
            if (args[0] != PX_EXCEPTION) {
                PX_ROOT(vm, args[0]);
                args[1] = px_get_index(vm, item, 1);
                px_pop_roots(vm, 1);
            }
            px_pop_roots(vm, 1);
            if (args[0] == PX_EXCEPTION || args[1] == PX_EXCEPTION) goto close;
        }
        if (px_call(vm, adder, mv, is_map ? 2 : 1, args) == PX_EXCEPTION) goto close;
    }
    if (r < 0) goto fail;
    px_pop_roots(vm, 3);
    return mv;
close:
    px_iterator_close_throw(vm, ((PxVec *)px_ptr(rec))->items[0]);
fail:
    px_pop_roots(vm, 3);
    return PX_EXCEPTION;
}

/* The receiver, if it is a collection of the kind in the method's magic
 * (bits 4 and up; the low bits are the method's own). */
#define KIND(k) ((k) << 4)
static PxMap *this_map(PxVM *vm, PxValue t) {
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP || ((PxMap *)px_ptr(t))->kind != (uint32_t)(vm->native_magic >> 4)) {
        px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
        return NULL;
    }
    return (PxMap *)px_ptr(t);
}

/* get (0) / has (1) / delete (2) */
static PxValue mapp_lookup(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap  *m = this_map(vm, t);
    int32_t e;
    if (!m) return PX_EXCEPTION;
    e = map_find(vm, m, ARG(0));
    switch (vm->native_magic & 15) {
    case 0: return e < 0 ? PX_UNDEFINED : m->entries->items[2 * e + 1];
    case 1: return px_bool(e >= 0);
    default:
        if (e < 0) return PX_FALSE;
        m->entries->items[2 * e]     = PX_HOLE; /* the index keeps pointing here; never matches */
        m->entries->items[2 * e + 1] = PX_UNDEFINED;
        m->count--;
        return PX_TRUE;
    }
}

static PxValue mapp_set(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap *m = this_map(vm, t);
    if (!m) return PX_EXCEPTION;
    if ((m->kind == PX_MAP_WEAKMAP || m->kind == PX_MAP_WEAKSET) && !weak_ok(vm, ARG(0)))
        return px_throw_error(vm, PX_TYPE_ERROR, "invalid value used as weak key");
    PX_ROOT(vm, t);
    if (map_set(vm, t, ARG(0), (m->kind == PX_MAP_SET || m->kind == PX_MAP_WEAKSET) ? ARG(0) : ARG(1)) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    return t;
}

static PxValue mapp_clear(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap *m = this_map(vm, t);
    (void)argc;
    (void)argv;
    if (!m) return PX_EXCEPTION;
    m->entries = NULL;
    m->index   = NULL;
    m->cap = m->used = m->count = 0;
    return PX_UNDEFINED;
}

static PxValue mapp_size(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (!this_map(vm, t)) return PX_EXCEPTION;
    return px_number(vm, ((PxMap *)px_ptr(t))->count);
}

static PxValue mapp_for_each(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap   *m;
    PxValue  fn = ARG(0);
    uint32_t i;
    if (!this_map(vm, t)) return PX_EXCEPTION;
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "forEach: callback is not a function");
    PX_ROOT(vm, t);
    for (i = 0; i < ((PxMap *)px_ptr(t))->used; i++) {
        PxValue args[3], r;
        m = (PxMap *)px_ptr(t);
        if (m->entries->items[2 * i] == PX_HOLE) continue;
        args[0] = m->entries->items[2 * i + 1];
        args[1] = m->entries->items[2 * i];
        args[2] = t;
        r       = px_call(vm, fn, ARG(1), 3, args);
        if (r == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            return r;
        }
    }
    px_pop_roots(vm, 1);
    return PX_UNDEFINED;
}

static PxValue mapp_iter(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (!this_map(vm, t)) return PX_EXCEPTION;
    return px_make_iterobj(vm, t, vm->native_magic & 15);
}

int px_map_iter_next(PxVM *vm, PxIterObj *it, PxValue *key, PxValue *value) {
    PxMap *m = (PxMap *)px_ptr(it->target);
    (void)vm;
    while (it->index < m->used) {
        uint32_t i = it->index++;
        if (m->entries->items[2 * i] == PX_HOLE) continue;
        *key   = m->entries->items[2 * i];
        *value = m->entries->items[2 * i + 1];
        return 1;
    }
    return 0;
}

/* groupBy helpers are left to userland; Map.groupBy is small enough: */
static PxValue map_group_by(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue iter, item, mv, fn = ARG(1);
    PxMap  *m;
    double  k = 0;
    int     r;
    (void)t;
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "groupBy: callback is not a function");
    m = (PxMap *)px_obj_new(vm, PX_T_MAP, sizeof(PxMap), vm->protos[PX_PROTO_MAP]);
    if (!m) return PX_EXCEPTION;
    mv = px_from_ptr(m);
    PX_ROOT(vm, mv);
    iter = px_get_iterator(vm, ARG(0));
    if (iter == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, iter);
    while ((r = px_iterator_step(vm, iter, &item)) > 0) {
        PxValue args[2], key, group;
        int32_t e;
        PX_ROOT(vm, item);
        args[0] = item;
        args[1] = px_number(vm, k++);
        key     = px_call(vm, fn, PX_UNDEFINED, 2, args);
        if (key == PX_EXCEPTION) goto fail3;
        PX_ROOT(vm, key);
        e = map_find(vm, (PxMap *)px_ptr(mv), key);
        if (e < 0) {
            group = px_array_new(vm, 0);
            if (group == PX_EXCEPTION || map_set(vm, mv, key, group) < 0) goto fail4;
        } else {
            group = ((PxMap *)px_ptr(mv))->entries->items[2 * e + 1];
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
    return mv;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* ------------------------------------------------------------ for C code */

PxValue px_map_create(PxVM *vm, int kind) {
    PxMap *m = (PxMap *)px_obj_new(vm, PX_T_MAP, sizeof(PxMap), vm->protos[PX_PROTO_MAP + kind]);
    if (!m) return PX_EXCEPTION;
    m->kind = (uint32_t)kind;
    return px_from_ptr(m);
}

int px_map_put(PxVM *vm, PxValue map, PxValue key, PxValue value) { return map_set(vm, map, key, value); }

/* The value for key, or PX_HOLE if absent. */
PxValue px_map_lookup(PxVM *vm, PxValue map, PxValue key) {
    PxMap  *m = (PxMap *)px_ptr(map);
    int32_t e = map_find(vm, m, key);
    return e < 0 ? PX_HOLE : m->entries->items[2 * e + 1];
}

/* ------------------------------------------------------------ Set methods
 *
 * union, intersection, difference, symmetricDifference, isSubsetOf,
 * isSupersetOf, isDisjointFrom (ES2025). The argument is set-like: a Set or
 * Map directly, or any object with size, has() and keys(). */

typedef struct SetLike {
    PxValue obj, has, keys; /* has/keys: undefined for a Set/Map */
    double  size;
} SetLike;

static int set_like(PxVM *vm, PxValue v, SetLike *s) {
    PxValue sz;
    s->obj  = v;
    s->has  = PX_UNDEFINED;
    s->keys = PX_UNDEFINED;
    if (px_is_obj(v) && px_type_of(v) == PX_T_MAP && ((PxMap *)px_ptr(v))->kind <= PX_MAP_SET) {
        s->size = ((PxMap *)px_ptr(v))->count;
        return 0;
    }
    if (!px_is_obj(v)) {
        px_throw_error(vm, PX_TYPE_ERROR, "the argument must be set-like (size, has, keys)");
        return -1;
    }
    sz = px_get(vm, v, px_intern_cstr(vm, "size"));
    if (sz == PX_EXCEPTION || px_to_number(vm, sz, &s->size) < 0) return -1;
    if (s->size != s->size) {
        px_throw_error(vm, PX_TYPE_ERROR, "the argument's size is not a number");
        return -1;
    }
    s->has = px_get(vm, v, px_intern_cstr(vm, "has"));
    if (s->has == PX_EXCEPTION) return -1;
    s->keys = px_get(vm, v, px_intern_cstr(vm, "keys"));
    if (s->keys == PX_EXCEPTION) return -1;
    if (!px_is_callable(s->has) || !px_is_callable(s->keys)) {
        px_throw_error(vm, PX_TYPE_ERROR, "the argument must have has() and keys() methods");
        return -1;
    }
    return 0;
}

/* 1, 0, or -1 (exception) */
static int set_like_has(PxVM *vm, SetLike *s, PxValue k) {
    PxValue r;
    if (s->has == PX_UNDEFINED) return map_find(vm, (PxMap *)px_ptr(s->obj), k) >= 0;
    r = px_call(vm, s->has, s->obj, 1, &k);
    return r == PX_EXCEPTION ? -1 : px_truthy(r);
}

/* An iterator over the set-like's keys (for px_iterator_step). */
static PxValue set_like_keys(PxVM *vm, SetLike *s) {
    PxValue it;
    if (s->has == PX_UNDEFINED) {
        PxMap *m = (PxMap *)px_ptr(s->obj);
        it       = px_make_iterobj(vm, s->obj, m->kind == PX_MAP_SET ? PX_IT_MAP_VALUES : PX_IT_MAP_KEYS);
    } else {
        it = px_call(vm, s->keys, s->obj, 0, NULL);
    }
    if (it != PX_EXCEPTION && !px_is_obj(it)) return px_throw_error(vm, PX_TYPE_ERROR, "keys() did not return an iterator");
    return it;
}

static PxValue set_copy(PxVM *vm, PxValue src) {
    PxValue  out = px_map_create(vm, PX_MAP_SET);
    uint32_t i;
    if (out == PX_EXCEPTION) return out;
    PX_ROOT(vm, out);
    PX_ROOT(vm, src);
    for (i = 0; i < ((PxMap *)px_ptr(src))->used; i++) {
        PxValue k = ((PxMap *)px_ptr(src))->entries->items[2 * i];
        if (k == PX_HOLE) continue;
        if (map_set(vm, out, k, k) < 0) {
            px_pop_roots(vm, 2);
            return PX_EXCEPTION;
        }
    }
    px_pop_roots(vm, 2);
    return out;
}

static void set_remove(PxVM *vm, PxValue set, PxValue k) {
    PxMap  *m = (PxMap *)px_ptr(set);
    int32_t e = map_find(vm, m, k);
    if (e < 0) return;
    m->entries->items[2 * e]     = PX_HOLE;
    m->entries->items[2 * e + 1] = PX_UNDEFINED;
    m->count--;
}

enum { SO_UNION, SO_INTERSECTION, SO_DIFFERENCE, SO_SYMDIFF, SO_SUBSET, SO_SUPERSET, SO_DISJOINT };

static PxValue setp_op(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      op = vm->native_magic, r, iterate_this;
    SetLike  o;
    PxValue  out = PX_UNDEFINED, iter = PX_UNDEFINED, k;
    uint32_t i, this_size;

    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP || ((PxMap *)px_ptr(t))->kind != PX_MAP_SET)
        return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver: not a Set");
    PX_ROOT(vm, t);
    PX_ROOT(vm, o.obj);
    PX_ROOT(vm, o.has);
    PX_ROOT(vm, o.keys);
    PX_ROOT(vm, out);
    PX_ROOT(vm, iter);
    o.obj = o.has = o.keys = PX_UNDEFINED;
    if (set_like(vm, ARG(0), &o) < 0) goto fail;
    this_size = ((PxMap *)px_ptr(t))->count;
    iterate_this = (double)this_size <= o.size;

    switch (op) {
    case SO_SUBSET:
        if ((double)this_size > o.size) goto no;
        /* every element of this must be in other */
        __attribute__((fallthrough));
    case SO_INTERSECTION:
    case SO_DISJOINT:
        if (op == SO_INTERSECTION) {
            out = px_map_create(vm, PX_MAP_SET);
            if (out == PX_EXCEPTION) goto fail;
        }
        if (op == SO_SUBSET || iterate_this) {
            for (i = 0; i < ((PxMap *)px_ptr(t))->used; i++) {
                k = ((PxMap *)px_ptr(t))->entries->items[2 * i];
                if (k == PX_HOLE) continue;
                PX_ROOT(vm, k);
                r = set_like_has(vm, &o, k);
                if (r >= 0 && op == SO_INTERSECTION && r && map_set(vm, out, k, k) < 0) r = -1;
                px_pop_roots(vm, 1);
                if (r < 0) goto fail;
                if (op == SO_SUBSET && !r) goto no;
                if (op == SO_DISJOINT && r) goto no;
            }
            if (op == SO_INTERSECTION) goto done_out;
            goto yes;
        }
        /* other is smaller: walk its keys */
        break;
    case SO_SUPERSET:
        if ((double)this_size < o.size) goto no;
        break;
    case SO_DIFFERENCE:
        out = set_copy(vm, t);
        if (out == PX_EXCEPTION) goto fail;
        if (iterate_this) {
            for (i = 0; i < ((PxMap *)px_ptr(t))->used; i++) {
                k = ((PxMap *)px_ptr(t))->entries->items[2 * i];
                if (k == PX_HOLE) continue;
                PX_ROOT(vm, k);
                r = set_like_has(vm, &o, k);
                px_pop_roots(vm, 1);
                if (r < 0) goto fail;
                if (r) set_remove(vm, out, k);
            }
            goto done_out;
        }
        break;
    default: /* union, symmetric difference */
        out = set_copy(vm, t);
        if (out == PX_EXCEPTION) goto fail;
        break;
    }

    /* walk the other's keys */
    iter = set_like_keys(vm, &o);
    if (iter == PX_EXCEPTION) goto fail;
    while ((r = px_iterator_step(vm, iter, &k)) > 0) {
        int in_this;
        if (px_is_ptr(k) && px_type_of(k) == PX_T_NUMBER && ((PxNumber *)px_ptr(k))->d == 0) k = px_from_smi(0);
        in_this = map_find(vm, (PxMap *)px_ptr(t), k) >= 0;
        PX_ROOT(vm, k);
        switch (op) {
        case SO_UNION: r = map_set(vm, out, k, k); break;
        case SO_INTERSECTION: r = in_this ? map_set(vm, out, k, k) : 0; break;
        case SO_DIFFERENCE: set_remove(vm, out, k); r = 0; break;
        case SO_SYMDIFF:
            if (in_this) set_remove(vm, out, k), r = 0;
            else r = map_set(vm, out, k, k);
            break;
        case SO_SUPERSET: r = in_this ? 0 : 1; break;
        default: r = in_this ? 1 : 0; break; /* disjoint */
        }
        px_pop_roots(vm, 1);
        if (r < 0) goto fail;
        if (r > 0 && (op == SO_SUPERSET || op == SO_DISJOINT)) {
            px_iterator_close(vm, iter);
            goto no;
        }
    }
    if (r < 0) goto fail;
    if (op == SO_SUPERSET || op == SO_DISJOINT) goto yes;
done_out:
    px_pop_roots(vm, 6);
    return out;
yes:
    px_pop_roots(vm, 6);
    return PX_TRUE;
no:
    px_pop_roots(vm, 6);
    return PX_FALSE;
fail:
    px_pop_roots(vm, 6);
    return PX_EXCEPTION;
}

/* ------------------------------------------------------------ WeakRef */

static int keep_alive(PxVM *vm, PxValue target) {
    if (vm->kept == PX_UNDEFINED) {
        PxValue a;
        PX_ROOT(vm, target);
        a = px_array_new(vm, 0);
        px_pop_roots(vm, 1);
        if (a == PX_EXCEPTION) return -1;
        vm->kept = a;
    }
    return px_array_push(vm, vm->kept, target);
}

static PxValue weakref_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue r;
    (void)t;
    if (vm->native_new_target == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "WeakRef requires 'new'");
    if (!weak_ok(vm, ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "WeakRef: the target must be an object or a symbol");
    r = new_collection(vm, PX_PROTO_WEAKREF, PX_MAP_WEAKREF);
    if (r == PX_EXCEPTION) return r;
    PX_ROOT(vm, r);
    if (map_set(vm, r, argv[0], argv[0]) < 0 || keep_alive(vm, argv[0]) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    return r;
}

static PxValue weakref_deref(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap  *m;
    PxValue target;
    (void)argc;
    (void)argv;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP || ((PxMap *)px_ptr(t))->kind != PX_MAP_WEAKREF)
        return px_throw_error(vm, PX_TYPE_ERROR, "WeakRef.prototype.deref called on a non-WeakRef");
    m      = (PxMap *)px_ptr(t);
    target = m->entries && m->used > 0 ? m->entries->items[0] : PX_HOLE;
    if (target == PX_HOLE) return PX_UNDEFINED; /* collected */
    PX_ROOT(vm, target);
    if (keep_alive(vm, target) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    return target;
}

/* ------------------------------------------------------------ FinalizationRegistry
 *
 * Each target maps to an array [held, token, held, token, ...]. A token
 * equal to the target is stored as a hole (holding the target strongly
 * would keep it alive forever); other tokens are held strongly, a
 * deviation from the spec's weak tokens. */

static PxMap *this_finreg(PxVM *vm, PxValue t) {
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP || ((PxMap *)px_ptr(t))->kind != PX_MAP_FINREG) {
        px_throw_error(vm, PX_TYPE_ERROR, "not a FinalizationRegistry");
        return NULL;
    }
    return (PxMap *)px_ptr(t);
}

static PxValue finreg_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue r;
    (void)t;
    if (vm->native_new_target == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "FinalizationRegistry requires 'new'");
    if (!px_is_callable(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "FinalizationRegistry: the cleanup callback must be a function");
    r = new_collection(vm, PX_PROTO_FINREG, PX_MAP_FINREG);
    if (r != PX_EXCEPTION) ((PxMap *)px_ptr(r))->extra = argv[0];
    return r;
}

static PxValue finreg_register(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue target = ARG(0), held = ARG(1), token = ARG(2), list;
    if (!this_finreg(vm, t)) return PX_EXCEPTION;
    if (!weak_ok(vm, target)) return px_throw_error(vm, PX_TYPE_ERROR, "register: the target must be an object or a symbol");
    if (held == target) return px_throw_error(vm, PX_TYPE_ERROR, "register: the held value cannot be the target");
    if (token != PX_UNDEFINED && !weak_ok(vm, token)) return px_throw_error(vm, PX_TYPE_ERROR, "register: the token must be an object or a symbol");
    PX_ROOT(vm, t);
    list = px_map_lookup(vm, t, target);
    if (list == PX_HOLE) {
        list = px_array_new(vm, 2);
        if (list == PX_EXCEPTION || map_set(vm, t, target, list) < 0) goto fail;
    }
    PX_ROOT(vm, list);
    if (px_array_push(vm, list, held) < 0 || px_array_push(vm, list, token == target ? PX_HOLE : token) < 0) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    px_pop_roots(vm, 2);
    return PX_UNDEFINED;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue finreg_unregister(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap   *m = this_finreg(vm, t);
    PxValue  token = ARG(0);
    uint32_t i, j, w;
    int      removed = 0;
    if (!m) return PX_EXCEPTION;
    if (!weak_ok(vm, token)) return px_throw_error(vm, PX_TYPE_ERROR, "unregister: the token must be an object or a symbol");
    for (i = 0; m->entries && i < m->used; i++) {
        PxValue  key = m->entries->items[2 * i];
        PxArray *a;
        if (key == PX_HOLE) continue;
        a = (PxArray *)px_ptr(m->entries->items[2 * i + 1]);
        for (j = w = 0; a->elems && j + 1 < a->length; j += 2) {
            PxValue tok = a->elems->items[j + 1];
            if (tok == token || (tok == PX_HOLE && key == token)) {
                removed = 1;
                continue;
            }
            a->elems->items[w]     = a->elems->items[j];
            a->elems->items[w + 1] = tok;
            w += 2;
        }
        if (a->elems) px_array_set_length(vm, a, w);
        if (w == 0) {
            m->entries->items[2 * i]     = PX_HOLE;
            m->entries->items[2 * i + 1] = PX_UNDEFINED;
            m->count--;
        }
    }
    return px_bool(removed);
}

int px_collections_init(PxVM *vm) {
    static const char *const names[4] = {"Map", "Set", "WeakMap", "WeakSet"};
#define M KIND(PX_MAP_MAP)
#define S KIND(PX_MAP_SET)
#define WM KIND(PX_MAP_WEAKMAP)
#define WS KIND(PX_MAP_WEAKSET)
    static const PxFnDef map_fns[] = {
        {"get", mapp_lookup, 1, M | 0},     {"has", mapp_lookup, 1, M | 1},   {"delete", mapp_lookup, 1, M | 2},
        {"set", mapp_set, 2, M},            {"clear", mapp_clear, 0, M},      {"forEach", mapp_for_each, 1, M},
        {"entries", mapp_iter, 0, M | PX_IT_MAP_ENTRIES}, {"keys", mapp_iter, 0, M | PX_IT_MAP_KEYS},
        {"values", mapp_iter, 0, M | PX_IT_MAP_VALUES},
    };
    static const PxFnDef set_fns[] = {
        {"has", mapp_lookup, 1, S | 1},     {"delete", mapp_lookup, 1, S | 2}, {"add", mapp_set, 1, S},
        {"clear", mapp_clear, 0, S},        {"forEach", mapp_for_each, 1, S},
        {"entries", mapp_iter, 0, S | PX_IT_MAP_ENTRIES}, {"values", mapp_iter, 0, S | PX_IT_MAP_VALUES},
        {"union", setp_op, 1, SO_UNION},               {"intersection", setp_op, 1, SO_INTERSECTION},
        {"difference", setp_op, 1, SO_DIFFERENCE},     {"symmetricDifference", setp_op, 1, SO_SYMDIFF},
        {"isSubsetOf", setp_op, 1, SO_SUBSET},         {"isSupersetOf", setp_op, 1, SO_SUPERSET},
        {"isDisjointFrom", setp_op, 1, SO_DISJOINT},
    };
    static const PxFnDef weakmap_fns[] = {{"get", mapp_lookup, 1, WM | 0}, {"has", mapp_lookup, 1, WM | 1},
                                          {"delete", mapp_lookup, 1, WM | 2}, {"set", mapp_set, 2, WM}};
    static const PxFnDef weakset_fns[] = {
        {"has", mapp_lookup, 1, WS | 1}, {"delete", mapp_lookup, 1, WS | 2}, {"add", mapp_set, 1, WS}};
#undef M
#undef S
#undef WM
#undef WS
    static const PxFnDef map_statics[] = {{"groupBy", map_group_by, 2, 0}};
    int kind;
    for (kind = 0; kind < 4; kind++) {
        PxValue ctor = px_make_native(vm, map_ctor, names[kind], 0, kind), proto = vm->protos[PX_PROTO_MAP + kind];
        const PxFnDef *fns = kind == 0 ? map_fns : kind == 1 ? set_fns : kind == 2 ? weakmap_fns : weakset_fns;
        int            n   = kind == 0 ? PX_COUNTOF(map_fns) : kind == 1 ? PX_COUNTOF(set_fns)
                             : kind == 2 ? PX_COUNTOF(weakmap_fns) : PX_COUNTOF(weakset_fns);
        if (ctor == PX_EXCEPTION) return -1;
        vm->ctors[PX_PROTO_MAP + kind] = ctor;
        if (px_def_value(vm, vm->global, names[kind], ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_value(vm, ctor, "prototype", proto, 0) < 0 ||
            px_define(vm, proto, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, proto, fns, n) < 0 || px_def_tag(vm, proto, names[kind]) < 0)
            return -1;
        if (kind < 2) {
            PxValue size = px_make_native(vm, mapp_size, "get size", 0, KIND(kind)), k, it;
            if (size == PX_EXCEPTION) return -1;
            ((PxObject *)px_ptr(size))->flags |= PX_OBJ_NOT_CTOR;
            k = px_intern_cstr(vm, "size");
            if (k == PX_EXCEPTION || px_define_accessor(vm, proto, k, size, PX_UNDEFINED, PX_ATTR_CONFIGURABLE) < 0)
                return -1;
            /* Map: [Symbol.iterator] is entries; Set: it and keys are values */
            it = px_get(vm, proto, px_intern_cstr(vm, kind == 0 ? "entries" : "values"));
            if (it == PX_EXCEPTION || px_define(vm, proto, vm->sym_iterator, it, PX_ATTR_HIDDEN) < 0 ||
                (kind == 1 && px_def_value(vm, proto, "keys", it, PX_ATTR_HIDDEN) < 0) || px_def_species(vm, ctor) < 0)
                return -1;
        }
        if (kind == 0 && px_def_fns(vm, ctor, map_statics, PX_COUNTOF(map_statics)) < 0) return -1;
    }
    {
        static const PxFnDef weakref_fns[] = {{"deref", weakref_deref, 0, 0}};
        PxValue ctor = px_make_native(vm, weakref_ctor, "WeakRef", 1, 0), proto = vm->protos[PX_PROTO_WEAKREF];
        if (ctor == PX_EXCEPTION) return -1;
        vm->ctors[PX_PROTO_WEAKREF] = ctor;
        if (px_def_value(vm, vm->global, "WeakRef", ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_value(vm, ctor, "prototype", proto, 0) < 0 ||
            px_define(vm, proto, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, proto, weakref_fns, PX_COUNTOF(weakref_fns)) < 0 || px_def_tag(vm, proto, "WeakRef") < 0)
            return -1;
    }
    {
        static const PxFnDef finreg_fns[] = {{"register", finreg_register, 2, 0}, {"unregister", finreg_unregister, 1, 0}};
        PxValue ctor = px_make_native(vm, finreg_ctor, "FinalizationRegistry", 1, 0), proto = vm->protos[PX_PROTO_FINREG];
        if (ctor == PX_EXCEPTION) return -1;
        vm->ctors[PX_PROTO_FINREG] = ctor;
        if (px_def_value(vm, vm->global, "FinalizationRegistry", ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_value(vm, ctor, "prototype", proto, 0) < 0 ||
            px_define(vm, proto, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
            px_def_fns(vm, proto, finreg_fns, PX_COUNTOF(finreg_fns)) < 0 ||
            px_def_tag(vm, proto, "FinalizationRegistry") < 0)
            return -1;
    }
    return 0;
}
