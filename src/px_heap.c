/* The PXJS heap: one fixed arena, segregated free lists, mark-sweep.
 *
 * Why this shape, on a PSP-1000:
 *   - One arena, allocated once: JS can never take memory from the rest of
 *     the app, and running out is an ordinary JS exception, not a crash.
 *   - No reference counts: nothing is written on every copy of a value,
 *     which on a 333 MHz in-order core is a real share of the time.
 *   - Non-moving: a C pointer to a live cell is valid for as long as the
 *     cell lives, so native code only has to keep things alive, never
 *     update pointers.
 *   - Marking uses an explicit stack, never C recursion: the PSP's thread
 *     stacks are small, and a long linked list must not overflow one.
 *     When the mark stack itself fills up, marked cells are rescanned from
 *     the heap instead (slower, bounded, correct). */

#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PX_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define PX_ASAN 1
#endif
#ifdef PX_ASAN
#include <sanitizer/asan_interface.h>
#define POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define POISON(p, n)   ((void)0)
#define UNPOISON(p, n) ((void)0)
#endif

#define GC_MIN_THRESHOLD (256u * 1024u)
#define MARK_STACK_CAP   4096u

typedef struct FreeCell {
    uint32_t         hdr;
    struct FreeCell *next;
} FreeCell;

int px_heap_init(PxVM *vm, size_t bytes) {
    bytes &= ~(size_t)7;
    if (bytes < 64 * 1024) bytes = 64 * 1024;
    vm->arena = (uint8_t *)malloc(bytes);
    if (!vm->arena) return -1;
    /* malloc gives 8-byte alignment on every target PXJS runs on; checked
     * rather than assumed, since the value encoding depends on it. */
    if (((uintptr_t)vm->arena & 7u) != 0) {
        free(vm->arena);
        vm->arena = NULL;
        return -1;
    }
    vm->arena_end    = vm->arena + bytes;
    vm->bump         = vm->arena;
    vm->gc_threshold = GC_MIN_THRESHOLD;
    vm->shape_epoch  = 1;
    vm->kept         = PX_UNDEFINED;
    vm->mark_cap     = MARK_STACK_CAP;
    vm->mark_stack   = (PxValue *)malloc(sizeof(PxValue) * vm->mark_cap);
    if (!vm->mark_stack) {
        free(vm->arena);
        vm->arena = NULL;
        return -1;
    }
    POISON(vm->arena, bytes);
    return 0;
}

typedef struct PxFinalizer {
    PxValue obj; /* weak: not marked by the collector */
    void (*fn)(void *opaque);
    void *opaque;
} PxFinalizer;

int px_set_finalizer(PxVM *vm, PxValue obj, void (*fn)(void *opaque), void *opaque) {
    if (!px_is_obj(obj) || !fn) return -1;
    if (vm->nfinal == vm->final_cap) {
        uint32_t     cap = vm->final_cap ? vm->final_cap * 2 : 16;
        PxFinalizer *f   = (PxFinalizer *)realloc(vm->finalizers, cap * sizeof *f);
        if (!f) return -1;
        vm->finalizers = f;
        vm->final_cap  = cap;
    }
    vm->finalizers[vm->nfinal].obj    = obj;
    vm->finalizers[vm->nfinal].fn     = fn;
    vm->finalizers[vm->nfinal].opaque = opaque;
    vm->nfinal++;
    return 0;
}

/* After marking, before sweeping: an unmarked object's finalizer runs now
 * (it frees native memory; it must not call into the VM). */
static void run_finalizers(PxVM *vm) {
    uint32_t i = 0;
    while (i < vm->nfinal) {
        PxFinalizer *f = &vm->finalizers[i];
        if (((PxCell *)px_ptr(f->obj))->hdr & PX_HDR_MARK) {
            i++;
            continue;
        }
        f->fn(f->opaque);
        vm->finalizers[i] = vm->finalizers[--vm->nfinal];
    }
}

void px_heap_free(PxVM *vm) {
    uint8_t *p;
    uint32_t i;
    /* everything dies with the VM: its native resources go too */
    for (i = 0; i < vm->nfinal; i++) vm->finalizers[i].fn(vm->finalizers[i].opaque);
    free(vm->finalizers);
    vm->finalizers = NULL;
    vm->nfinal = vm->final_cap = 0;
    if (vm->arena) UNPOISON(vm->arena, (size_t)(vm->arena_end - vm->arena));
    for (p = vm->arena; p && p < vm->bump; p += px_cell_bytes(p))
        if (px_hdr_type(((PxCell *)p)->hdr) == PX_T_SHAPE) px_shape_table_free((PxShape *)p);
    free(vm->arena);
    free(vm->mark_stack);
    free(vm->weak_maps);
    vm->weak_maps  = NULL;
    vm->arena      = NULL;
    vm->mark_stack = NULL;
}

size_t px_cell_bytes(const void *cell) { return (size_t)px_hdr_units(((const PxCell *)cell)->hdr) * 8u; }

/* ------------------------------------------------------------ free lists */

static void free_push(PxVM *vm, uint8_t *p, size_t bytes) {
    FreeCell *c = (FreeCell *)p;
    size_t    units = bytes / 8u;

    UNPOISON(p, 8);
    c->hdr = PX_HDR(PX_T_FREE, units);
    if (units <= PX_SIZE_CLASSES) {
        c->next                     = (FreeCell *)vm->free_small[units - 1];
        vm->free_small[units - 1]   = (PxCell *)c;
    } else {
        c->next        = (FreeCell *)vm->free_large;
        vm->free_large = (PxCell *)c;
    }
    POISON(p + 8, bytes - 8);
}

static uint8_t *take_from_lists(PxVM *vm, size_t bytes) {
    size_t    units = bytes / 8u;
    FreeCell *c, **link;

    if (units <= PX_SIZE_CLASSES && vm->free_small[units - 1]) {
        c                         = (FreeCell *)vm->free_small[units - 1];
        vm->free_small[units - 1] = (PxCell *)c->next;
        return (uint8_t *)c;
    }
    if (vm->bump + bytes <= vm->arena_end) {
        uint8_t *p = vm->bump;
        vm->bump += bytes;
        return p;
    }
    /* First fit in the large list, splitting off the remainder. */
    for (link = (FreeCell **)&vm->free_large; *link; link = &(*link)->next) {
        size_t have;
        c    = *link;
        have = px_cell_bytes(c);
        if (have < bytes) continue;
        *link = c->next;
        if (have > bytes) free_push(vm, (uint8_t *)c + bytes, have - bytes);
        return (uint8_t *)c;
    }
    /* A small request may still fit in a bigger small cell. */
    if (units < PX_SIZE_CLASSES) {
        size_t k;
        for (k = units; k < PX_SIZE_CLASSES; k++) {
            if (!vm->free_small[k]) continue;
            c                  = (FreeCell *)vm->free_small[k];
            vm->free_small[k]  = (PxCell *)c->next;
            free_push(vm, (uint8_t *)c + bytes, (k + 1) * 8u - bytes);
            return (uint8_t *)c;
        }
    }
    return NULL;
}

void *px_alloc(PxVM *vm, PxType type, size_t bytes) {
    uint8_t *p;

    bytes = (bytes + 7u) & ~(size_t)7u;
    if (bytes < 8) bytes = 8;
    if (bytes > (size_t)(vm->arena_end - vm->arena) || (bytes >> 3) > 0xFFFFFFu) {
        px_throw_oom(vm);
        return NULL;
    }
    if (vm->fail_alloc_at && !vm->in_gc && ++vm->alloc_count == vm->fail_alloc_at) {
        px_throw_oom(vm); /* injected (px_set_alloc_failure) */
        return NULL;
    }
    if (!vm->in_gc && (vm->gc_stress || vm->bytes_since_gc >= vm->gc_threshold)) px_collect(vm);
    p = take_from_lists(vm, bytes);
    if (!p && !vm->in_gc) {
        px_collect(vm);
        p = take_from_lists(vm, bytes);
    }
    if (!p) {
        px_throw_oom(vm);
        return NULL;
    }
    UNPOISON(p, bytes);
    memset(p, 0, bytes);
    ((PxCell *)p)->hdr = PX_HDR(type, bytes / 8u);
    vm->bytes_since_gc += bytes;
    return p;
}

/* Numbers are the most frequent allocation in arithmetic-heavy code (every
 * double, every int32 outside the SMI range), so they get a path that
 * skips the general one's checks: a pop from the 16-byte free list or a
 * bump. Anything unusual (GC due, list and bump empty, stress mode) falls
 * back to px_alloc. */
PxValue px_box_number(PxVM *vm, double d) {
    PxNumber *n;
    if (!vm->gc_stress && !vm->fail_alloc_at && vm->bytes_since_gc < vm->gc_threshold) {
        FreeCell *c = (FreeCell *)vm->free_small[sizeof(PxNumber) / 8u - 1];
        if (c) {
            UNPOISON(c, sizeof(PxNumber));
            vm->free_small[sizeof(PxNumber) / 8u - 1] = (PxCell *)c->next;
            n                                          = (PxNumber *)c;
            goto init;
        }
        if (vm->bump + sizeof(PxNumber) <= vm->arena_end) {
            n = (PxNumber *)vm->bump;
            vm->bump += sizeof(PxNumber);
            UNPOISON(n, sizeof(PxNumber));
            goto init;
        }
    }
    n = (PxNumber *)px_alloc(vm, PX_T_NUMBER, sizeof(PxNumber));
    if (!n) return PX_EXCEPTION;
    n->d = d;
    return px_from_ptr(n);
init:
    n->hdr = PX_HDR(PX_T_NUMBER, sizeof(PxNumber) / 8u);
    n->pad = 0;
    n->d   = d;
    vm->bytes_since_gc += sizeof(PxNumber);
    return px_from_ptr(n);
}

PxVec *px_vec_new(PxVM *vm, uint32_t cap) {
    PxVec   *v;
    uint32_t i;
    if (cap > (0x7FFFFFFFu - sizeof(PxVec)) / sizeof(PxValue)) {
        px_throw_oom(vm);
        return NULL;
    }
    v = (PxVec *)px_alloc(vm, PX_T_VEC, sizeof(PxVec) + (size_t)cap * sizeof(PxValue));
    if (!v) return NULL;
    v->cap = cap;
    for (i = 0; i < cap; i++) v->items[i] = PX_UNDEFINED;
    return v;
}

PxVec *px_vec_grow(PxVM *vm, PxVec *v, uint32_t need) {
    PxVec   *n;
    uint32_t cap;
    if (v && v->cap >= need) return v;
    cap = v ? v->cap : 0;
    cap = cap < 4 ? 4 : cap + cap / 2;
    if (cap < need) cap = need;
    /* v stays alive through its owner, which the caller keeps rooted. */
    n = px_vec_new(vm, cap);
    if (!n) return NULL;
    if (v) memcpy(n->items, v->items, v->cap * sizeof(PxValue));
    return n;
}

PxBytes *px_bytes_new(PxVM *vm, const void *data, uint32_t len) {
    PxBytes *b = (PxBytes *)px_alloc(vm, PX_T_BYTES, sizeof(PxBytes) + len);
    if (!b) return NULL;
    b->len = len;
    if (data && len) memcpy(b->data, data, len);
    return b;
}

/* ------------------------------------------------------------ roots */

void px_push_root(PxVM *vm, PxValue *slot) {
    /* Overflow is a PXJS bug (unbalanced push/pop), not an app error. */
    if (vm->nroots >= PX_MAX_ROOTS) {
        fprintf(stderr, "pxjs: root stack overflow\n");
        abort();
    }
    vm->roots[vm->nroots++] = slot;
}

void px_pop_roots(PxVM *vm, int n) {
    if ((uint32_t)n > vm->nroots) {
        fprintf(stderr, "pxjs: root stack underflow\n");
        abort();
    }
    vm->nroots -= (uint32_t)n;
}

/* ------------------------------------------------------------ marking */

void px_mark_value(PxVM *vm, PxValue v) {
    PxCell *c;
    if (!px_is_ptr(v)) return;
    c = (PxCell *)px_ptr(v);
    if (c->hdr & PX_HDR_MARK) return;
    c->hdr |= PX_HDR_MARK;
    if (vm->mark_top < vm->mark_cap)
        vm->mark_stack[vm->mark_top++] = v;
    else
        vm->mark_overflow = 1;
}

/* ------------------------------------------------------------ weak maps
 *
 * WeakMap, WeakSet and WeakRef hold their keys weakly, with ephemeron
 * semantics: an entry's value is kept only while its key is reachable
 * from elsewhere. Marking does not trace their entries; it lists the maps
 * (remember_weak), and weak_process then marks values whose keys turned
 * out live -- repeatedly, since a value can make another key live -- and
 * finally removes entries whose keys are dead. If the list cannot grow
 * (malloc failed), the map is traced strongly: correct, only not weak for
 * that collection. */

static int remember_weak(PxVM *vm, PxMap *m) {
    if (vm->nweak == vm->weak_cap) {
        uint32_t ncap = vm->weak_cap ? vm->weak_cap * 2 : 16;
        PxMap  **n    = (PxMap **)realloc(vm->weak_maps, ncap * sizeof(PxMap *));
        if (!n) return 0;
        vm->weak_maps = n;
        vm->weak_cap  = ncap;
    }
    vm->weak_maps[vm->nweak++] = m;
    return 1;
}

static int is_marked(PxValue v) { return !px_is_ptr(v) || (((PxCell *)px_ptr(v))->hdr & PX_HDR_MARK); }

/* A registered target died: one job per held value, calling the cleanup
 * callback with it. Both are marked already (the registry is live, and the
 * held values were kept above). The job queue is malloc'd, so this does
 * not allocate from the heap being collected. */
static void queue_cleanup(PxVM *vm, PxMap *m, PxValue list) {
    PxArray *a = (PxArray *)px_ptr(list);
    uint32_t j;
    for (j = 0; a->elems && j + 1 < a->length && j + 1 < a->elems->cap; j += 2)
        if (px_enqueue_job(vm, px_from_smi(0) /* a reaction: callback(held) */, m->extra, a->elems->items[j],
                           PX_UNDEFINED) < 0)
            vm->exception = PX_UNDEFINED; /* out of memory: that callback is lost */
}

static void drain(PxVM *vm);

static void weak_process(PxVM *vm) {
    uint32_t k, i;
    int      changed = 1;
    while (changed) {
        changed = 0;
        for (k = 0; k < vm->nweak; k++) { /* nweak may grow while draining */
            PxMap *m = vm->weak_maps[k];
            for (i = 0; m->entries && i < m->used; i++) {
                PxValue key = m->entries->items[2 * i], val = m->entries->items[2 * i + 1];
                if (key == PX_HOLE || is_marked(val)) continue;
                /* a FinalizationRegistry keeps what it will hand to the
                 * cleanup callback even when (especially when) the target
                 * dies */
                if (!is_marked(key) && m->kind != PX_MAP_FINREG) continue;
                px_mark_value(vm, val);
                changed = 1;
            }
            drain(vm);
        }
    }
    for (k = 0; k < vm->nweak; k++) {
        PxMap *m = vm->weak_maps[k];
        if (!m->entries || (m->entries->hdr & PX_HDR_MARK)) continue; /* listed twice */
        for (i = 0; i < m->used; i++) {
            PxValue key = m->entries->items[2 * i];
            if (key == PX_HOLE || is_marked(key)) continue;
            if (m->kind == PX_MAP_FINREG) queue_cleanup(vm, m, m->entries->items[2 * i + 1]);
            m->entries->items[2 * i]     = PX_HOLE; /* as delete does: the index keeps pointing here */
            m->entries->items[2 * i + 1] = PX_UNDEFINED;
            m->count--;
        }
        m->entries->hdr |= PX_HDR_MARK; /* kept, and its live contents are marked already */
    }
    vm->nweak = 0;
}

static void trace(PxVM *vm, PxCell *c) {
    uint32_t i;
    switch (px_hdr_type(c->hdr)) {
    case PX_T_ROPE: {
        PxRope *r = (PxRope *)c;
        px_mark_value(vm, r->left);
        px_mark_value(vm, r->right);
        break;
    }
    case PX_T_SYMBOL: px_mark_value(vm, ((PxSymbol *)c)->description); break;
    case PX_T_SHAPE: {
        PxShape *s = (PxShape *)c;
        px_mark_ptr(vm, s->parent);
        px_mark_ptr(vm, s->proto);
        px_mark_value(vm, s->key);
        break;
    }
    case PX_T_VEC: {
        PxVec *v = (PxVec *)c;
        for (i = 0; i < v->cap; i++) px_mark_value(vm, v->items[i]);
        break;
    }
    case PX_T_DICT: {
        PxDict *d = (PxDict *)c;
        for (i = 0; i < d->used; i++) {
            if (!d->entries[i].key) continue;
            px_mark_value(vm, d->entries[i].key);
            px_mark_value(vm, d->entries[i].value);
        }
        break;
    }
    case PX_T_PROTO: {
        PxProto *p = (PxProto *)c;
        px_mark_ptr(vm, p->code);
        px_mark_ptr(vm, p->consts);
        px_mark_ptr(vm, p->lines);
        px_mark_ptr(vm, p->upval_desc);
        px_mark_ptr(vm, p->ics);
        px_mark_value(vm, p->name);
        px_mark_value(vm, p->filename);
        break;
    }
    case PX_T_UPVAL: px_mark_value(vm, ((PxUpval *)c)->closed); break;
    case PX_T_ACCESSOR:
        px_mark_value(vm, ((PxAccessor *)c)->get);
        px_mark_value(vm, ((PxAccessor *)c)->set);
        break;
    case PX_T_ITER: {
        PxIter *it = (PxIter *)c;
        px_mark_value(vm, it->target);
        px_mark_ptr(vm, it->keys);
        px_mark_value(vm, it->next_fn);
        break;
    }
    case PX_T_OBJECT:
    case PX_T_ARRAY:
    case PX_T_CLOSURE:
    case PX_T_NATIVE:
    case PX_T_BOUND:
    case PX_T_ERROR:
    case PX_T_BOXED:
    case PX_T_GENERATOR:
    case PX_T_PROMISE:
    case PX_T_ITEROBJ:
    case PX_T_MAP:
    case PX_T_DATE:
    case PX_T_REGEXP:
    case PX_T_ARRAYBUFFER:
    case PX_T_TYPEDARRAY:
    case PX_T_DATAVIEW:
    case PX_T_PROXY: {
        PxObject *o = (PxObject *)c;
        px_mark_ptr(vm, o->shape);
        px_mark_ptr(vm, o->slots);
        for (i = 0; i < PX_INLINE_SLOTS; i++) px_mark_value(vm, o->inline_slots[i]);
        switch (px_hdr_type(c->hdr)) {
        case PX_T_ARRAY: px_mark_ptr(vm, ((PxArray *)c)->elems); break;
        case PX_T_CLOSURE: {
            PxClosure *f = (PxClosure *)c;
            px_mark_ptr(vm, f->proto);
            px_mark_value(vm, f->this_val);
            px_mark_value(vm, f->home);
            for (i = 0; f->proto && i < f->proto->nupvals; i++) px_mark_ptr(vm, f->upvals[i]);
            break;
        }
        case PX_T_NATIVE:
            px_mark_value(vm, ((PxNative *)c)->name);
            px_mark_value(vm, ((PxNative *)c)->data);
            break;
        case PX_T_BOUND: {
            PxBound *b = (PxBound *)c;
            px_mark_value(vm, b->target);
            px_mark_value(vm, b->this_val);
            px_mark_ptr(vm, b->args);
            break;
        }
        case PX_T_BOXED: px_mark_value(vm, ((PxBoxed *)c)->value); break;
        case PX_T_GENERATOR: {
            PxGen *g = (PxGen *)c;
            px_mark_ptr(vm, g->fn);
            px_mark_ptr(vm, g->saved);
            px_mark_ptr(vm, g->handlers);
            px_mark_ptr(vm, g->upvals);
            px_mark_value(vm, g->this_val);
            px_mark_value(vm, g->new_target);
            px_mark_value(vm, g->promise);
            px_mark_value(vm, g->ret_value);
            px_mark_ptr(vm, g->queue);
            break;
        }
        case PX_T_PROMISE:
            px_mark_value(vm, ((PxPromise *)c)->result);
            px_mark_ptr(vm, ((PxPromise *)c)->reactions);
            break;
        case PX_T_ITEROBJ: px_mark_value(vm, ((PxIterObj *)c)->target); break;
        case PX_T_MAP: {
            PxMap *m = (PxMap *)c;
            px_mark_ptr(vm, m->index);
            px_mark_value(vm, m->extra);
            if (m->kind >= PX_MAP_WEAKMAP && m->entries && remember_weak(vm, m)) break; /* see weak_process */
            px_mark_ptr(vm, m->entries);
            break;
        }
        case PX_T_ARRAYBUFFER: px_mark_ptr(vm, ((PxArrayBuffer *)c)->data); break;
        case PX_T_TYPEDARRAY:
        case PX_T_DATAVIEW: px_mark_value(vm, ((PxTyped *)c)->buffer); break;
        case PX_T_PROXY:
            px_mark_value(vm, ((PxProxy *)c)->target);
            px_mark_value(vm, ((PxProxy *)c)->handler);
            break;
        case PX_T_REGEXP:
            px_mark_value(vm, ((PxRegExp *)c)->source);
            px_mark_ptr(vm, ((PxRegExp *)c)->prog);
            px_mark_value(vm, ((PxRegExp *)c)->names);
            break;
        default: break;
        }
        break;
    }
    default: break;
    }
}
static void drain(PxVM *vm) {
    for (;;) {
        while (vm->mark_top > 0) trace(vm, (PxCell *)px_ptr(vm->mark_stack[--vm->mark_top]));
        if (!vm->mark_overflow) return;
        /* Some marked cells were never pushed: rescan every marked cell.
         * Tracing an already-traced cell again is harmless. */
        vm->mark_overflow = 0;
        {
            uint8_t *p = vm->arena;
            while (p < vm->bump) {
                PxCell *c = (PxCell *)p;
                size_t  n;
                UNPOISON(p, 8);
                n = px_cell_bytes(c);
                if (px_hdr_type(c->hdr) != PX_T_FREE && (c->hdr & PX_HDR_MARK)) trace(vm, c);
                while (vm->mark_top > 0) trace(vm, (PxCell *)px_ptr(vm->mark_stack[--vm->mark_top]));
                p += n;
            }
        }
    }
}

static void mark_roots(PxVM *vm) {
    PxValue *v;
    PxUpval *u;
    uint32_t i;

    px_mark_value(vm, vm->global);
    for (i = 0; i < PX_PROTO_COUNT; i++) {
        px_mark_value(vm, vm->protos[i]);
        px_mark_value(vm, vm->ctors[i]);
    }
    for (i = 0; i < PX_ATOM_COUNT; i++) px_mark_value(vm, vm->atom[i]);
    px_mark_value(vm, vm->exception);
    px_mark_value(vm, vm->oom_error);
    px_mark_value(vm, vm->kept);
    px_mark_value(vm, vm->proxy_proto);
    px_mark_value(vm, vm->sym_fields);
    px_mark_value(vm, vm->sym_iterator);
    px_mark_value(vm, vm->sym_async_iterator);
    px_mark_value(vm, vm->sym_has_instance);
    px_mark_value(vm, vm->sym_to_primitive);
    px_mark_value(vm, vm->sym_to_string_tag);
    px_mark_value(vm, vm->sym_species);
    px_mark_value(vm, vm->sym_is_concat_spreadable);
    px_mark_value(vm, vm->sym_unscopables);
    px_mark_value(vm, vm->throw_type_error);
    px_mark_value(vm, vm->sym_match);
    px_mark_value(vm, vm->sym_match_all);
    px_mark_value(vm, vm->sym_replace);
    px_mark_value(vm, vm->sym_search);
    px_mark_value(vm, vm->sym_split);
    for (v = vm->stack; v < vm->sp; v++) px_mark_value(vm, *v);
    for (i = 0; i < vm->nframes; i++) {
        px_mark_ptr(vm, vm->frames[i].fn);
        px_mark_value(vm, vm->frames[i].this_val);
        px_mark_value(vm, vm->frames[i].new_target);
        px_mark_ptr(vm, vm->frames[i].gen);
    }
    for (u = vm->open_upvals; u; u = u->next) px_mark_ptr(vm, u);
    for (i = 0; i < vm->jobs_count * PX_JOB_WORDS; i++)
        px_mark_value(vm, vm->jobs[(vm->jobs_head * PX_JOB_WORDS + i) % (vm->jobs_cap * PX_JOB_WORDS)]);
    for (i = 0; i < vm->nroots; i++) px_mark_value(vm, *vm->roots[i]);
    for (i = 0; i < vm->handles_used; i++) px_mark_value(vm, vm->handles[i]);
    px_compiler_mark(vm);
    drain(vm);
}

/* ------------------------------------------------------------ sweeping */

void px_collect(PxVM *vm) {
    uint8_t *p, *run = NULL;
    size_t   live = 0, cells = 0;
    uint64_t t0 = vm->now_us ? vm->now_us() : 0;

    if (vm->in_gc) return;
    vm->in_gc = 1;
    vm->mark_top      = 0;
    vm->mark_overflow = 0;
    mark_roots(vm);
    weak_process(vm);
    run_finalizers(vm);

    /* Weak tables first, while every dead cell is still readable. */
    px_atoms_sweep(vm);
    px_root_shapes_sweep(vm);
    for (p = vm->arena; p < vm->bump;) {
        PxCell *c = (PxCell *)p;
        UNPOISON(p, 8);
        if (px_hdr_type(c->hdr) == PX_T_SHAPE && !(c->hdr & PX_HDR_MARK)) {
            UNPOISON(p, px_cell_bytes(c));
            px_shapes_unlink_dead(vm, (PxShape *)c);
        }
        p += px_cell_bytes(c);
    }

    memset(vm->free_small, 0, sizeof vm->free_small);
    vm->free_large = NULL;
    for (p = vm->arena; p < vm->bump;) {
        PxCell *c = (PxCell *)p;
        size_t  n;
        UNPOISON(p, 8);
        n = px_cell_bytes(c);
        if (px_hdr_type(c->hdr) != PX_T_FREE && (c->hdr & PX_HDR_MARK)) {
            c->hdr &= ~PX_HDR_MARK;
            live += n;
            cells++;
            if (run) {
                free_push(vm, run, (size_t)(p - run));
                run = NULL;
            }
        } else if (!run) {
            run = p;
        }
        p += n;
    }
    if (run) {
        /* Free space that reaches the bump pointer goes back to it: one
         * contiguous region is better than a long free list entry. */
        POISON(run, (size_t)(vm->bump - run));
        vm->bump = run;
    }

    vm->live_after_gc  = live;
    vm->live_cells     = cells;
    /* shapes may have been freed: every inline cache is stale */
    if (++vm->shape_epoch == 0) vm->shape_epoch = 1;
    vm->bytes_since_gc = 0;
    /* Collect again after allocating as much as survived (heap may double),
     * but never more often than every GC_MIN_THRESHOLD bytes. */
    vm->gc_threshold = live > GC_MIN_THRESHOLD ? live : GC_MIN_THRESHOLD;
    vm->gc_count++;
    if (vm->now_us) vm->last_gc_us = vm->now_us() - t0;
    vm->in_gc = 0;
}

void px_gc(PxVM *vm) { px_collect(vm); }

void px_mem_stats(PxVM *vm, PxMemStats *out) {
    size_t total = (size_t)(vm->arena_end - vm->arena);
    size_t used  = vm->live_after_gc + vm->bytes_since_gc;
    memset(out, 0, sizeof *out);
    out->heap_bytes = total;
    out->used_bytes = used > total ? total : used;
    out->free_bytes = total - out->used_bytes;
    out->gc_count   = vm->gc_count;
    out->last_gc_us = (size_t)vm->last_gc_us;
    out->objects    = vm->live_cells;
}

void px_set_clock(PxVM *vm, uint64_t (*now_us)(void)) { vm->now_us = now_us; }

uint32_t px_set_alloc_failure(PxVM *vm, uint32_t nth) {
    uint32_t n        = vm->alloc_count;
    vm->alloc_count   = 0;
    vm->fail_alloc_at = nth;
    return n;
}
