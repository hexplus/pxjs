/* Promises and the job (microtask) queue.
 *
 * The queue is a ring of fixed-size records in C memory (its values are
 * GC roots): a PSP app's promise traffic is small and bursty, and a ring
 * that grows by doubling never allocates on the steady state.
 *
 * Job records (PX_JOB_WORDS values):
 *   JOB_REACTION_FUL / _REJ   handler, argument, capability (or undefined)
 *   JOB_THENABLE              promise to settle, thenable, its then()
 *   JOB_CALL                  function (queueMicrotask) */

#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))

enum { JOB_REACTION_FUL = 0, JOB_REACTION_REJ, JOB_THENABLE, JOB_CALL };

/* ============================================================ the queue */

int px_enqueue_job(PxVM *vm, PxValue a, PxValue b, PxValue c, PxValue d) {
    uint32_t w = PX_JOB_WORDS, at;
    if (vm->jobs_count == vm->jobs_cap) {
        uint32_t ncap = vm->jobs_cap ? vm->jobs_cap * 2 : 32, i;
        PxValue *n    = (PxValue *)malloc((size_t)ncap * w * sizeof(PxValue));
        if (!n) {
            px_throw_oom(vm);
            return -1;
        }
        for (i = 0; i < vm->jobs_count * w; i++) n[i] = vm->jobs[(vm->jobs_head * w + i) % (vm->jobs_cap * w)];
        free(vm->jobs);
        vm->jobs      = n;
        vm->jobs_head = 0;
        vm->jobs_cap  = ncap;
    }
    at                = ((vm->jobs_head + vm->jobs_count) % vm->jobs_cap) * w;
    vm->jobs[at]      = a;
    vm->jobs[at + 1]  = b;
    vm->jobs[at + 2]  = c;
    vm->jobs[at + 3]  = d;
    vm->jobs_count++;
    return 0;
}

int px_has_jobs(PxVM *vm) { return vm->jobs_count > 0; }

void px_set_rejection_tracker(PxVM *vm, PxRejectionTracker fn, void *opaque) {
    vm->rejection_tracker = fn;
    vm->rejection_opaque  = opaque;
}

/* ============================================================ promises */

static int is_promise(PxValue v) { return px_is_obj(v) && px_type_of(v) == PX_T_PROMISE; }

static PxValue promise_alloc(PxVM *vm, PxValue proto) {
    PxPromise *p = (PxPromise *)px_obj_new(vm, PX_T_PROMISE, sizeof(PxPromise), proto);
    if (!p) return PX_EXCEPTION;
    p->result = PX_UNDEFINED;
    return px_from_ptr(p);
}

PxValue px_promise_new(PxVM *vm) { return promise_alloc(vm, vm->protos[PX_PROTO_PROMISE]); }

/* A built-in function made by the promise machinery: named "", and never
 * a constructor. */
static PxValue closure(PxVM *vm, PxNativeFn fn, int length, int magic, PxValue data) {
    PxValue f = px_make_native_data(vm, fn, "", length, data);
    if (f == PX_EXCEPTION) return f;
    ((PxNative *)px_ptr(f))->magic = (int16_t)magic;
    ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
    return f;
}

static int trigger(PxVM *vm, PxValue promise) {
    PxPromise *p = (PxPromise *)px_ptr(promise);
    uint32_t   i, n = p->nreactions;
    PxVec     *rs   = p->reactions;
    PxValue    rv   = rs ? px_from_ptr(rs) : 0;
    int        rej  = p->state == PX_PROMISE_REJECTED;
    p->reactions    = NULL;
    p->nreactions   = 0;
    PX_ROOT(vm, promise);
    PX_ROOT(vm, rv);
    for (i = 0; i < n; i++) {
        PxValue handler = rs->items[3 * i + (rej ? 1 : 0)];
        if (px_enqueue_job(vm, px_from_smi(rej ? JOB_REACTION_REJ : JOB_REACTION_FUL), handler,
                           ((PxPromise *)px_ptr(promise))->result, rs->items[3 * i + 2]) < 0) {
            px_pop_roots(vm, 2);
            return -1;
        }
    }
    px_pop_roots(vm, 2);
    return 0;
}

static int settle(PxVM *vm, PxValue promise, PxValue value, int state) {
    PxPromise *p = (PxPromise *)px_ptr(promise);
    if (p->state != PX_PROMISE_PENDING) return 0;
    p->state  = (uint8_t)state;
    p->result = value;
    if (state == PX_PROMISE_REJECTED && !p->handled && vm->rejection_tracker)
        vm->rejection_tracker(vm, promise, value, 0, vm->rejection_opaque);
    return trigger(vm, promise);
}

int px_promise_reject(PxVM *vm, PxValue promise, PxValue reason) {
    return settle(vm, promise, reason, PX_PROMISE_REJECTED);
}

int px_promise_resolve(PxVM *vm, PxValue promise, PxValue value) {
    PxValue then;
    if (((PxPromise *)px_ptr(promise))->state != PX_PROMISE_PENDING) return 0;
    if (value == promise) {
        px_throw_error(vm, PX_TYPE_ERROR, "a promise cannot be resolved with itself");
        {
            PxValue e     = vm->exception;
            vm->exception = PX_UNDEFINED;
            return px_promise_reject(vm, promise, e);
        }
    }
    if (!px_is_obj(value)) return settle(vm, promise, value, PX_PROMISE_FULFILLED);
    PX_ROOT(vm, promise);
    PX_ROOT(vm, value);
    then = px_get(vm, value, vm->atom[PX_ATOM_then]);
    if (then == PX_EXCEPTION) {
        PxValue e     = vm->exception;
        int     r;
        vm->exception = PX_UNDEFINED;
        r             = px_promise_reject(vm, promise, e);
        px_pop_roots(vm, 2);
        return vm->uncatchable ? -1 : r;
    }
    if (!px_is_callable(then)) {
        px_pop_roots(vm, 2);
        return settle(vm, promise, value, PX_PROMISE_FULFILLED);
    }
    {
        int r = px_enqueue_job(vm, px_from_smi(JOB_THENABLE), promise, value, then);
        px_pop_roots(vm, 2);
        return r;
    }
}

static int add_reaction(PxVM *vm, PxValue promise, PxValue on_ful, PxValue on_rej, PxValue derived) {
    PxPromise *p = (PxPromise *)px_ptr(promise);
    if (p->state == PX_PROMISE_REJECTED && !p->handled && vm->rejection_tracker)
        vm->rejection_tracker(vm, promise, p->result, 1, vm->rejection_opaque);
    p->handled = 1;
    if (p->state != PX_PROMISE_PENDING) {
        int rej = p->state == PX_PROMISE_REJECTED;
        return px_enqueue_job(vm, px_from_smi(rej ? JOB_REACTION_REJ : JOB_REACTION_FUL), rej ? on_rej : on_ful,
                              p->result, derived);
    }
    if (!p->reactions || (p->nreactions + 1) * 3 > p->reactions->cap) {
        PxVec *g;
        PX_ROOT(vm, promise);
        PX_ROOT(vm, on_ful);
        PX_ROOT(vm, on_rej);
        PX_ROOT(vm, derived);
        g = px_vec_grow(vm, p->reactions, (p->nreactions + 1) * 3);
        px_pop_roots(vm, 4);
        if (!g) return -1;
        p            = (PxPromise *)px_ptr(promise);
        p->reactions = g;
    }
    p->reactions->items[3 * p->nreactions]     = on_ful;
    p->reactions->items[3 * p->nreactions + 1] = on_rej;
    p->reactions->items[3 * p->nreactions + 2] = derived;
    p->nreactions++;
    return 0;
}

int px_promise_then_native(PxVM *vm, PxValue promise, PxValue on_ful, PxValue on_rej) {
    return add_reaction(vm, promise, on_ful, on_rej, PX_UNDEFINED);
}

int px_promise_then_derived(PxVM *vm, PxValue promise, PxValue on_ful, PxValue on_rej, PxValue derived) {
    return add_reaction(vm, promise, on_ful, on_rej, derived);
}

/* ============================================================ resolving functions */

/* data: a 2-slot vec [promise, already-resolved flag] shared by a pair */
static PxValue resolving_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxVec  *d = (PxVec *)px_ptr(vm->native_data);
    PxValue p;
    int     r;
    (void)t;
    if (d->items[1] == PX_TRUE) return PX_UNDEFINED;
    d->items[1] = PX_TRUE;
    p           = d->items[0];
    r           = vm->native_magic ? px_promise_reject(vm, p, ARG(0)) : px_promise_resolve(vm, p, ARG(0));
    return r < 0 ? PX_EXCEPTION : PX_UNDEFINED;
}

/* CreateResolvingFunctions */
static int make_resolvers(PxVM *vm, PxValue promise, PxValue *res, PxValue *rej) {
    PxVec  *d = px_vec_new(vm, 2);
    PxValue dv;
    if (!d) return -1;
    d->items[0] = promise;
    d->items[1] = PX_FALSE;
    dv          = px_from_ptr(d);
    PX_ROOT(vm, dv);
    *res = closure(vm, resolving_fn, 1, 0, dv);
    if (*res == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return -1;
    }
    PX_ROOT(vm, *res);
    *rej = closure(vm, resolving_fn, 1, 1, dv);
    px_pop_roots(vm, 2);
    return *rej == PX_EXCEPTION ? -1 : 0;
}

/* ============================================================ capabilities
 *
 * A PromiseCapability is a promise and the functions that settle it. For
 * %Promise% itself (nearly always) the functions need not exist: the
 * promise is settled directly, which nothing can tell apart. So a
 * capability is either a PxPromise, or a vec [promise, resolve, reject]
 * (a subclass's, or one whose functions are handed to user code). */

static int is_cap_vec(PxValue cap) { return px_is_ptr(cap) && px_type_of(cap) == PX_T_VEC; }

static PxValue cap_promise(PxValue cap) { return is_cap_vec(cap) ? ((PxVec *)px_ptr(cap))->items[0] : cap; }

/* Calls the capability's resolve (ok) or reject with v; -1 on exception. */
static int cap_settle(PxVM *vm, PxValue cap, int ok, PxValue v) {
    if (!is_cap_vec(cap)) return ok ? px_promise_resolve(vm, cap, v) : px_promise_reject(vm, cap, v);
    return px_call(vm, ((PxVec *)px_ptr(cap))->items[ok ? 1 : 2], PX_UNDEFINED, 1, &v) == PX_EXCEPTION ? -1 : 0;
}

/* IfAbruptRejectPromise: the pending exception rejects the capability,
 * whose promise is returned (or the exception, if reject throws). */
static PxValue cap_reject_pending(PxVM *vm, PxValue cap) {
    PxValue e = vm->exception;
    if (vm->uncatchable) return PX_EXCEPTION;
    vm->exception = PX_UNDEFINED;
    PX_ROOT(vm, cap);
    if (cap_settle(vm, cap, 0, e) < 0) cap = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return cap == PX_EXCEPTION ? cap : cap_promise(cap);
}

/* GetCapabilitiesExecutor; data: the capability vec being filled */
static PxValue capability_executor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxVec *c = (PxVec *)px_ptr(vm->native_data);
    (void)t;
    if (c->items[1] != PX_UNDEFINED || c->items[2] != PX_UNDEFINED)
        return px_throw_error(vm, PX_TYPE_ERROR, "promise capability executor already called");
    c->items[1] = ARG(0);
    c->items[2] = ARG(1);
    return PX_UNDEFINED;
}

/* NewPromiseCapability(C) */
static PxValue new_capability(PxVM *vm, PxValue c) {
    PxVec  *v;
    PxValue cv, ex, p;
    if (c == vm->ctors[PX_PROTO_PROMISE]) return px_promise_new(vm);
    if (!px_is_constructor(c)) return px_throw_error(vm, PX_TYPE_ERROR, "the promise constructor is not a constructor");
    PX_ROOT(vm, c);
    v = px_vec_new(vm, 3);
    if (!v) goto fail;
    cv = px_from_ptr(v);
    PX_ROOT(vm, cv);
    ex = closure(vm, capability_executor, 2, 0, cv);
    p  = ex == PX_EXCEPTION ? ex : px_construct(vm, c, 1, &ex);
    px_pop_roots(vm, 2);
    if (p == PX_EXCEPTION) return p;
    v           = (PxVec *)px_ptr(cv);
    v->items[0] = p;
    if (!px_is_callable(v->items[1]) || !px_is_callable(v->items[2]))
        return px_throw_error(vm, PX_TYPE_ERROR, "the promise constructor did not supply resolve and reject functions");
    return cv;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* A capability with real resolve/reject functions (to hand to user code). */
static PxValue cap_with_functions(PxVM *vm, PxValue cap) {
    PxValue res, rej;
    PxVec  *v;
    if (is_cap_vec(cap)) return cap;
    PX_ROOT(vm, cap);
    if (make_resolvers(vm, cap, &res, &rej) < 0) goto fail;
    PX_ROOT(vm, res);
    PX_ROOT(vm, rej);
    v = px_vec_new(vm, 3);
    px_pop_roots(vm, 3);
    if (!v) return PX_EXCEPTION;
    v->items[0] = cap;
    v->items[1] = res;
    v->items[2] = rej;
    return px_from_ptr(v);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* PromiseResolve(C, x): a promise whose `constructor` is C is used as it
 * is (reading `constructor` is observable). */
static PxValue promise_resolve_with(PxVM *vm, PxValue c, PxValue x) {
    PxValue cap;
    PX_ROOT(vm, c);
    PX_ROOT(vm, x);
    if (is_promise(x)) {
        PxValue xc = px_get(vm, x, vm->atom[PX_ATOM_constructor]);
        if (xc == PX_EXCEPTION || xc == c) {
            px_pop_roots(vm, 2);
            return xc == PX_EXCEPTION ? xc : x;
        }
    }
    cap = new_capability(vm, c);
    if (cap != PX_EXCEPTION) {
        PX_ROOT(vm, cap);
        if (cap_settle(vm, cap, 1, x) < 0) cap = PX_EXCEPTION;
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 2);
    return cap == PX_EXCEPTION ? cap : cap_promise(cap);
}

/* PromiseResolve(%Promise%, value): what await does to its operand */
PxValue px_promise_resolved(PxVM *vm, PxValue value) {
    return promise_resolve_with(vm, vm->ctors[PX_PROTO_PROMISE], value);
}

/* ============================================================ running jobs */

static int run_one(PxVM *vm, PxValue *job) {
    int kind = px_smi(job[0]);
    switch (kind) {
    case JOB_REACTION_FUL:
    case JOB_REACTION_REJ: {
        PxValue handler = job[1], arg = job[2], cap = job[3], r;
        if (!px_is_callable(handler)) {
            if (cap == PX_UNDEFINED) return 0;
            return cap_settle(vm, cap, kind == JOB_REACTION_FUL, arg);
        }
        r = px_call(vm, handler, PX_UNDEFINED, 1, &arg);
        if (r == PX_EXCEPTION) {
            PxValue e;
            if (vm->uncatchable || cap == PX_UNDEFINED) return -1; /* nobody to hand it to: the host reports it */
            e             = vm->exception;
            vm->exception = PX_UNDEFINED;
            return cap_settle(vm, cap, 0, e);
        }
        if (cap == PX_UNDEFINED) return 0;
        return cap_settle(vm, cap, 1, r);
    }
    case JOB_THENABLE: {
        PxValue promise = job[1], thenable = job[2], then = job[3], args[2], r;
        if (make_resolvers(vm, promise, &args[0], &args[1]) < 0) return -1;
        r = px_call(vm, then, thenable, 2, args);
        if (r == PX_EXCEPTION) {
            PxValue e;
            if (vm->uncatchable) return -1;
            e             = vm->exception;
            vm->exception = PX_UNDEFINED;
            return px_call(vm, args[1], PX_UNDEFINED, 1, &e) == PX_EXCEPTION ? -1 : 0;
        }
        return 0;
    }
    default: {
        PxValue r = px_call(vm, job[1], PX_UNDEFINED, 0, NULL);
        return r == PX_EXCEPTION ? -1 : 0;
    }
    }
}

int px_run_jobs(PxVM *vm) {
    while (vm->jobs_count > 0) {
        PxValue  job[PX_JOB_WORDS];
        uint32_t w = PX_JOB_WORDS, i, at = vm->jobs_head * w;
        int      r;
        for (i = 0; i < w; i++) job[i] = vm->jobs[at + i];
        vm->jobs_head = (vm->jobs_head + 1) % vm->jobs_cap;
        vm->jobs_count--;
        /* the popped job's values are only on the C stack now */
        for (i = 0; i < w; i++) PX_ROOT(vm, job[i]);
        r = run_one(vm, job);
        px_pop_roots(vm, (int)w);
        if (r < 0) {
            PxValue e = vm->exception;
            PX_ROOT(vm, e);
            if (e == vm->oom_error) snprintf(vm->error_text, sizeof vm->error_text, "InternalError: out of memory");
            else px_inspect(vm, e, vm->error_text, sizeof vm->error_text);
            px_pop_roots(vm, 1);
            vm->exception   = PX_UNDEFINED;
            vm->uncatchable = 0;
            return -1;
        }
    }
    px_end_task(vm);
    return 0;
}

void px_end_task(PxVM *vm) { vm->kept = PX_UNDEFINED; /* WeakRef targets need not outlive the task */ }

/* ============================================================ JS API */

static PxValue this_promise(PxVM *vm, PxValue t) {
    if (!is_promise(t)) return px_throw_error(vm, PX_TYPE_ERROR, "not a promise");
    return t;
}

static PxValue promise_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue p, args[2], r, nt = vm->native_new_target, proto = vm->protos[PX_PROTO_PROMISE];
    (void)t;
    if (nt == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "Promise constructor cannot be invoked without 'new'");
    if (!px_is_callable(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "Promise resolver is not a function");
    if (nt != vm->native_callee) {
        /* a subclass: OrdinaryCreateFromConstructor(newTarget) */
        proto = px_get(vm, nt, vm->atom[PX_ATOM_prototype]);
        if (proto == PX_EXCEPTION) return proto;
        if (!px_is_obj(proto)) proto = vm->protos[PX_PROTO_PROMISE];
    }
    PX_ROOT(vm, proto);
    p = promise_alloc(vm, proto);
    px_pop_roots(vm, 1);
    if (p == PX_EXCEPTION) return p;
    PX_ROOT(vm, p);
    if (make_resolvers(vm, p, &args[0], &args[1]) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    PX_ROOT(vm, args[0]);
    PX_ROOT(vm, args[1]);
    r = px_call(vm, argv[0], PX_UNDEFINED, 2, args);
    if (r == PX_EXCEPTION && !vm->uncatchable) {
        PxValue e     = vm->exception;
        vm->exception = PX_UNDEFINED;
        r             = px_call(vm, args[1], PX_UNDEFINED, 1, &e);
    }
    px_pop_roots(vm, 3);
    return r == PX_EXCEPTION ? r : p;
}

/* SpeciesConstructor(o, %Promise%) */
static PxValue species_constructor(PxVM *vm, PxValue o) {
    PxValue c = px_get(vm, o, vm->atom[PX_ATOM_constructor]), s;
    if (c == PX_EXCEPTION) return c;
    if (c == PX_UNDEFINED) return vm->ctors[PX_PROTO_PROMISE];
    if (!px_is_obj(c)) return px_throw_error(vm, PX_TYPE_ERROR, "the promise's constructor is not an object");
    s = px_get(vm, c, vm->sym_species);
    if (s == PX_EXCEPTION) return s;
    if (s == PX_UNDEFINED || s == PX_NULL) return vm->ctors[PX_PROTO_PROMISE];
    if (!px_is_constructor(s)) return px_throw_error(vm, PX_TYPE_ERROR, "[Symbol.species] is not a constructor");
    return s;
}

/* Invoke(v, key, args): v's method (found through ToObject), called on v */
static PxValue invoke(PxVM *vm, PxValue v, PxValue key, int argc, PxValue *argv) {
    PxValue o, f;
    int     i;
    PX_ROOT(vm, v);
    for (i = 0; i < argc; i++) PX_ROOT(vm, argv[i]);
    o = px_to_object(vm, v);
    f = o == PX_EXCEPTION ? o : px_get(vm, o, key);
    if (f != PX_EXCEPTION) f = px_call(vm, f, v, argc, argv);
    px_pop_roots(vm, argc + 1);
    return f;
}

static PxValue promisep_then(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue cap, f = ARG(0), r = ARG(1);
    if (this_promise(vm, t) == PX_EXCEPTION) return PX_EXCEPTION;
    PX_ROOT(vm, t);
    cap = species_constructor(vm, t);
    if (cap != PX_EXCEPTION) cap = new_capability(vm, cap);
    if (cap != PX_EXCEPTION) {
        PX_ROOT(vm, cap);
        if (add_reaction(vm, t, px_is_callable(f) ? f : PX_UNDEFINED, px_is_callable(r) ? r : PX_UNDEFINED, cap) < 0)
            cap = PX_EXCEPTION;
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 1);
    return cap == PX_EXCEPTION ? cap : cap_promise(cap);
}

static PxValue promisep_catch(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue args[2];
    args[0] = PX_UNDEFINED;
    args[1] = ARG(0);
    return invoke(vm, t, vm->atom[PX_ATOM_then], 2, args);
}

/* finally(fn): thenFinally (magic 0) and catchFinally (1) call fn, then
 * pass the value on (or throw the reason) once fn's result has settled.
 * data: [fn, C]. The value thunk and the thrower: data is the value. */
static PxValue finally_pass(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return vm->native_magic ? px_throw(vm, vm->native_data) : vm->native_data;
}

static PxValue finally_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxVec  *d   = (PxVec *)px_ptr(vm->native_data);
    PxValue v   = ARG(0), c = d->items[1], r, pass;
    int     rej = vm->native_magic;
    (void)t;
    PX_ROOT(vm, v);
    PX_ROOT(vm, c);
    r = px_call(vm, d->items[0], PX_UNDEFINED, 0, NULL);
    if (r != PX_EXCEPTION) r = promise_resolve_with(vm, c, r);
    if (r != PX_EXCEPTION) {
        PX_ROOT(vm, r);
        pass = closure(vm, finally_pass, 0, rej, v);
        r    = pass == PX_EXCEPTION ? pass : invoke(vm, r, vm->atom[PX_ATOM_then], 1, &pass);
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 2);
    return r;
}

static PxValue promisep_finally(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue fn = ARG(0), args[2], c, dv;
    PxVec  *d;
    if (!px_is_obj(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Promise.prototype.finally called on a non-object");
    PX_ROOT(vm, t);
    c = species_constructor(vm, t);
    if (c == PX_EXCEPTION) goto fail;
    args[0] = args[1] = fn;
    if (px_is_callable(fn)) {
        PX_ROOT(vm, c);
        d = px_vec_new(vm, 2);
        px_pop_roots(vm, 1);
        if (!d) goto fail;
        d->items[0] = fn;
        d->items[1] = c;
        dv          = px_from_ptr(d);
        PX_ROOT(vm, dv);
        args[0] = closure(vm, finally_fn, 1, 0, dv);
        if (args[0] == PX_EXCEPTION) goto fail2;
        PX_ROOT(vm, args[0]);
        args[1] = closure(vm, finally_fn, 1, 1, dv);
        px_pop_roots(vm, 2);
        if (args[1] == PX_EXCEPTION) goto fail;
    }
    px_pop_roots(vm, 1);
    return invoke(vm, t, vm->atom[PX_ATOM_then], 2, args);
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

static PxValue promise_resolve_static(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    if (!px_is_obj(t)) return px_throw_error(vm, PX_TYPE_ERROR, "Promise.resolve called on a non-object");
    return promise_resolve_with(vm, t, ARG(0));
}

static PxValue promise_reject_static(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue cap = new_capability(vm, t);
    if (cap == PX_EXCEPTION) return cap;
    PX_ROOT(vm, cap);
    if (cap_settle(vm, cap, 0, ARG(0)) < 0) cap = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return cap == PX_EXCEPTION ? cap : cap_promise(cap);
}

/* Combinators: all, allSettled, any, race. State vec: [values (a vec
 * list), count, remaining, capability, mode]; element functions' data:
 * [state, index, already called]. The values become an array only at the
 * end, as the spec's List does (nothing on Array.prototype is touched). */
enum { COMB_ALL = 0, COMB_ALL_SETTLED, COMB_ANY, COMB_RACE };
enum { ST_LIST = 0, ST_COUNT, ST_REMAINING, ST_CAP, ST_MODE };

static PxValue settled_record(PxVM *vm, int ok, PxValue v) {
    PxValue o = px_object_new(vm), s;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, v);
    PX_ROOT(vm, o);
    s = px_str_from_cstr(vm, ok ? "fulfilled" : "rejected");
    if (s == PX_EXCEPTION || px_def_value(vm, o, "status", s, PX_ATTR_DEFAULT) < 0 ||
        px_def_value(vm, o, ok ? "value" : "reason", v, PX_ATTR_DEFAULT) < 0)
        o = PX_EXCEPTION;
    px_pop_roots(vm, 2);
    return o;
}

/* All elements are in: resolve with the values (all, allSettled), or
 * reject with an AggregateError of the reasons (any). -1 on exception. */
static int comb_finish(PxVM *vm, PxVec *st) {
    int      mode = px_smi(st->items[ST_MODE]), r = -1;
    uint32_t i, n = (uint32_t)px_smi(st->items[ST_COUNT]);
    PxValue  stv  = px_from_ptr(st), arr = px_array_new(vm, n);
    if (arr == PX_EXCEPTION) return -1;
    PX_ROOT(vm, stv);
    PX_ROOT(vm, arr);
    for (i = 0; i < n; i++)
        if (px_array_push(vm, arr, ((PxVec *)px_ptr(((PxVec *)px_ptr(stv))->items[ST_LIST]))->items[i]) < 0) goto out;
    if (mode == COMB_ANY) {
        PxValue e = px_str_from_cstr(vm, "All promises were rejected");
        if (e != PX_EXCEPTION) e = px_error_new(vm, PX_AGGREGATE_ERROR, e);
        if (e == PX_EXCEPTION) goto out;
        PX_ROOT(vm, e);
        r = px_def_value(vm, e, "errors", arr, PX_ATTR_HIDDEN) < 0 ? -1 : cap_settle(vm, st->items[ST_CAP], 0, e);
        px_pop_roots(vm, 1);
    } else {
        r = cap_settle(vm, st->items[ST_CAP], 1, arr);
    }
out:
    px_pop_roots(vm, 2);
    return r;
}

/* Promise.all / allSettled resolve element functions, and allSettled /
 * any reject element functions (magic 1) */
static PxValue comb_element(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxVec  *d  = (PxVec *)px_ptr(vm->native_data);
    PxVec  *st = (PxVec *)px_ptr(d->items[0]);
    int     mode = px_smi(st->items[ST_MODE]), rejected = vm->native_magic;
    PxValue v    = ARG(0);
    (void)t;
    if (d->items[2] == PX_TRUE) return PX_UNDEFINED;
    d->items[2] = PX_TRUE;
    if (mode == COMB_ALL_SETTLED) {
        v = settled_record(vm, !rejected, v);
        if (v == PX_EXCEPTION) return v;
    }
    ((PxVec *)px_ptr(st->items[ST_LIST]))->items[px_smi(d->items[1])] = v;
    st->items[ST_REMAINING] = px_from_smi(px_smi(st->items[ST_REMAINING]) - 1);
    if (px_smi(st->items[ST_REMAINING]) == 0 && comb_finish(vm, st) < 0) return PX_EXCEPTION;
    return PX_UNDEFINED;
}

/* One element: nextPromise = C.resolve(item); nextPromise.then(onF, onR). */
static int comb_step(PxVM *vm, PxValue c, PxValue resolve, PxValue stv, PxValue item) {
    PxVec  *st = (PxVec *)px_ptr(stv), *cap = (PxVec *)px_ptr(st->items[ST_CAP]), *list, *d;
    int     mode = px_smi(st->items[ST_MODE]), n = px_smi(st->items[ST_COUNT]);
    PxValue args[2], p, dv;
    args[0] = cap->items[1];
    args[1] = cap->items[2];
    PX_ROOT(vm, stv);
    p = px_call(vm, resolve, c, 1, &item);
    if (p == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, p);
    if (mode != COMB_RACE) {
        st   = (PxVec *)px_ptr(stv);
        list = px_vec_grow(vm, (PxVec *)px_ptr(st->items[ST_LIST]), (uint32_t)n + 1);
        if (!list) goto fail2;
        st                      = (PxVec *)px_ptr(stv);
        st->items[ST_LIST]      = px_from_ptr(list);
        list->items[n]          = PX_UNDEFINED;
        st->items[ST_COUNT]     = px_from_smi(n + 1);
        d                       = px_vec_new(vm, 3);
        if (!d) goto fail2;
        d->items[0] = stv;
        d->items[1] = px_from_smi(n);
        d->items[2] = PX_FALSE;
        dv          = px_from_ptr(d);
        PX_ROOT(vm, dv);
        PX_ROOT(vm, args[0]);
        PX_ROOT(vm, args[1]);
        if (mode != COMB_ANY) args[0] = closure(vm, comb_element, 1, 0, dv);
        if (args[0] != PX_EXCEPTION && mode != COMB_ALL) args[1] = closure(vm, comb_element, 1, 1, dv);
        px_pop_roots(vm, 3);
        if (args[0] == PX_EXCEPTION || args[1] == PX_EXCEPTION) goto fail2;
        st                      = (PxVec *)px_ptr(stv);
        st->items[ST_REMAINING] = px_from_smi(px_smi(st->items[ST_REMAINING]) + 1);
    }
    p = invoke(vm, p, vm->atom[PX_ATOM_then], 2, args);
    px_pop_roots(vm, 2);
    return p == PX_EXCEPTION ? -1 : 0;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    return -1;
}

static PxValue promise_combinator(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int      mode = vm->native_magic, r;
    PxValue  cap, resolve = PX_UNDEFINED, rec = PX_UNDEFINED, stv = PX_UNDEFINED, item;
    PxVec   *st;
    cap = new_capability(vm, t);
    if (cap != PX_EXCEPTION) cap = cap_with_functions(vm, cap);
    if (cap == PX_EXCEPTION) return cap;
    PX_ROOT(vm, cap);
    PX_ROOT(vm, resolve);
    PX_ROOT(vm, rec);
    PX_ROOT(vm, stv);
    /* GetPromiseResolve, then GetIterator: their errors reject */
    resolve = px_get(vm, t, px_intern_cstr(vm, "resolve"));
    if (resolve == PX_EXCEPTION) goto reject;
    if (!px_is_callable(resolve)) {
        px_throw_error(vm, PX_TYPE_ERROR, "the constructor's resolve is not a function");
        goto reject;
    }
    rec = px_iter_record(vm, ARG(0), 0);
    if (rec == PX_EXCEPTION) goto reject;
    st = px_vec_new(vm, 4); /* the values list, for now */
    if (!st) goto close;
    stv = px_from_ptr(st);
    st  = px_vec_new(vm, 5);
    if (!st) goto close;
    st->items[ST_LIST]      = stv;
    st->items[ST_COUNT]     = px_from_smi(0);
    st->items[ST_REMAINING] = px_from_smi(1); /* one extra, released after the loop */
    st->items[ST_CAP]       = cap;
    st->items[ST_MODE]      = px_from_smi(mode);
    stv                     = px_from_ptr(st);
    while ((r = px_record_step(vm, rec, &item)) > 0)
        if (comb_step(vm, t, resolve, stv, item) < 0) goto close;
    if (r < 0) goto reject; /* the iterator itself failed: it is not closed */
    st                      = (PxVec *)px_ptr(stv);
    st->items[ST_REMAINING] = px_from_smi(px_smi(st->items[ST_REMAINING]) - 1);
    if (mode != COMB_RACE && px_smi(st->items[ST_REMAINING]) == 0 && comb_finish(vm, st) < 0) goto reject;
    px_pop_roots(vm, 4);
    return cap_promise(cap);
close:
    px_iterator_close_throw(vm, ((PxVec *)px_ptr(rec))->items[0]);
reject:
    px_pop_roots(vm, 4);
    return cap_reject_pending(vm, cap);
}

static PxValue promise_with_resolvers(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue cap = new_capability(vm, t), o;
    PxVec  *c;
    (void)argc;
    (void)argv;
    if (cap != PX_EXCEPTION) cap = cap_with_functions(vm, cap);
    if (cap == PX_EXCEPTION) return cap;
    PX_ROOT(vm, cap);
    o = px_object_new(vm);
    c = (PxVec *)px_ptr(cap);
    if (o == PX_EXCEPTION || px_def_value(vm, o, "promise", c->items[0], PX_ATTR_DEFAULT) < 0 ||
        px_def_value(vm, o, "resolve", c->items[1], PX_ATTR_DEFAULT) < 0 ||
        px_def_value(vm, o, "reject", c->items[2], PX_ATTR_DEFAULT) < 0)
        o = PX_EXCEPTION;
    px_pop_roots(vm, 1);
    return o;
}

static PxValue queue_microtask(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (!px_is_callable(ARG(0))) return px_throw_error(vm, PX_TYPE_ERROR, "queueMicrotask: callback is not a function");
    return px_enqueue_job(vm, px_from_smi(JOB_CALL), argv[0], PX_UNDEFINED, PX_UNDEFINED) < 0 ? PX_EXCEPTION
                                                                                              : PX_UNDEFINED;
}

/* ============================================================ public API */

PxValue px_new_promise(PxVM *vm) { return px_promise_new(vm); }
int     px_resolve_promise(PxVM *vm, PxValue p, PxValue v) { return is_promise(p) ? px_promise_resolve(vm, p, v) : -1; }
int     px_reject_promise(PxVM *vm, PxValue p, PxValue v) { return is_promise(p) ? px_promise_reject(vm, p, v) : -1; }
int     px_promise_state(PxValue v) { return is_promise(v) ? ((PxPromise *)px_ptr(v))->state : -1; }
PxValue px_promise_result(PxValue v) { return is_promise(v) ? ((PxPromise *)px_ptr(v))->result : PX_UNDEFINED; }

int px_promise_init(PxVM *vm) {
    static const PxFnDef statics[] = {
        {"resolve", promise_resolve_static, 1, 0},  {"reject", promise_reject_static, 1, 0},
        {"all", promise_combinator, 1, COMB_ALL},   {"allSettled", promise_combinator, 1, COMB_ALL_SETTLED},
        {"any", promise_combinator, 1, COMB_ANY},   {"race", promise_combinator, 1, COMB_RACE},
        {"withResolvers", promise_with_resolvers, 0, 0},
    };
    static const PxFnDef methods[] = {
        {"then", promisep_then, 2, 0}, {"catch", promisep_catch, 1, 0}, {"finally", promisep_finally, 1, 0}};
    static const PxFnDef globals[] = {{"queueMicrotask", queue_microtask, 1, 0}};
    PxValue ctor = px_make_native(vm, promise_ctor, "Promise", 1, 0);
    if (ctor == PX_EXCEPTION) return -1;
    vm->ctors[PX_PROTO_PROMISE] = ctor;
    if (px_def_value(vm, vm->global, "Promise", ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_value(vm, ctor, "prototype", vm->protos[PX_PROTO_PROMISE], 0) < 0 ||
        px_define(vm, vm->protos[PX_PROTO_PROMISE], vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_fns(vm, ctor, statics, PX_COUNTOF(statics)) < 0 ||
        px_def_fns(vm, vm->protos[PX_PROTO_PROMISE], methods, PX_COUNTOF(methods)) < 0 ||
        px_def_fns(vm, vm->global, globals, PX_COUNTOF(globals)) < 0 || px_def_species(vm, ctor) < 0 ||
        px_def_tag(vm, vm->protos[PX_PROTO_PROMISE], "Promise") < 0)
        return -1;
    return 0;
}
