/* The embedding API (pxjs.h): creating a VM, running code, reporting
 * errors, and the value helpers native code uses. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

void px_config_default(PxConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->heap_bytes       = 4u * 1024u * 1024u;
    cfg->stack_values     = 16384;
    cfg->max_frames       = 1024;
    cfg->max_native_depth = 48;
}

static const char *const k_atom_names[PX_ATOM_COUNT] = {
#define X(n) #n,
    PX_ATOM_LIST(X)
#undef X
};

PxVM *px_new(const PxConfig *cfg_in) {
    PxConfig cfg;
    PxVM    *vm;
    int      i;

    if (cfg_in) cfg = *cfg_in;
    else px_config_default(&cfg);
    if (cfg.stack_values < 1024) cfg.stack_values = 1024;
    if (cfg.max_frames < 32) cfg.max_frames = 32;
    if (cfg.max_native_depth < 4) cfg.max_native_depth = 4;

    vm = (PxVM *)calloc(1, sizeof(PxVM));
    if (!vm) return NULL;
    vm->stack  = (PxValue *)calloc(cfg.stack_values, sizeof(PxValue));
    vm->frames = (PxFrame *)calloc(cfg.max_frames, sizeof(PxFrame));
    if (!vm->stack || !vm->frames || px_heap_init(vm, cfg.heap_bytes) != 0) {
        free(vm->stack);
        free(vm->frames);
        free(vm);
        return NULL;
    }
    vm->stack_end         = vm->stack + cfg.stack_values;
    vm->sp                = vm->stack;
    vm->max_frames        = cfg.max_frames;
    vm->max_native_depth  = cfg.max_native_depth;
    vm->interrupt_counter = 10000;
    vm->handle_free       = -1;
    vm->global            = PX_UNDEFINED;
    vm->exception         = PX_UNDEFINED;
    vm->oom_error         = PX_UNDEFINED;
    for (i = 0; i < PX_PROTO_COUNT; i++) {
        vm->protos[i] = PX_UNDEFINED;
        vm->ctors[i]  = PX_UNDEFINED;
    }
    for (i = 0; i < PX_ATOM_COUNT; i++) vm->atom[i] = PX_UNDEFINED;
    /* Atoms first: everything after uses them. "empty" is the empty string. */
    for (i = 0; i < PX_ATOM_COUNT; i++) {
        const char *n = i == PX_ATOM_empty ? "" : k_atom_names[i];
        vm->atom[i]   = px_intern_cstr(vm, n);
        if (vm->atom[i] == PX_EXCEPTION) goto fail;
    }
    if (px_builtins_init(vm) != 0) goto fail;
    vm->gc_stress = cfg.gc_stress;
    vm->fail_alloc_at = cfg.fail_alloc_at;
    return vm;
fail:
    px_free(vm);
    return NULL;
}

void px_free(PxVM *vm) {
    if (!vm) return;
    px_heap_free(vm);
    free(vm->atoms);
    free(vm->root_shapes);
    free(vm->jobs);
    free(vm->handles);
    free(vm->stack);
    free(vm->frames);
    free(vm);
}

void px_set_interrupt(PxVM *vm, PxInterruptFn fn, void *opaque) {
    vm->interrupt        = fn;
    vm->interrupt_opaque = opaque;
}

static void describe_exception(PxVM *vm) {
    PxValue e = vm->exception;
    PX_ROOT(vm, e);
    if (e == vm->oom_error) {
        snprintf(vm->error_text, sizeof vm->error_text, "InternalError: out of memory");
    } else {
        px_inspect(vm, e, vm->error_text, sizeof vm->error_text);
    }
    px_pop_roots(vm, 1);
}

int px_eval(PxVM *vm, const char *src, size_t len, const char *filename, PxValue *result) {
    PxProto *p;
    PxValue  pv, r;

    vm->error_text[0] = '\0';
    vm->exception     = PX_UNDEFINED;
    p                 = px_compile(vm, src, len, filename, 0);
    if (!p) {
        describe_exception(vm);
        vm->exception = PX_UNDEFINED;
        return -1;
    }
    pv = px_from_ptr(p);
    PX_ROOT(vm, pv);
    r = px_run_proto(vm, p, vm->global);
    px_pop_roots(vm, 1);
    if (r == PX_EXCEPTION) {
        vm->uncatchable = 0;
        describe_exception(vm);
        vm->exception = PX_UNDEFINED;
        return -1;
    }
    if (result) *result = r;
    return 0;
}

const char *px_error_text(PxVM *vm) { return vm->error_text; }

int px_set_global_function(PxVM *vm, const char *name, PxNativeFn fn, int length) {
    PxValue f = px_make_native(vm, fn, name, length, 0), k;
    int     r;
    if (f == PX_EXCEPTION) return -1;
    PX_ROOT(vm, f);
    k = px_intern_cstr(vm, name);
    r = k == PX_EXCEPTION ? -1 : px_define(vm, vm->global, k, f, PX_ATTR_HIDDEN);
    px_pop_roots(vm, 1);
    return r;
}

int px_set_global_value(PxVM *vm, const char *name, PxValue v) {
    PxValue k;
    int     r;
    PX_ROOT(vm, v);
    k = px_intern_cstr(vm, name);
    r = k == PX_EXCEPTION ? -1 : px_define(vm, vm->global, k, v, PX_ATTR_HIDDEN);
    px_pop_roots(vm, 1);
    return r;
}

int px_to_utf8(PxVM *vm, PxValue v, char *dst, size_t cap) {
    PxValue s = px_to_string(vm, v);
    if (s == PX_EXCEPTION) {
        if (cap) dst[0] = '\0';
        return -1;
    }
    return (int)px_str_to_utf8(vm, s, dst, cap);
}

/* ------------------------------------------------------------ modules */

void px_set_module_resolver(PxVM *vm, PxModuleResolver fn, void *opaque) {
    vm->resolver        = fn;
    vm->resolver_opaque = opaque;
}

int px_eval_module(PxVM *vm, const char *src, size_t len, const char *filename, PxValue *promise) {
    PxProto *p;
    PxValue  pv, r;

    vm->error_text[0] = '\0';
    vm->exception     = PX_UNDEFINED;
    p                 = px_compile(vm, src, len, filename, 1);
    if (!p) {
        describe_exception(vm);
        vm->exception = PX_UNDEFINED;
        return -1;
    }
    pv = px_from_ptr(p);
    PX_ROOT(vm, pv);
    /* the module body is an async function: the call returns its promise */
    r = px_run_proto(vm, p, PX_UNDEFINED);
    px_pop_roots(vm, 1);
    if (r == PX_EXCEPTION) {
        vm->uncatchable = 0;
        describe_exception(vm);
        vm->exception = PX_UNDEFINED;
        return -1;
    }
    if (promise) *promise = r;
    return 0;
}

/* ------------------------------------------------------------ handles */

PxHandle px_retain(PxVM *vm, PxValue v) {
    uint32_t i;
    if (vm->handle_free >= 0) {
        i               = (uint32_t)vm->handle_free;
        vm->handle_free = px_smi(vm->handles[i]);
    } else {
        if (vm->handles_used == vm->handles_cap) {
            uint32_t ncap = vm->handles_cap ? vm->handles_cap * 2 : 32;
            PxValue *n    = (PxValue *)realloc(vm->handles, ncap * sizeof(PxValue));
            if (!n) return 0;
            vm->handles     = n;
            vm->handles_cap = ncap;
        }
        i = vm->handles_used++;
    }
    vm->handles[i] = v;
    return i + 1;
}

PxValue px_handle_value(PxVM *vm, PxHandle h) {
    if (h == 0 || h > vm->handles_used) return PX_UNDEFINED;
    return vm->handles[h - 1];
}

void px_release(PxVM *vm, PxHandle h) {
    if (h == 0 || h > vm->handles_used) return;
    vm->handles[h - 1] = px_from_smi(vm->handle_free);
    vm->handle_free    = (int32_t)(h - 1);
}

/* ------------------------------------------------------------ objects */

PxValue px_new_object(PxVM *vm) { return px_object_new(vm); }
PxValue px_new_array(PxVM *vm) { return px_array_new(vm, 0); }

PxValue px_new_function(PxVM *vm, PxNativeFn fn, const char *name, int length) {
    return px_make_native(vm, fn, name, length, 0);
}

PxValue px_new_function_data(PxVM *vm, PxNativeFn fn, const char *name, int length, PxValue data) {
    return px_make_native_data(vm, fn, name, length, data);
}

PxValue px_function_data(PxVM *vm) { return vm->native_data; }

PxValue px_get_prop(PxVM *vm, PxValue obj, const char *name) {
    PxValue k;
    PX_ROOT(vm, obj);
    k = px_intern_cstr(vm, name);
    px_pop_roots(vm, 1);
    if (k == PX_EXCEPTION) return k;
    return px_get(vm, obj, k);
}

int px_set_prop(PxVM *vm, PxValue obj, const char *name, PxValue v) {
    PxValue k;
    int     r;
    PX_ROOT(vm, obj);
    PX_ROOT(vm, v);
    k = px_intern_cstr(vm, name);
    r = k == PX_EXCEPTION ? -1 : px_set(vm, obj, k, v);
    px_pop_roots(vm, 2);
    return r;
}

int px_define_prop(PxVM *vm, PxValue obj, const char *name, PxValue v, int enumerable) {
    return px_def_value(vm, obj, name, v, enumerable ? PX_ATTR_DEFAULT : PX_ATTR_HIDDEN);
}

PxValue px_get_index_value(PxVM *vm, PxValue obj, uint32_t i) { return px_get_index(vm, obj, i); }

int px_array_append(PxVM *vm, PxValue arr, PxValue v) {
    if (!px_is_obj(arr) || px_type_of(arr) != PX_T_ARRAY) return -1;
    return px_array_push(vm, arr, v);
}

PxValue px_global_object(PxVM *vm) { return vm->global; }
int     px_is_undefined(PxValue v) { return v == PX_UNDEFINED; }
int     px_is_error(PxValue v) { return px_is_obj(v) && px_type_of(v) == PX_T_ERROR; }
int     px_to_bool(PxValue v) { return px_truthy(v); }
int     px_to_double(PxVM *vm, PxValue v, double *out) { return px_to_number(vm, v, out); }

PxValue px_take_exception(PxVM *vm) {
    PxValue e     = vm->exception;
    vm->exception = PX_UNDEFINED;
    return e;
}

int px_exception_is_uncatchable(PxVM *vm) { return vm->uncatchable; }

PxValue px_throw_uncatchable(PxVM *vm, const char *message) {
    px_throw_error(vm, PX_INTERNAL_ERROR, "%s", message);
    vm->uncatchable = 1;
    return PX_EXCEPTION;
}
