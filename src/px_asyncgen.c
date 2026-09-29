/* Async generators (async function*), yield* inside them, and for await.
 *
 * An async generator is a PxGen like a generator's, driven by a queue of
 * requests: each next()/return()/throw() call returns a promise and queues
 * (mode, value, promise). While the generator is idle the head request
 * resumes it; the body then either
 *   - awaits: the generator stays busy, and resumes when the promise
 *     settles (the continuation is a promise reaction, as for async
 *     functions);
 *   - yields: the request's promise is fulfilled with { value, done: false }
 *     (the compiler has already awaited the operand, as `yield` does in an
 *     async generator) and the next request, if any, resumes it;
 *   - reaches a yield*: the driver runs the delegation itself (g->deleg
 *     holds the inner iterator's record) and resumes the frame with its
 *     completion once the inner iterator is done;
 *   - returns or throws: the request settles with { value, done: true } or
 *     the exception, and requests after it see a finished generator.
 * The steps follow ECMA-262's AsyncGenerator* operations one to one, so
 * the number of promise jobs each takes (observable) is the spec's. The
 * driver is a loop over actions (run), so requests that complete at once
 * do not recurse on the C stack.
 *
 * for await (x of it) and yield* take it[Symbol.asyncIterator](), or wrap
 * a sync iterator in an %AsyncFromSyncIteratorPrototype% object, which
 * awaits each value. */

#include <string.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))

/* ------------------------------------------------------------ the queue */

static int enqueue(PxVM *vm, PxGen *g, int mode, PxValue v, PxValue p) {
    PxValue gv = px_from_ptr(g);
    if (!g->queue || 3u * (g->qlen + 1u) > g->queue->cap) {
        PxVec *n;
        PX_ROOT(vm, gv);
        PX_ROOT(vm, v);
        PX_ROOT(vm, p);
        n = px_vec_grow(vm, g->queue, 3u * (g->qlen + 1u) + 6u);
        px_pop_roots(vm, 3);
        if (!n) return -1;
        g->queue = n;
    }
    g->queue->items[3 * g->qlen]     = px_from_smi(mode);
    g->queue->items[3 * g->qlen + 1] = v;
    g->queue->items[3 * g->qlen + 2] = p;
    g->qlen++;
    return 0;
}

/* AsyncGeneratorCompleteStep: removes the head request and settles its
 * promise. A queue that grew for a burst of requests is let go once empty. */
static void complete_step(PxVM *vm, PxGen *g, int ok, PxValue v, int done) {
    PxValue  gv = px_from_ptr(g), p = g->queue->items[2];
    uint32_t i;
    for (i = 0; i + 3 < 3 * g->qlen; i++) g->queue->items[i] = g->queue->items[i + 3];
    for (; i < 3 * g->qlen; i++) g->queue->items[i] = PX_UNDEFINED;
    g->qlen--;
    if (g->qlen == 0 && g->queue->cap > 12) g->queue = NULL;
    PX_ROOT(vm, gv);
    PX_ROOT(vm, p);
    PX_ROOT(vm, v);
    if (ok) {
        PxValue r = px_iter_result(vm, v, done);
        if (r == PX_EXCEPTION) {
            r = vm->exception;
            vm->exception = PX_UNDEFINED;
            px_promise_reject(vm, p, r);
        } else {
            px_promise_resolve(vm, p, r);
        }
    } else {
        px_promise_reject(vm, p, v);
    }
    px_pop_roots(vm, 3);
}

/* ------------------------------------------------------------ running
 *
 * run() is a loop over actions: each one sets the next, or waits on a
 * promise whose reaction (on_settled) calls run() again. g->busy stands
 * for the spec's "executing" and "awaiting-return" states, in which
 * requests only queue up. */

enum {
    ACT_STOP = 0, /* waiting on a promise, or idle */
    ACT_FRAME,    /* resume the frame with (mode, v) */
    ACT_TARGET,   /* the same, or the yield* in progress */
    ACT_REQUEST,  /* the head request's completion, at a yield: a return's value is awaited first */
    ACT_YIELD,    /* hand v out (AsyncGeneratorYield) */
    ACT_DELEGATE, /* yield* v begins */
    ACT_RESULT,   /* yield*: v is the awaited inner result of a next/throw (mode NEXT) or a return (RETURN) */
    ACT_DRAIN     /* the generator is done: settle the queued requests */
};

/* What an await is for; its reactions' magic is phase * 2 + rejected. */
enum {
    AW_FRAME = 0, /* an await in the body */
    AW_UNWRAP,    /* return(v) at a yield: v, then returned (or thrown) there */
    AW_RESULT,    /* yield*: the inner next/throw result */
    AW_RRESULT,   /* yield*: the inner return result */
    AW_RVALUE,    /* yield*: the value the generator then returns */
    AW_CLOSE,     /* yield*: return() of an inner iterator that has no throw() */
    AW_RETURN     /* return(v) on a finished generator: v */
};

typedef struct Step {
    int     act, mode;
    PxValue v; /* rooted by run() */
} Step;

static void run(PxVM *vm, PxGen *g, Step *s);

static void take_exception(PxVM *vm, Step *s) {
    s->v          = vm->exception;
    vm->exception = PX_UNDEFINED;
}

/* A thrown completion for the frame; the yield*, if any, is over. */
static void throw_to_frame(PxVM *vm, PxGen *g, Step *s) {
    if (vm->uncatchable) {
        s->act = ACT_STOP; /* the host is stopping the script: leave it be */
        return;
    }
    g->deleg = PX_UNDEFINED;
    s->act   = ACT_FRAME;
    s->mode  = PX_RESUME_THROW;
    take_exception(vm, s);
}

/* What follows an await of `phase` that settled with x. */
static void after_await(PxVM *vm, PxGen *g, int phase, int rejected, PxValue x, Step *s) {
    s->v    = x;
    s->mode = rejected ? PX_RESUME_THROW : PX_RESUME_NEXT;
    s->act  = ACT_FRAME;
    switch (phase) {
    case AW_UNWRAP:
        s->act = ACT_TARGET;
        if (!rejected) s->mode = PX_RESUME_RETURN;
        break;
    case AW_RESULT:
    case AW_RRESULT:
        if (rejected) g->deleg = PX_UNDEFINED;
        else {
            s->act  = ACT_RESULT;
            s->mode = phase == AW_RRESULT ? PX_RESUME_RETURN : PX_RESUME_NEXT;
        }
        break;
    case AW_RVALUE:
        g->deleg = PX_UNDEFINED;
        if (!rejected) s->mode = PX_RESUME_RETURN;
        break;
    case AW_CLOSE:
        /* the inner iterator is closed; its lack of throw() is a TypeError */
        g->deleg = PX_UNDEFINED;
        if (!rejected) {
            px_throw_error(vm, PX_TYPE_ERROR, px_is_obj(x) ? "the iterator has no throw method"
                                                           : "iterator return() result is not an object");
            throw_to_frame(vm, g, s);
        }
        break;
    case AW_RETURN:
        complete_step(vm, g, !rejected, x, 1);
        s->act = ACT_DRAIN;
        break;
    }
}

static PxValue on_settled(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxGen *g = (PxGen *)px_ptr(vm->native_data);
    Step   s;
    (void)t;
    after_await(vm, g, vm->native_magic >> 1, vm->native_magic & 1, ARG(0), &s);
    run(vm, g, &s);
    return vm->uncatchable ? PX_EXCEPTION : PX_UNDEFINED;
}

/* Await(v) for `phase`: s->act becomes ACT_STOP with the reactions in
 * place. If Await itself throws (PromiseResolve reads a promise's
 * `constructor`), that is its rejection. */
static void await_value(PxVM *vm, PxGen *g, PxValue v, int phase, Step *s) {
    PxValue gv = px_from_ptr(g), p, onf = PX_UNDEFINED, onr;
    PX_ROOT(vm, gv);
    p = px_promise_resolved(vm, v);
    if (p == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, p);
    PX_ROOT(vm, onf);
    onf = px_make_native_data(vm, on_settled, "", 1, gv);
    if (onf == PX_EXCEPTION) goto fail3;
    ((PxNative *)px_ptr(onf))->magic = (int16_t)(phase * 2);
    onr = px_make_native_data(vm, on_settled, "", 1, gv);
    if (onr == PX_EXCEPTION) goto fail3;
    ((PxNative *)px_ptr(onr))->magic = (int16_t)(phase * 2 + 1);
    if (px_promise_then_native(vm, p, onf, onr) < 0) goto fail3;
    px_pop_roots(vm, 3);
    s->act = ACT_STOP;
    return;
fail3:
    px_pop_roots(vm, 2);
fail:
    px_pop_roots(vm, 1);
    if (vm->uncatchable) {
        s->act = ACT_STOP;
        return;
    }
    {
        PxValue e     = vm->exception;
        vm->exception = PX_UNDEFINED;
        after_await(vm, g, phase, 1, e, s);
    }
}

/* Resumes the frame with (s->mode, s->v). */
static void frame_step(PxVM *vm, PxGen *g, Step *s) {
    int     done;
    PxValue r = px_gen_resume(vm, g, s->v, s->mode, &done);
    if (r == PX_EXCEPTION) {
        if (vm->uncatchable) {
            s->act = ACT_STOP;
            return;
        }
        /* thrown out of the body (or, stack exhausted, before it could run) */
        g->state = PX_GEN_DONE;
        g->saved = NULL;
        take_exception(vm, s);
        complete_step(vm, g, 0, s->v, 1);
        s->act = ACT_DRAIN;
        return;
    }
    s->v = r;
    if (done) {
        complete_step(vm, g, 1, r, 1);
        s->act = ACT_DRAIN;
    } else if (vm->suspend_await == PX_SUSPEND_AWAIT) {
        await_value(vm, g, r, AW_FRAME, s);
    } else {
        s->act = vm->suspend_await == PX_SUSPEND_DELEGATE ? ACT_DELEGATE : ACT_YIELD;
    }
}

/* yield*: the inner iterator's next/throw/return with the completion
 * received, (s->mode, s->v). */
static void deleg_call(PxVM *vm, PxGen *g, Step *s) {
    PxValue rec = g->deleg, iter, m, r;
    int     is_return = s->mode == PX_RESUME_RETURN;
    PX_ROOT(vm, rec);
    iter = ((PxVec *)px_ptr(rec))->items[0];
    if (s->mode == PX_RESUME_NEXT) {
        m = ((PxVec *)px_ptr(rec))->items[1];
    } else {
        m = px_get_method(vm, iter, vm->atom[is_return ? PX_ATOM_return : PX_ATOM_throw]);
        if (m == PX_EXCEPTION) goto thrown;
        if (m == PX_UNDEFINED && is_return) {
            await_value(vm, g, s->v, AW_RVALUE, s);
            px_pop_roots(vm, 1);
            return;
        }
        if (m == PX_UNDEFINED) {
            /* no throw(): AsyncIteratorClose, then a TypeError */
            m = px_get_method(vm, iter, vm->atom[PX_ATOM_return]);
            if (m == PX_EXCEPTION) goto thrown;
            if (m == PX_UNDEFINED) {
                px_throw_error(vm, PX_TYPE_ERROR, "the iterator has no throw method");
                goto thrown;
            }
            r = px_call(vm, m, iter, 0, NULL);
            if (r == PX_EXCEPTION) goto thrown;
            await_value(vm, g, r, AW_CLOSE, s);
            px_pop_roots(vm, 1);
            return;
        }
    }
    r = px_call(vm, m, iter, 1, &s->v);
    if (r == PX_EXCEPTION) goto thrown;
    await_value(vm, g, r, is_return ? AW_RRESULT : AW_RESULT, s);
    px_pop_roots(vm, 1);
    return;
thrown:
    px_pop_roots(vm, 1);
    throw_to_frame(vm, g, s);
}

/* yield*: the awaited inner result s->v. Not done: its value is yielded
 * (not awaited). Done: the yield* evaluates to its value, or, after a
 * return(), the generator returns it (awaited). */
static void deleg_result(PxVM *vm, PxGen *g, Step *s) {
    PxValue d, v;
    if (!px_is_obj(s->v)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator result is not an object");
        throw_to_frame(vm, g, s);
        return;
    }
    d = px_get(vm, s->v, vm->atom[PX_ATOM_done]);
    if (d == PX_EXCEPTION) {
        throw_to_frame(vm, g, s);
        return;
    }
    d = px_bool(px_truthy(d));
    v = px_get(vm, s->v, vm->atom[PX_ATOM_value]);
    if (v == PX_EXCEPTION) {
        throw_to_frame(vm, g, s);
        return;
    }
    s->v = v;
    if (d == PX_FALSE) {
        s->act = ACT_YIELD;
    } else if (s->mode != PX_RESUME_RETURN) {
        g->deleg = PX_UNDEFINED;
        s->act   = ACT_FRAME;
    } else {
        await_value(vm, g, v, AW_RVALUE, s);
    }
}

/* AsyncGeneratorDrainQueue: the generator is done; requests settle in
 * order, a return() waiting for its value to be awaited. */
static void drain(PxVM *vm, PxGen *g, Step *s) {
    s->act  = ACT_STOP;
    g->busy = 0;
    while (g->qlen > 0 && !g->busy && !vm->uncatchable) {
        int mode = px_smi(g->queue->items[0]);
        if (mode == PX_RESUME_RETURN) {
            g->busy = 1; /* awaiting-return */
            await_value(vm, g, g->queue->items[1], AW_RETURN, s);
            return;
        }
        complete_step(vm, g, mode == PX_RESUME_NEXT, mode == PX_RESUME_NEXT ? PX_UNDEFINED : g->queue->items[1], 1);
    }
    if (g->qlen == 0) g->queue = NULL;
}

static void run(PxVM *vm, PxGen *g, Step *s) {
    PxValue gv = px_from_ptr(g);
    PX_ROOT(vm, gv);
    PX_ROOT(vm, s->v);
    while (s->act != ACT_STOP && !vm->uncatchable) {
        switch (s->act) {
        case ACT_REQUEST:
            if (s->mode == PX_RESUME_RETURN) await_value(vm, g, s->v, AW_UNWRAP, s);
            else s->act = ACT_TARGET;
            break;
        case ACT_TARGET:
            if (g->deleg != PX_UNDEFINED) deleg_call(vm, g, s);
            else s->act = ACT_FRAME;
            break;
        case ACT_FRAME: frame_step(vm, g, s); break;
        case ACT_YIELD:
            complete_step(vm, g, 1, s->v, 0);
            if (g->qlen == 0) {
                g->busy = 0; /* suspended at the yield, or in the yield* */
                s->act  = ACT_STOP;
            } else {
                /* the next request resumes it at once */
                s->act  = ACT_REQUEST;
                s->mode = px_smi(g->queue->items[0]);
                s->v    = g->queue->items[1];
            }
            break;
        case ACT_DELEGATE: {
            PxValue rec = px_iter_record(vm, s->v, 1);
            if (rec == PX_EXCEPTION) {
                throw_to_frame(vm, g, s);
                break;
            }
            g->deleg = rec;
            s->act   = ACT_TARGET;
            s->mode  = PX_RESUME_NEXT;
            s->v     = PX_UNDEFINED;
            break;
        }
        case ACT_RESULT: deleg_result(vm, g, s); break;
        default: drain(vm, g, s); break;
        }
    }
    px_pop_roots(vm, 2);
}

/* next (0) / throw (1) / return (2): AsyncGenerator.prototype.* */
static PxValue agen_method(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     mode = vm->native_magic;
    PxValue p    = px_promise_new(vm), v = ARG(0);
    PxGen  *g;
    Step    s;
    if (p == PX_EXCEPTION) return p;
    PX_ROOT(vm, p);
    PX_ROOT(vm, t);
    if (!px_is_obj(t) || px_type_of(t) != PX_T_GENERATOR || ((PxGen *)px_ptr(t))->is_async != 2) {
        px_throw_error(vm, PX_TYPE_ERROR, "not an async generator");
        goto reject;
    }
    g = (PxGen *)px_ptr(t);
    if (!g->busy && g->state == PX_GEN_START && mode == PX_RESUME_THROW) {
        g->state = PX_GEN_DONE;
        g->saved = NULL;
    }
    if (!g->busy && g->state == PX_GEN_DONE && mode != PX_RESUME_RETURN) {
        /* completed: settled at once, without queueing */
        if (mode == PX_RESUME_THROW) {
            px_promise_reject(vm, p, v);
        } else {
            PxValue r = px_iter_result(vm, PX_UNDEFINED, 1);
            if (r == PX_EXCEPTION) goto reject;
            px_promise_resolve(vm, p, r);
        }
        goto out;
    }
    if (enqueue(vm, g, mode, v, p) < 0) goto reject;
    if (g->busy) goto out; /* it gets to this request in turn */
    g->busy = 1;
    s.mode  = mode;
    s.v     = v;
    if (g->state == PX_GEN_SUSPENDED) {
        s.act = ACT_REQUEST;
    } else if (mode == PX_RESUME_RETURN) {
        /* not started, or completed: the value is awaited, then returned */
        g->state = PX_GEN_DONE;
        g->saved = NULL;
        s.act    = ACT_DRAIN;
    } else {
        s.act = ACT_FRAME; /* the first next() starts the body */
    }
    run(vm, g, &s);
out:
    px_pop_roots(vm, 2);
    return vm->uncatchable ? PX_EXCEPTION : p;
reject:
    if (!vm->uncatchable) {
        PxValue e     = vm->exception;
        vm->exception = PX_UNDEFINED;
        px_promise_reject(vm, p, e);
    }
    goto out;
}

static PxValue return_this(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return t;
}

/* ------------------------------------------------------------ AsyncFromSyncIterator
 *
 * The object for-await and yield* use for a sync iterable: an iterator
 * object (PX_IT_ASYNC_FROM_SYNC) whose target is the sync iterator's
 * record, [iterator, next]. User code never sees it; its methods are the
 * spec's %AsyncFromSyncIteratorPrototype% ones. */

/* the continuation's onFulfilled: { value: v, done } (done: magic) */
static PxValue afs_unwrap(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    return px_iter_result(vm, ARG(0), vm->native_magic);
}

/* ...and onRejected for next/throw: the sync iterator is closed, and the
 * rejection passes on (data: the sync iterator) */
static PxValue afs_close(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    px_throw(vm, ARG(0));
    px_iterator_close_throw(vm, vm->native_data);
    return PX_EXCEPTION;
}

/* AsyncFromSyncIteratorContinuation: p settles with the sync result r
 * once its value is awaited. */
static void afs_continue(PxVM *vm, PxValue r, PxValue p, PxValue iter, int close_on_rejection) {
    PxValue d, v, vw, onf, onr = PX_UNDEFINED;
    PX_ROOT(vm, r);
    PX_ROOT(vm, p);
    PX_ROOT(vm, iter);
    PX_ROOT(vm, onr);
    d = px_get(vm, r, vm->atom[PX_ATOM_done]);
    if (d == PX_EXCEPTION) goto fail;
    d = px_bool(px_truthy(d));
    v = px_get(vm, r, vm->atom[PX_ATOM_value]);
    if (v == PX_EXCEPTION) goto fail;
    vw = px_promise_resolved(vm, v);
    if (vw == PX_EXCEPTION) {
        if (d == PX_FALSE && close_on_rejection) px_iterator_close_throw(vm, iter);
        goto fail;
    }
    PX_ROOT(vm, vw);
    onf = px_make_native_data(vm, afs_unwrap, "", 1, PX_UNDEFINED);
    if (onf == PX_EXCEPTION) goto fail2;
    ((PxNative *)px_ptr(onf))->magic = d == PX_TRUE;
    PX_ROOT(vm, onf);
    if (d == PX_FALSE && close_on_rejection) {
        onr = px_make_native_data(vm, afs_close, "", 1, iter);
        if (onr == PX_EXCEPTION) goto fail3;
    }
    if (px_promise_then_derived(vm, vw, onf, onr, p) < 0) goto fail3;
    px_pop_roots(vm, 6);
    return;
fail3:
    px_pop_roots(vm, 1);
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 4);
    if (!vm->uncatchable) {
        PxValue e     = vm->exception;
        vm->exception = PX_UNDEFINED;
        px_promise_reject(vm, p, e);
    }
}

/* next (0) / throw (1) / return (2) */
static PxValue afs_method(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int     which = vm->native_magic;
    PxValue p     = px_promise_new(vm), iter, m, r;
    PxVec  *rec;
    if (p == PX_EXCEPTION) return p;
    PX_ROOT(vm, p);
    if (!px_is_obj(t) || px_type_of(t) != PX_T_ITEROBJ || ((PxIterObj *)px_ptr(t))->kind != PX_IT_ASYNC_FROM_SYNC) {
        px_throw_error(vm, PX_TYPE_ERROR, "not an async-from-sync iterator");
        goto reject;
    }
    rec  = (PxVec *)px_ptr(((PxIterObj *)px_ptr(t))->target);
    iter = rec->items[0];
    PX_ROOT(vm, iter);
    if (which == PX_RESUME_NEXT) {
        m = rec->items[1];
    } else {
        m = px_get_method(vm, iter, vm->atom[which == PX_RESUME_RETURN ? PX_ATOM_return : PX_ATOM_throw]);
        if (m == PX_EXCEPTION) goto reject2;
        if (m == PX_UNDEFINED) {
            if (which == PX_RESUME_RETURN) {
                r = px_iter_result(vm, ARG(0), 1);
                if (r == PX_EXCEPTION) goto reject2;
                px_promise_resolve(vm, p, r);
                px_pop_roots(vm, 2);
                return p;
            }
            /* no throw(): the sync iterator is closed, and that is a protocol error */
            if (px_iterator_close(vm, iter) == 0) px_throw_error(vm, PX_TYPE_ERROR, "the iterator has no throw method");
            goto reject2;
        }
    }
    r = argc > 0 ? px_call(vm, m, iter, 1, argv) : px_call(vm, m, iter, 0, NULL);
    if (r == PX_EXCEPTION) goto reject2;
    if (!px_is_obj(r)) {
        px_throw_error(vm, PX_TYPE_ERROR, "iterator result is not an object");
        goto reject2;
    }
    afs_continue(vm, r, p, iter, which != PX_RESUME_RETURN);
    px_pop_roots(vm, 2);
    return vm->uncatchable ? PX_EXCEPTION : p;
reject2:
    px_pop_roots(vm, 1);
reject:
    if (!vm->uncatchable) {
        PxValue e     = vm->exception;
        vm->exception = PX_UNDEFINED;
        px_promise_reject(vm, p, e);
    }
    px_pop_roots(vm, 1);
    return vm->uncatchable ? PX_EXCEPTION : p;
}

PxValue px_get_async_iterator(PxVM *vm, PxValue iterable) {
    PxValue m, rec;
    if (iterable == PX_UNDEFINED || iterable == PX_NULL)
        return px_throw_error(vm, PX_TYPE_ERROR, "%s is not async iterable", iterable == PX_NULL ? "null" : "undefined");
    PX_ROOT(vm, iterable);
    m = px_get(vm, iterable, vm->sym_async_iterator);
    if (m == PX_EXCEPTION) goto fail;
    if (m != PX_UNDEFINED && m != PX_NULL) {
        PxValue it;
        if (!px_is_callable(m)) {
            px_pop_roots(vm, 1);
            return px_throw_error(vm, PX_TYPE_ERROR, "[Symbol.asyncIterator] is not a function");
        }
        it = px_call(vm, m, iterable, 0, NULL);
        px_pop_roots(vm, 1);
        if (it != PX_EXCEPTION && !px_is_obj(it)) return px_throw_error(vm, PX_TYPE_ERROR, "async iterator is not an object");
        return it;
    }
    /* a sync iterable: CreateAsyncFromSyncIterator */
    rec = px_iter_record(vm, iterable, 0);
    px_pop_roots(vm, 1);
    return rec == PX_EXCEPTION ? rec : px_make_iterobj(vm, rec, PX_IT_ASYNC_FROM_SYNC);
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* Leaving a for-await loop early: it.return(), whose result is awaited
 * next (undefined when there is no return method). */
PxValue px_async_iter_close(PxVM *vm, PxValue it) {
    PxValue m;
    PX_ROOT(vm, it);
    m = px_get_method(vm, it, vm->atom[PX_ATOM_return]);
    px_pop_roots(vm, 1);
    if (m == PX_EXCEPTION || m == PX_UNDEFINED) return m;
    return px_call(vm, m, it, 0, NULL);
}

int px_asyncgen_init(PxVM *vm) {
    static const PxFnDef fns[]     = {{"next", agen_method, 1, PX_RESUME_NEXT},
                                      {"return", agen_method, 1, PX_RESUME_RETURN},
                                      {"throw", agen_method, 1, PX_RESUME_THROW}};
    static const PxFnDef afs_fns[] = {{"next", afs_method, 1, PX_RESUME_NEXT},
                                      {"return", afs_method, 1, PX_RESUME_RETURN},
                                      {"throw", afs_method, 1, PX_RESUME_THROW}};
    PxValue proto = vm->protos[PX_PROTO_ASYNCGENOBJ], ai = vm->protos[PX_PROTO_ASYNC_ITERATOR], f;
    /* %AsyncIteratorPrototype%: [Symbol.asyncIterator]() { return this } */
    f = px_make_native(vm, return_this, "[Symbol.asyncIterator]", 0, 0);
    if (f == PX_EXCEPTION || px_define(vm, ai, vm->sym_async_iterator, f, PX_ATTR_HIDDEN) < 0 ||
        px_set_proto(vm, proto, ai) < 0 || px_def_fns(vm, proto, fns, PX_COUNTOF(fns)) < 0 ||
        px_def_tag(vm, proto, "AsyncGenerator") < 0 || px_set_proto(vm, vm->protos[PX_PROTO_ASYNC_FROM_SYNC], ai) < 0 ||
        px_def_fns(vm, vm->protos[PX_PROTO_ASYNC_FROM_SYNC], afs_fns, PX_COUNTOF(afs_fns)) < 0)
        return -1;
    return 0;
}
