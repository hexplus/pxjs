/* Map, Set, WeakMap and WeakSet.
 *
 * One insertion-ordered hash table (see PxMap) serves all four. Keys
 * compare with SameValueZero: numbers by value (NaN equals NaN, -0 equals
 * 0), strings by content, everything else by identity.
 *
 * WeakMap and WeakSet hold their keys strongly: entries are not dropped
 * when a key becomes otherwise unreachable. Apps that use them as caches
 * keyed by short-lived objects should delete entries themselves. This is
 * documented in docs/engine.md. */

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

static PxValue map_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     kind = vm->native_magic;
    PxMap  *m;
    PxValue mv, iter, item;
    int     r;
    (void)t;
    m = (PxMap *)px_obj_new(vm, PX_T_MAP, sizeof(PxMap), vm->protos[PX_PROTO_MAP + kind]);
    if (!m) return PX_EXCEPTION;
    m->kind = (uint32_t)kind;
    mv      = px_from_ptr(m);
    if (ARG(0) == PX_UNDEFINED || ARG(0) == PX_NULL) return mv;
    PX_ROOT(vm, mv);
    iter = px_get_iterator(vm, argv[0]);
    if (iter == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, iter);
    while ((r = px_iterator_step(vm, iter, &item)) > 0) {
        PX_ROOT(vm, item);
        if (kind == PX_MAP_SET || kind == PX_MAP_WEAKSET) {
            if ((kind == PX_MAP_WEAKSET && !px_is_obj(item)) || map_set(vm, mv, item, item) < 0) {
                if (kind == PX_MAP_WEAKSET && !px_is_obj(item)) px_throw_error(vm, PX_TYPE_ERROR, "invalid value used in weak set");
                px_pop_roots(vm, 1);
                goto fail2;
            }
        } else {
            PxValue k, v;
            if (!px_is_obj(item)) {
                px_throw_error(vm, PX_TYPE_ERROR, "iterator value is not an entry object");
                px_pop_roots(vm, 1);
                goto fail2;
            }
            k = px_get_index(vm, item, 0);
            if (k == PX_EXCEPTION) {
                px_pop_roots(vm, 1);
                goto fail2;
            }
            PX_ROOT(vm, k);
            v = px_get_index(vm, item, 1);
            if (v == PX_EXCEPTION || (kind == PX_MAP_WEAKMAP && !px_is_obj(k)) || map_set(vm, mv, k, v) < 0) {
                if (v != PX_EXCEPTION && kind == PX_MAP_WEAKMAP && !px_is_obj(k))
                    px_throw_error(vm, PX_TYPE_ERROR, "invalid value used as weak map key");
                px_pop_roots(vm, 2);
                goto fail2;
            }
            px_pop_roots(vm, 1);
        }
        px_pop_roots(vm, 1);
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

/* get (0) / has (1) / delete (2): magic; the kind comes from the receiver. */
static PxValue mapp_lookup(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap  *m;
    int32_t e;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
    m = (PxMap *)px_ptr(t);
    e = map_find(vm, m, ARG(0));
    switch (vm->native_magic) {
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
    PxMap *m;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
    m = (PxMap *)px_ptr(t);
    if ((m->kind == PX_MAP_WEAKMAP || m->kind == PX_MAP_WEAKSET) && !px_is_obj(ARG(0)))
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
    PxMap *m;
    (void)argc;
    (void)argv;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
    m          = (PxMap *)px_ptr(t);
    m->entries = NULL;
    m->index   = NULL;
    m->cap = m->used = m->count = 0;
    return PX_UNDEFINED;
}

static PxValue mapp_size(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
    return px_number(vm, ((PxMap *)px_ptr(t))->count);
}

static PxValue mapp_for_each(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMap   *m;
    PxValue  fn = ARG(0);
    uint32_t i;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
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
    if (!px_is_obj(t) || px_type_of(t) != PX_T_MAP) return px_throw_error(vm, PX_TYPE_ERROR, "incompatible receiver");
    return px_make_iterobj(vm, t, vm->native_magic);
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
    if (!px_is_obj(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "WeakRef: the target must be an object");
    r = px_map_create(vm, PX_MAP_WEAKREF);
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
    if (!px_is_callable(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "FinalizationRegistry: the cleanup callback must be a function");
    r = px_map_create(vm, PX_MAP_FINREG);
    if (r != PX_EXCEPTION) ((PxMap *)px_ptr(r))->extra = argv[0];
    return r;
}

static PxValue finreg_register(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue target = ARG(0), held = ARG(1), token = ARG(2), list;
    if (!this_finreg(vm, t)) return PX_EXCEPTION;
    if (!px_is_obj(target)) return px_throw_error(vm, PX_TYPE_ERROR, "register: the target must be an object");
    if (held == target) return px_throw_error(vm, PX_TYPE_ERROR, "register: the held value cannot be the target");
    if (token != PX_UNDEFINED && !px_is_obj(token)) return px_throw_error(vm, PX_TYPE_ERROR, "register: the token must be an object");
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
    if (!px_is_obj(token)) return px_throw_error(vm, PX_TYPE_ERROR, "unregister: the token must be an object");
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
    static const PxFnDef map_fns[] = {
        {"get", mapp_lookup, 1, 0},     {"has", mapp_lookup, 1, 1},       {"delete", mapp_lookup, 1, 2},
        {"set", mapp_set, 2, 0},        {"clear", mapp_clear, 0, 0},      {"forEach", mapp_for_each, 1, 0},
        {"entries", mapp_iter, 0, PX_IT_MAP_ENTRIES}, {"keys", mapp_iter, 0, PX_IT_MAP_KEYS},
        {"values", mapp_iter, 0, PX_IT_MAP_VALUES},
    };
    static const PxFnDef set_fns[] = {
        {"has", mapp_lookup, 1, 1},     {"delete", mapp_lookup, 1, 2},   {"add", mapp_set, 1, 0},
        {"clear", mapp_clear, 0, 0},    {"forEach", mapp_for_each, 1, 0},
        {"entries", mapp_iter, 0, PX_IT_MAP_ENTRIES}, {"values", mapp_iter, 0, PX_IT_MAP_VALUES},
        {"keys", mapp_iter, 0, PX_IT_MAP_VALUES},
        {"union", setp_op, 1, SO_UNION},               {"intersection", setp_op, 1, SO_INTERSECTION},
        {"difference", setp_op, 1, SO_DIFFERENCE},     {"symmetricDifference", setp_op, 1, SO_SYMDIFF},
        {"isSubsetOf", setp_op, 1, SO_SUBSET},         {"isSupersetOf", setp_op, 1, SO_SUPERSET},
        {"isDisjointFrom", setp_op, 1, SO_DISJOINT},
    };
    static const PxFnDef weakmap_fns[] = {{"get", mapp_lookup, 1, 0}, {"has", mapp_lookup, 1, 1},
                                          {"delete", mapp_lookup, 1, 2}, {"set", mapp_set, 2, 0}};
    static const PxFnDef weakset_fns[] = {
        {"has", mapp_lookup, 1, 1}, {"delete", mapp_lookup, 1, 2}, {"add", mapp_set, 1, 0}};
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
            px_def_fns(vm, proto, fns, n) < 0)
            return -1;
        if (kind < 2) {
            PxValue size = px_make_native(vm, mapp_size, "size", 0, 0), k, it;
            if (size == PX_EXCEPTION) return -1;
            k = px_intern_cstr(vm, "size");
            if (k == PX_EXCEPTION || px_define_accessor(vm, proto, k, size, PX_UNDEFINED, PX_ATTR_CONFIGURABLE) < 0)
                return -1;
            it = px_get(vm, proto, px_intern_cstr(vm, kind == 0 ? "entries" : "values"));
            if (it == PX_EXCEPTION || px_define(vm, proto, vm->sym_iterator, it, PX_ATTR_HIDDEN) < 0) return -1;
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
            px_def_fns(vm, proto, weakref_fns, PX_COUNTOF(weakref_fns)) < 0)
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
            px_def_fns(vm, proto, finreg_fns, PX_COUNTOF(finreg_fns)) < 0)
            return -1;
    }
    return 0;
}
