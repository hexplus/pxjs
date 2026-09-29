/* PXJS -- the PSPX JavaScript engine.
 *
 * A JavaScript engine written for the Sony PSP-1000 (MIPS Allegrex, 32 MB,
 * single-precision FPU). See docs/engine.md for the design and for which
 * parts of the language are implemented so far.
 *
 * Embedding in five lines:
 *
 *   PxConfig cfg; px_config_default(&cfg);
 *   PxVM *vm = px_new(&cfg);
 *   px_set_global_function(vm, "print", my_print, 1);
 *   if (px_eval(vm, src, len, "app.js", NULL) != 0) report(px_error_text(vm));
 *   px_free(vm);
 *
 * Values: a PxValue is one 32-bit word. Values handed to native functions
 * are valid for the duration of the call. A native function that keeps a
 * value across a call that may allocate must root it (px_push_root). */
#ifndef PXJS_H
#define PXJS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PXJS_VERSION "0.1.0"

typedef uint32_t     PxValue;
typedef struct PxVM  PxVM;

typedef struct PxConfig {
    size_t   heap_bytes;    /* the whole JS heap, allocated once; default 4 MB */
    uint32_t stack_values;  /* VM value stack slots; default 16384 (64 kB) */
    uint32_t max_frames;    /* JS call depth; default 1024 */
    uint32_t max_native_depth; /* native -> JS -> native nesting; default 48 */
    int      gc_stress;     /* debug: collect on every allocation */
    uint32_t fail_alloc_at; /* debug: the Nth heap allocation fails as out of memory (0: never) */
} PxConfig;

/* Debug: from now on, the Nth heap allocation fails (0: never; UINT32_MAX:
 * only count). Returns how many allocations were counted since the last
 * call. Tests use it to check that every allocation site recovers from
 * out-of-memory. */
uint32_t px_set_alloc_failure(PxVM *vm, uint32_t nth);

void  px_config_default(PxConfig *cfg);
PxVM *px_new(const PxConfig *cfg);
void  px_free(PxVM *vm);

/* Compiles and runs a script. 0 on success (result, if not NULL, gets the
 * completion value -- valid until the next allocation); -1 if it threw or
 * failed to compile: px_error_text() then describes it. */
int         px_eval(PxVM *vm, const char *src, size_t len, const char *filename, PxValue *result);
const char *px_error_text(PxVM *vm);

/* ------------------------------------------------------------ natives */

/* Return a value, or PX_EXCEPTION after px_throw*(). */
typedef PxValue (*PxNativeFn)(PxVM *vm, PxValue this_val, int argc, PxValue *argv);

int px_set_global_function(PxVM *vm, const char *name, PxNativeFn fn, int length);
int px_set_global_value(PxVM *vm, const char *name, PxValue v);

typedef enum PxErrorType {
    PX_ERROR = 0,
    PX_TYPE_ERROR,
    PX_RANGE_ERROR,
    PX_REFERENCE_ERROR,
    PX_SYNTAX_ERROR,
    PX_INTERNAL_ERROR,
    PX_AGGREGATE_ERROR,
    PX_EVAL_ERROR,
    PX_URI_ERROR,
    PX_ERROR_TYPES
} PxErrorType;

PxValue px_throw(PxVM *vm, PxValue exception);
/* An error no try/catch can stop: unwinds to the host (used for "exit"). */
PxValue px_throw_uncatchable(PxVM *vm, const char *message);
PxValue px_throw_error(PxVM *vm, PxErrorType type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------ values */

#define PX_MAKE_SPECIAL(n) ((PxValue)(((uint32_t)(n) << 3) | 2u))
#define PX_UNDEFINED PX_MAKE_SPECIAL(0)
#define PX_NULL      PX_MAKE_SPECIAL(1)
#define PX_FALSE     PX_MAKE_SPECIAL(2)
#define PX_TRUE      PX_MAKE_SPECIAL(3)
#define PX_HOLE      PX_MAKE_SPECIAL(4) /* internal: array hole, uninitialised let */
#define PX_EXCEPTION PX_MAKE_SPECIAL(5) /* internal: "an exception is pending" */

PxValue px_number(PxVM *vm, double d);
PxValue px_int(PxVM *vm, int32_t i);
PxValue px_bool(int b);
PxValue px_string(PxVM *vm, const char *utf8, size_t len);

int    px_is_number(PxValue v);
int    px_is_string(PxValue v);
int    px_is_object(PxValue v);
int    px_is_function(PxValue v);
double px_get_number(PxValue v); /* v must be a number */

/* Small integers (31 bits, signed) are stored in the value itself. Testing
 * for one and reading it involves no floating point -- which matters on the
 * PSP, where every double operation is done in software. Bindings on hot
 * paths check px_is_int first and fall back to px_get_number. */
#define px_is_int(v)  (((v) & 1u) != 0)
#define px_get_int(v) ((int32_t)(v) >> 1)

/* UTF-8 of ToString(v) into dst (always NUL-terminated when cap > 0).
 * Returns the full length it needed, or -1 if ToString threw. */
int px_to_utf8(PxVM *vm, PxValue v, char *dst, size_t cap);

/* The display form console.log uses (strings bare, objects as JSON-like). */
int px_inspect(PxVM *vm, PxValue v, char *dst, size_t cap);

PxValue px_call(PxVM *vm, PxValue fn, PxValue this_val, int argc, PxValue *argv);

/* ------------------------------------------------------------ roots */

void px_push_root(PxVM *vm, PxValue *slot);
void px_pop_roots(PxVM *vm, int n);

/* Handles keep a value alive across calls (a callback a timer will call
 * later, a listener): 0 on failure. */
typedef uint32_t PxHandle;
PxHandle px_retain(PxVM *vm, PxValue v);
PxValue  px_handle_value(PxVM *vm, PxHandle h);
void     px_release(PxVM *vm, PxHandle h);

/* ------------------------------------------------------------ objects */

PxValue px_new_object(PxVM *vm);
PxValue px_new_array(PxVM *vm);
PxValue px_new_function(PxVM *vm, PxNativeFn fn, const char *name, int length);
/* A native function carrying a value, read back with px_function_data()
 * inside the call (a closure for C). */
PxValue px_new_function_data(PxVM *vm, PxNativeFn fn, const char *name, int length, PxValue data);
PxValue px_function_data(PxVM *vm);

/* Properties by name. set/define return 0, or -1 with an exception. */
PxValue px_get_prop(PxVM *vm, PxValue obj, const char *name);
int     px_set_prop(PxVM *vm, PxValue obj, const char *name, PxValue v);
int     px_define_prop(PxVM *vm, PxValue obj, const char *name, PxValue v, int enumerable);
PxValue px_get_index_value(PxVM *vm, PxValue obj, uint32_t i);
int     px_array_append(PxVM *vm, PxValue arr, PxValue v);
PxValue px_global_object(PxVM *vm);

int px_is_undefined(PxValue v);
int px_is_error(PxValue v);
int px_to_bool(PxValue v);
/* ToNumber; 0 or -1 (exception) */
int px_to_double(PxVM *vm, PxValue v, double *out);

/* The exception pending after a call returned PX_EXCEPTION, cleared. */
PxValue px_take_exception(PxVM *vm);
/* 1 if the pending exception is the uncatchable kind (interrupt). */
int     px_exception_is_uncatchable(PxVM *vm);

/* Native resources owned by a JS object: fn(opaque) runs once the object
 * is garbage (during a collection), or when the VM is freed. It must only
 * release native memory -- it may not call into the VM. */
int px_set_finalizer(PxVM *vm, PxValue obj, void (*fn)(void *opaque), void *opaque);

/* Binary data. The heap never moves, so a pointer into a buffer stays valid
 * while the typed array is reachable. */
PxValue px_new_uint8_clamped_array(PxVM *vm, uint32_t len, uint8_t **data); /* zeroed */
/* The bytes behind a typed array or DataView: 0, or -1 if v is neither. */
int px_typed_array_bytes(PxValue v, uint8_t **data, size_t *len);

/* ------------------------------------------------------------ modules
 *
 * Module mode: `import ... from "spec"` resolves through a host function
 * (px_set_module_resolver), `export` marks declarations and is otherwise
 * ignored, top-level `await` works, and the evaluation returns a promise
 * that settles when the module body has run. */

typedef PxValue (*PxModuleResolver)(PxVM *vm, const char *specifier, void *opaque);
void px_set_module_resolver(PxVM *vm, PxModuleResolver fn, void *opaque);
int  px_eval_module(PxVM *vm, const char *src, size_t len, const char *filename, PxValue *promise);

/* ------------------------------------------------------------ jobs
 *
 * Promise reactions and queueMicrotask callbacks queue as jobs; the host's
 * event loop runs them (px_run_jobs) after each task. */

int px_has_jobs(PxVM *vm);
/* Runs queued jobs until none are left (including ones they queue).
 * Returns 0, or -1 if a job threw: px_error_text() says what, and the
 * remaining jobs stay queued for the next call. */
int px_run_jobs(PxVM *vm);

/* The end of a task: the current script run and its jobs are over, so
 * WeakRef targets no longer need to be kept alive (the spec's
 * ClearKeptObjects). px_run_jobs does this when it empties the queue;
 * call it after a task that queued no jobs. */
void px_end_task(PxVM *vm);

/* Called when a promise is rejected with no handler (handled = 0), and
 * when a handler is attached to such a promise later (handled = 1). */
typedef void (*PxRejectionTracker)(PxVM *vm, PxValue promise, PxValue reason, int handled, void *opaque);
void px_set_rejection_tracker(PxVM *vm, PxRejectionTracker fn, void *opaque);

/* Promises for native async APIs: create one, hand it to JS, settle it
 * later from the runtime thread. */
PxValue px_new_promise(PxVM *vm);
int     px_resolve_promise(PxVM *vm, PxValue promise, PxValue value);
int     px_reject_promise(PxVM *vm, PxValue promise, PxValue reason);
/* 0 pending, 1 fulfilled, 2 rejected; -1 if v is not a promise */
int     px_promise_state(PxValue v);
PxValue px_promise_result(PxValue v);

/* ------------------------------------------------------------ control */

/* Called now and then while JS runs; return nonzero to abort the running
 * script with an uncatchable error. */
typedef int (*PxInterruptFn)(PxVM *vm, void *opaque);
void px_set_interrupt(PxVM *vm, PxInterruptFn fn, void *opaque);

typedef struct PxMemStats {
    size_t heap_bytes;  /* arena size */
    size_t used_bytes;  /* live after the last GC plus allocated since */
    size_t free_bytes;
    size_t gc_count;
    size_t last_gc_us;  /* 0 unless a clock was set with px_set_clock */
    size_t objects;     /* live cells counted by the last GC */
} PxMemStats;

void px_gc(PxVM *vm);
void px_mem_stats(PxVM *vm, PxMemStats *out);
void px_set_clock(PxVM *vm, uint64_t (*now_us)(void));

#ifdef __cplusplus
}
#endif

#endif
