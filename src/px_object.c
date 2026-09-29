/* Objects, hidden classes (shapes), dictionaries and arrays.
 *
 * An object is 32 bytes: header, shape, overflow slots, flags, and four
 * inline property slots -- most objects an app makes ({x, y}, {id, name,
 * url}) never need a second cell. Objects built the same way share one
 * shape, so the per-object cost of a property is one 4-byte slot.
 *
 * Objects that change shape unpredictably (deletes, more than
 * SHAPE_MAX_PROPS keys, keys from data) switch to dictionary mode: their
 * own ordered hash table, and no shape-tree growth. */

#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define SHAPE_MAX_PROPS 64

/* ------------------------------------------------------------ root shapes */

static uint32_t ptr_hash(const void *p) { return ((uint32_t)(uintptr_t)p >> 3) * 2654435761u; }

PxShape *px_root_shape(PxVM *vm, PxObject *proto) {
    uint32_t j;
    PxShape *s;

    if (vm->root_cap && vm->root_count * 2 < vm->root_cap) {
        for (j = ptr_hash(proto) & (vm->root_cap - 1); vm->root_shapes[j]; j = (j + 1) & (vm->root_cap - 1))
            if (vm->root_shapes[j]->proto == proto) return vm->root_shapes[j];
    } else {
        /* Grow (or create) the table. */
        uint32_t  ncap = vm->root_cap ? vm->root_cap * 2 : 64, i;
        PxShape **n    = (PxShape **)calloc(ncap, sizeof(PxShape *));
        if (!n) {
            px_throw_oom(vm);
            return NULL;
        }
        for (i = 0; i < vm->root_cap; i++) {
            PxShape *e = vm->root_shapes[i];
            if (!e) continue;
            for (j = ptr_hash(e->proto) & (ncap - 1); n[j]; j = (j + 1) & (ncap - 1)) {}
            n[j] = e;
        }
        free(vm->root_shapes);
        vm->root_shapes = n;
        vm->root_cap    = ncap;
        return px_root_shape(vm, proto);
    }
    {
        PxValue pv = px_from_ptr(proto);
        if (proto) PX_ROOT(vm, pv);
        s = (PxShape *)px_alloc(vm, PX_T_SHAPE, sizeof(PxShape));
        if (proto) px_pop_roots(vm, 1);
    }
    if (!s) return NULL;
    s->proto = proto;
    if (proto) proto->flags |= PX_OBJ_IS_PROTOTYPE;
    /* The allocation may have collected and rebuilt the table: search the
     * slot again rather than reuse j. */
    for (j = ptr_hash(proto) & (vm->root_cap - 1); vm->root_shapes[j]; j = (j + 1) & (vm->root_cap - 1)) {}
    vm->root_shapes[j] = s;
    vm->root_count++;
    return s;
}

void px_root_shapes_sweep(PxVM *vm) {
    PxShape **old = vm->root_shapes;
    uint32_t  cap = vm->root_cap, i, j;
    if (!old) return;
    vm->root_shapes = (PxShape **)calloc(cap, sizeof(PxShape *));
    if (!vm->root_shapes) {
        vm->root_shapes = old;
        for (i = 0; i < cap; i++)
            if (old[i]) old[i]->hdr |= PX_HDR_MARK; /* keep them all this time */
        return;
    }
    vm->root_count = 0;
    for (i = 0; i < cap; i++) {
        PxShape *s = old[i];
        if (!s || !(s->hdr & PX_HDR_MARK)) continue;
        for (j = ptr_hash(s->proto) & (cap - 1); vm->root_shapes[j]; j = (j + 1) & (cap - 1)) {}
        vm->root_shapes[j] = s;
        vm->root_count++;
    }
    free(old);
}

void px_shape_table_free(PxShape *s) {
    free(s->table);
    s->table = NULL;
}

void px_shapes_unlink_dead(PxVM *vm, PxShape *dead) {
    PxShape *p = dead->parent, **link;
    (void)vm;
    px_shape_table_free(dead);
    if (!p || !(p->hdr & PX_HDR_MARK)) return;
    for (link = &p->first_child; *link; link = &(*link)->next_sibling) {
        if (*link == dead) {
            *link = dead->next_sibling;
            return;
        }
    }
}

/* ------------------------------------------------------------ shapes */

#define SHAPE_TABLE_MIN_KEYS 10 /* below this, walking the chain is as fast */
#define SHAPE_TABLE_AFTER    16 /* lookups before a shape earns a table */

static uint32_t shape_hash(PxValue key) { return (uint32_t)(key >> 3) * 2654435761u; }

/* Built with malloc, not the JS heap: a lookup must never allocate from the
 * heap (its callers hold raw pointers), and malloc cannot trigger a
 * collection. If malloc fails, the chain walk still works. */
static void shape_table_build(PxShape *s) {
    uint32_t       cap = 16, i;
    PxShapeTable  *t;
    const PxShape *n;
    while (cap < 2u * s->count) cap *= 2;
    t = (PxShapeTable *)calloc(1, sizeof(PxShapeTable) + (cap - 1) * sizeof(t->e[0]));
    if (!t) return;
    t->mask = cap - 1;
    for (n = s; n && n->count; n = n->parent) {
        for (i = shape_hash(n->key) >> 16 & t->mask; t->e[i].key; i = (i + 1) & t->mask) {}
        t->e[i].key   = n->key;
        t->e[i].index = (uint16_t)(n->count - 1u);
        t->e[i].attrs = n->attrs;
    }
    s->table = t;
}

static int shape_find(const PxShape *cs, PxValue key, uint32_t *index, uint32_t *attrs) {
    const PxShape *s = cs;
    if (s && s->count >= SHAPE_TABLE_MIN_KEYS) {
        PxShape *m = (PxShape *)cs;
        if (!m->table && ++m->lookups >= SHAPE_TABLE_AFTER) shape_table_build(m);
        if (m->table) {
            PxShapeTable *t = m->table;
            uint32_t      i;
            for (i = shape_hash(key) >> 16 & t->mask; t->e[i].key; i = (i + 1) & t->mask)
                if (t->e[i].key == key) {
                    *index = t->e[i].index;
                    if (attrs) *attrs = t->e[i].attrs;
                    return 1;
                }
            return 0;
        }
    }
    for (; s && s->count; s = s->parent) {
        if (s->key == key) {
            *index = s->count - 1u;
            if (attrs) *attrs = s->attrs;
            return 1;
        }
    }
    return 0;
}

static PxShape *shape_add(PxVM *vm, PxShape *s, PxValue key, uint32_t attrs) {
    PxShape *c;
    PxValue  sv = px_from_ptr(s);
    for (c = s->first_child; c; c = c->next_sibling)
        if (c->key == key && c->attrs == attrs) return c;
    PX_ROOT(vm, sv);
    PX_ROOT(vm, key);
    c = (PxShape *)px_alloc(vm, PX_T_SHAPE, sizeof(PxShape));
    px_pop_roots(vm, 2);
    if (!c) return NULL;
    c->parent       = s;
    c->proto        = s->proto;
    c->key          = key;
    c->count        = (uint16_t)(s->count + 1);
    c->attrs        = (uint8_t)attrs;
    c->next_sibling = s->first_child;
    s->first_child  = c;
    return c;
}

/* ------------------------------------------------------------ dicts */

static int32_t *dict_index(PxDict *d) { return (int32_t *)(void *)&d->entries[d->cap]; }

static uint32_t key_hash(PxValue k) { return (uint32_t)k * 2654435761u; }

static PxDict *dict_new(PxVM *vm, uint32_t cap) {
    PxDict  *d;
    uint32_t i;
    size_t   bytes = sizeof(PxDict) + (size_t)cap * sizeof(PxDictEntry) + (size_t)cap * 2u * sizeof(int32_t);
    d              = (PxDict *)px_alloc(vm, PX_T_DICT, bytes);
    if (!d) return NULL;
    d->cap = cap;
    for (i = 0; i < cap * 2; i++) dict_index(d)[i] = -1;
    return d;
}

static int32_t dict_find(PxDict *d, PxValue key) {
    uint32_t mask = d->cap * 2 - 1, j;
    for (j = key_hash(key) & mask;; j = (j + 1) & mask) {
        int32_t e = dict_index(d)[j];
        if (e < 0) return -1;
        if (d->entries[e].key == key) return e;
    }
}

static void dict_insert_index(PxDict *d, uint32_t e) {
    uint32_t mask = d->cap * 2 - 1, j;
    for (j = key_hash(d->entries[e].key) & mask; dict_index(d)[j] >= 0; j = (j + 1) & mask) {}
    dict_index(d)[j] = (int32_t)e;
}

/* Appends; the object `o` owns the dict and gets a bigger one if needed. */
static int dict_put(PxVM *vm, PxObject *o, PxValue key, PxValue value, uint32_t attrs) {
    PxDict *d = (PxDict *)o->slots;
    int32_t e = dict_find(d, key);
    if (e >= 0) {
        d->entries[e].value = value;
        d->entries[e].attrs = attrs;
        return 0;
    }
    if (d->used == d->cap) {
        PxDict  *n;
        uint32_t i, ncap = d->count * 2 < 8 ? 8 : d->count * 2;
        PxValue  ov = px_from_ptr(o);
        /* power of two keeps the index mask valid */
        while (ncap & (ncap - 1)) ncap++;
        PX_ROOT(vm, ov);
        PX_ROOT(vm, key);
        PX_ROOT(vm, value);
        n = dict_new(vm, ncap);
        px_pop_roots(vm, 3);
        if (!n) return -1;
        d = (PxDict *)o->slots;
        for (i = 0; i < d->used; i++) {
            if (!d->entries[i].key) continue;
            n->entries[n->used] = d->entries[i];
            dict_insert_index(n, n->used);
            n->used++;
            n->count++;
        }
        o->slots = (PxVec *)n;
        d        = n;
    }
    d->entries[d->used].key   = key;
    d->entries[d->used].value = value;
    d->entries[d->used].attrs = attrs;
    dict_insert_index(d, d->used);
    d->used++;
    d->count++;
    return 0;
}

static PxValue *slot_ptr(PxObject *o, uint32_t i) {
    return i < PX_INLINE_SLOTS ? &o->inline_slots[i] : &o->slots->items[i - PX_INLINE_SLOTS];
}

/* Converts a shaped object to dictionary mode. */
static int to_dict(PxVM *vm, PxObject *o) {
    PxShape *s = o->shape, *ds, *chain[SHAPE_MAX_PROPS + 1];
    PxDict  *d;
    PxValue  ov = px_from_ptr(o);
    uint32_t n = 0, i, cap = 8;

    if (s->flags & PX_SHAPE_DICT) return 0;
    for (; s && s->count; s = s->parent) chain[n++] = s;
    while (cap < n * 2) cap *= 2;
    PX_ROOT(vm, ov);
    d = dict_new(vm, cap);
    if (d) {
        PxValue dv = px_from_ptr(d);
        PX_ROOT(vm, dv);
        ds = (PxShape *)px_alloc(vm, PX_T_SHAPE, sizeof(PxShape));
        px_pop_roots(vm, 1);
    } else {
        ds = NULL;
    }
    px_pop_roots(vm, 1);
    if (!d || !ds) return -1;
    ds->flags = PX_SHAPE_DICT;
    ds->proto = o->shape->proto;
    /* chain[] is leaf-first; insertion order is root-first. */
    for (i = n; i-- > 0;) {
        PxShape *c = chain[i];
        d->entries[d->used].key   = c->key;
        d->entries[d->used].value = *slot_ptr(o, c->count - 1u);
        d->entries[d->used].attrs = c->attrs;
        dict_insert_index(d, d->used);
        d->used++;
        d->count++;
    }
    for (i = 0; i < PX_INLINE_SLOTS; i++) o->inline_slots[i] = PX_UNDEFINED;
    o->shape = ds;
    o->slots = (PxVec *)d;
    return 0;
}

/* ------------------------------------------------------------ objects */

PxObject *px_obj_new(PxVM *vm, PxType type, size_t bytes, PxValue proto) {
    PxObject *o;
    PxShape  *s;
    PxValue   sv;
    uint32_t  i;

    PX_ROOT(vm, proto);
    s = px_root_shape(vm, px_is_ptr(proto) ? (PxObject *)px_ptr(proto) : NULL);
    px_pop_roots(vm, 1);
    if (!s) return NULL;
    sv = px_from_ptr(s);
    PX_ROOT(vm, sv);
    o = (PxObject *)px_alloc(vm, type, bytes);
    px_pop_roots(vm, 1);
    if (!o) return NULL;
    o->shape = s;
    for (i = 0; i < PX_INLINE_SLOTS; i++) o->inline_slots[i] = PX_UNDEFINED;
    return o;
}

PxValue px_object_new(PxVM *vm) {
    PxObject *o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), vm->protos[PX_PROTO_OBJECT]);
    return o ? px_from_ptr(o) : PX_EXCEPTION;
}

PxValue px_array_new(PxVM *vm, uint32_t cap) {
    PxArray *a = (PxArray *)px_obj_new(vm, PX_T_ARRAY, sizeof(PxArray), vm->protos[PX_PROTO_ARRAY]);
    PxValue  av;
    if (!a) return PX_EXCEPTION;
    av = px_from_ptr(a);
    if (cap) {
        PxVec *v;
        PX_ROOT(vm, av);
        v = px_vec_new(vm, cap);
        px_pop_roots(vm, 1);
        if (!v) return PX_EXCEPTION;
        a->elems = v;
    }
    return av;
}

PxValue px_error_new(PxVM *vm, PxErrorType type, PxValue message) {
    PxObject *o;
    PxValue   ov;
    PX_ROOT(vm, message);
    o = px_obj_new(vm, PX_T_ERROR, sizeof(PxObject), vm->protos[PX_PROTO_ERROR + type]);
    if (!o) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    ov = px_from_ptr(o);
    PX_ROOT(vm, ov);
    if (message != PX_UNDEFINED && px_define(vm, ov, vm->atom[PX_ATOM_message], message, PX_ATTR_HIDDEN) < 0) {
        px_pop_roots(vm, 2);
        return PX_EXCEPTION;
    }
    px_capture_stack(vm, ov);
    px_pop_roots(vm, 2);
    return ov;
}

PxValue *px_own_slot(PxVM *vm, PxObject *o, PxValue key, uint32_t *attrs) {
    uint32_t idx;
    (void)vm;
    if (px_hdr_type(o->hdr) == PX_T_PROXY) return NULL; /* no slots of its own: see px_proxy.c */
    if (o->shape->flags & PX_SHAPE_DICT) {
        PxDict *d = (PxDict *)o->slots;
        int32_t e = dict_find(d, key);
        if (e < 0) return NULL;
        if (attrs) *attrs = d->entries[e].attrs;
        return &d->entries[e].value;
    }
    if (!shape_find(o->shape, key, &idx, attrs)) return NULL;
    return slot_ptr(o, idx);
}

/* Adds a property the object does not have yet. */
static int add_prop(PxVM *vm, PxObject *o, PxValue key, PxValue v, uint32_t attrs) {
    PxValue  ov = px_from_ptr(o);
    PxShape *ns;
    uint32_t idx;

    if (o->flags & PX_OBJ_NOT_EXTENSIBLE) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot add a property to a non-extensible object");
        return -1;
    }
    PX_ROOT(vm, ov);
    PX_ROOT(vm, key);
    PX_ROOT(vm, v);
    if (!(o->shape->flags & PX_SHAPE_DICT) && o->shape->count >= SHAPE_MAX_PROPS && to_dict(vm, o) != 0) goto fail;
    if (o->shape->flags & PX_SHAPE_DICT) {
        int r = dict_put(vm, o, key, v, attrs);
        px_pop_roots(vm, 3);
        return r;
    }
    idx = o->shape->count;
    if (idx >= PX_INLINE_SLOTS) {
        PxVec *grown = px_vec_grow(vm, o->slots, idx - PX_INLINE_SLOTS + 1);
        if (!grown) goto fail;
        o->slots = grown;
    }
    ns = shape_add(vm, o->shape, key, attrs);
    if (!ns) goto fail;
    o->shape         = ns;
    *slot_ptr(o, idx) = v;
    px_pop_roots(vm, 3);
    return 0;
fail:
    px_pop_roots(vm, 3);
    return -1;
}

/* ------------------------------------------------------------ keys */

PxValue px_key_to_value(PxVM *vm, PxValue key) {
    if (px_is_smi(key)) return px_number_to_string(vm, px_smi(key), 10);
    return key;
}

/* ------------------------------------------------------------ functions */

static PxValue function_name(PxVM *vm, PxObject *o) {
    switch (px_hdr_type(o->hdr)) {
    case PX_T_CLOSURE: {
        PxValue n = ((PxClosure *)o)->proto->name;
        return px_is_ptr(n) ? n : vm->atom[PX_ATOM_empty];
    }
    case PX_T_NATIVE: return ((PxNative *)o)->name;
    default: return vm->atom[PX_ATOM_empty];
    }
}

static int function_length(PxObject *o) {
    switch (px_hdr_type(o->hdr)) {
    case PX_T_CLOSURE: return ((PxClosure *)o)->proto->length;
    case PX_T_NATIVE: return ((PxNative *)o)->length;
    default: return 0;
    }
}

/* The `prototype` object of a constructor function, made on first use:
 * most functions are never used with `new`, and an object each would be a
 * noticeable share of a small heap. */
static PxValue lazy_prototype(PxVM *vm, PxObject *fn) {
    PxValue fv = px_from_ptr(fn), p;
    int     is_gen = px_hdr_type(fn->hdr) == PX_T_CLOSURE && (((PxClosure *)fn)->proto->flags & PX_PROTO_GENERATOR);
    PX_ROOT(vm, fv);
    if (is_gen) {
        /* a generator function's instances inherit next/return/throw */
        int       is_async = (((PxClosure *)fn)->proto->flags & PX_PROTO_ASYNC) != 0;
        PxObject *o        = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject),
                                        vm->protos[is_async ? PX_PROTO_ASYNCGENOBJ : PX_PROTO_GENOBJ]);
        p           = o ? px_from_ptr(o) : PX_EXCEPTION;
    } else {
        p = px_object_new(vm);
    }
    if (p == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, p);
    {
        /* making it is no addition: allowed on a frozen function */
        uint32_t ne = fn->flags & PX_OBJ_NOT_EXTENSIBLE;
        int      r  = !is_gen ? px_define(vm, p, vm->atom[PX_ATOM_constructor], fv, PX_ATTR_HIDDEN) : 0;
        fn->flags &= ~ne;
        if (r == 0) r = add_prop(vm, fn, vm->atom[PX_ATOM_prototype], p, PX_ATTR_WRITABLE);
        fn->flags |= ne;
        if (r < 0) {
            px_pop_roots(vm, 1); /* p; `fail` pops fv */
            goto fail;
        }
    }
    fn->flags |= PX_OBJ_PROTO_MADE;
    px_pop_roots(vm, 2);
    return p;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* 1 if `key` is a function's `length` or `name` still answered from its
 * code: { writable: false, enumerable: false, configurable: true }. Most
 * functions never have these read, so they cost no slots. Redefining or
 * deleting one sets PX_OBJ_NO_LENGTH/NO_NAME for good. */
static int fn_virtual(PxVM *vm, const PxObject *o, PxValue key) {
    PxType t = px_hdr_type(o->hdr);
    if (t != PX_T_CLOSURE && t != PX_T_NATIVE) return 0;
    if (key == vm->atom[PX_ATOM_length]) return !(o->flags & PX_OBJ_NO_LENGTH);
    if (key == vm->atom[PX_ATOM_name]) return !(o->flags & PX_OBJ_NO_NAME);
    return 0;
}

static uint32_t fn_virtual_flag(PxVM *vm, PxValue key) {
    return key == vm->atom[PX_ATOM_length] ? PX_OBJ_NO_LENGTH : PX_OBJ_NO_NAME;
}

static int has_lazy_prototype(PxObject *o) {
    uint32_t pf;
    if (px_hdr_type(o->hdr) != PX_T_CLOSURE) return 0;
    if (o->flags & (PX_OBJ_PROTO_MADE | PX_OBJ_ARROW)) return 0;
    pf = ((PxClosure *)o)->proto->flags;
    if (pf & PX_PROTO_GENERATOR) return 1;
    return !(pf & (PX_PROTO_ARROW | PX_PROTO_METHOD | PX_PROTO_ASYNC));
}

/* ------------------------------------------------------------ get */

PxValue px_proto_of(PxVM *vm, PxValue v) {
    if (px_is_obj(v) && px_type_of(v) == PX_T_PROXY) return px_proxy_proto_of(vm, v);
    if (px_is_obj(v)) {
        PxObject *p = ((PxObject *)px_ptr(v))->shape->proto;
        return p ? px_from_ptr(p) : PX_NULL;
    }
    if (px_is_smi(v)) return vm->protos[PX_PROTO_NUMBER];
    if (px_is_ptr(v)) {
        switch (px_type_of(v)) {
        case PX_T_NUMBER: return vm->protos[PX_PROTO_NUMBER];
        case PX_T_STRING:
        case PX_T_ROPE: return vm->protos[PX_PROTO_STRING];
        case PX_T_SYMBOL: return vm->protos[PX_PROTO_SYMBOL];
        default: return PX_NULL;
        }
    }
    if (v == PX_TRUE || v == PX_FALSE) return vm->protos[PX_PROTO_BOOLEAN];
    return PX_NULL;
}

static PxValue string_char(PxVM *vm, PxValue s, uint32_t i) {
    PxString *f;
    uint16_t  c;
    PX_ROOT(vm, s);
    f = px_str_flat(vm, s);
    px_pop_roots(vm, 1);
    if (!f) return PX_EXCEPTION;
    c = px_str_at(f, i);
    return px_str_new_u16(vm, &c, 1);
}

static PxValue describe_key(PxVM *vm, PxValue key, char *buf, size_t cap) {
    PxValue k = px_key_to_value(vm, key);
    if (px_is_str(k)) px_str_to_utf8(vm, k, buf, cap);
    else snprintf(buf, cap, "?");
    return k;
}

static PxValue call_getter(PxVM *vm, PxValue acc, PxValue receiver) {
    PxValue g = ((PxAccessor *)px_ptr(acc))->get;
    if (!px_is_callable(g)) return PX_UNDEFINED;
    return px_call(vm, g, receiver, 0, NULL);
}

/* [[Get]] from `start` up its prototype chain, with getters called on
 * `receiver` (the object the access was made on; differs for super.x). */
PxValue px_get_recv(PxVM *vm, PxValue start, PxValue key, PxValue receiver) {
    PxValue cur;

    if (!px_is_obj(start)) {
        if (start == PX_UNDEFINED || start == PX_NULL) {
            char name[64];
            describe_key(vm, key, name, sizeof name);
            return px_throw_error(vm, PX_TYPE_ERROR, "cannot read property '%s' of %s", name,
                                  start == PX_NULL ? "null" : "undefined");
        }
        if (px_is_str(start)) {
            if (key == vm->atom[PX_ATOM_length]) return px_from_smi((int32_t)px_str_len(start));
            if (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(start))
                return string_char(vm, start, (uint32_t)px_smi(key));
        }
        cur = px_proto_of(vm, start);
    } else {
        cur = start;
    }
    while (px_is_obj(cur)) {
        PxObject *o = (PxObject *)px_ptr(cur);
        PxValue  *slot;
        uint32_t  attrs;
        switch (px_hdr_type(o->hdr)) {
        case PX_T_PROXY: return px_proxy_get(vm, cur, key, receiver);
        case PX_T_ARRAY: {
            PxArray *a = (PxArray *)o;
            if (px_is_smi(key)) {
                uint32_t i = (uint32_t)px_smi(key);
                if (a->elems && i < a->elems->cap && i < a->length && a->elems->items[i] != PX_HOLE)
                    return a->elems->items[i];
            } else if (key == vm->atom[PX_ATOM_length]) {
                return px_number(vm, (double)a->length);
            }
            break;
        }
        case PX_T_TYPEDARRAY: {
            /* a numeric key names an element or nothing: never inherited */
            int r = px_typed_numeric_key(vm, cur, key);
            if (r < 0) return PX_EXCEPTION;
            if (r) {
                PxTyped *t = (PxTyped *)o;
                return px_is_smi(key) && (uint32_t)px_smi(key) < px_typed_length(t)
                           ? px_typed_get(vm, t, (uint32_t)px_smi(key))
                           : PX_UNDEFINED;
            }
            break;
        }
        case PX_T_CLOSURE:
        case PX_T_NATIVE:
        case PX_T_BOUND:
            if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) return lazy_prototype(vm, o);
            if (fn_virtual(vm, o, key))
                return key == vm->atom[PX_ATOM_name] ? function_name(vm, o) : px_from_smi(function_length(o));
            break;
        case PX_T_BOXED: {
            PxValue pv = ((PxBoxed *)o)->value;
            if (px_is_str(pv)) {
                if (key == vm->atom[PX_ATOM_length]) return px_from_smi((int32_t)px_str_len(pv));
                if (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(pv))
                    return string_char(vm, pv, (uint32_t)px_smi(key));
            }
            break;
        }
        default: break;
        }
        slot = px_own_slot(vm, o, key, &attrs);
        if (slot) return (attrs & PX_ATTR_ACCESSOR) ? call_getter(vm, *slot, receiver) : *slot;
        cur = o->shape->proto ? px_from_ptr(o->shape->proto) : PX_NULL;
    }
    return PX_UNDEFINED;
}

PxValue px_get(PxVM *vm, PxValue obj, PxValue key) { return px_get_recv(vm, obj, key, obj); }

/* Which keys an inline cache may remember: names looked up through shapes
 * alone. length/name/prototype can be answered by the object's type
 * instead (arrays, functions), so they always take the full path. */
static int ic_key_ok(PxVM *vm, PxValue key) {
    return !px_is_smi(key) && key != vm->atom[PX_ATOM_length] && key != vm->atom[PX_ATOM_name] &&
           key != vm->atom[PX_ATOM_prototype];
}

/* Objects whose properties are all in their shape (for ic_key_ok keys). */
static int ic_type_ok(const PxObject *o) {
    PxType t = px_hdr_type(o->hdr);
    /* a String object's other own properties (indices, length) are not
     * keys the caches take (ic_key_ok) */
    return (t == PX_T_OBJECT || t == PX_T_ARRAY || t == PX_T_BOXED) && !(o->shape->flags & PX_SHAPE_DICT);
}

PxValue px_get_ic(PxVM *vm, PxValue obj, PxValue key, PxIC *ic) {
    /* a primitive: from its prototype, as that object's own property (the
     * receiver matters only to getters, which are never cached) */
    PxObject *po = ic && !px_is_obj(obj) ? px_prim_proto(vm, obj) : NULL;
    if (ic && (po || px_is_obj(obj)) && ic_key_ok(vm, key)) {
        PxObject *o = po ? po : (PxObject *)px_ptr(obj);
        uint32_t  idx, attrs;
        if (ic_type_ok(o)) {
            if (shape_find(o->shape, key, &idx, &attrs)) {
                if (!(attrs & PX_ATTR_ACCESSOR)) {
                    ic->shape        = o->shape;
                    ic->holder_shape = NULL;
                    ic->slot         = (uint16_t)idx;
                    ic->epoch        = vm->shape_epoch;
                    return *px_slot_at(o, idx);
                }
            } else {
                /* one level up: methods on a class's prototype */
                PxObject *p = o->shape->proto;
                if (p && ic_type_ok(p) && shape_find(p->shape, key, &idx, &attrs) && !(attrs & PX_ATTR_ACCESSOR)) {
                    ic->shape        = o->shape;
                    ic->holder_shape = p->shape;
                    ic->slot         = (uint16_t)idx;
                    ic->epoch        = vm->shape_epoch;
                    return *px_slot_at(p, idx);
                }
            }
        }
    }
    return px_get_recv(vm, obj, key, obj);
}

int px_set_ic(PxVM *vm, PxValue obj, PxValue key, PxValue v, PxIC *ic) {
    if (ic && px_is_obj(obj) && ic_key_ok(vm, key)) {
        PxObject *o = (PxObject *)px_ptr(obj);
        uint32_t  idx, attrs;
        /* only stores into an existing, writable, own data property */
        if (px_hdr_type(o->hdr) == PX_T_OBJECT && !(o->shape->flags & PX_SHAPE_DICT) &&
            shape_find(o->shape, key, &idx, &attrs) && (attrs & PX_ATTR_WRITABLE) && !(attrs & PX_ATTR_ACCESSOR)) {
            ic->shape        = o->shape;
            ic->holder_shape = NULL;
            ic->slot         = (uint16_t)idx;
            ic->epoch        = vm->shape_epoch;
            *px_slot_at(o, idx) = v;
            return 0;
        }
    }
    return px_set(vm, obj, key, v);
}

PxValue px_get_value(PxVM *vm, PxValue obj, PxValue key) {
    PxValue k;
    PX_ROOT(vm, obj);
    k = px_intern(vm, key);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    return px_get(vm, obj, k);
}

/* ------------------------------------------------------------ arrays */

int px_array_set_length(PxVM *vm, PxArray *a, uint32_t len) {
    uint32_t i;
    (void)vm;
    if (a->elems && len < a->length) {
        uint32_t end = a->length < a->elems->cap ? a->length : a->elems->cap;
        for (i = len; i < end; i++) a->elems->items[i] = PX_HOLE;
    }
    if (len > a->length && a->elems) {
        uint32_t end = len < a->elems->cap ? len : a->elems->cap;
        for (i = a->length; i < end; i++) a->elems->items[i] = PX_HOLE;
    }
    a->length = len;
    return 0;
}

/* Dense growth limit: an index this far past the end is stored as a plain
 * property, so `a[1e6] = 1` does not allocate four megabytes. */
#define ARRAY_DENSE_SLACK 1024u

/* 1 (and *out) if key is an array index, 0 .. 2^32-2. Indices above the
 * SMI range are interned strings of exactly ten digits. */
static int key_index(PxValue key, uint32_t *out) {
    const PxString *s;
    uint64_t        v = 0;
    uint32_t        i;
    if (px_is_smi(key)) {
        *out = (uint32_t)px_smi(key);
        return px_smi(key) >= 0;
    }
    if (!px_is_ptr(key) || px_type_of(key) != PX_T_STRING) return 0;
    s = (const PxString *)px_ptr(key);
    if (s->len != 10 || s->wide || px_str_l1(s)[0] == '0') return 0;
    for (i = 0; i < 10; i++) {
        uint8_t c = px_str_l1(s)[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    if (v > 0xFFFFFFFEull) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* The property key of array index i. */
static PxValue index_key_of(PxVM *vm, uint32_t i) {
    PxValue s;
    if (i <= (uint32_t)PX_SMI_MAX) return px_from_smi((int32_t)i);
    s = px_number_to_string(vm, (double)i, 10);
    return s == PX_EXCEPTION ? s : px_intern(vm, s);
}

static int array_set_index(PxVM *vm, PxArray *a, uint32_t i, PxValue v) {
    if (!a->elems || i >= a->elems->cap) {
        uint32_t have = a->elems ? a->elems->cap : 0;
        if (i > have + ARRAY_DENSE_SLACK && i >= a->length + ARRAY_DENSE_SLACK) {
            PxObject *o  = &a->obj;
            PxValue   av = px_from_ptr(a), k;
            PxValue  *slot;
            PX_ROOT(vm, av);
            PX_ROOT(vm, v);
            k = index_key_of(vm, i);
            px_pop_roots(vm, 2);
            if (k == PX_EXCEPTION) return -1;
            slot = px_own_slot(vm, o, k, NULL);
            if (slot) *slot = v;
            else if (add_prop(vm, o, k, v, PX_ATTR_DEFAULT) < 0) return -1;
            if (i >= a->length) a->length = i + 1;
            return 0;
        } else {
            PxValue av = px_from_ptr(a);
            PxVec  *g;
            uint32_t k, old = have;
            PX_ROOT(vm, av);
            PX_ROOT(vm, v);
            g = px_vec_grow(vm, a->elems, i + 1);
            px_pop_roots(vm, 2);
            if (!g) return -1;
            for (k = old; k < g->cap; k++) g->items[k] = PX_HOLE;
            a->elems = g;
        }
    }
    if (i >= a->length) {
        uint32_t k;
        for (k = a->length; k < i; k++) a->elems->items[k] = PX_HOLE;
        a->length = i + 1;
    }
    a->elems->items[i] = v;
    return 0;
}

int px_array_push(PxVM *vm, PxValue arr, PxValue v) {
    PxArray *a = (PxArray *)px_ptr(arr);
    if (a->length >= 0xFFFFFFFEu) {
        px_throw_error(vm, PX_RANGE_ERROR, "array too long");
        return -1;
    }
    return array_set_index(vm, a, a->length, v);
}

/* ------------------------------------------------------------ set / define */

/* Stores a property in the object's shape or dictionary (no array
 * elements, no validation). */
static int define_slot(PxVM *vm, PxObject *o, PxValue key, PxValue v, uint32_t attrs) {
    PxValue *slot;
    uint32_t old;
    if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) o->flags |= PX_OBJ_PROTO_MADE;
    slot = px_own_slot(vm, o, key, &old);
    if (slot) {
        if (old == attrs) {
            *slot = v;
            return 0;
        }
        /* Changing attributes: shapes key on them, so go dictionary. */
        {
            int r;
            PX_ROOT(vm, key);
            PX_ROOT(vm, v);
            r = to_dict(vm, o);
            px_pop_roots(vm, 2);
            if (r != 0) return -1;
        }
        return dict_put(vm, o, key, v, attrs);
    }
    if (fn_virtual(vm, o, key)) {
        /* a function's length/name redefined: it becomes an ordinary
         * property, which is not an addition (allowed when frozen) */
        uint32_t ne = o->flags & PX_OBJ_NOT_EXTENSIBLE;
        int      r;
        o->flags = (o->flags & ~ne) | fn_virtual_flag(vm, key);
        r        = add_prop(vm, o, key, v, attrs);
        o->flags |= ne;
        return r;
    }
    return add_prop(vm, o, key, v, attrs);
}

int px_define(PxVM *vm, PxValue obj, PxValue key, PxValue v, uint32_t attrs) {
    PxObject *o = (PxObject *)px_ptr(obj);
    if (px_hdr_type(o->hdr) == PX_T_PROXY) {
        PxDesc d;
        d.has   = PX_DESC_VALUE | PX_DESC_WRITABLE | PX_DESC_ENUMERABLE | PX_DESC_CONFIGURABLE;
        d.attrs = attrs;
        d.value = v;
        d.get = d.set = PX_UNDEFINED;
        return px_define_or_throw(vm, obj, key, &d);
    }
    if (px_hdr_type(o->hdr) == PX_T_ARRAY && px_is_smi(key) && attrs == PX_ATTR_DEFAULT &&
        !px_own_slot(vm, o, key, NULL))
        return array_set_index(vm, (PxArray *)o, (uint32_t)px_smi(key), v);
    return define_slot(vm, o, key, v, attrs);
}

static PxValue new_accessor(PxVM *vm, PxValue get, PxValue set) {
    PxAccessor *acc;
    PX_ROOT(vm, get);
    PX_ROOT(vm, set);
    acc = (PxAccessor *)px_alloc(vm, PX_T_ACCESSOR, sizeof(PxAccessor));
    px_pop_roots(vm, 2);
    if (!acc) return PX_EXCEPTION;
    acc->get = get;
    acc->set = set;
    return px_from_ptr(acc);
}

/* ------------------------------------------------------------ private names */

/* Private elements (#x) are own properties keyed by a class's private
 * symbols. They live on any object -- on a proxy's own cell too, never
 * forwarded to its target -- and are added even to non-extensible objects.
 * Key lists leave private symbols out, so nothing else sees them. */
PxValue *px_private_slot(PxVM *vm, PxObject *o, PxValue key, uint32_t *attrs) {
    uint32_t idx;
    (void)vm;
    if (o->shape->flags & PX_SHAPE_DICT) {
        PxDict *d = (PxDict *)o->slots;
        int32_t e = dict_find(d, key);
        if (e < 0) return NULL;
        if (attrs) *attrs = d->entries[e].attrs;
        return &d->entries[e].value;
    }
    if (!shape_find(o->shape, key, &idx, attrs)) return NULL;
    return slot_ptr(o, idx);
}

int px_private_add(PxVM *vm, PxObject *o, PxValue key, PxValue v, PxValue get, PxValue set, uint32_t attrs) {
    uint32_t ne = o->flags & PX_OBJ_NOT_EXTENSIBLE;
    int      r;
    if (get != PX_UNDEFINED || set != PX_UNDEFINED) {
        PxValue ov = px_from_ptr(o);
        PX_ROOT(vm, ov);
        PX_ROOT(vm, key);
        v = new_accessor(vm, get, set);
        px_pop_roots(vm, 2);
        if (v == PX_EXCEPTION) return -1;
        attrs |= PX_ATTR_ACCESSOR;
    }
    o->flags &= ~PX_OBJ_NOT_EXTENSIBLE;
    r = add_prop(vm, o, key, v, attrs);
    o->flags |= ne;
    return r;
}

int px_same_value(PxVM *vm, PxValue a, PxValue b) {
    if (px_is_num(a) && px_is_num(b)) {
        double x = px_num(a), y = px_num(b);
        if (x != x) return y != y;
        return x == y && (x != 0 || px_dbits(x) == px_dbits(y));
    }
    return px_strict_equals(vm, a, b);
}

/* IsCompatiblePropertyDescriptor: could `d` be applied to a property
 * described by `cur` (NULL: absent) of an object with that extensibility? */
int px_desc_compatible(PxVM *vm, int extensible, const PxDesc *d, const PxDesc *cur) {
    if (!cur) return extensible;
    if (cur->attrs & PX_ATTR_CONFIGURABLE) return 1;
    if ((d->has & PX_DESC_CONFIGURABLE) && (d->attrs & PX_ATTR_CONFIGURABLE)) return 0;
    if ((d->has & PX_DESC_ENUMERABLE) && ((d->attrs ^ cur->attrs) & PX_ATTR_ENUMERABLE)) return 0;
    if ((PX_DESC_IS_ACCESSOR(d) && !PX_DESC_IS_ACCESSOR(cur)) || (PX_DESC_IS_DATA(d) && PX_DESC_IS_ACCESSOR(cur)))
        return 0;
    if (PX_DESC_IS_ACCESSOR(cur))
        return !((d->has & PX_DESC_GET) && !px_same_value(vm, d->get, cur->get)) &&
               !((d->has & PX_DESC_SET) && !px_same_value(vm, d->set, cur->set));
    if (cur->attrs & PX_ATTR_WRITABLE) return 1;
    return !((d->has & PX_DESC_WRITABLE) && (d->attrs & PX_ATTR_WRITABLE)) &&
           !((d->has & PX_DESC_VALUE) && !px_same_value(vm, d->value, cur->value));
}

/* ------------------------------------------------------------ own properties */

/* An own property as [[GetOwnProperty]] sees it: in a slot (shape,
 * dictionary, dense element), or answered by the object's type. */
enum { OWN_SLOT, OWN_ELEM, OWN_ARRAY_LENGTH, OWN_VALUE, OWN_CHAR, OWN_TYPED };

typedef struct OwnProp {
    PxValue *slot;  /* OWN_SLOT, OWN_ELEM */
    PxValue  value; /* OWN_VALUE */
    uint32_t attrs; /* PX_ATTR_*, PX_ATTR_ACCESSOR: the slot holds a PxAccessor */
    uint32_t kind;
} OwnProp;

/* Not for proxies. 1 found, 0 absent, -1 exception (making a function's
 * lazy prototype can run out of memory). */
static int own_lookup(PxVM *vm, PxObject *o, PxValue key, OwnProp *p) {
    switch (px_hdr_type(o->hdr)) {
    case PX_T_ARRAY: {
        PxArray *a = (PxArray *)o;
        if (px_is_smi(key)) {
            uint32_t i = (uint32_t)px_smi(key);
            if (a->elems && i < a->length && i < a->elems->cap && a->elems->items[i] != PX_HOLE) {
                p->kind  = OWN_ELEM;
                p->slot  = &a->elems->items[i];
                p->attrs = PX_ATTR_DEFAULT;
                return 1;
            }
        } else if (key == vm->atom[PX_ATOM_length]) {
            p->kind  = OWN_ARRAY_LENGTH;
            p->attrs = (o->flags & PX_OBJ_LENGTH_RO) ? 0 : PX_ATTR_WRITABLE;
            return 1;
        }
        break;
    }
    case PX_T_TYPEDARRAY:
        if (px_is_smi(key) && (uint32_t)px_smi(key) < px_typed_length((PxTyped *)o)) {
            p->kind  = OWN_TYPED;
            p->attrs = PX_ATTR_DEFAULT;
            return 1;
        }
        break;
    case PX_T_BOXED: {
        PxValue pv = ((PxBoxed *)o)->value;
        if (px_is_str(pv)) {
            if (key == vm->atom[PX_ATOM_length]) {
                p->kind  = OWN_VALUE;
                p->value = px_from_smi((int32_t)px_str_len(pv));
                p->attrs = 0;
                return 1;
            }
            if (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(pv)) {
                p->kind  = OWN_CHAR;
                p->attrs = PX_ATTR_ENUMERABLE;
                return 1;
            }
        }
        break;
    }
    case PX_T_CLOSURE:
    case PX_T_NATIVE:
    case PX_T_BOUND:
        if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) {
            if (lazy_prototype(vm, o) == PX_EXCEPTION) return -1;
            break;
        }
        if (fn_virtual(vm, o, key)) {
            p->kind  = OWN_VALUE;
            p->value = key == vm->atom[PX_ATOM_name] ? function_name(vm, o) : px_from_smi(function_length(o));
            p->attrs = PX_ATTR_CONFIGURABLE;
            return 1;
        }
        break;
    default: break;
    }
    p->slot = px_own_slot(vm, o, key, &p->attrs);
    p->kind = OWN_SLOT;
    return p->slot != NULL;
}

/* The value of a data property own_lookup found (may allocate). */
static PxValue own_value(PxVM *vm, PxObject *o, PxValue key, const OwnProp *p) {
    switch (p->kind) {
    case OWN_ARRAY_LENGTH: return px_idx_value(vm, ((PxArray *)o)->length);
    case OWN_VALUE: return p->value;
    case OWN_CHAR: return string_char(vm, ((PxBoxed *)o)->value, (uint32_t)px_smi(key));
    case OWN_TYPED: return px_typed_get(vm, (PxTyped *)o, (uint32_t)px_smi(key));
    default: return *p->slot;
    }
}

int px_get_own_property(PxVM *vm, PxValue obj, PxValue key, PxDesc *d) {
    PxObject *o;
    OwnProp   p;
    int       r;
    if (!px_is_obj(obj)) return 0;
    o = (PxObject *)px_ptr(obj);
    if (px_hdr_type(o->hdr) == PX_T_PROXY) return px_proxy_get_own_property(vm, obj, key, d);
    PX_ROOT(vm, obj);
    PX_ROOT(vm, key);
    r = own_lookup(vm, o, key, &p);
    if (r <= 0) {
        px_pop_roots(vm, 2);
        return r;
    }
    d->attrs = p.attrs & (PX_ATTR_WRITABLE | PX_ATTR_ENUMERABLE | PX_ATTR_CONFIGURABLE);
    if (p.attrs & PX_ATTR_ACCESSOR) {
        PxAccessor *acc = (PxAccessor *)px_ptr(*p.slot);
        d->has   = PX_DESC_GET | PX_DESC_SET | PX_DESC_ENUMERABLE | PX_DESC_CONFIGURABLE;
        d->attrs &= ~(uint32_t)PX_ATTR_WRITABLE;
        d->get   = acc->get;
        d->set   = acc->set;
        d->value = PX_UNDEFINED;
        px_pop_roots(vm, 2);
        return 1;
    }
    d->has = PX_DESC_VALUE | PX_DESC_WRITABLE | PX_DESC_ENUMERABLE | PX_DESC_CONFIGURABLE;
    d->get = d->set = PX_UNDEFINED;
    d->value        = own_value(vm, o, key, &p);
    px_pop_roots(vm, 2);
    return d->value == PX_EXCEPTION ? -1 : 1;
}

/* The attributes a new property gets from a descriptor: absent fields are false. */
static uint32_t desc_attrs(const PxDesc *d) {
    uint32_t a = 0;
    if ((d->has & PX_DESC_WRITABLE) && (d->attrs & PX_ATTR_WRITABLE)) a |= PX_ATTR_WRITABLE;
    if ((d->has & PX_DESC_ENUMERABLE) && (d->attrs & PX_ATTR_ENUMERABLE)) a |= PX_ATTR_ENUMERABLE;
    if ((d->has & PX_DESC_CONFIGURABLE) && (d->attrs & PX_ATTR_CONFIGURABLE)) a |= PX_ATTR_CONFIGURABLE;
    return a;
}

/* Stores a property that validation allowed. */
static int store_prop(PxVM *vm, PxObject *o, PxValue key, PxValue v, PxValue get, PxValue set, uint32_t attrs,
                      int accessor) {
    if (accessor) {
        PxValue ov = px_from_ptr(o), acc;
        int     r;
        PX_ROOT(vm, ov);
        PX_ROOT(vm, key);
        acc = new_accessor(vm, get, set);
        r   = acc == PX_EXCEPTION ? -1 : define_slot(vm, o, key, acc, attrs | PX_ATTR_ACCESSOR);
        px_pop_roots(vm, 2);
        return r;
    }
    if (px_hdr_type(o->hdr) == PX_T_ARRAY && px_is_smi(key) && attrs == PX_ATTR_DEFAULT &&
        !px_own_slot(vm, o, key, NULL))
        return array_set_index(vm, (PxArray *)o, (uint32_t)px_smi(key), v);
    return define_slot(vm, o, key, v, attrs);
}

/* ValidateAndApplyPropertyDescriptor (ECMA-262 10.1.6.3) on an object's
 * own property. 1 done, 0 refused, -1 exception. */
static int ordinary_define(PxVM *vm, PxObject *o, PxValue key, const PxDesc *d) {
    OwnProp  p;
    PxValue  ov = px_from_ptr(o), curv = PX_UNDEFINED, get = PX_UNDEFINED, set = PX_UNDEFINED, v = PX_UNDEFINED;
    uint32_t cur, attrs;
    int      r, to_acc;

    PX_ROOT(vm, ov);
    PX_ROOT(vm, key);
    PX_ROOT(vm, curv);
    r = own_lookup(vm, o, key, &p);
    if (r < 0) goto fail;
    if (r == 0) {
        if (o->flags & PX_OBJ_NOT_EXTENSIBLE) goto refuse;
        if (PX_DESC_IS_ACCESSOR(d))
            r = store_prop(vm, o, key, PX_UNDEFINED, (d->has & PX_DESC_GET) ? d->get : PX_UNDEFINED,
                           (d->has & PX_DESC_SET) ? d->set : PX_UNDEFINED, desc_attrs(d) & ~(uint32_t)PX_ATTR_WRITABLE,
                           1);
        else
            r = store_prop(vm, o, key, (d->has & PX_DESC_VALUE) ? d->value : PX_UNDEFINED, PX_UNDEFINED, PX_UNDEFINED,
                           desc_attrs(d), 0);
        if (r < 0) goto fail;
        px_pop_roots(vm, 3);
        return 1;
    }
    cur = p.attrs;
    if (!(cur & PX_ATTR_ACCESSOR)) {
        curv = own_value(vm, o, key, &p);
        if (curv == PX_EXCEPTION) goto fail;
    } else {
        get = ((PxAccessor *)px_ptr(*p.slot))->get;
        set = ((PxAccessor *)px_ptr(*p.slot))->set;
    }
    if (!(cur & PX_ATTR_CONFIGURABLE)) {
        if ((d->has & PX_DESC_CONFIGURABLE) && (d->attrs & PX_ATTR_CONFIGURABLE)) goto refuse;
        if ((d->has & PX_DESC_ENUMERABLE) && ((d->attrs ^ cur) & PX_ATTR_ENUMERABLE)) goto refuse;
        if (PX_DESC_IS_ACCESSOR(d) && !(cur & PX_ATTR_ACCESSOR)) goto refuse;
        if (PX_DESC_IS_DATA(d) && (cur & PX_ATTR_ACCESSOR)) goto refuse;
        if (cur & PX_ATTR_ACCESSOR) {
            if ((d->has & PX_DESC_GET) && !px_same_value(vm, d->get, get)) goto refuse;
            if ((d->has & PX_DESC_SET) && !px_same_value(vm, d->set, set)) goto refuse;
        } else if (!(cur & PX_ATTR_WRITABLE)) {
            if ((d->has & PX_DESC_WRITABLE) && (d->attrs & PX_ATTR_WRITABLE)) goto refuse;
            if ((d->has & PX_DESC_VALUE) && !px_same_value(vm, d->value, curv)) goto refuse;
        }
    }
    /* the new state */
    attrs = cur & (PX_ATTR_ENUMERABLE | PX_ATTR_CONFIGURABLE);
    if (d->has & PX_DESC_ENUMERABLE) attrs = (attrs & ~(uint32_t)PX_ATTR_ENUMERABLE) | (d->attrs & PX_ATTR_ENUMERABLE);
    if (d->has & PX_DESC_CONFIGURABLE)
        attrs = (attrs & ~(uint32_t)PX_ATTR_CONFIGURABLE) | (d->attrs & PX_ATTR_CONFIGURABLE);
    to_acc = PX_DESC_IS_ACCESSOR(d) || (!PX_DESC_IS_DATA(d) && (cur & PX_ATTR_ACCESSOR));
    if (to_acc) {
        if (!(cur & PX_ATTR_ACCESSOR)) get = set = PX_UNDEFINED; /* data -> accessor */
        if (d->has & PX_DESC_GET) get = d->get;
        if (d->has & PX_DESC_SET) set = d->set;
    } else {
        if (cur & PX_ATTR_ACCESSOR) {
            v = PX_UNDEFINED; /* accessor -> data: writable false unless given */
        } else {
            v = curv;
            attrs |= cur & PX_ATTR_WRITABLE;
        }
        if (d->has & PX_DESC_VALUE) v = d->value;
        if (d->has & PX_DESC_WRITABLE) attrs = (attrs & ~(uint32_t)PX_ATTR_WRITABLE) | (d->attrs & PX_ATTR_WRITABLE);
    }
    switch (p.kind) {
    case OWN_ELEM:
        if (!to_acc && attrs == PX_ATTR_DEFAULT) {
            *p.slot = v;
            break;
        }
        {
            /* an element with other attributes lives as a property (which
             * is no addition: allowed when not extensible) */
            uint32_t ne = o->flags & PX_OBJ_NOT_EXTENSIBLE;
            *p.slot     = PX_HOLE;
            o->flags &= ~ne;
            r = store_prop(vm, o, key, v, get, set, attrs, to_acc);
            o->flags |= ne;
            if (r < 0) goto fail;
        }
        break;
    case OWN_SLOT:
        if (!to_acc && !(cur & PX_ATTR_ACCESSOR) && attrs == cur) {
            *p.slot = v;
            break;
        }
        if (store_prop(vm, o, key, v, get, set, attrs, to_acc) < 0) goto fail;
        break;
    case OWN_TYPED:
        if (px_typed_set(vm, ov, (uint32_t)px_smi(key), v) < 0) goto fail;
        break;
    case OWN_VALUE:
        /* a function's length/name; the other synthesised properties are
         * read-only and non-configurable, so only a no-op got here */
        if (fn_virtual(vm, o, key) && store_prop(vm, o, key, v, get, set, attrs, to_acc) < 0) goto fail;
        break;
    default: break;
    }
    px_pop_roots(vm, 3);
    return 1;
refuse:
    px_pop_roots(vm, 3);
    return 0;
fail:
    px_pop_roots(vm, 3);
    return -1;
}

/* Removes the array's index properties (not elements) at or above `from`
 * (those that are configurable, when `from` stops at the others). */
static int array_delete_props_from(PxVM *vm, PxArray *a, uint32_t from) {
    PxObject *o = &a->obj;
    PxDict   *d;
    uint32_t  i, idx;
    if (!(o->shape->flags & PX_SHAPE_DICT)) {
        const PxShape *s;
        for (s = o->shape; s && s->count; s = s->parent)
            if (key_index(s->key, &idx) && idx >= from) break;
        if (!s || !s->count) return 0; /* none: keep the shape */
        if (to_dict(vm, o) != 0) return -1;
    }
    d = (PxDict *)o->slots;
    for (i = 0; i < d->used; i++) {
        if (!d->entries[i].key || !key_index(d->entries[i].key, &idx) || idx < from) continue;
        d->entries[i].key   = 0; /* tombstone, as in px_delete */
        d->entries[i].value = PX_UNDEFINED;
        d->count--;
    }
    return 0;
}

/* The lowest length the array can shrink to on the way to `len`: one past
 * its highest non-configurable index property at or above `len`. */
static uint32_t array_shrink_limit(PxArray *a, uint32_t len) {
    PxObject *o = &a->obj;
    uint32_t  stop = len, idx;
    if (o->shape->flags & PX_SHAPE_DICT) {
        PxDict  *d = (PxDict *)o->slots;
        uint32_t i;
        for (i = 0; i < d->used; i++)
            if (d->entries[i].key && !(d->entries[i].attrs & PX_ATTR_CONFIGURABLE) &&
                key_index(d->entries[i].key, &idx) && idx >= stop)
                stop = idx + 1;
    } else {
        const PxShape *s;
        for (s = o->shape; s && s->count; s = s->parent)
            if (!(s->attrs & PX_ATTR_CONFIGURABLE) && key_index(s->key, &idx) && idx >= stop) stop = idx + 1;
    }
    return stop;
}

/* ArraySetLength (ECMA-262 10.4.2.4). */
static int array_define_length(PxVM *vm, PxArray *a, const PxDesc *d) {
    PxObject *o = &a->obj;
    uint32_t  newlen, stop;
    int       make_ro = (d->has & PX_DESC_WRITABLE) && !(d->attrs & PX_ATTR_WRITABLE);
    if (d->has & PX_DESC_VALUE) {
        PxValue  av = px_from_ptr(a);
        double   n;
        int      r;
        PX_ROOT(vm, av);
        r = px_to_uint32(vm, d->value, &newlen);
        if (r == 0) r = px_to_number(vm, d->value, &n);
        px_pop_roots(vm, 1);
        if (r < 0) return -1;
        if ((double)newlen != n) {
            px_throw_error(vm, PX_RANGE_ERROR, "invalid array length");
            return -1;
        }
    } else {
        newlen = a->length;
    }
    /* length is { writable: ?, enumerable: false, configurable: false } */
    if ((d->has & PX_DESC_CONFIGURABLE) && (d->attrs & PX_ATTR_CONFIGURABLE)) return 0;
    if ((d->has & PX_DESC_ENUMERABLE) && (d->attrs & PX_ATTR_ENUMERABLE)) return 0;
    if (PX_DESC_IS_ACCESSOR(d)) return 0;
    if (o->flags & PX_OBJ_LENGTH_RO) {
        if ((d->has & PX_DESC_WRITABLE) && (d->attrs & PX_ATTR_WRITABLE)) return 0;
        return newlen == a->length;
    }
    if (newlen >= a->length) {
        px_array_set_length(vm, a, newlen);
        if (make_ro) o->flags |= PX_OBJ_LENGTH_RO;
        return 1;
    }
    stop = array_shrink_limit(a, newlen);
    if (array_delete_props_from(vm, a, stop) < 0) return -1;
    px_array_set_length(vm, a, stop);
    if (make_ro) o->flags |= PX_OBJ_LENGTH_RO;
    return stop == newlen;
}

int px_define_own_property(PxVM *vm, PxValue obj, PxValue key, const PxDesc *d) {
    PxObject *o = (PxObject *)px_ptr(obj);
    uint32_t  i;
    switch (px_hdr_type(o->hdr)) {
    case PX_T_PROXY: return px_proxy_define_own_property(vm, obj, key, d);
    case PX_T_ARRAY: {
        PxArray *a = (PxArray *)o;
        int      r;
        if (key == vm->atom[PX_ATOM_length]) return array_define_length(vm, a, d);
        if (!key_index(key, &i)) break;
        if (i >= a->length && (o->flags & PX_OBJ_LENGTH_RO)) return 0;
        r = ordinary_define(vm, o, key, d);
        if (r == 1 && i >= a->length) a->length = i + 1;
        return r;
    }
    case PX_T_TYPEDARRAY: {
        /* integer-indexed elements: present ones only, as plain data;
         * other numeric keys cannot be defined at all */
        int r = px_typed_numeric_key(vm, obj, key);
        if (r < 0) return -1;
        if (r) {
            if (!px_is_smi(key) || (uint32_t)px_smi(key) >= px_typed_length((PxTyped *)o)) return 0;
            if (((d->has & PX_DESC_CONFIGURABLE) && !(d->attrs & PX_ATTR_CONFIGURABLE)) ||
                ((d->has & PX_DESC_ENUMERABLE) && !(d->attrs & PX_ATTR_ENUMERABLE)) || PX_DESC_IS_ACCESSOR(d) ||
                ((d->has & PX_DESC_WRITABLE) && !(d->attrs & PX_ATTR_WRITABLE)))
                return 0;
            if ((d->has & PX_DESC_VALUE) && px_typed_set(vm, obj, (uint32_t)px_smi(key), d->value) < 0) return -1;
            return 1;
        }
        break;
    }
    default: break;
    }
    return ordinary_define(vm, o, key, d);
}

static int refused(PxVM *vm, PxValue key, const char *what) {
    char name[64];
    describe_key(vm, key, name, sizeof name);
    px_throw_error(vm, PX_TYPE_ERROR, "cannot %s property '%s'", what, name);
    return -1;
}

int px_define_or_throw(PxVM *vm, PxValue obj, PxValue key, const PxDesc *d) {
    int r = px_define_own_property(vm, obj, key, d);
    if (r == 0) return refused(vm, key, "define");
    return r < 0 ? -1 : 0;
}

int px_create_data_property(PxVM *vm, PxValue obj, PxValue key, PxValue v) {
    PxDesc d;
    d.has   = PX_DESC_VALUE | PX_DESC_WRITABLE | PX_DESC_ENUMERABLE | PX_DESC_CONFIGURABLE;
    d.attrs = PX_ATTR_DEFAULT;
    d.value = v;
    d.get = d.set = PX_UNDEFINED;
    return px_define_own_property(vm, obj, key, &d);
}

/* A new own data property on an object known not to have `key`: the tail
 * of OrdinarySet. 1 / 0 / -1. */
static int add_data(PxVM *vm, PxObject *o, PxValue key, PxValue v) {
    uint32_t i;
    if (o->flags & PX_OBJ_NOT_EXTENSIBLE) return 0;
    if (px_hdr_type(o->hdr) == PX_T_ARRAY && key_index(key, &i)) {
        PxArray *a = (PxArray *)o;
        if (i >= a->length && (o->flags & PX_OBJ_LENGTH_RO)) return 0;
        if (px_is_smi(key)) return array_set_index(vm, a, i, v) < 0 ? -1 : 1;
        if (add_prop(vm, o, key, v, PX_ATTR_DEFAULT) < 0) return -1;
        if (i >= a->length) a->length = i + 1;
        return 1;
    }
    if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) o->flags |= PX_OBJ_PROTO_MADE;
    return add_prop(vm, o, key, v, PX_ATTR_DEFAULT) < 0 ? -1 : 1;
}

int px_set_recv(PxVM *vm, PxValue obj, PxValue key, PxValue v, PxValue receiver) {
    PxValue   cur = obj;
    PxObject *o   = NULL;
    OwnProp   p;
    int       found = 0, r;

    if (!px_is_obj(cur)) {
        if (cur == PX_UNDEFINED || cur == PX_NULL) {
            char name[64];
            describe_key(vm, key, name, sizeof name);
            px_throw_error(vm, PX_TYPE_ERROR, "cannot set property '%s' of %s", name, cur == PX_NULL ? "null" : "undefined");
            return -1;
        }
        /* a string's characters and length are read-only own properties */
        if (px_is_str(cur) && (key == vm->atom[PX_ATOM_length] ||
                               (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(cur))))
            return 0;
        cur = px_proto_of(vm, cur);
    }
    if (cur == receiver && key == vm->atom[PX_ATOM_prototype] && px_is_obj(cur) &&
        has_lazy_prototype((PxObject *)px_ptr(cur))) {
        /* F.prototype = {...}: no need to make the lazy one first */
        o = (PxObject *)px_ptr(cur);
        return define_slot(vm, o, key, v, PX_ATTR_WRITABLE) < 0 ? -1 : 1;
    }
    PX_ROOT(vm, cur);
    PX_ROOT(vm, key);
    PX_ROOT(vm, v);
    PX_ROOT(vm, receiver);
    while (px_is_obj(cur)) {
        o = (PxObject *)px_ptr(cur);
        if (px_hdr_type(o->hdr) == PX_T_PROXY) {
            r = px_proxy_set(vm, cur, key, v, receiver);
            px_pop_roots(vm, 4);
            return r;
        }
        if (px_hdr_type(o->hdr) == PX_T_TYPEDARRAY && (r = px_typed_numeric_key(vm, cur, key)) != 0) {
            /* integer-indexed [[Set]]: on the array itself the value is
             * converted whatever the index; through a prototype chain an
             * index out of range (or any other numeric key) does nothing */
            int valid = px_is_smi(key) && (uint32_t)px_smi(key) < px_typed_length((PxTyped *)o);
            if (r > 0 && cur == receiver)
                r = px_typed_set(vm, cur, px_is_smi(key) ? (uint32_t)px_smi(key) : UINT32_MAX, v) < 0 ? -1 : 1;
            if (r < 0 || cur == receiver || !valid) {
                px_pop_roots(vm, 4);
                return r < 0 ? -1 : 1;
            }
        }
        r = own_lookup(vm, o, key, &p);
        if (r < 0) goto fail;
        if (r) {
            found = 1;
            break;
        }
        cur = o->shape->proto ? px_from_ptr(o->shape->proto) : PX_NULL;
    }
    if (found && (p.attrs & PX_ATTR_ACCESSOR)) {
        PxValue st = ((PxAccessor *)px_ptr(*p.slot))->set;
        px_pop_roots(vm, 4);
        if (!px_is_callable(st)) return 0;
        return px_call(vm, st, receiver, 1, &v) == PX_EXCEPTION ? -1 : 1;
    }
    if (found && !(p.attrs & PX_ATTR_WRITABLE)) goto refuse;
    if (!px_is_obj(receiver)) goto refuse;
    if (found && cur == receiver) {
        /* an own writable data property */
        px_pop_roots(vm, 4);
        if (p.kind == OWN_ARRAY_LENGTH) {
            PxDesc d;
            d.has   = PX_DESC_VALUE;
            d.attrs = 0;
            d.value = v;
            d.get = d.set = PX_UNDEFINED;
            return array_define_length(vm, (PxArray *)o, &d);
        }
        *p.slot = v;
        return 1;
    }
    if (receiver == obj) {
        /* the receiver has no such own property (looked at first) */
        r = add_data(vm, (PxObject *)px_ptr(receiver), key, v);
        px_pop_roots(vm, 4);
        return r;
    }
    {
        /* a different receiver (Reflect.set, super.x = v): its own property decides */
        PxDesc d;
        PX_ROOT_DESC(vm, d);
        d.value = d.get = d.set = PX_UNDEFINED;
        r = px_get_own_property(vm, receiver, key, &d);
        if (r > 0) {
            if (PX_DESC_IS_ACCESSOR(&d) || !(d.attrs & PX_ATTR_WRITABLE)) r = 0;
            else {
                d.has   = PX_DESC_VALUE;
                d.value = v;
                r       = px_define_own_property(vm, receiver, key, &d);
            }
        } else if (r == 0) {
            r = px_create_data_property(vm, receiver, key, v);
        }
        px_pop_roots(vm, 7);
        return r;
    }
refuse:
    px_pop_roots(vm, 4);
    return 0;
fail:
    px_pop_roots(vm, 4);
    return -1;
}

int px_set(PxVM *vm, PxValue obj, PxValue key, PxValue v) {
    int r = px_set_recv(vm, obj, key, v, obj);
    if (r == 0) return refused(vm, key, px_is_obj(obj) ? "assign to" : "create");
    return r < 0 ? -1 : 0;
}

int px_define_accessor(PxVM *vm, PxValue obj, PxValue key, PxValue getter, PxValue setter, uint32_t attrs) {
    PxObject   *o = (PxObject *)px_ptr(obj);
    PxValue    *slot, av;
    PxAccessor *acc;
    uint32_t    old;
    int         r;

    slot = px_own_slot(vm, o, key, &old);
    if (slot && (old & PX_ATTR_ACCESSOR)) {
        /* get and set defined one after the other share one accessor */
        acc = (PxAccessor *)px_ptr(*slot);
        if (getter != PX_UNDEFINED) acc->get = getter;
        if (setter != PX_UNDEFINED) acc->set = setter;
        if ((old & ~(uint32_t)PX_ATTR_ACCESSOR) == attrs) return 0;
        return px_define(vm, obj, key, *slot, attrs | PX_ATTR_ACCESSOR);
    }
    PX_ROOT(vm, obj);
    PX_ROOT(vm, key);
    PX_ROOT(vm, getter);
    PX_ROOT(vm, setter);
    acc = (PxAccessor *)px_alloc(vm, PX_T_ACCESSOR, sizeof(PxAccessor));
    if (!acc) {
        px_pop_roots(vm, 4);
        return -1;
    }
    acc->get = getter;
    acc->set = setter;
    av       = px_from_ptr(acc);
    PX_ROOT(vm, av);
    r = px_define(vm, obj, key, av, attrs | PX_ATTR_ACCESSOR);
    px_pop_roots(vm, 5);
    return r;
}

/* [[SetPrototypeOf]]. The prototype lives in the shape; changing it gives
 * the object its own dictionary shape, so objects sharing the old shape
 * are unaffected. */
int px_set_proto_ok(PxVM *vm, PxValue obj, PxValue proto) {
    PxObject *o = (PxObject *)px_ptr(obj);
    PxValue   cur;
    if (px_hdr_type(o->hdr) == PX_T_PROXY) return px_proxy_set_proto(vm, obj, proto);
    if (proto != PX_NULL && !px_is_obj(proto)) {
        px_throw_error(vm, PX_TYPE_ERROR, "prototype must be an object or null");
        return -1;
    }
    if ((px_is_obj(proto) ? (PxObject *)px_ptr(proto) : NULL) == o->shape->proto) return 1;
    /* Object.prototype is an immutable prototype exotic object */
    if ((o->flags & PX_OBJ_NOT_EXTENSIBLE) || obj == vm->protos[PX_PROTO_OBJECT]) return 0;
    /* no cycles; the walk stops at a proxy, whose prototype is its own business */
    for (cur = proto; px_is_obj(cur) && px_type_of(cur) != PX_T_PROXY; cur = px_proto_of(vm, cur))
        if (cur == obj) return 0;
    if (!(o->shape->flags & PX_SHAPE_DICT)) {
        PX_ROOT(vm, obj);
        PX_ROOT(vm, proto);
        if (to_dict(vm, o) != 0) {
            px_pop_roots(vm, 2);
            return -1;
        }
        px_pop_roots(vm, 2);
    }
    o->shape->proto = px_is_obj(proto) ? (PxObject *)px_ptr(proto) : NULL;
    if (o->shape->proto) o->shape->proto->flags |= PX_OBJ_IS_PROTOTYPE;
    return 1;
}

int px_set_proto(PxVM *vm, PxValue obj, PxValue proto) {
    int r = px_set_proto_ok(vm, obj, proto);
    if (r == 0) px_throw_error(vm, PX_TYPE_ERROR, "cannot set the prototype (cyclic or non-extensible)");
    return r > 0 ? 0 : -1;
}

int px_prevent_extensions(PxVM *vm, PxValue obj) {
    if (!px_is_obj(obj)) return 1;
    if (px_type_of(obj) == PX_T_PROXY) return px_proxy_prevent_extensions(vm, obj);
    ((PxObject *)px_ptr(obj))->flags |= PX_OBJ_NOT_EXTENSIBLE;
    return 1;
}

int px_is_extensible(PxVM *vm, PxValue obj) {
    if (!px_is_obj(obj)) return 0;
    if (px_type_of(obj) == PX_T_PROXY) return px_proxy_is_extensible(vm, obj);
    return !(((PxObject *)px_ptr(obj))->flags & PX_OBJ_NOT_EXTENSIBLE);
}

int px_set_value(PxVM *vm, PxValue obj, PxValue key, PxValue v) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, v);
    k = px_intern(vm, key);
    r = k == PX_EXCEPTION ? -1 : px_set(vm, obj, k, v);
    px_pop_roots(vm, 2);
    return r;
}

int px_delete(PxVM *vm, PxValue obj, PxValue key) {
    PxObject *o;
    PxDict   *d;
    int32_t   e;
    uint32_t  attrs;

    if (!px_is_obj(obj)) return 1;
    o = (PxObject *)px_ptr(obj);
    switch (px_hdr_type(o->hdr)) {
    case PX_T_PROXY: return px_proxy_delete(vm, obj, key);
    case PX_T_ARRAY:
        if (px_is_smi(key)) {
            PxArray *a = (PxArray *)o;
            uint32_t i = (uint32_t)px_smi(key);
            if (a->elems && i < a->elems->cap && i < a->length && a->elems->items[i] != PX_HOLE) {
                a->elems->items[i] = PX_HOLE;
                return 1;
            }
        } else if (key == vm->atom[PX_ATOM_length]) {
            return 0;
        }
        break;
    case PX_T_TYPEDARRAY:
        if (px_is_smi(key) && (uint32_t)px_smi(key) < px_typed_length((PxTyped *)o)) return 0;
        break;
    case PX_T_BOXED: {
        /* a String object's characters and length are not configurable */
        PxValue pv = ((PxBoxed *)o)->value;
        if (px_is_str(pv) && (key == vm->atom[PX_ATOM_length] ||
                              (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(pv))))
            return 0;
        break;
    }
    default: break;
    }
    if (fn_virtual(vm, o, key)) {
        o->flags |= fn_virtual_flag(vm, key);
        return 1;
    }
    if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) return 0; /* not configurable */
    if (!px_own_slot(vm, o, key, &attrs)) return 1;
    if (!(attrs & PX_ATTR_CONFIGURABLE)) return 0;
    if (to_dict(vm, o) != 0) return -1;
    d = (PxDict *)o->slots;
    e = dict_find(d, key);
    if (e >= 0) {
        d->entries[e].key   = 0; /* tombstone: the index still points here, and 0 never matches */
        d->entries[e].value = PX_UNDEFINED;
        d->count--;
    }
    return 1;
}

/* IsArray: an array, or a proxy of one (TypeError once revoked); -1 on exception. */
int px_is_array(PxVM *vm, PxValue v) {
    while (px_is_obj(v) && px_type_of(v) == PX_T_PROXY) {
        v = px_proxy_target(vm, v);
        if (v == PX_EXCEPTION) return -1;
    }
    return px_is_obj(v) && px_type_of(v) == PX_T_ARRAY;
}

/* The delete operator in strict code: a refused delete is a TypeError. */
int px_delete_strict(PxVM *vm, PxValue obj, PxValue key) {
    int r;
    if (obj == PX_UNDEFINED || obj == PX_NULL) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot delete a property of %s", obj == PX_NULL ? "null" : "undefined");
        return -1;
    }
    /* a string's characters and length are its non-configurable own properties */
    if (px_is_str(obj))
        r = !(key == vm->atom[PX_ATOM_length] || (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(obj)));
    else r = px_delete(vm, obj, key);
    if (r == 0) return refused(vm, key, "delete");
    return r < 0 ? -1 : 1;
}

int px_has_own(PxVM *vm, PxValue obj, PxValue key) {
    PxObject *o;
    if (!px_is_obj(obj)) {
        if (px_is_str(obj))
            return key == vm->atom[PX_ATOM_length] ||
                   (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(obj));
        return 0;
    }
    o = (PxObject *)px_ptr(obj);
    switch (px_hdr_type(o->hdr)) {
    case PX_T_PROXY: {
        PxDesc d;
        int    r;
        PX_ROOT_DESC(vm, d);
        d.value = d.get = d.set = PX_UNDEFINED;
        r = px_proxy_get_own_property(vm, obj, key, &d);
        px_pop_roots(vm, 3);
        return r;
    }
    case PX_T_TYPEDARRAY:
        if (px_is_smi(key)) return (uint32_t)px_smi(key) < px_typed_length((PxTyped *)o);
        break;
    case PX_T_ARRAY: {
        PxArray *a = (PxArray *)o;
        if (key == vm->atom[PX_ATOM_length]) return 1;
        if (px_is_smi(key)) {
            uint32_t i = (uint32_t)px_smi(key);
            if (a->elems && i < a->elems->cap && i < a->length && a->elems->items[i] != PX_HOLE) return 1;
        }
        break;
    }
    case PX_T_BOXED: {
        PxValue pv = ((PxBoxed *)o)->value;
        if (px_is_str(pv) && (key == vm->atom[PX_ATOM_length] ||
                              (px_is_smi(key) && (uint32_t)px_smi(key) < px_str_len(pv))))
            return 1;
        break;
    }
    case PX_T_CLOSURE:
    case PX_T_NATIVE:
    case PX_T_BOUND:
        if (fn_virtual(vm, o, key)) return 1;
        if (key == vm->atom[PX_ATOM_prototype] && has_lazy_prototype(o)) return 1;
        break;
    default: break;
    }
    return px_own_slot(vm, o, key, NULL) != NULL;
}

int px_has(PxVM *vm, PxValue obj, PxValue key) {
    PxValue cur = obj;
    while (px_is_obj(cur)) {
        int r;
        if (px_type_of(cur) == PX_T_PROXY) return px_proxy_has(vm, cur, key);
        r = px_has_own(vm, cur, key);
        if (r) return r; /* 1, or -1 from a trap */
        /* a typed array's numeric key is an element or nothing */
        if (px_type_of(cur) == PX_T_TYPEDARRAY && (r = px_typed_numeric_key(vm, cur, key)) != 0) return r < 0 ? -1 : 0;
        cur = px_proto_of(vm, cur);
    }
    return 0;
}

/* ------------------------------------------------------------ own keys */

/* Appends x to the vector held (rooted) in *vv, 0 while there is none. */
static int keys_push(PxVM *vm, PxValue *vv, uint32_t *n, PxValue x) {
    PxVec *v = *vv ? (PxVec *)px_ptr(*vv) : NULL;
    if (!v || *n >= v->cap) {
        PX_ROOT(vm, x);
        v = px_vec_grow(vm, v, *n + 1);
        px_pop_roots(vm, 1);
        if (!v) return -1;
        *vv = px_from_ptr(v);
    }
    v->items[(*n)++] = x;
    return 0;
}

static uint32_t index_of_key(PxValue k) {
    uint32_t i = 0;
    key_index(k, &i);
    return i;
}

PxVec *px_own_keys(PxVM *vm, PxValue obj, int flags, uint32_t *count) {
    PxValue   outv = 0, intsv = 0, propsv = 0, str = 0;
    PxVec    *ints, *props;
    PxObject *o = NULL;
    PxType    t = PX_T_FREE;
    uint32_t  n = 0, ni = 0, np = 0, i, j, idx;
    int       strings = !(flags & PX_KEYS_NO_STRINGS), enum_only = flags & PX_KEYS_ENUMERABLE;

    *count = 0;
    if (px_is_obj(obj)) {
        o = (PxObject *)px_ptr(obj);
        t = px_hdr_type(o->hdr);
        if (t == PX_T_PROXY) return px_proxy_own_keys(vm, obj, flags, count);
        if (t == PX_T_BOXED && px_is_str(((PxBoxed *)o)->value)) str = ((PxBoxed *)o)->value;
    } else if (px_is_str(obj)) {
        str = obj;
    }
    PX_ROOT(vm, obj);
    PX_ROOT(vm, outv);
    PX_ROOT(vm, intsv);
    PX_ROOT(vm, propsv);
    /* integer keys the object's type answers for */
    if (str)
        for (i = 0; i < px_str_len(str); i++)
            if (keys_push(vm, &intsv, &ni, px_from_smi((int32_t)i)) < 0) goto fail;
    if (t == PX_T_ARRAY) {
        for (i = 0;; i++) {
            PxArray *a = (PxArray *)px_ptr(obj);
            if (!a->elems || i >= a->length || i >= a->elems->cap) break;
            if (a->elems->items[i] != PX_HOLE && keys_push(vm, &intsv, &ni, px_from_smi((int32_t)i)) < 0) goto fail;
        }
    }
    if (t == PX_T_TYPEDARRAY)
        for (i = 0; i < px_typed_length((PxTyped *)px_ptr(obj)); i++)
            if (keys_push(vm, &intsv, &ni, px_from_smi((int32_t)i)) < 0) goto fail;
    /* the properties, in creation order */
    if (o) {
        o = (PxObject *)px_ptr(obj);
        if (o->shape->flags & PX_SHAPE_DICT) {
            for (i = 0; i < ((PxDict *)((PxObject *)px_ptr(obj))->slots)->used; i++) {
                PxDictEntry *e = &((PxDict *)((PxObject *)px_ptr(obj))->slots)->entries[i];
                if (!e->key || (enum_only && !(e->attrs & PX_ATTR_ENUMERABLE))) continue;
                if (keys_push(vm, &propsv, &np, e->key) < 0) goto fail;
            }
        } else if (o->shape->count) {
            const PxShape *s;
            uint32_t       total = o->shape->count;
            props                = px_vec_new(vm, total);
            if (!props) goto fail;
            propsv = px_from_ptr(props);
            o      = (PxObject *)px_ptr(obj);
            for (s = o->shape; s && s->count; s = s->parent)
                props->items[s->count - 1] = (enum_only && !(s->attrs & PX_ATTR_ENUMERABLE)) ? 0 : s->key;
            for (i = 0, j = 0; i < total; i++)
                if (props->items[i]) props->items[j++] = props->items[i];
            np = j;
        }
    }
    /* integer-keyed properties join the others, all ascending */
    for (i = 0; i < np; i++) {
        PxValue k = ((PxVec *)px_ptr(propsv))->items[i];
        if (key_index(k, &idx) && keys_push(vm, &intsv, &ni, k) < 0) goto fail;
    }
    ints = intsv ? (PxVec *)px_ptr(intsv) : NULL;
    for (i = 1; i < ni; i++) {
        PxValue x = ints->items[i];
        idx       = index_of_key(x);
        for (j = i; j > 0 && index_of_key(ints->items[j - 1]) > idx; j--) ints->items[j] = ints->items[j - 1];
        ints->items[j] = x;
    }
    if (strings) {
        for (i = 0; i < ni; i++) {
            PxValue k = px_key_to_value(vm, ((PxVec *)px_ptr(intsv))->items[i]);
            if (k == PX_EXCEPTION || keys_push(vm, &outv, &n, k) < 0) goto fail;
        }
        if (!enum_only && (str || t == PX_T_ARRAY) && keys_push(vm, &outv, &n, vm->atom[PX_ATOM_length]) < 0) goto fail;
        if (!enum_only && o) {
            o = (PxObject *)px_ptr(obj);
            if ((fn_virtual(vm, o, vm->atom[PX_ATOM_length]) &&
                 keys_push(vm, &outv, &n, vm->atom[PX_ATOM_length]) < 0) ||
                (fn_virtual(vm, (PxObject *)px_ptr(obj), vm->atom[PX_ATOM_name]) &&
                 keys_push(vm, &outv, &n, vm->atom[PX_ATOM_name]) < 0) ||
                (has_lazy_prototype((PxObject *)px_ptr(obj)) &&
                 keys_push(vm, &outv, &n, vm->atom[PX_ATOM_prototype]) < 0))
                goto fail;
        }
        for (i = 0; i < np; i++) {
            PxValue k = ((PxVec *)px_ptr(propsv))->items[i];
            if (px_is_str(k) && !key_index(k, &idx) && keys_push(vm, &outv, &n, k) < 0) goto fail;
        }
    }
    if (flags & PX_KEYS_SYMBOLS) {
        for (i = 0; i < np; i++) {
            PxValue k = ((PxVec *)px_ptr(propsv))->items[i];
            if (px_is_ptr(k) && px_type_of(k) == PX_T_SYMBOL && !((PxSymbol *)px_ptr(k))->is_private &&
                keys_push(vm, &outv, &n, k) < 0)
                goto fail;
        }
    }
    if (!outv) {
        PxVec *e = px_vec_new(vm, 0);
        if (!e) goto fail;
        outv = px_from_ptr(e);
    }
    px_pop_roots(vm, 4);
    *count = n;
    return (PxVec *)px_ptr(outv);
fail:
    px_pop_roots(vm, 4);
    return NULL;
}

/* ------------------------------------------------------------ descriptor objects */

/* ToPropertyDescriptor. The caller roots *d (PX_ROOT_DESC) beforehand:
 * the getters of later fields can collect. */
int px_to_property_descriptor(PxVM *vm, PxValue obj, PxDesc *d) {
    static const struct {
        uint8_t atom;
        uint8_t has;
    } fields[] = {
        {PX_ATOM_enumerable, PX_DESC_ENUMERABLE}, {PX_ATOM_configurable, PX_DESC_CONFIGURABLE},
        {PX_ATOM_value, PX_DESC_VALUE},           {PX_ATOM_writable, PX_DESC_WRITABLE},
        {PX_ATOM_get, PX_DESC_GET},               {PX_ATOM_set, PX_DESC_SET},
    };
    int i;
    d->has = d->attrs = 0;
    d->value = d->get = d->set = PX_UNDEFINED;
    if (!px_is_obj(obj)) {
        px_throw_error(vm, PX_TYPE_ERROR, "property descriptor must be an object");
        return -1;
    }
    PX_ROOT(vm, obj);
    for (i = 0; i < PX_COUNTOF(fields); i++) {
        PxValue k = vm->atom[fields[i].atom], v;
        int     r = px_has(vm, obj, k);
        if (r < 0) goto fail;
        if (!r) continue;
        v = px_get(vm, obj, k);
        if (v == PX_EXCEPTION) goto fail;
        d->has |= fields[i].has;
        switch (fields[i].has) {
        case PX_DESC_ENUMERABLE: d->attrs |= px_truthy(v) ? PX_ATTR_ENUMERABLE : 0; break;
        case PX_DESC_CONFIGURABLE: d->attrs |= px_truthy(v) ? PX_ATTR_CONFIGURABLE : 0; break;
        case PX_DESC_WRITABLE: d->attrs |= px_truthy(v) ? PX_ATTR_WRITABLE : 0; break;
        case PX_DESC_VALUE: d->value = v; break;
        default:
            if (v != PX_UNDEFINED && !px_is_callable(v)) {
                px_throw_error(vm, PX_TYPE_ERROR, "a property's %s must be a function", fields[i].has == PX_DESC_GET ? "getter" : "setter");
                goto fail;
            }
            if (fields[i].has == PX_DESC_GET) d->get = v;
            else d->set = v;
        }
    }
    px_pop_roots(vm, 1);
    if (PX_DESC_IS_ACCESSOR(d) && PX_DESC_IS_DATA(d)) {
        px_throw_error(vm, PX_TYPE_ERROR, "a property cannot have both accessors and a value or writable");
        return -1;
    }
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

/* FromPropertyDescriptor: { value, writable, get, set, enumerable, configurable }, the fields present. */
PxValue px_from_property_descriptor(PxVM *vm, const PxDesc *d) {
    PxValue o = px_object_new(vm);
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    if (((d->has & PX_DESC_VALUE) && px_define(vm, o, vm->atom[PX_ATOM_value], d->value, PX_ATTR_DEFAULT) < 0) ||
        ((d->has & PX_DESC_WRITABLE) &&
         px_define(vm, o, vm->atom[PX_ATOM_writable], px_bool(d->attrs & PX_ATTR_WRITABLE), PX_ATTR_DEFAULT) < 0) ||
        ((d->has & PX_DESC_GET) && px_define(vm, o, vm->atom[PX_ATOM_get], d->get, PX_ATTR_DEFAULT) < 0) ||
        ((d->has & PX_DESC_SET) && px_define(vm, o, vm->atom[PX_ATOM_set], d->set, PX_ATTR_DEFAULT) < 0) ||
        ((d->has & PX_DESC_ENUMERABLE) &&
         px_define(vm, o, vm->atom[PX_ATOM_enumerable], px_bool(d->attrs & PX_ATTR_ENUMERABLE), PX_ATTR_DEFAULT) < 0) ||
        ((d->has & PX_DESC_CONFIGURABLE) &&
         px_define(vm, o, vm->atom[PX_ATOM_configurable], px_bool(d->attrs & PX_ATTR_CONFIGURABLE), PX_ATTR_DEFAULT) < 0))
        o = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return o;
}

/* Object.getOwnPropertyDescriptor's result: a descriptor object or undefined. */
PxValue px_descriptor_of(PxVM *vm, PxValue o, PxValue key) {
    PxDesc  d;
    PxValue r;
    int     found;
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    found = px_get_own_property(vm, o, key, &d);
    r     = found < 0 ? PX_EXCEPTION : found ? px_from_property_descriptor(vm, &d) : PX_UNDEFINED;
    px_pop_roots(vm, 3);
    return r;
}
