/* Symbols, the iteration protocol, iterator objects, and generator
 * objects' next/return/throw. */

#include <stdio.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))

/* ============================================================ Symbol */

PxValue px_symbol_new(PxVM *vm, PxValue description) {
    PxSymbol *s;
    PX_ROOT(vm, description);
    s = (PxSymbol *)px_alloc(vm, PX_T_SYMBOL, sizeof(PxSymbol));
    px_pop_roots(vm, 1);
    if (!s) return PX_EXCEPTION;
    s->description = description;
    return px_from_ptr(s);
}

static PxValue symbol_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue d = ARG(0);
    (void)t;
    if (vm->native_new_target != PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "Symbol is not a constructor");
    if (d != PX_UNDEFINED) {
        d = px_to_string(vm, d);
        if (d == PX_EXCEPTION) return d;
    }
    return px_symbol_new(vm, d);
}

/* Symbol.for / Symbol.keyFor: the registry is an object keyed by the
 * description, kept as a hidden property of the Symbol constructor. */
static PxValue symbol_registry(PxVM *vm) {
    PxValue ctor = vm->ctors[PX_PROTO_SYMBOL], reg, k;
    k            = px_intern_cstr(vm, "__registry");
    if (k == PX_EXCEPTION) return k;
    reg = px_get(vm, ctor, k);
    if (reg == PX_EXCEPTION || px_is_obj(reg)) return reg;
    {
        PxObject *o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), PX_NULL);
        if (!o) return PX_EXCEPTION;
        reg = px_from_ptr(o);
        PX_ROOT(vm, reg);
        k = px_intern_cstr(vm, "__registry");
        if (k == PX_EXCEPTION || px_define(vm, ctor, k, reg, 0) < 0) {
            px_pop_roots(vm, 1);
            return PX_EXCEPTION;
        }
        px_pop_roots(vm, 1);
        return reg;
    }
}

static PxValue symbol_for(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue key = px_to_string(vm, ARG(0)), reg, k, s;
    (void)t;
    if (key == PX_EXCEPTION) return key;
    PX_ROOT(vm, key);
    reg = symbol_registry(vm);
    if (reg == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, reg);
    k = px_intern(vm, key);
    if (k == PX_EXCEPTION) goto fail2;
    s = px_get(vm, reg, k);
    if (s == PX_EXCEPTION) goto fail2;
    if (s == PX_UNDEFINED) {
        s = px_symbol_new(vm, key);
        if (s == PX_EXCEPTION || px_define(vm, reg, k, s, PX_ATTR_DEFAULT) < 0) goto fail2;
    }
    px_pop_roots(vm, 2);
    return s;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue symbol_key_for(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = ARG(0), reg, k, found;
    (void)t;
    if (!px_is_ptr(s) || px_type_of(s) != PX_T_SYMBOL) return px_throw_error(vm, PX_TYPE_ERROR, "not a symbol");
    if (!px_is_str(((PxSymbol *)px_ptr(s))->description)) return PX_UNDEFINED;
    PX_ROOT(vm, s);
    reg = symbol_registry(vm);
    px_pop_roots(vm, 1);
    if (reg == PX_EXCEPTION) return reg;
    PX_ROOT(vm, reg);
    k = px_intern(vm, ((PxSymbol *)px_ptr(s))->description);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    found = px_get(vm, reg, k);
    if (found == PX_EXCEPTION) return found;
    return found == s ? ((PxSymbol *)px_ptr(s))->description : PX_UNDEFINED;
}

/* 1 if Symbol.for made s (such a symbol cannot be held weakly), 0 if
 * not, -1 on an exception */
int px_symbol_registered(PxVM *vm, PxValue s) {
    PxValue r;
    PX_ROOT(vm, s);
    r = symbol_key_for(vm, PX_UNDEFINED, 1, &s);
    px_pop_roots(vm, 1);
    return r == PX_EXCEPTION ? -1 : r != PX_UNDEFINED;
}

static PxValue this_symbol(PxVM *vm, PxValue t) {
    if (px_is_ptr(t) && px_type_of(t) == PX_T_SYMBOL) return t;
    if (px_is_obj(t) && px_type_of(t) == PX_T_BOXED) {
        PxValue v = ((PxBoxed *)px_ptr(t))->value;
        if (px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL) return v;
    }
    return px_throw_error(vm, PX_TYPE_ERROR, "not a symbol");
}

static PxValue symbolp_to_string(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = this_symbol(vm, t), d, open, r;
    (void)argc;
    (void)argv;
    if (s == PX_EXCEPTION) return s;
    d    = ((PxSymbol *)px_ptr(s))->description;
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

static PxValue symbolp_description(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = this_symbol(vm, t);
    (void)argc;
    (void)argv;
    if (s == PX_EXCEPTION) return s;
    return ((PxSymbol *)px_ptr(s))->description;
}

static PxValue symbolp_value_of(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)argc;
    (void)argv;
    return this_symbol(vm, t);
}

/* ============================================================ protocol */

PxValue px_iter_result(PxVM *vm, PxValue value, int done) {
    PxValue o;
    PX_ROOT(vm, value);
    o = px_object_new(vm);
    if (o == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return o;
    }
    PX_ROOT(vm, o);
    if (px_define(vm, o, vm->atom[PX_ATOM_value], value, PX_ATTR_DEFAULT) < 0 ||
        px_define(vm, o, vm->atom[PX_ATOM_done], px_bool(done), PX_ATTR_DEFAULT) < 0) {
        px_pop_roots(vm, 2);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 2);
    return o;
}

PxValue px_get_iterator(PxVM *vm, PxValue iterable) {
    PxValue m, it;
    PX_ROOT(vm, iterable);
    if (iterable == PX_UNDEFINED || iterable == PX_NULL) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_TYPE_ERROR, "%s is not iterable", iterable == PX_NULL ? "null" : "undefined");
    }
    m = px_get(vm, iterable, vm->sym_iterator);
    if (m == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return m;
    }
    if (!px_is_callable(m)) {
        px_pop_roots(vm, 1);
        return px_throw_error(vm, PX_TYPE_ERROR, "value is not iterable");
    }
    it = px_call(vm, m, iterable, 0, NULL);
    px_pop_roots(vm, 1);
    if (it != PX_EXCEPTION && !px_is_obj(it)) return px_throw_error(vm, PX_TYPE_ERROR, "iterator is not an object");
    return it;
}

int px_iterator_step(PxVM *vm, PxValue iter, PxValue *out) {
    PxValue next, r, d;
    PX_ROOT(vm, iter);
    next = px_get(vm, iter, vm->atom[PX_ATOM_next]);
    if (next == PX_EXCEPTION) goto fail;
    r = px_call(vm, next, iter, 0, NULL);
    if (r == PX_EXCEPTION) goto fail;
    if (!px_is_obj(r)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator result is not an object");
        goto fail;
    }
    PX_ROOT(vm, r);
    d = px_get(vm, r, vm->atom[PX_ATOM_done]);
    if (d == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    if (px_truthy(d)) {
        px_pop_roots(vm, 2);
        return 0;
    }
    *out = px_get(vm, r, vm->atom[PX_ATOM_value]);
    px_pop_roots(vm, 2);
    return *out == PX_EXCEPTION ? -1 : 1;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

/* IteratorClose after a normal completion: return() must be a function if
 * present, and its result an object. */
int px_iterator_close(PxVM *vm, PxValue iter) {
    PxValue m, r;
    PX_ROOT(vm, iter);
    m = px_get_method(vm, iter, vm->atom[PX_ATOM_return]);
    px_pop_roots(vm, 1);
    if (m == PX_EXCEPTION) return -1;
    if (m == PX_UNDEFINED) return 0;
    r = px_call(vm, m, iter, 0, NULL);
    if (r == PX_EXCEPTION) return -1;
    if (!px_is_obj(r)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator return() result is not an object");
        return -1;
    }
    return 0;
}

/* IteratorClose after a throw completion: return() runs, but the pending
 * exception wins over anything it does. Always returns -1. */
int px_iterator_close_throw(PxVM *vm, PxValue iter) {
    PxValue e = vm->exception, m;
    if (vm->uncatchable) return -1;
    vm->exception = PX_UNDEFINED;
    PX_ROOT(vm, e);
    PX_ROOT(vm, iter);
    m = px_get_method(vm, iter, vm->atom[PX_ATOM_return]);
    if (m != PX_EXCEPTION && m != PX_UNDEFINED) px_call(vm, m, iter, 0, NULL);
    px_pop_roots(vm, 2);
    if (!vm->uncatchable) vm->exception = e;
    return -1;
}

/* GetMethod: undefined for undefined or null, else it must be callable. */
PxValue px_get_method(PxVM *vm, PxValue v, PxValue key) {
    PxValue m = px_get(vm, v, key);
    if (m == PX_NULL) return PX_UNDEFINED;
    if (m == PX_EXCEPTION || m == PX_UNDEFINED || px_is_callable(m)) return m;
    return px_throw_error(vm, PX_TYPE_ERROR, "iterator method is not a function");
}

/* GetIterator (sync, or async with a sync fallback) as a record: a vec
 * [iterator, next method], the method read once as the spec does. */
PxValue px_iter_record(PxVM *vm, PxValue iterable, int async) {
    PxValue it = async ? px_get_async_iterator(vm, iterable) : px_get_iterator(vm, iterable), next;
    PxVec  *rec;
    if (it == PX_EXCEPTION) return it;
    PX_ROOT(vm, it);
    next = px_get(vm, it, vm->atom[PX_ATOM_next]);
    if (next == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return next;
    }
    PX_ROOT(vm, next);
    rec = px_vec_new(vm, 2);
    px_pop_roots(vm, 2);
    if (!rec) return PX_EXCEPTION;
    rec->items[0] = it;
    rec->items[1] = next;
    return px_from_ptr(rec);
}

/* IteratorStep + IteratorValue on a record: 1 with *out, 0 done, -1 exception. */
int px_record_step(PxVM *vm, PxValue rec, PxValue *out) {
    PxValue r, d;
    PX_ROOT(vm, rec);
    r = px_call(vm, ((PxVec *)px_ptr(rec))->items[1], ((PxVec *)px_ptr(rec))->items[0], 0, NULL);
    px_pop_roots(vm, 1);
    if (r == PX_EXCEPTION) return -1;
    if (!px_is_obj(r)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator result is not an object");
        return -1;
    }
    PX_ROOT(vm, r);
    d = px_get(vm, r, vm->atom[PX_ATOM_done]);
    if (d == PX_EXCEPTION || px_truthy(d)) {
        px_pop_roots(vm, 1);
        return d == PX_EXCEPTION ? -1 : 0;
    }
    *out = px_get(vm, r, vm->atom[PX_ATOM_value]);
    px_pop_roots(vm, 1);
    return *out == PX_EXCEPTION ? -1 : 1;
}

static PxValue species_getter(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return t;
}

/* get [Symbol.species]() { return this } on a constructor */
int px_def_species(PxVM *vm, PxValue ctor) {
    PxValue g = px_make_native(vm, species_getter, "get [Symbol.species]", 0, 0);
    int     r;
    if (g == PX_EXCEPTION) return -1;
    ((PxObject *)px_ptr(g))->flags |= PX_OBJ_NOT_CTOR;
    PX_ROOT(vm, ctor);
    PX_ROOT(vm, g);
    r = px_define_accessor(vm, ctor, vm->sym_species, g, PX_UNDEFINED, PX_ATTR_CONFIGURABLE);
    px_pop_roots(vm, 2);
    return r;
}

int px_def_tag(PxVM *vm, PxValue obj, const char *tag) {
    PxValue s = px_str_from_cstr(vm, tag);
    int     r;
    if (s == PX_EXCEPTION) return -1;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, s);
    r = px_define(vm, obj, vm->sym_to_string_tag, s, PX_ATTR_CONFIGURABLE);
    px_pop_roots(vm, 2);
    return r;
}

/* ============================================================ iterator objects */

/* The prototype (and so the next() brand) an iterator of `kind` has. */
static int iterobj_proto(int kind, int is_set) {
    if (kind <= PX_IT_ARRAY_ENTRIES) return PX_PROTO_ARRAY_ITERATOR;
    if (kind == PX_IT_STRING) return PX_PROTO_STRING_ITERATOR;
    if (kind == PX_IT_ASYNC_FROM_SYNC) return PX_PROTO_ASYNC_FROM_SYNC;
    return is_set ? PX_PROTO_SET_ITERATOR : PX_PROTO_MAP_ITERATOR;
}

PxValue px_make_iterobj(PxVM *vm, PxValue target, int kind) {
    PxIterObj *it;
    int        is_set = kind >= PX_IT_MAP_ENTRIES && kind <= PX_IT_MAP_VALUES &&
                 ((PxMap *)px_ptr(target))->kind == PX_MAP_SET;
    PX_ROOT(vm, target);
    it = (PxIterObj *)px_obj_new(vm, PX_T_ITEROBJ, sizeof(PxIterObj), vm->protos[iterobj_proto(kind, is_set)]);
    px_pop_roots(vm, 1);
    if (!it) return PX_EXCEPTION;
    it->target = target;
    it->kind   = (uint8_t)kind;
    it->is_set = (uint8_t)is_set;
    return px_from_ptr(it);
}

/* Map/Set iteration lives with the collections. */
int px_map_iter_next(PxVM *vm, PxIterObj *it, PxValue *key, PxValue *value);

/* %ArrayIteratorPrototype%.next and the string, map and set ones (magic:
 * the PX_PROTO_* whose iterators this one accepts) */
static PxValue iterobj_next(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxIterObj *it;
    PxValue    v = PX_UNDEFINED;
    (void)argc;
    (void)argv;
    if (!px_is_obj(t) || px_type_of(t) != PX_T_ITEROBJ ||
        iterobj_proto(((PxIterObj *)px_ptr(t))->kind, ((PxIterObj *)px_ptr(t))->is_set) != vm->native_magic)
        return px_throw_error(vm, PX_TYPE_ERROR, "next called on an incompatible receiver");
    it = (PxIterObj *)px_ptr(t);
    if (it->done) return px_iter_result(vm, PX_UNDEFINED, 1);
    PX_ROOT(vm, t);
    switch (it->kind) {
    case PX_IT_ARRAY_VALUES:
    case PX_IT_ARRAY_KEYS:
    case PX_IT_ARRAY_ENTRIES: {
        PxIdx len;
        if (px_is_obj(it->target) && px_type_of(it->target) == PX_T_TYPEDARRAY) {
            /* a typed array's own length, not its (overridable) length property */
            PxTyped *ta = (PxTyped *)px_ptr(it->target);
            if (!((PxArrayBuffer *)px_ptr(ta->buffer))->data) {
                px_throw_error(vm, PX_TYPE_ERROR, "the typed array's buffer is detached");
                goto fail;
            }
            len = ta->length;
        } else if (px_length_of(vm, it->target, &len) < 0) {
            goto fail;
        }
        if (it->index >= len) goto done;
        if (it->kind == PX_IT_ARRAY_KEYS) {
            v = px_idx_value(vm, it->index);
        } else {
            v = px_get_index(vm, it->target, it->index);
            if (v != PX_EXCEPTION && it->kind == PX_IT_ARRAY_ENTRIES) {
                PxValue pair;
                PX_ROOT(vm, v);
                pair = px_array_new(vm, 2);
                if (pair != PX_EXCEPTION) {
                    PX_ROOT(vm, pair);
                    if (px_array_push(vm, pair, px_from_smi((int32_t)it->index)) < 0 ||
                        px_array_push(vm, pair, v) < 0)
                        pair = PX_EXCEPTION;
                    px_pop_roots(vm, 1);
                }
                px_pop_roots(vm, 1);
                v = pair;
            }
        }
        if (v == PX_EXCEPTION) goto fail;
        it->index++;
        break;
    }
    case PX_IT_STRING: {
        PxString *s = px_str_flat(vm, it->target);
        uint16_t  u[2];
        uint32_t  n = 1;
        if (!s) goto fail;
        if (it->index >= s->len) goto done;
        u[0] = px_str_at(s, it->index);
        if (u[0] >= 0xD800 && u[0] <= 0xDBFF && it->index + 1 < s->len) {
            u[1] = px_str_at(s, it->index + 1);
            if (u[1] >= 0xDC00 && u[1] <= 0xDFFF) n = 2;
        }
        it->index += n;
        v = px_str_new_u16(vm, u, n);
        if (v == PX_EXCEPTION) goto fail;
        break;
    }
    default: {
        PxValue key, value;
        int     r = px_map_iter_next(vm, it, &key, &value);
        if (r < 0) goto fail;
        if (r == 0) goto done;
        if (it->kind == PX_IT_MAP_KEYS) v = key;
        else if (it->kind == PX_IT_MAP_VALUES) v = value;
        else {
            PX_ROOT(vm, key);
            PX_ROOT(vm, value);
            v = px_array_new(vm, 2);
            if (v != PX_EXCEPTION) {
                PX_ROOT(vm, v);
                if (px_array_push(vm, v, key) < 0 || px_array_push(vm, v, value) < 0) v = PX_EXCEPTION;
                px_pop_roots(vm, 1);
            }
            px_pop_roots(vm, 2);
            if (v == PX_EXCEPTION) goto fail;
        }
        break;
    }
    }
    px_pop_roots(vm, 1);
    return px_iter_result(vm, v, 0);
done:
    it->done   = 1;
    it->target = PX_UNDEFINED;
    px_pop_roots(vm, 1);
    return px_iter_result(vm, PX_UNDEFINED, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue iterator_self(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return t;
}

/* Array.prototype.keys / values / entries, String.prototype[Symbol.iterator] */
static PxValue make_array_iterator(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o;
    (void)argc;
    (void)argv;
    if (vm->native_magic == PX_IT_STRING) {
        if (t == PX_UNDEFINED || t == PX_NULL) return px_throw_error(vm, PX_TYPE_ERROR, "not a string");
        o = px_to_string(vm, t);
    } else {
        o = px_to_object(vm, t);
    }
    if (o == PX_EXCEPTION) return o;
    return px_make_iterobj(vm, o, vm->native_magic);
}

/* ============================================================ generators */

static PxGen *this_gen(PxVM *vm, PxValue t) {
    if (!px_is_obj(t) || px_type_of(t) != PX_T_GENERATOR || ((PxGen *)px_ptr(t))->is_async) {
        px_throw_error(vm, PX_TYPE_ERROR, "not a generator");
        return NULL;
    }
    return (PxGen *)px_ptr(t);
}

/* yield*: a generator that suspends at PX_SUSPEND_DELEGATE hands over the
 * iterable, and next/throw/return then go to the inner iterator directly
 * (g->deleg holds its record) without resuming the frame, until the inner
 * iterator is done. The frame then resumes with the yield*'s completion:
 * the inner value, an exception, or a return (its finally blocks run).
 * While delegating, next() hands out the inner result objects as they are. */

enum { DG_YIELD = 0, DG_DONE };

/* One delegation step with the completion received (*mode, v). DG_YIELD:
 * *out is the inner result to hand out; DG_DONE: the delegation is over
 * and (*mode, *out) is the completion for the frame; -1: uncatchable. */
static int deleg_step(PxVM *vm, PxGen *g, int *mode, PxValue v, PxValue *out) {
    PxValue gv = px_from_ptr(g), rec = g->deleg, iter, m, r = PX_UNDEFINED, d;
    int     is_return = *mode == PX_RESUME_RETURN;
    PX_ROOT(vm, gv);
    PX_ROOT(vm, rec);
    PX_ROOT(vm, v);
    PX_ROOT(vm, r);
    iter = ((PxVec *)px_ptr(rec))->items[0];
    if (*mode == PX_RESUME_NEXT) {
        m = ((PxVec *)px_ptr(rec))->items[1];
    } else {
        m = px_get_method(vm, iter, vm->atom[is_return ? PX_ATOM_return : PX_ATOM_throw]);
        if (m == PX_EXCEPTION) goto thrown;
        if (m == PX_UNDEFINED) {
            if (is_return) {
                *out = v;
                goto done;
            }
            /* no throw(): the iterator is closed, and that is a protocol error */
            if (px_iterator_close(vm, iter) == 0) px_throw_error(vm, PX_TYPE_ERROR, "the iterator has no throw method");
            goto thrown;
        }
    }
    r = px_call(vm, m, iter, 1, &v);
    if (r == PX_EXCEPTION) goto thrown;
    if (!px_is_obj(r)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator result is not an object");
        goto thrown;
    }
    d = px_get(vm, r, vm->atom[PX_ATOM_done]);
    if (d == PX_EXCEPTION) goto thrown;
    if (!px_truthy(d)) {
        *out = r;
        px_pop_roots(vm, 4);
        return DG_YIELD;
    }
    *out = px_get(vm, r, vm->atom[PX_ATOM_value]);
    if (*out == PX_EXCEPTION) goto thrown;
    if (!is_return) *mode = PX_RESUME_NEXT; /* the yield* evaluates to it */
done:
    g->deleg = PX_UNDEFINED;
    px_pop_roots(vm, 4);
    return DG_DONE;
thrown:
    px_pop_roots(vm, 4);
    if (vm->uncatchable) return -1;
    *out          = vm->exception;
    vm->exception = PX_UNDEFINED;
    *mode         = PX_RESUME_THROW;
    g->deleg      = PX_UNDEFINED;
    return DG_DONE;
}

static PxValue gen_method(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxGen  *g = this_gen(vm, t);
    PxValue r, v = ARG(0);
    int     done, mode = vm->native_magic;
    if (!g) return PX_EXCEPTION;
    PX_ROOT(vm, t);
    PX_ROOT(vm, v);
    for (;;) {
        if (g->deleg != PX_UNDEFINED) {
            int k;
            if (g->state == PX_GEN_RUNNING) {
                r = px_throw_error(vm, PX_TYPE_ERROR, "generator is already running");
                break;
            }
            g->state = PX_GEN_RUNNING;
            k        = deleg_step(vm, g, &mode, v, &r);
            g->state = PX_GEN_SUSPENDED;
            if (k < 0) r = PX_EXCEPTION;
            if (k != DG_DONE) break; /* the inner result, as it is */
            v = r;
        }
        r = px_gen_resume(vm, g, v, mode, &done);
        if (r == PX_EXCEPTION) break;
        if (done || vm->suspend_await != PX_SUSPEND_DELEGATE) {
            r = px_iter_result(vm, r, done);
            break;
        }
        /* a yield*: GetIterator runs as part of the generator */
        g->state = PX_GEN_RUNNING;
        v        = px_iter_record(vm, r, 0);
        g->state = PX_GEN_SUSPENDED;
        mode     = PX_RESUME_NEXT;
        if (v == PX_EXCEPTION) {
            if (vm->uncatchable) {
                r = v;
                break;
            }
            v             = vm->exception;
            vm->exception = PX_UNDEFINED;
            mode          = PX_RESUME_THROW;
            continue;
        }
        g->deleg = v;
        v        = PX_UNDEFINED;
    }
    px_pop_roots(vm, 2);
    return r;
}

/* %GeneratorFunction%, %AsyncGeneratorFunction% and %AsyncFunction%: not
 * globals, but the constructors of the prototypes that generator and async
 * functions inherit from. Like Function, they would compile source at run
 * time, which PXJS does not do. */
static PxValue no_codegen(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_throw_error(vm, PX_TYPE_ERROR, "function constructors are not supported (no runtime code generation)");
}

/* fn_proto: the functions' prototype; inst_proto: their instances' (-1: none) */
static int def_fn_kind(PxVM *vm, const char *name, int fn_proto, int inst_proto) {
    PxValue ctor = px_make_native(vm, no_codegen, name, 1, 0), fp = vm->protos[fn_proto];
    int     r;
    if (ctor == PX_EXCEPTION) return -1;
    PX_ROOT(vm, ctor);
    vm->ctors[fn_proto] = ctor;
    r = px_set_proto(vm, ctor, vm->ctors[PX_PROTO_FUNCTION]) < 0 ||
        px_set_proto(vm, fp, vm->protos[PX_PROTO_FUNCTION]) < 0 ||
        px_def_value(vm, ctor, "prototype", fp, 0) < 0 ||
        px_define(vm, fp, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_CONFIGURABLE) < 0 ||
        px_def_tag(vm, fp, name) < 0 ||
        (inst_proto >= 0 &&
         (px_def_value(vm, fp, "prototype", vm->protos[inst_proto], PX_ATTR_CONFIGURABLE) < 0 ||
          px_define(vm, vm->protos[inst_proto], vm->atom[PX_ATOM_constructor], fp, PX_ATTR_CONFIGURABLE) < 0));
    px_pop_roots(vm, 1);
    return r ? -1 : 0;
}

/* ============================================================ setup */

int px_iter_init(PxVM *vm) {
    static const PxFnDef symbol_fns[]  = {{"for", symbol_for, 1, 0}, {"keyFor", symbol_key_for, 1, 0}};
    static const PxFnDef symbolp_fns[] = {{"toString", symbolp_to_string, 0, 0},
                                          {"valueOf", symbolp_value_of, 0, 0}};
    static const PxFnDef gen_fns[]     = {{"next", gen_method, 1, PX_RESUME_NEXT},
                                          {"return", gen_method, 1, PX_RESUME_RETURN},
                                          {"throw", gen_method, 1, PX_RESUME_THROW}};
    static const struct {
        int         proto;
        const char *tag;
    } iter_kinds[] = {{PX_PROTO_ARRAY_ITERATOR, "Array Iterator"},
                      {PX_PROTO_STRING_ITERATOR, "String Iterator"},
                      {PX_PROTO_MAP_ITERATOR, "Map Iterator"},
                      {PX_PROTO_SET_ITERATOR, "Set Iterator"}};
    static const PxFnDef arr_fns[]     = {{"keys", make_array_iterator, 0, PX_IT_ARRAY_KEYS},
                                          {"values", make_array_iterator, 0, PX_IT_ARRAY_VALUES},
                                          {"entries", make_array_iterator, 0, PX_IT_ARRAY_ENTRIES}};
    static const struct {
        const char *name;
        int         which;
    } wk[] = {{"iterator", 0}, {"asyncIterator", 1}, {"hasInstance", 2}, {"toPrimitive", 3}, {"toStringTag", 4},
              {"species", 5}};
    PxValue  ctor, f, desc, getter;
    int      i;

    /* Symbol and the well-known symbols */
    ctor = px_make_native(vm, symbol_ctor, "Symbol", 0, 0);
    if (ctor == PX_EXCEPTION) return -1;
    vm->ctors[PX_PROTO_SYMBOL] = ctor;
    if (px_def_value(vm, vm->global, "Symbol", ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_value(vm, ctor, "prototype", vm->protos[PX_PROTO_SYMBOL], 0) < 0 ||
        px_define(vm, vm->protos[PX_PROTO_SYMBOL], vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_fns(vm, ctor, symbol_fns, PX_COUNTOF(symbol_fns)) < 0 ||
        px_def_fns(vm, vm->protos[PX_PROTO_SYMBOL], symbolp_fns, PX_COUNTOF(symbolp_fns)) < 0)
        return -1;
    for (i = 0; i < PX_COUNTOF(wk); i++) {
        char    buf[40];
        PxValue s;
        snprintf(buf, sizeof buf, "Symbol.%s", wk[i].name);
        desc = px_str_from_cstr(vm, buf);
        if (desc == PX_EXCEPTION) return -1;
        s = px_symbol_new(vm, desc);
        if (s == PX_EXCEPTION) return -1;
        switch (wk[i].which) {
        case 0: vm->sym_iterator = s; break;
        case 1: vm->sym_async_iterator = s; break;
        case 2: vm->sym_has_instance = s; break;
        case 3: vm->sym_to_primitive = s; break;
        case 4: vm->sym_to_string_tag = s; break;
        default: vm->sym_species = s; break;
        }
        if (px_def_value(vm, ctor, wk[i].name, s, 0) < 0) return -1;
    }
    desc = px_str_from_cstr(vm, "fields");
    if (desc == PX_EXCEPTION) return -1;
    vm->sym_fields = px_symbol_new(vm, desc);
    if (vm->sym_fields == PX_EXCEPTION) return -1;
    ((PxSymbol *)px_ptr(vm->sym_fields))->is_private = 1;
    getter = px_make_native(vm, symbolp_description, "get description", 0, 0);
    if (getter == PX_EXCEPTION) return -1;
    {
        PxValue k = px_intern_cstr(vm, "description");
        if (k == PX_EXCEPTION || px_define_accessor(vm, vm->protos[PX_PROTO_SYMBOL], k, getter, PX_UNDEFINED,
                                                    PX_ATTR_CONFIGURABLE) < 0)
            return -1;
    }

    /* %IteratorPrototype%: [Symbol.iterator]() { return this } */
    f = px_make_native(vm, iterator_self, "[Symbol.iterator]", 0, 0);
    if (f == PX_EXCEPTION || px_define(vm, vm->protos[PX_PROTO_ITERATOR], vm->sym_iterator, f, PX_ATTR_HIDDEN) < 0)
        return -1;
    /* array/string/map/set iterators: a prototype each, whose next() only
     * takes its own kind */
    for (i = 0; i < PX_COUNTOF(iter_kinds); i++) {
        PxValue p = vm->protos[iter_kinds[i].proto];
        if (px_set_proto(vm, p, vm->protos[PX_PROTO_ITERATOR]) < 0) return -1;
        f = px_make_native(vm, iterobj_next, "next", 0, iter_kinds[i].proto);
        if (f == PX_EXCEPTION || px_define(vm, p, vm->atom[PX_ATOM_next], f, PX_ATTR_HIDDEN) < 0 ||
            px_def_tag(vm, p, iter_kinds[i].tag) < 0)
            return -1;
        if (iter_kinds[i].proto == PX_PROTO_ARRAY_ITERATOR) vm->array_iter_next = f;
    }
    /* generators, and the prototypes of generator and async functions */
    if (px_set_proto(vm, vm->protos[PX_PROTO_GENOBJ], vm->protos[PX_PROTO_ITERATOR]) < 0 ||
        px_def_fns(vm, vm->protos[PX_PROTO_GENOBJ], gen_fns, PX_COUNTOF(gen_fns)) < 0 ||
        px_def_tag(vm, vm->protos[PX_PROTO_GENOBJ], "Generator") < 0 ||
        def_fn_kind(vm, "GeneratorFunction", PX_PROTO_GENFN, PX_PROTO_GENOBJ) < 0 ||
        def_fn_kind(vm, "AsyncGeneratorFunction", PX_PROTO_ASYNCGENFN, PX_PROTO_ASYNCGENOBJ) < 0 ||
        def_fn_kind(vm, "AsyncFunction", PX_PROTO_ASYNCFN, -1) < 0)
        return -1;
    /* Array.prototype.keys/values/entries and [Symbol.iterator] */
    if (px_def_fns(vm, vm->protos[PX_PROTO_ARRAY], arr_fns, PX_COUNTOF(arr_fns)) < 0) return -1;
    f = px_get(vm, vm->protos[PX_PROTO_ARRAY], px_intern_cstr(vm, "values"));
    if (f == PX_EXCEPTION || px_define(vm, vm->protos[PX_PROTO_ARRAY], vm->sym_iterator, f, PX_ATTR_HIDDEN) < 0)
        return -1;
    vm->array_values = f;
    f = px_make_native(vm, make_array_iterator, "[Symbol.iterator]", 0, PX_IT_STRING);
    if (f == PX_EXCEPTION) return -1;
    ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
    if (px_define(vm, vm->protos[PX_PROTO_STRING], vm->sym_iterator, f, PX_ATTR_HIDDEN) < 0) return -1;
    return 0;
}
