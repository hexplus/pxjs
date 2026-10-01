/* The PXJS interpreter, calls, generators, exceptions, and the value
 * conversions the language defines.
 *
 * JS-to-JS calls do not recurse in C: a call pushes a frame and the same
 * dispatch loop carries on in the callee, so the depth of JavaScript
 * recursion is bounded by the VM's own stack and frame table (sized in
 * PxConfig), never by the PSP thread's C stack. C recursion happens only
 * when native code calls back into JS (Array.prototype.map, toString,
 * resuming a generator), and that is counted and capped
 * (max_native_depth). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"

#define INTERRUPT_PERIOD 10000

/* ------------------------------------------------------------ numbers */

PxValue px_number(PxVM *vm, double d) {
    int32_t i;
    if (px_dbl_to_smi(d, &i)) return px_from_smi(i);
    return px_box_number(vm, d);
}

PxValue px_int(PxVM *vm, int32_t i) {
    if (i >= PX_SMI_MIN && i <= PX_SMI_MAX) return px_from_smi(i);
    return px_box_number(vm, (double)i);
}

PxValue px_bool(int b) { return b ? PX_TRUE : PX_FALSE; }
int     px_is_number(PxValue v) { return px_is_num(v); }
int     px_is_string(PxValue v) { return px_is_str(v); }
int     px_is_object(PxValue v) { return px_is_obj(v); }
double  px_get_number(PxValue v) { return px_num(v); }

int px_is_callable(PxValue v) {
    if (!px_is_obj(v)) return 0;
    switch (px_type_of(v)) {
    case PX_T_CLOSURE:
    case PX_T_NATIVE:
    case PX_T_BOUND: return 1;
    case PX_T_PROXY: return ((PxProxy *)px_ptr(v))->callable;
    default: return 0;
    }
}
int px_is_function(PxValue v) { return px_is_callable(v); }

int px_is_constructor(PxValue v) {
    while (px_is_obj(v) && px_type_of(v) == PX_T_BOUND) v = ((PxBound *)px_ptr(v))->target;
    if (!px_is_callable(v)) return 0;
    if (px_type_of(v) == PX_T_PROXY) return ((PxProxy *)px_ptr(v))->constructable;
    if (px_type_of(v) == PX_T_NATIVE) return !(((PxObject *)px_ptr(v))->flags & PX_OBJ_NOT_CTOR);
    if (px_type_of(v) == PX_T_CLOSURE) {
        PxProto *p = ((PxClosure *)px_ptr(v))->proto;
        if (p->flags & PX_PROTO_CLASS_CTOR) return 1;
        return !(p->flags & (PX_PROTO_ARROW | PX_PROTO_METHOD | PX_PROTO_GENERATOR | PX_PROTO_ASYNC));
    }
    return 1;
}

/* ------------------------------------------------------------ errors */

PxValue px_throw(PxVM *vm, PxValue exc) {
    vm->exception = exc;
    return PX_EXCEPTION;
}

PxValue px_throw_oom(PxVM *vm) {
    if (vm->oom_fn && !vm->in_oom_fn) {
        vm->in_oom_fn = 1;
        vm->oom_fn(vm, vm->oom_opaque);
        vm->in_oom_fn = 0;
    }
    vm->exception = vm->oom_error;
    return PX_EXCEPTION;
}

PxValue px_throw_error(PxVM *vm, PxErrorType type, const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    PxValue msg, err;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    msg = px_str_from_cstr(vm, buf);
    if (msg == PX_EXCEPTION) return PX_EXCEPTION;
    err = px_error_new(vm, type, msg);
    if (err == PX_EXCEPTION) return PX_EXCEPTION;
    vm->exception = err;
    return PX_EXCEPTION;
}

static uint32_t line_of(const PxProto *p, const uint8_t *pc) {
    const uint8_t *l, *end;
    uint32_t       target, at = 0, line = p->line;
    if (!p->lines || !pc) return line;
    target = (uint32_t)(pc - p->code->data);
    l      = p->lines->data;
    end    = l + p->lines->len;
    while (l < end) {
        uint32_t dpc = 0, dl = 0, shift = 0;
        do {
            dpc |= (uint32_t)(*l & 0x7F) << shift;
            shift += 7;
        } while (*l++ & 0x80 && l < end);
        shift = 0;
        do {
            dl |= (uint32_t)(*l & 0x7F) << shift;
            shift += 7;
        } while (l < end && (*l++ & 0x80));
        if (at + dpc >= target) break;
        at += dpc;
        line = (uint32_t)((int32_t)line + (int32_t)((dl >> 1) ^ (uint32_t)-(int32_t)(dl & 1)));
    }
    return line;
}

/* "    at name (file:line)" per frame, innermost first, as QuickJS does. */
void px_capture_stack(PxVM *vm, PxValue err) {
    char    buf[1024];
    size_t  o = 0;
    int32_t i;
    PxValue s;

    buf[0] = '\0';
    for (i = (int32_t)vm->nframes - 1; i >= 0 && o + 80 < sizeof buf; i--) {
        PxFrame *f = &vm->frames[i];
        PxProto *p = f->fn->proto;
        char     name[48], file[64];
        if (px_is_ptr(p->name)) px_str_to_utf8(vm, p->name, name, sizeof name);
        else snprintf(name, sizeof name, "%s", (p->flags & PX_PROTO_SCRIPT) ? "<script>" : "<anonymous>");
        px_str_to_utf8(vm, p->filename, file, sizeof file);
        o += (size_t)snprintf(buf + o, sizeof buf - o, "    at %s (%s:%u)\n", name, file,
                              (unsigned)line_of(p, f->pc));
    }
    if (o == 0) return;
    if (o >= sizeof buf) o = sizeof buf - 1;
    s = px_str_from_utf8(vm, buf, o);
    if (s == PX_EXCEPTION) {
        vm->exception = PX_UNDEFINED;
        return;
    }
    PX_ROOT(vm, s);
    px_define(vm, err, vm->atom[PX_ATOM_stack], s, PX_ATTR_HIDDEN);
    px_pop_roots(vm, 1);
}

/* ------------------------------------------------------------ conversion */

int px_truthy(PxValue v) {
    if (px_is_smi(v)) return px_smi(v) != 0;
    if (v == PX_TRUE) return 1;
    if (px_is_special(v)) return 0;
    switch (px_type_of(v)) {
    case PX_T_NUMBER: {
        double d = ((PxNumber *)px_ptr(v))->d;
        return !(d == 0 || isnan(d));
    }
    case PX_T_STRING:
    case PX_T_ROPE: return px_str_len(v) != 0;
    default: return 1;
    }
}

PxValue px_to_primitive(PxVM *vm, PxValue v, int hint_string) {
    PxValue names[2], r, exotic, hint;
    int     i;
    if (!px_is_obj(v)) return v;
    PX_ROOT(vm, v);
    /* [Symbol.toPrimitive]: GetMethod, so anything but undefined, null or
     * a function is a TypeError */
    exotic = px_get(vm, v, vm->sym_to_primitive);
    if (exotic == PX_EXCEPTION) goto fail;
    if (exotic != PX_UNDEFINED && exotic != PX_NULL) {
        if (!px_is_callable(exotic)) {
            px_throw_error(vm, PX_TYPE_ERROR, "Symbol.toPrimitive is not a function");
            goto fail;
        }
        hint = px_str_from_cstr(vm, hint_string == 1 ? "string" : hint_string == 0 ? "default" : "number");
        if (hint == PX_EXCEPTION) goto fail;
        r = px_call(vm, exotic, v, 1, &hint);
        px_pop_roots(vm, 1);
        if (r != PX_EXCEPTION && px_is_obj(r))
            return px_throw_error(vm, PX_TYPE_ERROR, "Symbol.toPrimitive must return a primitive");
        return r;
    }
    /* Dates convert to strings by default. */
    if (hint_string == 0 && px_type_of(v) == PX_T_DATE) hint_string = 1;
    names[0] = vm->atom[hint_string == 1 ? PX_ATOM_toString : PX_ATOM_valueOf];
    names[1] = vm->atom[hint_string == 1 ? PX_ATOM_valueOf : PX_ATOM_toString];
    for (i = 0; i < 2; i++) {
        PxValue fn = px_get(vm, v, names[i]);
        if (fn == PX_EXCEPTION) goto fail;
        if (!px_is_callable(fn)) continue;
        r = px_call(vm, fn, v, 0, NULL);
        if (r == PX_EXCEPTION) goto fail;
        if (!px_is_obj(r)) {
            px_pop_roots(vm, 1);
            return r;
        }
    }
    px_pop_roots(vm, 1);
    return px_throw_error(vm, PX_TYPE_ERROR, "cannot convert object to primitive value");
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

int px_to_number(PxVM *vm, PxValue v, double *out) {
    for (;;) {
        if (px_is_smi(v)) {
            *out = px_smi(v);
            return 0;
        }
        if (px_is_special(v)) {
            *out = v == PX_TRUE ? 1 : v == PX_FALSE ? 0 : v == PX_NULL ? 0 : NAN;
            return 0;
        }
        switch (px_type_of(v)) {
        case PX_T_NUMBER: *out = ((PxNumber *)px_ptr(v))->d; return 0;
        case PX_T_STRING:
        case PX_T_ROPE: *out = px_string_to_number(vm, v); return 0;
        case PX_T_SYMBOL: px_throw_error(vm, PX_TYPE_ERROR, "cannot convert a Symbol to a number"); return -1;
        default:
            v = px_to_primitive(vm, v, 2);
            if (v == PX_EXCEPTION) return -1;
        }
    }
}

int px_to_int32(PxVM *vm, PxValue v, int32_t *out) {
    double d;
    if (px_is_smi(v)) {
        *out = px_smi(v);
        return 0;
    }
    if (px_is_ptr(v) && px_type_of(v) == PX_T_NUMBER) {
        *out = px_dbl_to_int32(((PxNumber *)px_ptr(v))->d);
        return 0;
    }
    if (px_to_number(vm, v, &d) < 0) return -1;
    *out = px_dbl_to_int32(d);
    return 0;
}

int px_to_uint32(PxVM *vm, PxValue v, uint32_t *out) {
    int32_t i;
    if (px_to_int32(vm, v, &i) < 0) return -1;
    *out = (uint32_t)i;
    return 0;
}

PxValue px_to_string(PxVM *vm, PxValue v) {
    for (;;) {
        if (px_is_smi(v)) return px_int_to_string(vm, px_smi(v));
        if (px_is_special(v)) {
            const char *s = v == PX_UNDEFINED ? "undefined" : v == PX_NULL ? "null" : v == PX_TRUE ? "true" : "false";
            return px_str_from_cstr(vm, s);
        }
        switch (px_type_of(v)) {
        case PX_T_STRING:
        case PX_T_ROPE: return v;
        case PX_T_NUMBER: return px_number_to_string(vm, ((PxNumber *)px_ptr(v))->d, 10);
        case PX_T_SYMBOL: return px_throw_error(vm, PX_TYPE_ERROR, "cannot convert a Symbol to a string");
        default:
            v = px_to_primitive(vm, v, 1);
            if (v == PX_EXCEPTION) return v;
        }
    }
}

PxValue px_to_object(PxVM *vm, PxValue v) {
    PxBoxed *b;
    PxValue  proto;
    if (px_is_obj(v)) return v;
    if (v == PX_UNDEFINED || v == PX_NULL)
        return px_throw_error(vm, PX_TYPE_ERROR, "cannot convert %s to object", v == PX_NULL ? "null" : "undefined");
    proto = px_proto_of(vm, v);
    PX_ROOT(vm, v);
    b = (PxBoxed *)px_obj_new(vm, PX_T_BOXED, sizeof(PxBoxed), proto);
    px_pop_roots(vm, 1);
    if (!b) return PX_EXCEPTION;
    b->value = v;
    return px_from_ptr(b);
}

PxValue px_typeof(PxVM *vm, PxValue v) {
    int a;
    if (px_is_smi(v)) a = PX_ATOM_number;
    else if (v == PX_UNDEFINED) a = PX_ATOM_undefined;
    else if (v == PX_TRUE || v == PX_FALSE) a = PX_ATOM_boolean;
    else if (px_is_special(v)) a = PX_ATOM_object; /* null */
    else {
        switch (px_type_of(v)) {
        case PX_T_NUMBER: a = PX_ATOM_number; break;
        case PX_T_STRING:
        case PX_T_ROPE: a = PX_ATOM_string; break;
        case PX_T_SYMBOL: a = PX_ATOM_symbol; break;
        default: a = px_is_callable(v) ? PX_ATOM_function : PX_ATOM_object; break;
        }
    }
    return vm->atom[a];
}

int px_strict_equals(PxVM *vm, PxValue a, PxValue b) {
    /* two SMIs: equal iff the same word (no double compare, which on the
     * PSP is a libgcc call) */
    if (px_is_smi(a) && px_is_smi(b)) return a == b;
    if (px_is_num(a) && px_is_num(b)) return px_num(a) == px_num(b);
    if (a == b) return 1;
    if (px_is_str(a) && px_is_str(b)) return px_str_eq(vm, a, b) == 1;
    return 0;
}

int px_same_value_zero(PxVM *vm, PxValue a, PxValue b) {
    if (px_is_smi(a) && px_is_smi(b)) return a == b;
    if (px_is_num(a) && px_is_num(b)) {
        double x = px_num(a), y = px_num(b);
        return x == y || (isnan(x) && isnan(y));
    }
    return px_strict_equals(vm, a, b);
}

int px_loose_equals(PxVM *vm, PxValue a, PxValue b) {
    for (;;) {
        int an = a == PX_NULL || a == PX_UNDEFINED, bn = b == PX_NULL || b == PX_UNDEFINED;
        if (an || bn) return an && bn;
        if ((px_is_num(a) && px_is_num(b)) || (px_is_str(a) && px_is_str(b)) || (px_is_obj(a) && px_is_obj(b)) ||
            ((a == PX_TRUE || a == PX_FALSE) && (b == PX_TRUE || b == PX_FALSE)))
            return px_strict_equals(vm, a, b);
        if (px_is_ptr(a) && px_type_of(a) == PX_T_SYMBOL) return a == b;
        if (px_is_ptr(b) && px_type_of(b) == PX_T_SYMBOL) return 0;
        if (a == PX_TRUE || a == PX_FALSE) {
            a = px_from_smi(a == PX_TRUE);
            continue;
        }
        if (b == PX_TRUE || b == PX_FALSE) {
            b = px_from_smi(b == PX_TRUE);
            continue;
        }
        if (px_is_obj(a)) {
            a = px_to_primitive(vm, a, 0);
            if (a == PX_EXCEPTION) return -1;
            continue;
        }
        if (px_is_obj(b)) {
            b = px_to_primitive(vm, b, 0);
            if (b == PX_EXCEPTION) return -1;
            continue;
        }
        {
            double x, y;
            if (px_to_number(vm, a, &x) < 0 || px_to_number(vm, b, &y) < 0) return -1;
            return x == y;
        }
    }
}

/* ------------------------------------------------------------ upvalues */

static void open_list_insert(PxVM *vm, PxUpval *u) {
    PxUpval **link = &vm->open_upvals;
    while (*link && (*link)->v > u->v) link = &(*link)->next;
    u->next = *link;
    *link   = u;
}

static PxUpval *capture(PxVM *vm, PxValue *slot) {
    PxUpval **link = &vm->open_upvals, *u;
    while (*link && (*link)->v > slot) link = &(*link)->next;
    if (*link && (*link)->v == slot) return *link;
    u = (PxUpval *)px_alloc(vm, PX_T_UPVAL, sizeof(PxUpval));
    if (!u) return NULL;
    u->v      = slot;
    u->closed = PX_UNDEFINED;
    open_list_insert(vm, u);
    return u;
}

static void close_upvals(PxVM *vm, PxValue *level) {
    while (vm->open_upvals && vm->open_upvals->v >= level) {
        PxUpval *u      = vm->open_upvals;
        u->closed       = *u->v;
        u->v            = &u->closed;
        vm->open_upvals = u->next;
        u->next         = NULL;
    }
}

/* ------------------------------------------------------------ natives */

PxValue px_make_native(PxVM *vm, PxNativeFn fn, const char *name, int length, int magic) {
    PxNative *n;
    PxValue   nm = px_intern_cstr(vm, name);
    if (nm == PX_EXCEPTION) return nm;
    PX_ROOT(vm, nm);
    n = (PxNative *)px_obj_new(vm, PX_T_NATIVE, sizeof(PxNative), vm->protos[PX_PROTO_FUNCTION]);
    px_pop_roots(vm, 1);
    if (!n) return PX_EXCEPTION;
    n->fn     = fn;
    n->name   = nm;
    n->length = (int16_t)length;
    n->magic  = (int16_t)magic;
    n->data   = PX_UNDEFINED;
    return px_from_ptr(n);
}

PxValue px_make_native_data(PxVM *vm, PxNativeFn fn, const char *name, int length, PxValue data) {
    PxValue f;
    PX_ROOT(vm, data);
    f = px_make_native(vm, fn, name, length, 0);
    px_pop_roots(vm, 1);
    if (f != PX_EXCEPTION) ((PxNative *)px_ptr(f))->data = data;
    return f;
}

static PxValue call_native(PxVM *vm, PxNative *n, PxValue this_val, int argc, PxValue *argv, PxValue new_target) {
    PxValue r;
    /* The caller's magic/callee/data come back afterwards: a native that
     * calls into JS (a callback, a getter) still sees its own. */
    int     magic  = vm->native_magic;
    PxValue callee = vm->native_callee, data = vm->native_data, nt = vm->native_new_target;
    if (vm->native_depth >= vm->max_native_depth)
        return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded (native)");
    vm->native_depth++;
    vm->native_magic      = n->magic;
    vm->native_callee     = px_from_ptr(n);
    vm->native_data       = n->data;
    vm->native_new_target = new_target;
    r                     = n->fn(vm, this_val, argc, argv);
    vm->native_depth--;
    vm->native_magic      = magic;
    vm->native_callee     = callee;
    vm->native_data       = data;
    vm->native_new_target = nt;
    return r;
}

/* ------------------------------------------------------------ frames */

int px_check_interrupt(PxVM *vm) {
    int rc;
    vm->interrupt_counter = INTERRUPT_PERIOD;
    if (!vm->interrupt || !(rc = vm->interrupt(vm, vm->interrupt_opaque))) return 0;
    if (rc == PX_INTERRUPT_THROWN) return -1; /* the host threw its own */
    px_throw_error(vm, PX_INTERNAL_ERROR, "interrupted");
    vm->uncatchable = 1;
    return -1;
}

static PxClosure *make_closure(PxVM *vm, PxProto *p, PxFrame *f) {
    PxClosure *c;
    PxValue    pv = px_from_ptr(p);
    uint32_t   i;
    PX_ROOT(vm, pv);
    c = (PxClosure *)px_obj_new(vm, PX_T_CLOSURE, sizeof(PxClosure) + p->nupvals * sizeof(PxUpval *),
                                vm->protos[(p->flags & PX_PROTO_GENERATOR)
                                               ? ((p->flags & PX_PROTO_ASYNC) ? PX_PROTO_ASYNCGENFN : PX_PROTO_GENFN)
                                           : (p->flags & PX_PROTO_ASYNC) ? PX_PROTO_ASYNCFN
                                                                         : PX_PROTO_FUNCTION]);
    if (!c) {
        px_pop_roots(vm, 1);
        return NULL;
    }
    c->proto    = p;
    c->this_val = PX_UNDEFINED;
    c->home     = PX_UNDEFINED;
    if (p->flags & PX_PROTO_ARROW) {
        c->obj.flags |= PX_OBJ_ARROW;
        if (f) {
            c->this_val = f->this_val;
            c->home     = f->fn->home;
        }
    }
    {
        PxValue cv = px_from_ptr(c);
        PX_ROOT(vm, cv);
        for (i = 0; i < p->nupvals; i++) {
            const uint8_t *d        = p->upval_desc->data + 3 * i;
            uint8_t        is_local = d[0];
            uint16_t       idx      = (uint16_t)(d[1] | d[2] << 8);
            if (is_local) {
                PxUpval *u = capture(vm, f->base + idx);
                if (!u) {
                    px_pop_roots(vm, 2);
                    return NULL;
                }
                c->upvals[i] = u;
            } else {
                c->upvals[i] = f->fn->upvals[idx];
            }
        }
        px_pop_roots(vm, 1);
    }
    px_pop_roots(vm, 1);
    return c;
}

/* Sets up a frame for closure `fn` whose call occupies callee[0] (this),
 * callee[1] (fn), callee[2..] (args); sp is just past the args. */
/* The `arguments` object and the rest array of a new frame (the cold part
 * of push_frame): built while the arguments are still on the stack (and
 * covered by vm->sp, so rooted). */
static PX_NOINLINE int frame_arrays(PxVM *vm, PxProto *p, PxValue *base, int argc, PxValue *restp, PxValue *argsp) {
    PxValue rest = PX_UNDEFINED, args = PX_UNDEFINED;
    int     i;
    vm->sp = base + argc;
    PX_ROOT(vm, rest);
    PX_ROOT(vm, args);
    if (p->flags & PX_PROTO_ARGUMENTS) {
        args = px_array_new(vm, (uint32_t)argc);
        for (i = 0; args != PX_EXCEPTION && i < argc; i++)
            if (px_array_push(vm, args, base[i]) < 0) args = PX_EXCEPTION;
    }
    if (args != PX_EXCEPTION && (p->flags & PX_PROTO_REST)) {
        int from = p->nparams;
        rest     = px_array_new(vm, argc > from ? (uint32_t)(argc - from) : 0);
        for (i = from; rest != PX_EXCEPTION && i < argc; i++)
            if (px_array_push(vm, rest, base[i]) < 0) rest = PX_EXCEPTION;
    }
    px_pop_roots(vm, 2);
    if (args == PX_EXCEPTION || rest == PX_EXCEPTION) return -1;
    *restp = rest;
    *argsp = args;
    return 0;
}

static PX_NOINLINE int stack_overflow(PxVM *vm) {
    px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
    return -1;
}

/* A new frame for fn, its arguments at callee + 2. Inline: every JS call
 * goes through it; the rare parts are the functions above. */
static PX_ALWAYS_INLINE int push_frame(PxVM *vm, PxClosure *fn, PxValue *callee, int argc, PxValue this_val,
                                       PxValue new_target, int construct, int boundary) {
    PxProto *p    = fn->proto;
    PxValue *base = callee + 2;
    PxValue  rest = PX_UNDEFINED, args = PX_UNDEFINED;
    PxFrame *f;
    int      i;

    if (PX_UNLIKELY(vm->nframes >= vm->max_frames || base + p->nlocals + p->max_stack + 2 >= vm->stack_end))
        return stack_overflow(vm);
    if (PX_UNLIKELY(p->flags & (PX_PROTO_REST | PX_PROTO_ARGUMENTS)) &&
        frame_arrays(vm, p, base, argc, &rest, &args) < 0)
        return -1;
    for (i = argc < p->nparams ? argc : p->nparams; i < p->nlocals; i++) base[i] = PX_UNDEFINED;
    if (p->self_slot != 0xFF) base[p->self_slot] = px_from_ptr(fn);
    if (p->flags & PX_PROTO_REST) base[p->nparams] = rest;
    if (p->flags & PX_PROTO_ARGUMENTS) base[p->args_slot] = args;
    f         = &vm->frames[vm->nframes++];
    f->fn     = fn;
    f->pc     = p->code->data;
    f->base   = base;
    f->callee = callee;
    if (p->flags & PX_PROTO_ARROW) f->this_val = fn->this_val;
    else if (construct && (p->flags & PX_PROTO_DERIVED)) f->this_val = PX_HOLE; /* until super() */
    else f->this_val = this_val;
    f->new_target   = new_target;
    f->gen          = NULL;
    f->argc         = (uint16_t)argc;
    f->is_construct = (uint8_t)construct;
    f->is_boundary  = (uint8_t)boundary;
    vm->sp          = base + p->nlocals;
    return 0;
}

/* Pops frames above `keep` (exclusive), closing their upvalues. */
static void unwind_frames(PxVM *vm, uint32_t keep) {
    while (vm->nframes > keep) {
        PxFrame *f = &vm->frames[vm->nframes - 1];
        close_upvals(vm, f->base);
        while (vm->nhandlers > 0 && vm->handlers[vm->nhandlers - 1].frame >= vm->nframes - 1) vm->nhandlers--;
        vm->nframes--;
    }
}

/* ------------------------------------------------------------ generators */

/* Copies the running frame f (with operand stack up to sp) into g, and
 * detaches everything that pointed into it. The frame is still pushed. */
static int gen_save(PxVM *vm, PxGen *g, PxFrame *f, PxValue *sp) {
    PxValue  *seg = f->callee;
    uint32_t  n = (uint32_t)(sp - seg), nh = 0, nu = 0, i, k;
    uint32_t  fi = vm->nframes - 1;
    PxUpval  *u;
    PxValue   gv = px_from_ptr(g);

    /* The caller keeps vm->sp at or above sp: the segment is rooted. */
    PX_ROOT(vm, gv);
    for (i = vm->nhandlers; i > 0 && vm->handlers[i - 1].frame == fi; i--) nh++;
    for (u = vm->open_upvals; u && u->v >= seg; u = u->next) nu++;
    if (!g->saved || g->saved->cap < n) {
        PxVec *v = px_vec_new(vm, n + 8);
        if (!v) goto fail;
        g->saved = v;
    }
    if (nh && (!g->handlers || g->handlers->cap < 2 * nh)) {
        PxVec *v = px_vec_new(vm, 2 * nh);
        if (!v) goto fail;
        g->handlers = v;
    }
    if (nu && (!g->upvals || g->upvals->cap < 2 * nu)) {
        PxVec *v = px_vec_new(vm, 2 * nu);
        if (!v) goto fail;
        g->upvals = v;
    }
    px_pop_roots(vm, 1);
    memcpy(g->saved->items, seg, n * sizeof(PxValue));
    g->nsaved = n;
    g->pc_off = (uint32_t)(f->pc - f->fn->proto->code->data);
    for (k = 0; k < nh; k++) {
        PxHandler *h                  = &vm->handlers[vm->nhandlers - nh + k];
        g->handlers->items[2 * k]     = px_from_smi((int32_t)(h->pc - f->fn->proto->code->data));
        g->handlers->items[2 * k + 1] = px_from_smi((int32_t)(h->sp - seg));
    }
    vm->nhandlers -= nh;
    g->nhandlers = nh;
    for (k = 0; k < nu; k++) {
        u                           = vm->open_upvals;
        g->upvals->items[2 * k]     = px_from_ptr(u);
        g->upvals->items[2 * k + 1] = px_from_smi((int32_t)(u->v - seg));
        u->closed                   = *u->v;
        u->v                        = &u->closed;
        vm->open_upvals             = u->next;
        u->next                     = NULL;
    }
    g->nupvals = nu;
    return 0;
fail:
    px_pop_roots(vm, 1);
    return -1;
}

static PxValue run(PxVM *vm, uint32_t entry, int throw_now);

/* SetFunctionName for a method with a computed key (the compiler names the
 * others): "[description]" for a symbol, and "get "/"set " in front for an
 * accessor. An own `name` property over the one the function computes. */
static int set_computed_name(PxVM *vm, PxValue fn, PxValue key, int mk) {
    PxValue n, p;
    int     r;
    if (px_type_of(fn) != PX_T_CLOSURE) return 0;
    PX_ROOT(vm, fn);
    PX_ROOT(vm, key);
    /* every string is rooted while the next one is allocated */
    if (px_is_ptr(key) && px_type_of(key) == PX_T_SYMBOL) {
        PxValue d = ((PxSymbol *)px_ptr(key))->description;
        if (d == PX_UNDEFINED) n = vm->atom[PX_ATOM_empty];
        else {
            PX_ROOT(vm, d);
            p = px_str_from_cstr(vm, "[");
            PX_ROOT(vm, p);
            n = p == PX_EXCEPTION ? p : px_str_concat(vm, p, d);
            PX_ROOT(vm, n);
            p = n == PX_EXCEPTION ? n : px_str_from_cstr(vm, "]");
            PX_ROOT(vm, p);
            n = p == PX_EXCEPTION ? p : px_str_concat(vm, n, p);
            px_pop_roots(vm, 4);
        }
    } else {
        n = px_key_to_value(vm, key);
    }
    if (n != PX_EXCEPTION && mk != PX_MK_METHOD) {
        PX_ROOT(vm, n);
        p = px_str_from_cstr(vm, mk == PX_MK_GET ? "get " : "set ");
        PX_ROOT(vm, p);
        n = p == PX_EXCEPTION ? p : px_str_concat(vm, p, n);
        px_pop_roots(vm, 2);
    }
    r = n == PX_EXCEPTION ? -1 : px_define(vm, fn, vm->atom[PX_ATOM_name], n, PX_ATTR_CONFIGURABLE);
    px_pop_roots(vm, 2);
    return r < 0 ? -1 : 0;
}

PxValue px_gen_resume(PxVM *vm, PxGen *g, PxValue v, int mode, int *done) {
    PxValue *seg = vm->sp, r;
    PxFrame *f;
    uint32_t k;
    int      throw_now = 0;

    *done = 0;
    if (g->state == PX_GEN_RUNNING) return px_throw_error(vm, PX_TYPE_ERROR, "generator is already running");
    if (g->state == PX_GEN_DONE || (g->state == PX_GEN_START && mode != PX_RESUME_NEXT) ||
        (mode == PX_RESUME_RETURN && g->nhandlers == 0)) {
        /* nothing to run: no finally block can be active */
        g->state  = PX_GEN_DONE;
        g->saved  = NULL;
        *done     = 1;
        if (mode == PX_RESUME_THROW) return px_throw(vm, v);
        return mode == PX_RESUME_RETURN ? v : PX_UNDEFINED;
    }
    if (vm->native_depth >= vm->max_native_depth)
        return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded (native)");
    if (vm->nframes >= vm->max_frames || seg + g->nsaved + g->fn->proto->max_stack + 8 >= vm->stack_end)
        return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
    memcpy(seg, g->saved->items, g->nsaved * sizeof(PxValue));
    for (k = 0; k < g->nupvals; k++) {
        PxUpval *u = (PxUpval *)px_ptr(g->upvals->items[2 * k]);
        PxValue *s = seg + px_smi(g->upvals->items[2 * k + 1]);
        *s         = u->closed;
        u->v       = s;
        open_list_insert(vm, u);
    }
    f               = &vm->frames[vm->nframes++];
    f->fn           = g->fn;
    f->pc           = g->fn->proto->code->data + g->pc_off;
    f->base         = seg + 2;
    f->callee       = seg;
    f->this_val     = g->this_val;
    f->new_target   = g->new_target;
    f->gen          = g;
    f->argc         = g->argc;
    f->is_construct = 0;
    f->is_boundary  = 1;
    for (k = 0; k < g->nhandlers; k++) {
        PxHandler *h = &vm->handlers[vm->nhandlers++];
        h->frame     = vm->nframes - 1;
        h->pc        = g->fn->proto->code->data + px_smi(g->handlers->items[2 * k]);
        h->sp        = seg + px_smi(g->handlers->items[2 * k + 1]);
    }
    vm->sp = seg + g->nsaved;
    if (g->state == PX_GEN_SUSPENDED) {
        if (mode == PX_RESUME_NEXT) *vm->sp++ = v;
        else if (mode == PX_RESUME_RETURN) {
            g->ret_value  = v; /* its finally blocks run first */
            vm->exception = PX_RETURN_MARK;
            throw_now     = 1;
        } else {
            vm->exception = v;
            throw_now     = 1;
        }
    }
    g->state = PX_GEN_RUNNING;
    vm->native_depth++;
    r = run(vm, vm->nframes - 1, throw_now);
    vm->native_depth--;
    if (vm->suspended) {
        vm->suspended = 0;
        g->state      = PX_GEN_SUSPENDED;
        return r;
    }
    if (r == PX_EXCEPTION && vm->exception == PX_RETURN_MARK) {
        vm->exception = PX_UNDEFINED;
        r             = g->ret_value;
    }
    g->ret_value = PX_UNDEFINED;
    g->state = PX_GEN_DONE;
    g->saved = NULL;
    *done    = 1;
    return r;
}

static PxValue async_fulfilled(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    px_async_step(vm, (PxGen *)px_ptr(vm->native_data), px_arg(argc, argv, 0), PX_RESUME_NEXT);
    return vm->uncatchable ? PX_EXCEPTION : PX_UNDEFINED;
}

static PxValue async_rejected(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    px_async_step(vm, (PxGen *)px_ptr(vm->native_data), px_arg(argc, argv, 0), PX_RESUME_THROW);
    return vm->uncatchable ? PX_EXCEPTION : PX_UNDEFINED;
}

/* Runs an async function until its next await (or its end), and settles
 * or subscribes accordingly. */
void px_async_step(PxVM *vm, PxGen *g, PxValue v, int mode) {
    PxValue gv = px_from_ptr(g), r, p, onf, onr;
    int     done;
    PX_ROOT(vm, gv);
resume:
    r = px_gen_resume(vm, g, v, mode, &done);
    if (r == PX_EXCEPTION) {
        if (!vm->uncatchable) {
            PxValue exc   = vm->exception;
            vm->exception = PX_UNDEFINED;
            px_promise_reject(vm, g->promise, exc);
        }
        px_pop_roots(vm, 1);
        return;
    }
    if (done) {
        px_promise_resolve(vm, g->promise, r);
        px_pop_roots(vm, 1);
        return;
    }
    /* await r */
    PX_ROOT(vm, r);
    p = px_promise_resolved(vm, r);
    if (p == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, p);
    onf = px_make_native_data(vm, async_fulfilled, "", 1, gv);
    if (onf == PX_EXCEPTION) goto fail2;
    PX_ROOT(vm, onf);
    onr = px_make_native_data(vm, async_rejected, "", 1, gv);
    if (onr == PX_EXCEPTION || px_promise_then_native(vm, p, onf, onr) < 0) {
        px_pop_roots(vm, 1);
        goto fail2;
    }
    px_pop_roots(vm, 4);
    return;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 1);
    if (vm->uncatchable) {
        px_pop_roots(vm, 1);
        return;
    }
    /* Await itself threw (PromiseResolve reads a promise's `constructor`):
     * the exception is thrown at the await */
    v             = vm->exception;
    vm->exception = PX_UNDEFINED;
    mode          = PX_RESUME_THROW;
    goto resume;
}

/* A call to a generator or async function: the frame has been pushed;
 * turn it into a PxGen instead of running it. Returns the generator (or
 * the async function's promise), with the frame popped. */
static PxValue start_coroutine(PxVM *vm, PxFrame *f) {
    PxProto *p = f->fn->proto;
    PxGen   *g;
    PxValue  gv, promise = PX_UNDEFINED, *callee = f->callee;
    int      is_async = (p->flags & PX_PROTO_ASYNC) != 0;

    int      is_agen  = is_async && (p->flags & PX_PROTO_GENERATOR);

    if (is_agen) is_async = 0; /* made like a generator; driven by px_asyncgen.c */
    if (is_async) {
        promise = px_promise_new(vm);
        if (promise == PX_EXCEPTION) goto fail;
    }
    PX_ROOT(vm, promise);
    /* a generator gets the function's `prototype` once its parameters are
     * bound (below), as the spec orders it */
    g = (PxGen *)px_obj_new(vm, PX_T_GENERATOR, sizeof(PxGen),
                            vm->protos[is_async ? PX_PROTO_OBJECT : is_agen ? PX_PROTO_ASYNCGENOBJ : PX_PROTO_GENOBJ]);
    if (!g) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    gv            = px_from_ptr(g);
    g->fn         = f->fn;
    g->this_val   = f->this_val;
    g->new_target = f->new_target;
    g->argc       = f->argc;
    g->promise    = promise;
    g->is_async   = (uint8_t)(is_agen ? 2 : is_async);
    g->ret_value  = PX_UNDEFINED;
    g->state      = PX_GEN_START;
    g->this_val   = f->this_val;
    PX_ROOT(vm, gv);
    if (gen_save(vm, g, f, vm->sp) < 0) {
        px_pop_roots(vm, 2);
        goto fail;
    }
    close_upvals(vm, f->base); /* nothing is open into it now, but be sure */
    vm->nframes--;
    vm->sp = callee;
    if (is_async) {
        px_async_step(vm, g, PX_UNDEFINED, PX_RESUME_NEXT);
        px_pop_roots(vm, 2);
        if (vm->uncatchable) return PX_EXCEPTION;
        return promise;
    }
    /* A generator binds its parameters now, up to OP_GEN_START: their
     * errors are the call's. Then it waits for its first next(). */
    {
        int     done;
        PxValue proto = px_gen_resume(vm, g, PX_UNDEFINED, PX_RESUME_NEXT, &done);
        if (proto != PX_EXCEPTION) {
            if (!done) g->state = PX_GEN_START;
            proto    = px_get(vm, px_from_ptr(g->fn), vm->atom[PX_ATOM_prototype]);
        }
        if (proto == PX_EXCEPTION || (px_is_obj(proto) && px_set_proto(vm, gv, proto) < 0)) gv = PX_EXCEPTION;
    }
    px_pop_roots(vm, 2);
    return gv;
fail:
    unwind_frames(vm, vm->nframes - 1);
    vm->sp = callee;
    return PX_EXCEPTION;
}

/* ------------------------------------------------------------ calls */

/* Shifts a bound function's arguments into a call laid out at callee[..]:
 * returns the new argc, or -1 if the stack is full. */
static int expand_bound(PxVM *vm, PxValue *callee, int argc) {
    PxBound *b = (PxBound *)px_ptr(callee[1]);
    int      i, n = (int)b->nargs;
    if (callee + 2 + argc + n + 1 >= vm->stack_end) return -1;
    for (i = argc - 1; i >= 0; i--) callee[2 + n + i] = callee[2 + i];
    for (i = 0; i < n; i++) callee[2 + i] = b->args->items[i];
    callee[0] = b->this_val;
    callee[1] = b->target;
    return argc + n;
}

PxValue px_call(PxVM *vm, PxValue fn, PxValue this_val, int argc, PxValue *argv) {
    PxValue *callee = vm->sp, r;
    int      i;

    /* The common case first, with the fewest checks: a plain JS function
     * (the host's callbacks: timers, input, UI events, promise jobs). The
     * same steps as the general path's closure case below. */
    if (px_is_ptr(fn) && px_type_of(fn) == PX_T_CLOSURE) {
        PxClosure *c = (PxClosure *)px_ptr(fn);
        if (!(c->obj.flags & PX_OBJ_CLASS_CTOR) && !(c->proto->flags & (PX_PROTO_GENERATOR | PX_PROTO_ASYNC)) &&
            vm->native_depth < vm->max_native_depth && callee + 2 + argc + 8 < vm->stack_end) {
            callee[0] = this_val;
            callee[1] = fn;
            for (i = 0; i < argc; i++) callee[2 + i] = argv[i];
            vm->sp = callee + 2 + argc;
            if (push_frame(vm, c, callee, argc, this_val, PX_UNDEFINED, 0, 1) < 0) {
                vm->sp = callee;
                return PX_EXCEPTION;
            }
            PX_PROF(vm->prof.native_to_js++);
            vm->native_depth++;
            r = run(vm, vm->nframes - 1, 0);
            vm->native_depth--;
            vm->sp = callee;
            return r;
        }
    }
    if (!px_is_callable(fn)) return px_throw_error(vm, PX_TYPE_ERROR, "not a function");
    if (callee + 2 + argc + 8 >= vm->stack_end)
        return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
    callee[0] = this_val;
    callee[1] = fn;
    for (i = 0; i < argc; i++) callee[2 + i] = argv[i];
    vm->sp = callee + 2 + argc;
    while (px_type_of(callee[1]) == PX_T_BOUND) {
        argc = expand_bound(vm, callee, argc);
        if (argc < 0) {
            vm->sp = callee;
            return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
        }
        vm->sp = callee + 2 + argc;
    }
    if (px_type_of(callee[1]) == PX_T_NATIVE) {
        r      = call_native(vm, (PxNative *)px_ptr(callee[1]), callee[0], argc, callee + 2, PX_UNDEFINED);
        vm->sp = callee;
        return r;
    }
    if (px_type_of(callee[1]) == PX_T_PROXY) {
        r      = px_proxy_call(vm, callee[1], callee[0], argc, callee + 2);
        vm->sp = callee;
        return r;
    }
    {
        PxClosure *c = (PxClosure *)px_ptr(callee[1]);
        if (c->obj.flags & PX_OBJ_CLASS_CTOR) {
            vm->sp = callee;
            return px_throw_error(vm, PX_TYPE_ERROR, "class constructors must be called with new");
        }
        if (vm->native_depth >= vm->max_native_depth) {
            vm->sp = callee;
            return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded (native)");
        }
        if (push_frame(vm, c, callee, argc, callee[0], PX_UNDEFINED, 0, 1) < 0) {
            vm->sp = callee;
            return PX_EXCEPTION;
        }
        if (c->proto->flags & (PX_PROTO_GENERATOR | PX_PROTO_ASYNC)) {
            r      = start_coroutine(vm, &vm->frames[vm->nframes - 1]);
            vm->sp = callee;
            return r;
        }
        PX_PROF(vm->prof.native_to_js++);
        vm->native_depth++;
        r = run(vm, vm->nframes - 1, 0);
        vm->native_depth--;
        vm->sp = callee;
        return r;
    }
}

/* The `this` object for `new`: prototype from newTarget.prototype. */
static PxValue construct_this(PxVM *vm, PxValue new_target) {
    PxValue   proto;
    PxObject *o;
    PX_ROOT(vm, new_target);
    proto = px_get(vm, new_target, vm->atom[PX_ATOM_prototype]);
    if (proto == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return proto;
    }
    if (!px_is_obj(proto)) proto = vm->protos[PX_PROTO_OBJECT];
    o = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), proto);
    px_pop_roots(vm, 1);
    return o ? px_from_ptr(o) : PX_EXCEPTION;
}

/* After a native constructor returned `r` for a subclass (new_target is
 * not the native itself), give r the subclass's prototype. */
static PxValue adopt_prototype(PxVM *vm, PxValue r, PxValue ctor, PxValue new_target) {
    PxValue proto;
    if (new_target == ctor || !px_is_obj(r)) return r;
    PX_ROOT(vm, r);
    proto = px_get(vm, new_target, vm->atom[PX_ATOM_prototype]);
    if (proto == PX_EXCEPTION || (px_is_obj(proto) && px_set_proto(vm, r, proto) < 0)) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    return r;
}

PxValue px_construct_nt(PxVM *vm, PxValue ctor, int argc, PxValue *argv, PxValue new_target) {
    PxValue *callee = vm->sp, obj = PX_UNDEFINED, r;
    int      i;
    if (callee + 2 + argc + 8 >= vm->stack_end)
        return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
    for (i = 0; i < argc; i++) callee[2 + i] = argv[i];
    /* a bound function's [[Construct]]: its bound arguments go first */
    while (px_is_obj(ctor) && px_type_of(ctor) == PX_T_BOUND) {
        PxBound *b = (PxBound *)px_ptr(ctor);
        int      n = (int)b->nargs;
        if (callee + 2 + argc + n + 8 >= vm->stack_end)
            return px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
        memmove(callee + 2 + n, callee + 2, (size_t)argc * sizeof(PxValue));
        for (i = 0; i < n; i++) callee[2 + i] = b->args->items[i];
        argc += n;
        if (new_target == ctor) new_target = b->target;
        ctor = b->target;
    }
    if (!px_is_constructor(ctor)) return px_throw_error(vm, PX_TYPE_ERROR, "not a constructor");
    callee[0] = PX_UNDEFINED;
    callee[1] = ctor;
    vm->sp    = callee + 2 + argc;
    if (px_type_of(ctor) == PX_T_PROXY) {
        r      = px_proxy_construct(vm, ctor, argc, callee + 2, new_target);
        vm->sp = callee;
        return r;
    }
    PX_ROOT(vm, new_target);
    if (!(px_type_of(ctor) == PX_T_CLOSURE && (((PxClosure *)px_ptr(ctor))->proto->flags & PX_PROTO_DERIVED))) {
        obj = construct_this(vm, new_target);
        if (obj == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            vm->sp = callee;
            return obj;
        }
    }
    callee[0] = obj;
    if (px_type_of(ctor) == PX_T_NATIVE) {
        r      = call_native(vm, (PxNative *)px_ptr(ctor), obj, argc, callee + 2, new_target);
        vm->sp = callee;
        px_pop_roots(vm, 1);
        if (r == PX_EXCEPTION) return r;
        return px_is_obj(r) ? adopt_prototype(vm, r, ctor, new_target) : obj;
    }
    px_pop_roots(vm, 1);
    if (push_frame(vm, (PxClosure *)px_ptr(ctor), callee, argc, obj, new_target, 1, 1) < 0) {
        vm->sp = callee;
        return PX_EXCEPTION;
    }
    vm->native_depth++;
    r = run(vm, vm->nframes - 1, 0);
    vm->native_depth--;
    vm->sp = callee;
    return r;
}

PxValue px_construct(PxVM *vm, PxValue ctor, int argc, PxValue *argv) {
    return px_construct_nt(vm, ctor, argc, argv, ctor);
}

/* ------------------------------------------------------------ iteration */

static PxIter *iter_alloc(PxVM *vm, int kind, PxValue target) {
    PxIter *it;
    PX_ROOT(vm, target);
    it = (PxIter *)px_alloc(vm, PX_T_ITER, sizeof(PxIter));
    px_pop_roots(vm, 1);
    if (!it) return NULL;
    it->kind    = (uint32_t)kind;
    it->target  = target;
    it->next_fn = PX_UNDEFINED;
    return it;
}

/* for-in: own and inherited enumerable string keys, each once. */
static PxValue make_keys_iter(PxVM *vm, PxValue target) {
    PxVec   *all = NULL;
    PxValue  allv = 0, cur = target;
    PxIter  *it;
    uint32_t n = 0;
    PX_ROOT(vm, target);
    PX_ROOT(vm, allv);
    while (px_is_obj(cur) || (cur == target && px_is_str(cur))) {
        uint32_t cnt, i;
        PxVec   *keys = px_own_keys(vm, cur, 1, &cnt);
        PxValue  kv;
        if (!keys) goto fail;
        kv = px_from_ptr(keys);
        PX_ROOT(vm, kv);
        for (i = 0; i < cnt; i++) {
            uint32_t j;
            int      dup = 0;
            for (j = 0; j < n; j++) {
                int eq = px_str_eq(vm, all->items[j], keys->items[i]);
                if (eq < 0) {
                    px_pop_roots(vm, 1);
                    goto fail;
                }
                if (eq) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (!all || n >= all->cap) {
                PxVec *g = px_vec_grow(vm, all, n + 1);
                if (!g) {
                    px_pop_roots(vm, 1);
                    goto fail;
                }
                all  = g;
                allv = px_from_ptr(all);
            }
            all->items[n++] = keys->items[i];
        }
        px_pop_roots(vm, 1);
        cur = px_proto_of(vm, cur);
        if (!px_is_obj(cur)) break;
    }
    it = iter_alloc(vm, PX_ITK_KEYS, target);
    if (!it) goto fail;
    it->keys  = all;
    it->count = n;
    px_pop_roots(vm, 2);
    return px_from_ptr(it);
fail:
    px_pop_roots(vm, 2);
    return PX_EXCEPTION;
}

/* Would iterating array a run the original array iterator? (Then it can
 * be read directly.) */
static int array_iteration_intact(PxVM *vm, PxObject *a) {
    PxObject *ap = (PxObject *)px_ptr(vm->protos[PX_PROTO_ARRAY]);
    PxValue  *s;
    if (a->shape->proto != ap || px_own_slot(vm, a, vm->sym_iterator, NULL)) return 0;
    s = px_own_slot(vm, ap, vm->sym_iterator, NULL);
    if (!s || *s != vm->array_values) return 0;
    s = px_own_slot(vm, (PxObject *)px_ptr(vm->protos[PX_PROTO_ARRAY_ITERATOR]), vm->atom[PX_ATOM_next], NULL);
    return s && *s == vm->array_iter_next;
}

/* for-of / spread / destructuring source: arrays and strings directly,
 * everything else through [Symbol.iterator]. */
static PxValue make_values_iter(PxVM *vm, PxValue target) {
    PxIter *it;
    if (px_is_str(target)) {
        it = iter_alloc(vm, PX_ITK_STRING, target);
        return it ? px_from_ptr(it) : PX_EXCEPTION;
    }
    if (px_is_obj(target) && px_type_of(target) == PX_T_ARRAY && array_iteration_intact(vm, (PxObject *)px_ptr(target))) {
        it = iter_alloc(vm, PX_ITK_ARRAY, target);
        return it ? px_from_ptr(it) : PX_EXCEPTION;
    }
    {
        PxValue iter = px_get_iterator(vm, target), next;
        if (iter == PX_EXCEPTION) return iter;
        PX_ROOT(vm, iter);
        next = px_get(vm, iter, vm->atom[PX_ATOM_next]);
        if (next == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            return next;
        }
        PX_ROOT(vm, next);
        it = iter_alloc(vm, PX_ITK_PROTOCOL, iter);
        px_pop_roots(vm, 2);
        if (!it) return PX_EXCEPTION;
        it->next_fn = next;
        return px_from_ptr(it);
    }
}

/* 1 with *out set, 0 when done, -1 on exception. */
static int iter_next(PxVM *vm, PxIter *it, PxValue *out) {
    PxValue itv = px_from_ptr(it);
    if (it->done) return 0;
    switch (it->kind) {
    case PX_ITK_KEYS:
        while (it->index < it->count) {
            PxValue k = it->keys->items[it->index++];
            PxValue key;
            if (!px_is_obj(it->target)) {
                *out = k;
                return 1;
            }
            PX_ROOT(vm, itv);
            key = px_intern(vm, k);
            px_pop_roots(vm, 1);
            if (key == PX_EXCEPTION) return -1;
            if (px_has(vm, it->target, key)) { /* skip keys deleted meanwhile */
                *out = k;
                return 1;
            }
        }
        it->done = 1;
        return 0;
    case PX_ITK_ARRAY: {
        PxArray *a = (PxArray *)px_ptr(it->target);
        PxValue  v;
        if (it->index >= a->length) {
            it->done = 1;
            return 0;
        }
        v = px_get(vm, it->target, px_from_smi((int32_t)it->index));
        if (v == PX_EXCEPTION) return -1;
        it->index++;
        *out = v;
        return 1;
    }
    case PX_ITK_STRING: {
        PxString *s = px_str_flat(vm, it->target);
        uint16_t  u[2];
        uint32_t  n = 1;
        if (!s) return -1;
        if (it->index >= s->len) {
            it->done = 1;
            return 0;
        }
        u[0] = px_str_at(s, it->index);
        if (u[0] >= 0xD800 && u[0] <= 0xDBFF && it->index + 1 < s->len) {
            u[1] = px_str_at(s, it->index + 1);
            if (u[1] >= 0xDC00 && u[1] <= 0xDFFF) n = 2;
        }
        it->index += n;
        *out = px_str_new_u16(vm, u, n);
        return *out == PX_EXCEPTION ? -1 : 1;
    }
    default: {
        PxValue r, d;
        PX_ROOT(vm, itv);
        r = px_call(vm, it->next_fn, it->target, 0, NULL);
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
            it->done = 1;
            px_pop_roots(vm, 2);
            return 0;
        }
        *out = px_get(vm, r, vm->atom[PX_ATOM_value]);
        px_pop_roots(vm, 2);
        return *out == PX_EXCEPTION ? -1 : 1;
    fail:
        it->done = 1;
        px_pop_roots(vm, 1);
        return -1;
    }
    }
}

static int iter_close(PxVM *vm, PxIter *it) {
    if (it->done || it->kind != PX_ITK_PROTOCOL) return 0;
    it->done = 1;
    return px_iterator_close(vm, it->target);
}

/* ------------------------------------------------------------ operators */

static PxValue arith(PxVM *vm, PxOp op, PxValue a, PxValue b) {
    double x, y, r;
    if (px_is_num(a) && px_is_num(b)) {
        /* the common case: no conversions, nothing to root */
        x = px_num(a);
        y = px_num(b);
        if (op == OP_ADD) return px_number(vm, x + y);
        goto compute;
    }
    if (op == OP_ADD) {
        PxValue pa, pb;
        PX_ROOT(vm, a);
        PX_ROOT(vm, b);
        pa = px_to_primitive(vm, a, 0);
        if (pa == PX_EXCEPTION) goto fail;
        PX_ROOT(vm, pa);
        pb = px_to_primitive(vm, b, 0);
        if (pb == PX_EXCEPTION) {
            px_pop_roots(vm, 1);
            goto fail;
        }
        PX_ROOT(vm, pb);
        if (px_is_str(pa) || px_is_str(pb)) {
            PxValue sa = px_to_string(vm, pa), sb;
            if (sa == PX_EXCEPTION) goto fail2;
            PX_ROOT(vm, sa);
            sb = px_to_string(vm, pb);
            if (sb == PX_EXCEPTION) {
                px_pop_roots(vm, 1);
                goto fail2;
            }
            PX_ROOT(vm, sb);
            {
                PxValue res = px_str_concat(vm, sa, sb);
                px_pop_roots(vm, 6);
                return res;
            }
        }
        if (px_to_number(vm, pa, &x) < 0 || px_to_number(vm, pb, &y) < 0) goto fail2;
        px_pop_roots(vm, 4);
        return px_number(vm, x + y);
    fail2:
        px_pop_roots(vm, 2);
    fail:
        px_pop_roots(vm, 2);
        return PX_EXCEPTION;
    }
    PX_ROOT(vm, b);
    if (px_to_number(vm, a, &x) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    if (px_to_number(vm, b, &y) < 0) return PX_EXCEPTION;
compute:
    switch (op) {
    case OP_SUB: r = x - y; break;
    case OP_MUL: r = x * y; break;
    case OP_DIV: r = x / y; break;
    case OP_MOD: r = fmod(x, y); break;
    case OP_POW: r = (isnan(y) || (fabs(x) == 1 && isinf(y))) ? NAN : pow(x, y); break;
    default: r = NAN; break;
    }
    return px_number(vm, r);
}

static PxValue bitop(PxVM *vm, PxOp op, PxValue a, PxValue b) {
    int32_t x, y;
    PX_ROOT(vm, b);
    if (px_to_int32(vm, a, &x) < 0) {
        px_pop_roots(vm, 1);
        return PX_EXCEPTION;
    }
    px_pop_roots(vm, 1);
    if (px_to_int32(vm, b, &y) < 0) return PX_EXCEPTION;
    switch (op) {
    case OP_BAND: return px_int(vm, x & y);
    case OP_BOR: return px_int(vm, x | y);
    case OP_BXOR: return px_int(vm, x ^ y);
    case OP_SHL: return px_int(vm, (int32_t)((uint32_t)x << (y & 31)));
    case OP_SAR: return px_int(vm, x >> (y & 31));
    case OP_SHR: return px_number(vm, (double)((uint32_t)x >> (y & 31)));
    default: return PX_UNDEFINED;
    }
}

/* <, <=, >, >= : returns 0/1, or -1 on exception. NaN compares false. */
static int compare(PxVM *vm, PxOp op, PxValue a, PxValue b) {
    PxValue pa, pb;
    double  x, y;
    if (px_is_num(a) && px_is_num(b)) {
        x = px_num(a);
        y = px_num(b);
        goto numbers;
    }
    PX_ROOT(vm, a);
    PX_ROOT(vm, b);
    pa = px_to_primitive(vm, a, 2);
    if (pa == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, pa);
    pb = px_to_primitive(vm, b, 2);
    if (pb == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    if (px_is_str(pa) && px_is_str(pb)) {
        int c;
        PX_ROOT(vm, pb);
        if (px_str_cmp(vm, pa, pb, &c) < 0) {
            px_pop_roots(vm, 2);
            goto fail;
        }
        px_pop_roots(vm, 4);
        switch (op) {
        case OP_LT: return c < 0;
        case OP_LE: return c <= 0;
        case OP_GT: return c > 0;
        default: return c >= 0;
        }
    }
    PX_ROOT(vm, pb);
    if (px_to_number(vm, pa, &x) < 0 || px_to_number(vm, pb, &y) < 0) {
        px_pop_roots(vm, 2);
        goto fail;
    }
    px_pop_roots(vm, 4);
numbers:
    switch (op) {
    case OP_LT: return x < y;
    case OP_LE: return x <= y;
    case OP_GT: return x > y;
    default: return x >= y;
    }
fail:
    px_pop_roots(vm, 2);
    return -1;
}

static int instance_of(PxVM *vm, PxValue v, PxValue ctor) {
    PxValue proto, cur, h;
    if (!px_is_obj(ctor)) {
        px_throw_error(vm, PX_TYPE_ERROR, "right-hand side of instanceof is not an object");
        return -1;
    }
    PX_ROOT(vm, v);
    PX_ROOT(vm, ctor);
    h = px_get(vm, ctor, vm->sym_has_instance);
    if (h == PX_EXCEPTION) goto fail;
    if (px_is_callable(h) && px_type_of(h) != PX_T_NATIVE) {
        PxValue r = px_call(vm, h, ctor, 1, &v);
        px_pop_roots(vm, 2);
        return r == PX_EXCEPTION ? -1 : px_truthy(r);
    }
    px_pop_roots(vm, 2);
    if (!px_is_callable(ctor)) {
        px_throw_error(vm, PX_TYPE_ERROR, "right-hand side of instanceof is not callable");
        return -1;
    }
    while (px_type_of(ctor) == PX_T_BOUND) ctor = ((PxBound *)px_ptr(ctor))->target;
    if (!px_is_obj(v)) return 0;
    PX_ROOT(vm, v);
    proto = px_get(vm, ctor, vm->atom[PX_ATOM_prototype]);
    px_pop_roots(vm, 1);
    if (proto == PX_EXCEPTION) return -1;
    if (!px_is_obj(proto)) {
        px_throw_error(vm, PX_TYPE_ERROR, "function has no prototype object");
        return -1;
    }
    for (cur = px_proto_of(vm, v); px_is_obj(cur); cur = px_proto_of(vm, cur))
        if (cur == proto) return 1;
    return cur == PX_EXCEPTION ? -1 : 0; /* a proxy's getPrototypeOf can throw */
fail:
    px_pop_roots(vm, 2);
    return -1;
}

static PxValue key_of(PxVM *vm, PxValue k) {
    if (px_is_smi(k) && px_smi(k) >= 0) return k;
    if (px_is_ptr(k) && px_type_of(k) == PX_T_SYMBOL) return k;
    if (!px_is_str(k)) {
        k = px_to_primitive(vm, k, 1);
        if (k == PX_EXCEPTION) return k;
        if (px_is_ptr(k) && px_type_of(k) == PX_T_SYMBOL) return k;
        k = px_to_string(vm, k);
        if (k == PX_EXCEPTION) return k;
    }
    return px_intern(vm, k);
}

static PxValue global_lookup(PxVM *vm, PxValue name, int *found) {
    uint32_t  attrs, h = ((uint32_t)name >> 3) & 63;
    PxObject *g = (PxObject *)px_ptr(vm->global);
    PxValue  *slot;
    if (g->shape->flags & PX_SHAPE_DICT) {
        /* the hint: the entry this name had last time, if it still has it */
        PxDict  *d = (PxDict *)g->slots;
        uint32_t i = vm->global_hint[h].index;
        if (vm->global_hint[h].name == name && i < d->used && d->entries[i].key == name &&
            !(d->entries[i].attrs & PX_ATTR_ACCESSOR)) {
            *found = 1;
            return d->entries[i].value;
        }
        slot = px_own_slot(vm, g, name, &attrs);
        if (slot && !(attrs & PX_ATTR_ACCESSOR)) {
            vm->global_hint[h].name  = name;
            vm->global_hint[h].index = (uint32_t)((PxDictEntry *)(void *)((char *)slot - offsetof(PxDictEntry, value)) - d->entries);
            *found = 1;
            return *slot;
        }
    } else {
        slot = px_own_slot(vm, g, name, &attrs);
        if (slot && !(attrs & PX_ATTR_ACCESSOR)) {
            *found = 1;
            return *slot;
        }
    }
    *found = px_has(vm, vm->global, name);
    return *found ? px_get(vm, vm->global, name) : PX_UNDEFINED;
}

/* Spreads an array-like/iterable onto the stack at sp. Returns the count,
 * or -1 (exception). */
static int spread_onto(PxVM *vm, PxValue src, PxValue *sp) {
    int n = 0;
    if (px_is_obj(src) && px_type_of(src) == PX_T_ARRAY) {
        PxArray *a = (PxArray *)px_ptr(src);
        uint32_t i;
        if (sp + a->length + 8 >= vm->stack_end) {
            px_throw_error(vm, PX_RANGE_ERROR, "too many arguments");
            return -1;
        }
        for (i = 0; i < a->length; i++) {
            PxValue v = px_get(vm, src, px_from_smi((int32_t)i));
            if (v == PX_EXCEPTION) return -1;
            sp[n++] = v;
        }
        return n;
    }
    px_throw_error(vm, PX_INTERNAL_ERROR, "spread: expected an array");
    return -1;
}

static int append_all(PxVM *vm, PxValue arr, PxValue src) {
    PxValue itv, v;
    int     r;
    PX_ROOT(vm, arr);
    itv = make_values_iter(vm, src);
    if (itv == PX_EXCEPTION) {
        px_pop_roots(vm, 1);
        return -1;
    }
    PX_ROOT(vm, itv);
    while ((r = iter_next(vm, (PxIter *)px_ptr(itv), &v)) > 0)
        if (px_array_push(vm, arr, v) < 0) {
            r = -1;
            break;
        }
    px_pop_roots(vm, 2);
    return r;
}

static int copy_props(PxVM *vm, PxValue dst, PxValue src, PxValue exclude) {
    PxVec   *keys;
    PxValue  kv;
    uint32_t n, i;
    if (src == PX_UNDEFINED || src == PX_NULL) return 0;
    PX_ROOT(vm, dst);
    PX_ROOT(vm, src);
    /* own enumerable keys, symbols included (CopyDataProperties) */
    keys = px_own_keys(vm, src, PX_KEYS_ENUMERABLE | PX_KEYS_SYMBOLS, &n);
    if (!keys) goto fail;
    kv = px_from_ptr(keys);
    PX_ROOT(vm, kv);
    for (i = 0; i < n; i++) {
        PxValue k = px_intern(vm, keys->items[i]), v;
        if (k == PX_EXCEPTION) goto fail2;
        if (exclude != PX_UNDEFINED) {
            /* the excluded keys are in key form too (interned, or index SMIs) */
            PxArray *ex = (PxArray *)px_ptr(exclude);
            uint32_t j;
            int      skip = 0;
            for (j = 0; j < ex->length; j++)
                if (ex->elems->items[j] == k) {
                    skip = 1;
                    break;
                }
            if (skip) continue;
        }
        v = px_get(vm, src, k);
        if (v == PX_EXCEPTION || px_define(vm, dst, k, v, PX_ATTR_DEFAULT) < 0) goto fail2;
    }
    px_pop_roots(vm, 3);
    return 0;
fail2:
    px_pop_roots(vm, 1);
fail:
    px_pop_roots(vm, 2);
    return -1;
}

/* ------------------------------------------------------------ private names */

/* obj.#x: the own slot (see px_private_slot), or NULL with a TypeError. */
static PxValue *private_slot(PxVM *vm, PxValue obj, PxValue key, uint32_t *attrs) {
    PxValue *slot = px_is_obj(obj) ? px_private_slot(vm, (PxObject *)px_ptr(obj), key, attrs) : NULL;
    if (!slot) {
        char    name[64] = "";
        PxValue d        = ((PxSymbol *)px_ptr(key))->description;
        if (px_is_str(d)) px_str_to_utf8(vm, d, name, sizeof name);
        px_throw_error(vm, PX_TYPE_ERROR, "#%s is not a member of this object (its class did not add it)", name);
    }
    return slot;
}

static PxValue private_get(PxVM *vm, PxValue obj, PxValue key) {
    uint32_t attrs;
    PxValue *slot = private_slot(vm, obj, key, &attrs);
    if (!slot) return PX_EXCEPTION;
    if (attrs & PX_ATTR_ACCESSOR) {
        PxValue get = ((PxAccessor *)px_ptr(*slot))->get;
        if (get == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "private accessor has no getter");
        return px_call(vm, get, obj, 0, NULL);
    }
    return *slot;
}

static int private_set(PxVM *vm, PxValue obj, PxValue key, PxValue v) {
    uint32_t attrs;
    PxValue *slot = private_slot(vm, obj, key, &attrs);
    if (!slot) return -1;
    if (attrs & PX_ATTR_ACCESSOR) {
        PxValue set = ((PxAccessor *)px_ptr(*slot))->set;
        if (set == PX_UNDEFINED) {
            px_throw_error(vm, PX_TYPE_ERROR, "private accessor has no setter");
            return -1;
        }
        return px_call(vm, set, obj, 1, &v) == PX_EXCEPTION ? -1 : 0;
    }
    if (!(attrs & PX_ATTR_WRITABLE)) {
        px_throw_error(vm, PX_TYPE_ERROR, "private methods are not writable");
        return -1;
    }
    *slot = v;
    return 0;
}

/* Adds #x to obj: a field (kind -1), a method, or one half of an accessor
 * (PX_MK_GET/SET: the other half may be there already). An object gets a
 * class's private elements once: a second time is a TypeError. */
static int private_add(PxVM *vm, PxValue obj, PxValue key, PxValue v, int kind) {
    uint32_t attrs;
    PxValue *slot;
    if (!px_is_obj(obj)) {
        px_throw_error(vm, PX_TYPE_ERROR, "private members can only be added to objects");
        return -1;
    }
    slot = px_private_slot(vm, (PxObject *)px_ptr(obj), key, &attrs);
    if (slot && (kind == PX_MK_GET || kind == PX_MK_SET) && (attrs & PX_ATTR_ACCESSOR)) {
        PxAccessor *acc  = (PxAccessor *)px_ptr(*slot);
        PxValue    *half = kind == PX_MK_GET ? &acc->get : &acc->set;
        if (*half == PX_UNDEFINED && (kind == PX_MK_GET ? acc->set : acc->get) != PX_UNDEFINED) {
            *half = v;
            return 0;
        }
    }
    if (slot) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot initialize a private member twice on the same object");
        return -1;
    }
    switch (kind) {
    case PX_MK_GET: return px_private_add(vm, (PxObject *)px_ptr(obj), key, PX_UNDEFINED, v, PX_UNDEFINED, 0);
    case PX_MK_SET: return px_private_add(vm, (PxObject *)px_ptr(obj), key, PX_UNDEFINED, PX_UNDEFINED, v, 0);
    case PX_MK_METHOD: return px_private_add(vm, (PxObject *)px_ptr(obj), key, v, PX_UNDEFINED, PX_UNDEFINED, 0);
    default: return px_private_add(vm, (PxObject *)px_ptr(obj), key, v, PX_UNDEFINED, PX_UNDEFINED, PX_ATTR_WRITABLE);
    }
}

/* ------------------------------------------------------------ the loop */

#define READ_U8()  (*pc++)
#define READ_S8()  ((int8_t)*pc++)
#define READ_U16() (pc += 2, (uint16_t)(pc[-2] | (pc[-1] << 8)))
#define READ_S16() ((int16_t)READ_U16())
#define PUSH(v)    (*sp++ = (v))
#define POP()      (*--sp)
#define TOP()      (sp[-1])
#define SAVE()     (vm->sp = sp, f->pc = pc)
#define CONSTS     (f->fn->proto->consts->items)
#define THROW()    goto exception
#define CHECK(v)   do { if ((v) == PX_EXCEPTION) THROW(); } while (0)

static PxValue run(PxVM *vm, uint32_t entry, int throw_now) {
    PxFrame       *f  = &vm->frames[vm->nframes - 1];
    const uint8_t *pc = f->pc;
    PxValue       *sp = vm->sp;
    PxOp           op = OP_NOP;
    uint32_t       li = 0; /* the local of ITER_STEP_AT / ITER_REST_AT (OP_WIDE jumps in with its own) */

    if (throw_now) goto exception;
    for (;;) {
        op = (PxOp)*pc++;
        PX_PROF(vm->prof.op_count[op]++);
        switch (op) {
        case OP_NOP: break;
        case OP_UNDEF: PUSH(PX_UNDEFINED); break;
        case OP_NULL: PUSH(PX_NULL); break;
        case OP_TRUE: PUSH(PX_TRUE); break;
        case OP_FALSE: PUSH(PX_FALSE); break;
        case OP_HOLE: PUSH(PX_HOLE); break;
        case OP_INT8: PUSH(px_from_smi(READ_S8())); break;
        case OP_INT16: PUSH(px_from_smi(READ_S16())); break;
        case OP_CONST: PUSH(CONSTS[READ_U16()]); break;
        case OP_THIS: PUSH(f->this_val); break;
        case OP_THIS_CHECK:
            if (f->this_val == PX_HOLE) {
                SAVE();
                px_throw_error(vm, PX_REFERENCE_ERROR, "must call super() before accessing 'this'");
                THROW();
            }
            PUSH(f->this_val);
            break;
        case OP_CALLEE: PUSH(px_from_ptr(f->fn)); break;
        case OP_NEW_TARGET: PUSH(f->new_target); break;
        case OP_POP: sp--; break;
        case OP_DUP: sp[0] = sp[-1]; sp++; break;
        case OP_DUP2: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; break;
        case OP_OVER: sp[0] = sp[-2]; sp++; break;
        case OP_SWAP: {
            PxValue t = sp[-1];
            sp[-1]    = sp[-2];
            sp[-2]    = t;
            break;
        }
        case OP_ROT3: {
            PxValue c = sp[-1];
            sp[-1]    = sp[-2];
            sp[-2]    = sp[-3];
            sp[-3]    = c;
            break;
        }
        case OP_ROT4: {
            PxValue d = sp[-1];
            sp[-1]    = sp[-2];
            sp[-2]    = sp[-3];
            sp[-3]    = sp[-4];
            sp[-4]    = d;
            break;
        }
        case OP_GET_LOCAL: PUSH(f->base[READ_U8()]); break;
        case OP_SET_LOCAL: f->base[READ_U8()] = TOP(); break;
        case OP_PUT_LOCAL: f->base[READ_U8()] = POP(); break;
        case OP_INIT_HOLE: f->base[READ_U8()] = PX_HOLE; break;
        case OP_GET_LOCAL_CHECK: {
            PxValue v = f->base[READ_U8()];
            if (v == PX_HOLE) {
                SAVE();
                px_throw_error(vm, PX_REFERENCE_ERROR, "variable used before its declaration");
                THROW();
            }
            PUSH(v);
            break;
        }
        case OP_SET_LOCAL_CHECK: {
            uint8_t i = READ_U8();
            if (f->base[i] == PX_HOLE) {
                SAVE();
                px_throw_error(vm, PX_REFERENCE_ERROR, "variable assigned before its declaration");
                THROW();
            }
            f->base[i] = TOP();
            break;
        }
        case OP_GET_UPVAL: PUSH(*f->fn->upvals[READ_U8()]->v); break;
        case OP_SET_UPVAL: *f->fn->upvals[READ_U8()]->v = TOP(); break;
        case OP_GET_UPVAL_CHECK: {
            PxValue v = *f->fn->upvals[READ_U8()]->v;
            if (v == PX_HOLE) {
                SAVE();
                px_throw_error(vm, PX_REFERENCE_ERROR, "variable used before its declaration");
                THROW();
            }
            PUSH(v);
            break;
        }
        case OP_SET_UPVAL_CHECK: {
            PxValue *slot = f->fn->upvals[READ_U8()]->v;
            if (*slot == PX_HOLE) {
                SAVE();
                px_throw_error(vm, PX_REFERENCE_ERROR, "variable assigned before its declaration");
                THROW();
            }
            *slot = TOP();
            break;
        }
        case OP_CLOSE_UPVALS: close_upvals(vm, f->base + READ_U8()); break;
        case OP_WIDE: {
            /* a local past slot 255: the op, then the slot in two bytes */
            PxOp     wop = (PxOp)READ_U8();
            uint16_t i   = READ_U16();
            PX_PROF(vm->prof.op_count[wop]++);
            switch (wop) {
            case OP_GET_LOCAL: PUSH(f->base[i]); break;
            case OP_SET_LOCAL: f->base[i] = TOP(); break;
            case OP_PUT_LOCAL: f->base[i] = POP(); break;
            case OP_INIT_HOLE: f->base[i] = PX_HOLE; break;
            case OP_CLOSE_UPVALS: close_upvals(vm, f->base + i); break;
            case OP_GET_LOCAL_CHECK:
                if (f->base[i] == PX_HOLE) {
                    SAVE();
                    px_throw_error(vm, PX_REFERENCE_ERROR, "variable used before its declaration");
                    THROW();
                }
                PUSH(f->base[i]);
                break;
            case OP_SET_LOCAL_CHECK:
                if (f->base[i] == PX_HOLE) {
                    SAVE();
                    px_throw_error(vm, PX_REFERENCE_ERROR, "variable assigned before its declaration");
                    THROW();
                }
                f->base[i] = TOP();
                break;
            case OP_ITER_STEP_AT: li = i; goto iter_step_at;
            case OP_ITER_REST_AT: li = i; goto iter_rest_at;
            default:
                SAVE();
                px_throw_error(vm, PX_INTERNAL_ERROR, "bad wide instruction %d", (int)wop);
                THROW();
            }
            break;
        }
        case OP_THROW_REF: {
            char    msg[160];
            PxValue m = CONSTS[READ_U16()];
            SAVE();
            px_str_to_utf8(vm, m, msg, sizeof msg);
            px_throw_error(vm, PX_REFERENCE_ERROR, "%s", msg);
            THROW();
        }
        case OP_THROW_CONST: {
            char    name[64];
            PxValue n = CONSTS[READ_U16()];
            SAVE();
            px_str_to_utf8(vm, n, name, sizeof name);
            px_throw_error(vm, PX_TYPE_ERROR, "assignment to constant variable '%s'", name);
            THROW();
        }
        case OP_GET_GLOBAL:
        case OP_GET_GLOBAL_TYPEOF: {
            PxValue name = CONSTS[READ_U16()], v;
            int     found;
            SAVE();
            v = global_lookup(vm, name, &found);
            CHECK(v);
            if (!found && op == OP_GET_GLOBAL) {
                char buf[64];
                px_str_to_utf8(vm, name, buf, sizeof buf);
                px_throw_error(vm, PX_REFERENCE_ERROR, "%s is not defined", buf);
                THROW();
            }
            PUSH(v);
            break;
        }
        case OP_SET_GLOBAL: {
            PxValue   name = CONSTS[READ_U16()];
            PxObject *g    = (PxObject *)px_ptr(vm->global);
            if (g->shape->flags & PX_SHAPE_DICT) {
                /* a writable data property where global_lookup last found it */
                PxDict  *d = (PxDict *)g->slots;
                uint32_t h = ((uint32_t)name >> 3) & 63, i = vm->global_hint[h].index;
                if (vm->global_hint[h].name == name && i < d->used && d->entries[i].key == name &&
                    (d->entries[i].attrs & (PX_ATTR_ACCESSOR | PX_ATTR_WRITABLE)) == PX_ATTR_WRITABLE) {
                    d->entries[i].value = TOP();
                    break;
                }
            }
            SAVE();
            if (!px_has(vm, vm->global, name)) {
                char buf[64];
                px_str_to_utf8(vm, name, buf, sizeof buf);
                px_throw_error(vm, PX_REFERENCE_ERROR, "%s is not defined", buf);
                THROW();
            }
            if (px_set(vm, vm->global, name, TOP()) < 0) THROW();
            break;
        }
        case OP_DEF_GLOBAL: {
            PxValue name = CONSTS[READ_U16()], v = TOP();
            SAVE();
            /* a new global var/function is defined (writable, enumerable,
             * not configurable), not set: no setter up the chain runs */
            if (!px_has_own(vm, vm->global, name)) {
                if (px_define(vm, vm->global, name, v, PX_ATTR_WRITABLE | PX_ATTR_ENUMERABLE) < 0) THROW();
            } else if (v != PX_UNDEFINED) {
                if (px_set(vm, vm->global, name, v) < 0) THROW();
            }
            sp--;
            break;
        }
        case OP_GET_PROP:
        case OP_GET_PROP_KEEP: {
            PxValue  key = CONSTS[READ_U16()], obj = TOP(), v;
            uint32_t ici = READ_U16();
            PxIC    *ic  = ici != 0xFFFF ? (PxIC *)(void *)f->fn->proto->ics->data + ici : NULL;
            if (ic && ic->epoch == vm->shape_epoch) {
                /* a primitive looks up from its prototype (cached as that
                 * object's own property) */
                PxObject *o = px_is_obj(obj) ? (PxObject *)px_ptr(obj) : px_prim_proto(vm, obj);
                if (o && o->shape == ic->shape) {
                    if (!ic->holder_shape) {
                        v = *px_slot_at(o, ic->slot);
                        PX_PROF(vm->prof.ic_own_hit++);
                        goto got_prop;
                    }
                    if (o->shape->proto->shape == ic->holder_shape) {
                        v = *px_slot_at(o->shape->proto, ic->slot);
                        PX_PROF(vm->prof.ic_proto_hit++);
                        goto got_prop;
                    }
                }
            }
            if (key == vm->atom[PX_ATOM_length] && px_is_ptr(obj) &&
                (px_type_of(obj) == PX_T_STRING || px_type_of(obj) == PX_T_ROPE)) {
                v = px_from_smi((int32_t)px_str_len(obj));
                goto got_prop;
            }
            SAVE();
            PX_PROF(vm->prof.ic_get_miss++);
            v = px_get_ic(vm, obj, key, ic);
            CHECK(v);
        got_prop:
            if (op == OP_GET_PROP) TOP() = v;
            else PUSH(v);
            break;
        }
        case OP_SET_PROP: {
            PxValue  key = CONSTS[READ_U16()], obj = sp[-2];
            uint32_t ici = READ_U16();
            PxIC    *ic  = ici != 0xFFFF ? (PxIC *)(void *)f->fn->proto->ics->data + ici : NULL;
            if (ic && ic->epoch == vm->shape_epoch && !ic->holder_shape && px_is_obj(obj) &&
                ((PxObject *)px_ptr(obj))->shape == ic->shape) {
                *px_slot_at((PxObject *)px_ptr(obj), ic->slot) = sp[-1];
                PX_PROF(vm->prof.ic_set_hit++);
            } else {
                SAVE();
                PX_PROF(vm->prof.ic_set_miss++);
                if (px_set_ic(vm, obj, key, sp[-1], ic) < 0) THROW();
            }
            sp[-2] = sp[-1];
            sp--;
            break;
        }
        case OP_GET_ELEM:
        case OP_GET_ELEM_KEEP: {
            PxValue obj = sp[-2], k = sp[-1], v;
            SAVE();
            if (px_is_obj(obj) && px_type_of(obj) == PX_T_ARRAY && px_is_smi(k) && px_smi(k) >= 0) {
                PxArray *a = (PxArray *)px_ptr(obj);
                uint32_t i = (uint32_t)px_smi(k);
                if (a->elems && i < a->length && i < a->elems->cap && a->elems->items[i] != PX_HOLE) {
                    v = a->elems->items[i];
                    goto got_elem;
                }
            }
            k = key_of(vm, k);
            CHECK(k);
            sp[-1] = k;
            v      = px_get(vm, obj, k);
            CHECK(v);
        got_elem:
            if (op == OP_GET_ELEM) {
                sp[-2] = v;
                sp--;
            } else {
                sp[-1] = v;
            }
            break;
        }
        case OP_SET_ELEM: {
            PxValue k;
            SAVE();
            k = key_of(vm, sp[-2]);
            CHECK(k);
            sp[-2] = k;
            if (px_set(vm, sp[-3], k, sp[-1]) < 0) THROW();
            sp[-3] = sp[-1];
            sp -= 2;
            break;
        }
        case OP_DELETE_PROP: {
            int r;
            SAVE();
            r = px_delete_strict(vm, TOP(), CONSTS[READ_U16()]);
            if (r < 0) THROW();
            TOP() = px_bool(r);
            break;
        }
        case OP_DELETE_ELEM: {
            PxValue k;
            int     r;
            SAVE();
            k = key_of(vm, sp[-1]);
            CHECK(k);
            sp[-1] = k;
            r      = px_delete_strict(vm, sp[-2], k);
            if (r < 0) THROW();
            sp[-2] = px_bool(r);
            sp--;
            break;
        }
        case OP_NEW_OBJECT: {
            PxValue o;
            SAVE();
            o = px_object_new(vm);
            CHECK(o);
            PUSH(o);
            break;
        }
        case OP_NEW_ARRAY: {
            PxValue a;
            SAVE();
            a = px_array_new(vm, 0);
            CHECK(a);
            PUSH(a);
            break;
        }
        case OP_PACK_ARRAY: {
            int     n = READ_U8(), i;
            PxValue a;
            SAVE();
            a = px_array_new(vm, (uint32_t)n);
            CHECK(a);
            for (i = 0; i < n; i++) ((PxArray *)px_ptr(a))->elems->items[i] = sp[i - n];
            ((PxArray *)px_ptr(a))->length = (uint32_t)n;
            sp -= n;
            PUSH(a);
            break;
        }
        case OP_DEFINE_FIELD: {
            PxValue key = CONSTS[READ_U16()];
            SAVE();
            if (px_define(vm, sp[-2], key, sp[-1], PX_ATTR_DEFAULT) < 0) THROW();
            sp--;
            break;
        }
        case OP_DEFINE_ELEM: {
            PxValue k;
            SAVE();
            k = key_of(vm, sp[-2]);
            CHECK(k);
            sp[-2] = k;
            if (px_define(vm, sp[-3], k, sp[-1], PX_ATTR_DEFAULT) < 0) THROW();
            sp -= 2;
            break;
        }
        case OP_DEFINE_METHOD:
        case OP_DEFINE_METHOD_ELEM: {
            PxValue  key, obj, fn;
            int      kind, r;
            uint32_t attrs;
            if (op == OP_DEFINE_METHOD) {
                key  = CONSTS[READ_U16()];
                kind = READ_U8();
                obj  = sp[-2];
                fn   = sp[-1];
            } else {
                kind = READ_U8();
                SAVE();
                key = key_of(vm, sp[-2]);
                CHECK(key);
                sp[-2] = key;
                obj    = sp[-3];
                fn     = sp[-1];
            }
            SAVE();
            if (px_type_of(fn) == PX_T_CLOSURE) ((PxClosure *)px_ptr(fn))->home = obj;
            if (op == OP_DEFINE_METHOD_ELEM && set_computed_name(vm, fn, key, kind & 3) < 0) THROW();
            attrs = PX_ATTR_CONFIGURABLE | ((kind & PX_MK_ENUM) ? PX_ATTR_ENUMERABLE : 0);
            switch (kind & 3) {
            case PX_MK_GET: r = px_define_accessor(vm, obj, key, fn, PX_UNDEFINED, attrs); break;
            case PX_MK_SET: r = px_define_accessor(vm, obj, key, PX_UNDEFINED, fn, attrs); break;
            default: r = px_define(vm, obj, key, fn, attrs | PX_ATTR_WRITABLE); break;
            }
            if (r < 0) THROW();
            sp -= op == OP_DEFINE_METHOD ? 1 : 2;
            break;
        }
        case OP_APPEND: {
            SAVE();
            if (px_array_push(vm, sp[-2], sp[-1]) < 0) THROW();
            sp--;
            break;
        }
        case OP_APPEND_HOLE: {
            PxArray *a = (PxArray *)px_ptr(TOP());
            SAVE();
            if (px_array_set_length(vm, a, a->length + 1) < 0) THROW();
            break;
        }
        case OP_APPEND_SPREAD:
            SAVE();
            if (append_all(vm, sp[-2], sp[-1]) < 0) THROW();
            sp--;
            break;
        case OP_COPY_PROPS:
            SAVE();
            if (copy_props(vm, sp[-2], sp[-1], PX_UNDEFINED) < 0) THROW();
            sp--;
            break;
        case OP_OBJ_REST: {
            PxValue o;
            SAVE();
            o = px_object_new(vm);
            CHECK(o);
            PUSH(o);
            SAVE();
            if (copy_props(vm, sp[-1], sp[-3], sp[-2]) < 0) THROW();
            sp[-3] = sp[-1];
            sp -= 2;
            break;
        }
        case OP_REQUIRE_OBJ:
            if (TOP() == PX_UNDEFINED || TOP() == PX_NULL) {
                SAVE();
                px_throw_error(vm, PX_TYPE_ERROR, "cannot destructure %s", TOP() == PX_NULL ? "null" : "undefined");
                THROW();
            }
            break;
        case OP_CLOSURE: {
            PxProto   *p = (PxProto *)px_ptr(CONSTS[READ_U16()]);
            PxClosure *c;
            SAVE();
            c = make_closure(vm, p, f);
            if (!c) THROW();
            PUSH(px_from_ptr(c));
            break;
        }
        case OP_CLASS: {
            PxValue   parent = sp[-2], ctor = sp[-1], proto_parent, proto;
            PxObject *po;
            SAVE();
            if (parent == PX_HOLE) proto_parent = vm->protos[PX_PROTO_OBJECT];
            else if (parent == PX_NULL) proto_parent = PX_NULL;
            else {
                if (!px_is_constructor(parent)) {
                    px_throw_error(vm, PX_TYPE_ERROR, "class extends value is not a constructor");
                    THROW();
                }
                proto_parent = px_get(vm, parent, vm->atom[PX_ATOM_prototype]);
                CHECK(proto_parent);
                if (!px_is_obj(proto_parent) && proto_parent != PX_NULL) {
                    px_throw_error(vm, PX_TYPE_ERROR, "class extends value has an invalid prototype");
                    THROW();
                }
                if (px_set_proto(vm, ctor, parent) < 0) THROW();
            }
            PUSH(proto_parent);
            SAVE();
            po = px_obj_new(vm, PX_T_OBJECT, sizeof(PxObject), proto_parent);
            if (!po) THROW();
            proto  = px_from_ptr(po);
            sp[-1] = proto;
            ((PxObject *)px_ptr(ctor))->flags |= PX_OBJ_CLASS_CTOR | PX_OBJ_PROTO_MADE;
            ((PxClosure *)px_ptr(ctor))->home = proto;
            if (px_define(vm, ctor, vm->atom[PX_ATOM_prototype], proto, 0) < 0 ||
                px_define(vm, proto, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0)
                THROW();
            sp[-3] = ctor;
            sp[-2] = proto;
            sp--;
            break;
        }
        case OP_SET_FIELDS: {
            PxValue fn = sp[-1];
            SAVE();
            ((PxClosure *)px_ptr(fn))->home = sp[-2];
            if (px_define(vm, sp[-3], vm->sym_fields, fn, 0) < 0) THROW();
            sp--;
            break;
        }
        case OP_INIT_FIELDS: {
            uint32_t attrs;
            PxValue *slot = px_own_slot(vm, &f->fn->obj, vm->sym_fields, &attrs), r;
            if (slot && px_is_callable(*slot)) {
                SAVE();
                r = px_call(vm, *slot, f->this_val, 0, NULL);
                CHECK(r);
            }
            break;
        }
        case OP_GET_SUPER:
        case OP_GET_SUPER_ELEM: {
            PxValue key, home = f->fn->home, base, v;
            SAVE();
            if (op == OP_GET_SUPER) key = CONSTS[READ_U16()];
            else {
                key = key_of(vm, TOP());
                CHECK(key);
                sp--;
            }
            SAVE();
            if (!px_is_obj(home)) {
                px_throw_error(vm, PX_SYNTAX_ERROR, "'super' keyword unexpected here");
                THROW();
            }
            base = px_proto_of(vm, home);
            if (!px_is_obj(base)) v = PX_UNDEFINED;
            else {
                v = px_get_recv(vm, base, key, f->this_val == PX_HOLE ? PX_UNDEFINED : f->this_val);
                CHECK(v);
            }
            PUSH(v);
            break;
        }
        case OP_GET_SUPER_RECV:
        case OP_GET_SUPER_ELEM_RECV:
        case OP_SET_SUPER:
        case OP_SET_SUPER_ELEM: {
            /* a super reference: base home.[[GetPrototypeOf]](), the this
             * value below the key and value (if any) as the receiver */
            PxValue  home = f->fn->home, base, key, v, r;
            int      set  = op == OP_SET_SUPER || op == OP_SET_SUPER_ELEM;
            int      nin  = op == OP_GET_SUPER_RECV ? 1 : op == OP_SET_SUPER_ELEM ? 3 : 2;
            PxValue *in   = sp - nin;
            SAVE();
            if (op == OP_GET_SUPER_RECV || op == OP_SET_SUPER) {
                key = CONSTS[READ_U16()];
            } else {
                key = key_of(vm, in[1]);
                CHECK(key);
                in[1] = key;
            }
            SAVE();
            if (!px_is_obj(home)) {
                px_throw_error(vm, PX_SYNTAX_ERROR, "'super' keyword unexpected here");
                THROW();
            }
            base = px_proto_of(vm, home);
            if (!px_is_obj(base)) {
                px_throw_error(vm, PX_TYPE_ERROR, "cannot %s a property of super: the prototype is null",
                               set ? "set" : "read");
                THROW();
            }
            if (set) {
                int ok;
                v  = sp[-1];
                ok = px_set_recv(vm, base, key, v, in[0]);
                if (ok < 0) THROW();
                if (ok == 0) {
                    px_throw_error(vm, PX_TYPE_ERROR, "cannot assign to a property through super");
                    THROW();
                }
                r = v;
            } else {
                r = px_get_recv(vm, base, key, in[0]);
                CHECK(r);
            }
            sp     = in + 1;
            sp[-1] = r;
            break;
        }
        case OP_SUPER_CALL:
        case OP_SUPER_CALL_ARRAY: {
            int      argc;
            PxValue *args, parent, r;
            SAVE();
            if (op == OP_SUPER_CALL) {
                argc = READ_U8();
                args = sp - argc;
            } else {
                PxValue arr = POP();
                argc        = spread_onto(vm, arr, sp);
                if (argc < 0) THROW();
                args = sp;
                sp += argc;
            }
            SAVE();
            parent = px_proto_of(vm, px_from_ptr(f->fn));
            if (!px_is_constructor(parent)) {
                px_throw_error(vm, PX_TYPE_ERROR, "super constructor is not a constructor");
                THROW();
            }
            if (f->this_val != PX_HOLE) {
                px_throw_error(vm, PX_REFERENCE_ERROR, "super() called twice");
                THROW();
            }
            r = px_construct_nt(vm, parent, argc, args, f->new_target);
            CHECK(r);
            f->this_val = r;
            sp          = args;
            PUSH(r);
            break;
        }
        case OP_CALL:
        case OP_CALL_ARRAY: {
            int      argc;
            PxValue *callee;
            PxValue  fn;
            SAVE();
            if (op == OP_CALL) argc = READ_U8();
            else {
                PxValue arr = POP();
                argc        = spread_onto(vm, arr, sp);
                if (argc < 0) THROW();
                sp += argc;
            }
            callee = sp - argc - 2;
            SAVE();
            if (--vm->interrupt_counter <= 0 && px_check_interrupt(vm) < 0) THROW();
        call_again:
            fn = callee[1];
            if (!px_is_obj(fn)) goto not_callable;
            switch (px_type_of(fn)) {
            case PX_T_CLOSURE: {
                PxClosure *c = (PxClosure *)px_ptr(fn);
                if (c->obj.flags & PX_OBJ_CLASS_CTOR) {
                    px_throw_error(vm, PX_TYPE_ERROR, "class constructors must be called with new");
                    THROW();
                }
                if (push_frame(vm, c, callee, argc, callee[0], PX_UNDEFINED, 0, 0) < 0) THROW();
                if (c->proto->flags & (PX_PROTO_GENERATOR | PX_PROTO_ASYNC)) {
                    PxValue r = start_coroutine(vm, &vm->frames[vm->nframes - 1]);
                    CHECK(r);
                    callee[0] = r;
                    sp        = callee + 1;
                    break;
                }
                f  = &vm->frames[vm->nframes - 1];
                pc = f->pc;
                sp = vm->sp;
                break;
            }
            case PX_T_NATIVE: {
                PxValue r;
                vm->sp = sp;
                r      = call_native(vm, (PxNative *)px_ptr(fn), callee[0], argc, callee + 2, PX_UNDEFINED);
                CHECK(r);
                callee[0] = r;
                sp        = callee + 1;
                break;
            }
            case PX_T_BOUND:
                argc = expand_bound(vm, callee, argc);
                if (argc < 0) {
                    px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
                    THROW();
                }
                sp     = callee + 2 + argc;
                vm->sp = sp;
                goto call_again;
            case PX_T_PROXY: {
                PxValue r;
                if (!((PxProxy *)px_ptr(fn))->callable) goto not_callable;
                vm->sp = sp;
                r      = px_proxy_call(vm, fn, callee[0], argc, callee + 2);
                CHECK(r);
                callee[0] = r;
                sp        = callee + 1;
                break;
            }
            default:
            not_callable:
                px_throw_error(vm, PX_TYPE_ERROR, "%s is not a function",
                               fn == PX_UNDEFINED ? "undefined" : fn == PX_NULL ? "null" : "value");
                THROW();
            }
            break;
        }
        case OP_NEW:
        case OP_NEW_ARRAY_ARGS: {
            int      argc, i;
            PxValue *base, ctor, obj = PX_UNDEFINED;
            SAVE();
            if (op == OP_NEW) argc = READ_U8();
            else {
                PxValue arr = POP();
                argc        = spread_onto(vm, arr, sp);
                if (argc < 0) THROW();
                sp += argc;
            }
            base = sp - argc - 1; /* ctor, args */
            ctor = base[0];
            SAVE();
            while (px_is_obj(ctor) && px_type_of(ctor) == PX_T_BOUND) {
                /* new on a bound function: construct the target, bound
                 * args first (the bound `this` is ignored). */
                PxBound *b = (PxBound *)px_ptr(ctor);
                int      n = (int)b->nargs;
                if (sp + n + 2 >= vm->stack_end) {
                    px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
                    THROW();
                }
                for (i = argc - 1; i >= 0; i--) base[1 + n + i] = base[1 + i];
                for (i = 0; i < n; i++) base[1 + i] = b->args->items[i];
                argc += n;
                sp      = base + 1 + argc;
                ctor    = b->target;
                base[0] = ctor;
            }
            if (!px_is_constructor(ctor)) {
                px_throw_error(vm, PX_TYPE_ERROR, "not a constructor");
                THROW();
            }
            if (px_type_of(ctor) == PX_T_PROXY) {
                PxValue r;
                vm->sp = sp;
                r      = px_proxy_construct(vm, ctor, argc, base + 1, ctor);
                CHECK(r);
                base[0] = r;
                sp      = base + 1;
                break;
            }
            /* Make room for `this` under the constructor: [this][ctor][args]. */
            if (sp + 1 >= vm->stack_end) {
                px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded");
                THROW();
            }
            for (i = argc; i >= 0; i--) base[i + 1] = base[i];
            base[0] = PX_UNDEFINED;
            sp++;
            vm->sp = sp;
            if (!(px_type_of(ctor) == PX_T_CLOSURE &&
                  (((PxClosure *)px_ptr(ctor))->proto->flags & PX_PROTO_DERIVED))) {
                obj = construct_this(vm, ctor);
                CHECK(obj);
            }
            base[0] = obj;
            if (px_type_of(ctor) == PX_T_NATIVE) {
                PxValue r;
                vm->sp = sp;
                r      = call_native(vm, (PxNative *)px_ptr(ctor), obj, argc, base + 2, ctor);
                CHECK(r);
                base[0] = px_is_obj(r) ? r : obj;
                sp      = base + 1;
            } else {
                if (push_frame(vm, (PxClosure *)px_ptr(ctor), base, argc, obj, ctor, 1, 0) < 0) THROW();
                f  = &vm->frames[vm->nframes - 1];
                pc = f->pc;
                sp = vm->sp;
            }
            break;
        }
        case OP_RETURN:
        case OP_RETURN_UNDEF: {
            PxValue  r = op == OP_RETURN ? POP() : PX_UNDEFINED;
            PxFrame *done;
            if (f->is_construct && !px_is_obj(r)) {
                r = f->this_val;
                if (r == PX_HOLE) {
                    SAVE();
                    px_throw_error(vm, PX_REFERENCE_ERROR, "derived constructor did not call super()");
                    THROW();
                }
            }
            close_upvals(vm, f->base);
            while (vm->nhandlers > 0 && vm->handlers[vm->nhandlers - 1].frame >= vm->nframes - 1) vm->nhandlers--;
            done          = f;
            *done->callee = r;
            sp            = done->callee + 1;
            vm->nframes--;
            if (done->is_boundary || vm->nframes <= entry) {
                vm->sp = done->callee;
                return r;
            }
            f  = &vm->frames[vm->nframes - 1];
            pc = f->pc;
            break;
        }
        case OP_YIELD:
        case OP_AWAIT:
        case OP_DELEGATE:
        case OP_GEN_START: {
            PxFrame *yf = f;
            PxValue  v;
            SAVE(); /* vm->sp covers the value: it stays rooted while saving */
            if (!yf->gen) {
                px_throw_error(vm, PX_INTERNAL_ERROR, "yield outside a generator frame");
                THROW();
            }
            /* GEN_START hands nothing out, and its resume pushes nothing */
            if (gen_save(vm, yf->gen, yf, op == OP_GEN_START ? sp : sp - 1) < 0) THROW();
            v = op == OP_GEN_START ? PX_UNDEFINED : TOP();
            vm->nframes--;
            vm->sp            = yf->callee;
            vm->suspended     = 1;
            vm->suspend_await = op == OP_AWAIT ? PX_SUSPEND_AWAIT : op == OP_DELEGATE ? PX_SUSPEND_DELEGATE
                                                                                     : PX_SUSPEND_YIELD;
            return v;
        }
        case OP_JUMP: {
            int16_t off = READ_S16();
            pc += off;
            break;
        }
        case OP_LOOP: {
            int16_t off = READ_S16();
            pc += off;
            if (--vm->interrupt_counter <= 0) {
                SAVE();
                if (px_check_interrupt(vm) < 0) THROW();
            }
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int16_t off = READ_S16();
            PxValue v = POP();
            if (v == PX_FALSE || (v != PX_TRUE && !px_truthy(v))) pc += off;
            break;
        }
        case OP_LT_JUMP_IF_FALSE:
        case OP_LE_JUMP_IF_FALSE:
        case OP_GT_JUMP_IF_FALSE:
        case OP_GE_JUMP_IF_FALSE: {
            int16_t off = READ_S16();
            PxValue a = sp[-2], b = sp[-1];
            int     r;
            if (px_is_smi(a) && px_is_smi(b)) {
                int32_t x = px_smi(a), y = px_smi(b);
                r = op == OP_LT_JUMP_IF_FALSE ? x < y : op == OP_LE_JUMP_IF_FALSE ? x <= y
                    : op == OP_GT_JUMP_IF_FALSE ? x > y : x >= y;
            } else {
                SAVE();
                r = compare(vm, op == OP_LT_JUMP_IF_FALSE ? OP_LT : op == OP_LE_JUMP_IF_FALSE ? OP_LE
                                : op == OP_GT_JUMP_IF_FALSE ? OP_GT : OP_GE, a, b);
                if (r < 0) THROW();
            }
            sp -= 2;
            if (!r) pc += off;
            break;
        }
        case OP_SEQ_JUMP_IF_FALSE:
        case OP_SNE_JUMP_IF_FALSE: {
            int16_t off = READ_S16();
            PxValue a = sp[-2], b = sp[-1];
            int     r = a == b ? !(px_is_ptr(a) && px_type_of(a) == PX_T_NUMBER && isnan(px_num(a)))
                               : px_strict_equals(vm, a, b);
            sp -= 2;
            if (op == OP_SEQ_JUMP_IF_FALSE ? !r : r) pc += off;
            break;
        }
        case OP_JUMP_IF_TRUE: {
            int16_t off = READ_S16();
            PxValue v   = POP();
            if (v == PX_TRUE || (v != PX_FALSE && px_truthy(v))) pc += off;
            break;
        }
        case OP_JUMP_IF_FALSE_KEEP: {
            int16_t off = READ_S16();
            if (!px_truthy(TOP())) pc += off;
            else sp--;
            break;
        }
        case OP_JUMP_IF_TRUE_KEEP: {
            int16_t off = READ_S16();
            if (px_truthy(TOP())) pc += off;
            else sp--;
            break;
        }
        case OP_JUMP_IF_NOT_NULLISH_KEEP: {
            int16_t off = READ_S16();
            if (TOP() != PX_NULL && TOP() != PX_UNDEFINED) pc += off;
            else sp--;
            break;
        }
        case OP_JUMP_IF_NULLISH: {
            int16_t off = READ_S16();
            if (TOP() == PX_NULL || TOP() == PX_UNDEFINED) pc += off;
            break;
        }
        case OP_THROW:
            SAVE();
            vm->exception = POP();
            THROW();
        case OP_TRY: {
            int16_t off = READ_S16();
            if (vm->nhandlers >= PX_MAX_HANDLERS) {
                SAVE();
                px_throw_error(vm, PX_RANGE_ERROR, "try blocks nested too deeply");
                THROW();
            }
            vm->handlers[vm->nhandlers].frame = vm->nframes - 1;
            vm->handlers[vm->nhandlers].sp    = sp;
            vm->handlers[vm->nhandlers].pc    = pc + off;
            vm->nhandlers++;
            break;
        }
        case OP_END_TRY: vm->nhandlers--; break;
        case OP_FOR_IN:
        case OP_FOR_OF:
        case OP_ITER_START: {
            PxValue it;
            SAVE();
            if (op == OP_FOR_IN) {
                if (TOP() == PX_UNDEFINED || TOP() == PX_NULL) {
                    it = px_array_new(vm, 0); /* nothing to visit */
                    CHECK(it);
                    TOP() = it;
                    it    = make_values_iter(vm, TOP());
                } else {
                    it = make_keys_iter(vm, TOP());
                }
            } else {
                it = make_values_iter(vm, TOP());
            }
            CHECK(it);
            TOP() = it;
            break;
        }
        case OP_ITER_NEXT: {
            int16_t off = READ_S16();
            PxValue v;
            int     r;
            SAVE();
            r = iter_next(vm, (PxIter *)px_ptr(TOP()), &v);
            if (r < 0) THROW();
            if (r == 0) {
                sp--;
                pc += off;
            } else {
                PUSH(v);
            }
            break;
        }
        case OP_ITER_STEP: {
            PxValue v = PX_UNDEFINED;
            int     r;
            SAVE();
            r = iter_next(vm, (PxIter *)px_ptr(TOP()), &v);
            if (r < 0) THROW();
            PUSH(r ? v : PX_UNDEFINED);
            break;
        }
        case OP_ITER_REST: {
            PxValue a, v;
            int     r;
            SAVE();
            a = px_array_new(vm, 0);
            CHECK(a);
            PUSH(a);
            SAVE();
            while ((r = iter_next(vm, (PxIter *)px_ptr(sp[-2]), &v)) > 0)
                if (px_array_push(vm, sp[-1], v) < 0) THROW();
            if (r < 0) THROW();
            break;
        }
        case OP_ITER_STEP_AT:
            li = READ_U8();
        iter_step_at: {
            PxValue v = PX_UNDEFINED;
            int     r;
            SAVE();
            r = iter_next(vm, (PxIter *)px_ptr(f->base[li]), &v);
            if (r < 0) THROW();
            PUSH(r ? v : PX_UNDEFINED);
            break;
        }
        case OP_ITER_REST_AT:
            li = READ_U8();
        iter_rest_at: {
            PxIter *it = (PxIter *)px_ptr(f->base[li]);
            PxValue a, v;
            int     r;
            SAVE();
            a = px_array_new(vm, 0);
            CHECK(a);
            PUSH(a);
            SAVE();
            while ((r = iter_next(vm, it, &v)) > 0)
                if (px_array_push(vm, sp[-1], v) < 0) THROW();
            if (r < 0) THROW();
            break;
        }
        case OP_ITER_CLOSE_ABRUPT: {
            /* the pattern was left by an exception, or by a generator's
             * return() (whose completion a failing return() replaces) */
            PxIter *it = (PxIter *)px_ptr(sp[-1]);
            PxValue exc = sp[-2];
            SAVE();
            if (!it->done && it->kind == PX_ITK_PROTOCOL) {
                it->done = 1;
                if (exc == PX_RETURN_MARK) {
                    if (px_iterator_close(vm, it->target) < 0) THROW();
                } else {
                    vm->exception = exc;
                    px_iterator_close_throw(vm, it->target);
                }
            }
            vm->exception = exc;
            sp -= 2;
            THROW();
        }
        case OP_CATCH_FILTER: {
            int16_t off = READ_S16();
            if (TOP() == PX_RETURN_MARK) {
                if (off) {
                    pc += off;
                } else {
                    SAVE();
                    vm->exception = POP();
                    THROW();
                }
            }
            break;
        }
        case OP_GET_ASYNC_ITER: {
            PxValue it;
            SAVE();
            it = px_get_async_iterator(vm, TOP());
            CHECK(it);
            TOP() = it;
            break;
        }
        case OP_ASYNC_CLOSE: {
            PxValue r;
            SAVE();
            r = px_async_iter_close(vm, TOP());
            CHECK(r);
            TOP() = r;
            break;
        }
        case OP_ITER_CLOSE:
            SAVE();
            if (iter_close(vm, (PxIter *)px_ptr(TOP())) < 0) THROW();
            sp--;
            break;
        case OP_ADD: {
            PxValue a = sp[-2], b = sp[-1], r;
            if (px_is_smi(a) && px_is_smi(b)) {
                int32_t s = px_smi(a) + px_smi(b); /* 31-bit inputs cannot overflow int32 */
                if (s >= PX_SMI_MIN && s <= PX_SMI_MAX) {
                    sp[-2] = px_from_smi(s);
                    sp--;
                    break;
                }
            }
            /* (a + b) | 0, the int32 idiom (hashes, PRNGs, asm.js-style
             * code): when both are int32 numbers, add as integers and
             * wrap, skipping the intermediate heap number. */
            if (pc[0] == OP_INT8 && pc[1] == 0 && pc[2] == OP_BOR) {
                int32_t x, y;
                if (px_num_to_int32_exact(a, &x) && px_num_to_int32_exact(b, &y)) {
                    SAVE();
                    r = px_int(vm, (int32_t)((uint32_t)x + (uint32_t)y));
                    CHECK(r);
                    sp[-2] = r;
                    sp--;
                    pc += 3;
                    break;
                }
            }
            SAVE();
            r = arith(vm, OP_ADD, a, b);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_SUB: {
            PxValue a = sp[-2], b = sp[-1], r;
            if (px_is_smi(a) && px_is_smi(b)) {
                int32_t s = px_smi(a) - px_smi(b);
                if (s >= PX_SMI_MIN && s <= PX_SMI_MAX) {
                    sp[-2] = px_from_smi(s);
                    sp--;
                    break;
                }
            }
            SAVE();
            r = arith(vm, OP_SUB, a, b);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_MUL: {
            PxValue a = sp[-2], b = sp[-1], r;
            if (px_is_smi(a) && px_is_smi(b)) {
                int64_t m = (int64_t)px_smi(a) * px_smi(b);
                if (m >= PX_SMI_MIN && m <= PX_SMI_MAX && !(m == 0 && (px_smi(a) < 0 || px_smi(b) < 0))) {
                    sp[-2] = px_from_smi((int32_t)m);
                    sp--;
                    break;
                }
            }
            SAVE();
            r = arith(vm, OP_MUL, a, b);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_DIV:
        case OP_POW: {
            PxValue r;
            SAVE();
            r = arith(vm, op, sp[-2], sp[-1]);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_MOD: {
            PxValue a = sp[-2], b = sp[-1], r;
            if (px_is_smi(a) && px_is_smi(b) && px_smi(b) > 0 && px_smi(a) >= 0) {
                sp[-2] = px_from_smi(px_smi(a) % px_smi(b));
                sp--;
                break;
            }
            SAVE();
            r = arith(vm, OP_MOD, a, b);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_BAND:
        case OP_BOR:
        case OP_BXOR:
        case OP_SHL:
        case OP_SAR:
        case OP_SHR: {
            PxValue a = sp[-2], b = sp[-1], r;
            if (px_is_smi(a) && px_is_smi(b) && op != OP_SHL && op != OP_SHR) {
                int32_t x = px_smi(a), y = px_smi(b);
                /* results of &, |, ^ and >> on 31-bit inputs stay 31-bit */
                sp[-2] = px_from_smi(op == OP_BAND ? (x & y) : op == OP_BOR ? (x | y) : op == OP_BXOR ? (x ^ y) : (x >> (y & 31)));
                sp--;
                break;
            }
            SAVE();
            r = bitop(vm, op, a, b);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_EQ:
        case OP_NE: {
            int r;
            if (px_is_smi(sp[-2]) && px_is_smi(sp[-1])) {
                r      = sp[-2] == sp[-1];
                sp[-2] = px_bool(op == OP_EQ ? r : !r);
                sp--;
                break;
            }
            SAVE();
            r = px_loose_equals(vm, sp[-2], sp[-1]);
            if (r < 0) THROW();
            sp[-2] = px_bool(op == OP_EQ ? r : !r);
            sp--;
            break;
        }
        case OP_SEQ:
        case OP_SNE: {
            PxValue a = sp[-2], b = sp[-1];
            int     r = a == b ? !(px_is_ptr(a) && px_type_of(a) == PX_T_NUMBER && isnan(px_num(a)))
                               : px_strict_equals(vm, a, b);
            sp[-2] = px_bool(op == OP_SEQ ? r : !r);
            sp--;
            break;
        }
        case OP_LT:
        case OP_LE:
        case OP_GT:
        case OP_GE: {
            PxValue a = sp[-2], b = sp[-1];
            int     r;
            if (px_is_smi(a) && px_is_smi(b)) {
                int32_t x = px_smi(a), y = px_smi(b);
                r = op == OP_LT ? x < y : op == OP_LE ? x <= y : op == OP_GT ? x > y : x >= y;
            } else {
                SAVE();
                r = compare(vm, op, a, b);
                if (r < 0) THROW();
            }
            sp[-2] = px_bool(r);
            sp--;
            break;
        }
        case OP_IN: {
            PxValue k;
            int     r;
            SAVE();
            if (!px_is_obj(sp[-1])) {
                px_throw_error(vm, PX_TYPE_ERROR, "right-hand side of 'in' is not an object");
                THROW();
            }
            k = key_of(vm, sp[-2]);
            CHECK(k);
            r      = px_has(vm, sp[-1], k);
            if (r < 0) THROW();
            sp[-2] = px_bool(r);
            sp--;
            break;
        }
        case OP_INSTANCEOF: {
            int r;
            SAVE();
            r = instance_of(vm, sp[-2], sp[-1]);
            if (r < 0) THROW();
            sp[-2] = px_bool(r);
            sp--;
            break;
        }
        case OP_NEG: {
            PxValue a = TOP();
            double  d;
            if (px_is_smi(a) && px_smi(a) != 0 && px_smi(a) != PX_SMI_MIN) {
                TOP() = px_from_smi(-px_smi(a));
                break;
            }
            SAVE();
            if (px_to_number(vm, a, &d) < 0) THROW();
            a = px_number(vm, -d);
            CHECK(a);
            TOP() = a;
            break;
        }
        case OP_PLUS:
        case OP_TO_NUMERIC: {
            PxValue a = TOP();
            double  d;
            if (px_is_num(a)) break;
            SAVE();
            if (px_to_number(vm, a, &d) < 0) THROW();
            a = px_number(vm, d);
            CHECK(a);
            TOP() = a;
            break;
        }
        case OP_INC_LOCAL:
        case OP_DEC_LOCAL: {
            PxValue *slot = &f->base[READ_U8()], a = *slot;
            double   d;
            if (px_is_smi(a)) {
                int32_t n = px_smi(a) + (op == OP_INC_LOCAL ? 1 : -1);
                if (n >= PX_SMI_MIN && n <= PX_SMI_MAX) {
                    *slot = px_from_smi(n);
                    break;
                }
            }
            SAVE();
            if (px_to_number(vm, a, &d) < 0) THROW();
            a = px_number(vm, d + (op == OP_INC_LOCAL ? 1 : -1));
            CHECK(a);
            *slot = a;
            break;
        }
        case OP_INC:
        case OP_DEC: {
            PxValue a = TOP();
            double  d;
            if (px_is_smi(a)) {
                int32_t n = px_smi(a) + (op == OP_INC ? 1 : -1);
                if (n >= PX_SMI_MIN && n <= PX_SMI_MAX) {
                    TOP() = px_from_smi(n);
                    break;
                }
            }
            SAVE();
            if (px_to_number(vm, a, &d) < 0) THROW();
            a = px_number(vm, d + (op == OP_INC ? 1 : -1));
            CHECK(a);
            TOP() = a;
            break;
        }
        case OP_NOT: TOP() = px_bool(!px_truthy(TOP())); break;
        case OP_BNOT: {
            int32_t x;
            PxValue r;
            SAVE();
            if (px_to_int32(vm, TOP(), &x) < 0) THROW();
            r = px_int(vm, ~x);
            CHECK(r);
            TOP() = r;
            break;
        }
        case OP_TYPEOF: TOP() = px_typeof(vm, TOP()); break;
        case OP_TO_STRING: {
            PxValue s;
            if (px_is_str(TOP())) break;
            SAVE();
            s = px_to_string(vm, TOP());
            CHECK(s);
            TOP() = s;
            break;
        }
        case OP_REGEXP: {
            PxValue pat = CONSTS[READ_U16()], fl = CONSTS[READ_U16()], r;
            SAVE();
            r = px_regexp_create(vm, pat, fl);
            CHECK(r);
            PUSH(r);
            break;
        }
        case OP_IMPORT: {
            PxValue spec = CONSTS[READ_U16()], ns;
            char    name[256];
            SAVE();
            px_str_to_utf8(vm, spec, name, sizeof name);
            if (!vm->resolver) {
                px_throw_error(vm, PX_REFERENCE_ERROR, "cannot import \"%s\": no module resolver", name);
                THROW();
            }
            ns = vm->resolver(vm, name, vm->resolver_opaque);
            CHECK(ns);
            PUSH(ns);
            break;
        }
        case OP_NEW_PRIVATE: {
            PxValue s;
            SAVE();
            s = px_symbol_new(vm, CONSTS[READ_U16()]);
            CHECK(s);
            ((PxSymbol *)px_ptr(s))->is_private = 1;
            PUSH(s);
            break;
        }
        case OP_GET_PRIVATE:
        case OP_GET_PRIVATE_KEEP: {
            PxValue v;
            SAVE();
            v = private_get(vm, sp[-2], sp[-1]);
            CHECK(v);
            if (op == OP_GET_PRIVATE) {
                sp[-2] = v;
                sp--;
            } else {
                sp[-1] = v;
            }
            break;
        }
        case OP_SET_PRIVATE:
            SAVE();
            if (private_set(vm, sp[-3], sp[-2], sp[-1]) < 0) THROW();
            sp[-3] = sp[-1];
            sp -= 2;
            break;
        case OP_DEFINE_PRIVATE:
        case OP_ADD_PRIVATE_METHOD: {
            int kind = op == OP_DEFINE_PRIVATE ? -1 : READ_U8();
            SAVE();
            if (private_add(vm, sp[-3], sp[-2], sp[-1], kind) < 0) THROW();
            sp -= 2;
            break;
        }
        case OP_HAS_PRIVATE: {
            uint32_t attrs;
            if (!px_is_obj(sp[-1])) {
                SAVE();
                px_throw_error(vm, PX_TYPE_ERROR, "right-hand side of 'in' is not an object");
                THROW();
            }
            sp[-2] = px_bool(px_private_slot(vm, (PxObject *)px_ptr(sp[-1]), sp[-2], &attrs) != NULL);
            sp--;
            break;
        }
        case OP_SET_HOME:
            if (px_is_obj(sp[-1]) && px_type_of(sp[-1]) == PX_T_CLOSURE) ((PxClosure *)px_ptr(sp[-1]))->home = sp[-2];
            sp[-2] = sp[-1];
            sp--;
            break;
        case OP_ELEM_KEY: {
            PxValue k;
            SAVE();
            if (sp[-2] == PX_NULL || sp[-2] == PX_UNDEFINED) {
                px_throw_error(vm, PX_TYPE_ERROR, "cannot read properties of %s", sp[-2] == PX_NULL ? "null" : "undefined");
                THROW();
            }
            k = key_of(vm, TOP());
            CHECK(k);
            TOP() = k;
            break;
        }
        case OP_TO_PROPKEY: {
            PxValue k;
            SAVE();
            k = key_of(vm, TOP());
            CHECK(k);
            TOP() = k;
            break;
        }
        case OP_SET_PROTO:
            if (px_is_obj(sp[-1]) || sp[-1] == PX_NULL) {
                SAVE();
                if (px_set_proto(vm, sp[-2], sp[-1]) < 0) THROW();
            }
            sp--;
            break;
        case OP_TEMPLATE_OBJ: {
            PxVec   *parts = (PxVec *)px_ptr(CONSTS[READ_U16()]);
            PxValue  a;
            uint32_t i;
            PxValue  raw;
            uint32_t n = parts->cap / 2; /* cooked strings, then raw ones */
            SAVE();
            a = px_array_new(vm, n);
            CHECK(a);
            PUSH(a);
            SAVE(); /* a is on the stack: now a root */
            raw = px_array_new(vm, n);
            CHECK(raw);
            PUSH(raw);
            SAVE();
            parts = (PxVec *)px_ptr(CONSTS[pc[-2] | (pc[-1] << 8)]);
            for (i = 0; i < n; i++)
                if (px_array_push(vm, a, parts->items[i]) < 0 || px_array_push(vm, raw, parts->items[n + i]) < 0)
                    THROW();
            if (px_def_value(vm, a, "raw", raw, 0) < 0) THROW();
            sp--;
            break;
        }
        default:
            SAVE();
            px_throw_error(vm, PX_INTERNAL_ERROR, "bad opcode %d", (int)op);
            THROW();
        }
        continue;

    exception:
        /* vm->exception holds the value. Find a handler in a frame that
         * belongs to this activation of run(); else unwind to its entry
         * frame and report to the caller. */
        if (!vm->uncatchable && vm->nhandlers > 0 && vm->handlers[vm->nhandlers - 1].frame >= entry) {
            PxHandler h = vm->handlers[--vm->nhandlers];
            unwind_frames(vm, h.frame + 1);
            f  = &vm->frames[vm->nframes - 1];
            sp = h.sp;
            pc = h.pc;
            PUSH(vm->exception);
            vm->exception = PX_UNDEFINED;
            continue;
        }
        {
            PxValue *callee = vm->frames[entry].callee;
            unwind_frames(vm, entry);
            vm->sp = callee;
            return PX_EXCEPTION;
        }
    }
}

PxValue px_run_proto(PxVM *vm, PxProto *p, PxValue this_val) {
    PxClosure *c;
    PxValue    cv, r;
    PX_ROOT(vm, this_val);
    c = make_closure(vm, p, NULL);
    px_pop_roots(vm, 1);
    if (!c) return PX_EXCEPTION;
    cv = px_from_ptr(c);
    PX_ROOT(vm, cv);
    r = px_call(vm, cv, this_val, 0, NULL);
    px_pop_roots(vm, 1);
    return r;
}
