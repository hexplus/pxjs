/* Proxy and Proxy.revocable.
 *
 * A proxy is an object of type PX_T_PROXY with a target and a handler. The
 * object model's internal methods (px_get_recv, px_set_recv, px_has,
 * px_get_own_property, px_define_own_property, px_delete, px_own_keys,
 * px_proto_of, px_set_proto_ok, px_is_extensible, px_prevent_extensions)
 * and calls and `new` hand proxies to the functions here, which call the
 * handler's trap or, when it has none, do the same operation on the
 * target. Every trap's answer is checked against the target as ECMA-262
 * 10.5 says: a handler may not misreport non-configurable properties or a
 * non-extensible target.
 *
 * Proxies never share a shape with ordinary objects (their prototype
 * placeholder is private), so no inline cache can match one. */

#include <string.h>

#include "px_internal.h"

#define ARG(i) px_arg(argc, argv, (i))

static PxProxy *as_proxy(PxValue v) { return (PxProxy *)px_ptr(v); }

/* The handler's trap `name` (undefined if none) and the target; -1 with a
 * TypeError if the proxy was revoked. */
static int trap_of(PxVM *vm, PxValue proxy, const char *name, PxValue *trap, PxValue *target) {
    PxProxy *p = as_proxy(proxy);
    PxValue  t, h = p->handler, k;
    if (h == PX_NULL) {
        px_throw_error(vm, PX_TYPE_ERROR, "cannot perform '%s' on a proxy that has been revoked", name);
        return -1;
    }
    *target = p->target;
    PX_ROOT(vm, h);
    k = px_intern_cstr(vm, name);
    t = k == PX_EXCEPTION ? k : px_get(vm, h, k);
    px_pop_roots(vm, 1);
    if (t == PX_EXCEPTION) return -1;
    if (t == PX_NULL) t = PX_UNDEFINED;
    if (t != PX_UNDEFINED && !px_is_callable(t)) {
        px_throw_error(vm, PX_TYPE_ERROR, "proxy trap '%s' is not a function", name);
        return -1;
    }
    /* the trap's getter may have revoked the proxy; the target read first stands */
    *trap = t;
    return 0;
}

/* trap(target, a, b, c) with `this` = the handler */
static PxValue call_trap(PxVM *vm, PxValue handler, PxValue trap, PxValue target, int n, PxValue a, PxValue b,
                         PxValue c) {
    PxValue args[4];
    args[0] = target;
    args[1] = a;
    args[2] = b;
    args[3] = c;
    return px_call(vm, trap, handler, n, args);
}

/* Forwarding to the target recurses in C through a chain of trap-less
 * proxies: bounded like nested native calls. */
static int enter(PxVM *vm) {
    if (vm->native_depth >= vm->max_native_depth) {
        px_throw_error(vm, PX_RANGE_ERROR, "Maximum call stack size exceeded (proxy chain)");
        return -1;
    }
    vm->native_depth++;
    return 0;
}

/* res = expr, one level deeper; -1 (a RangeError) when too deep */
#define FORWARD(res, expr) (enter(vm) < 0 ? -1 : ((res) = (expr), vm->native_depth--, 0))

static int violated(PxVM *vm, const char *trap, const char *what) {
    px_throw_error(vm, PX_TYPE_ERROR, "proxy '%s' trap: %s", trap, what);
    return -1;
}

/* Every trap below follows ECMA-262 10.5: fetch the trap (or forward to
 * the target), call it, then check the answer against the target, whose
 * non-configurable properties and non-extensibility a handler may not
 * misreport. `st` holds the values each keeps rooted: proxy, handler,
 * trap, target, key, and the trap's result. */
typedef struct TrapCall {
    PxValue proxy, handler, trap, target, key, result;
} TrapCall;

/* 0 trap found (6 roots pushed), 1 no trap (nothing pushed), -1 exception */
static int trap_begin(PxVM *vm, TrapCall *c, PxValue proxy, PxValue key, const char *name) {
    c->proxy  = proxy;
    c->key    = key;
    c->result = PX_UNDEFINED;
    PX_ROOT(vm, c->proxy);
    PX_ROOT(vm, c->key);
    c->handler = as_proxy(proxy)->handler;
    if (trap_of(vm, proxy, name, &c->trap, &c->target) < 0) {
        px_pop_roots(vm, 2);
        return -1;
    }
    if (c->trap == PX_UNDEFINED) {
        px_pop_roots(vm, 2);
        return 1;
    }
    PX_ROOT(vm, c->handler);
    PX_ROOT(vm, c->trap);
    PX_ROOT(vm, c->target);
    PX_ROOT(vm, c->result);
    return 0;
}

PxValue px_proxy_target(PxVM *vm, PxValue proxy) {
    PxProxy *p = as_proxy(proxy);
    if (p->handler == PX_NULL) return px_throw_error(vm, PX_TYPE_ERROR, "the proxy has been revoked");
    return p->target;
}

/* The target's own property `key`: 1 found, 0 absent, -1 exception. *d rooted by the caller. */
static int target_desc(PxVM *vm, TrapCall *c, PxDesc *d) {
    d->value = d->get = d->set = PX_UNDEFINED;
    return px_get_own_property(vm, c->target, c->key, d);
}

PxValue px_proxy_get(PxVM *vm, PxValue proxy, PxValue key, PxValue receiver) {
    TrapCall c;
    PxDesc   d;
    PxValue  kv;
    int      r;
    PX_ROOT(vm, receiver);
    r = trap_begin(vm, &c, proxy, key, "get");
    if (r != 0) {
        px_pop_roots(vm, 1);
        return r < 0 || FORWARD(c.result, px_get_recv(vm, c.target, key, receiver)) < 0 ? PX_EXCEPTION : c.result;
    }
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 3, kv, receiver, PX_UNDEFINED);
    if (c.result != PX_EXCEPTION) {
        r = target_desc(vm, &c, &d);
        if (r < 0) c.result = PX_EXCEPTION;
        else if (r && !(d.attrs & PX_ATTR_CONFIGURABLE)) {
            if (PX_DESC_IS_DATA(&d) && !(d.attrs & PX_ATTR_WRITABLE) && !px_same_value(vm, c.result, d.value))
                c.result = (violated(vm, "get", "must report a non-writable, non-configurable property's value"),
                            PX_EXCEPTION);
            else if (PX_DESC_IS_ACCESSOR(&d) && d.get == PX_UNDEFINED && c.result != PX_UNDEFINED)
                c.result = (violated(vm, "get", "must report undefined for a property without a getter"), PX_EXCEPTION);
        }
    }
    px_pop_roots(vm, 10);
    return c.result;
}

int px_proxy_set(PxVM *vm, PxValue proxy, PxValue key, PxValue v, PxValue receiver) {
    TrapCall c;
    PxDesc   d;
    PxValue  kv;
    int      r;
    PX_ROOT(vm, v);
    PX_ROOT(vm, receiver);
    r = trap_begin(vm, &c, proxy, key, "set");
    if (r != 0) {
        px_pop_roots(vm, 2);
        return r < 0 || FORWARD(r, px_set_recv(vm, c.target, key, v, receiver)) < 0 ? -1 : r;
    }
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 4, kv, v, receiver);
    if (c.result == PX_EXCEPTION) r = -1;
    else if (!px_truthy(c.result)) r = 0;
    else {
        r = target_desc(vm, &c, &d);
        if (r > 0 && !(d.attrs & PX_ATTR_CONFIGURABLE)) {
            if (PX_DESC_IS_DATA(&d) && !(d.attrs & PX_ATTR_WRITABLE) && !px_same_value(vm, v, d.value))
                r = violated(vm, "set", "cannot change a non-writable, non-configurable property");
            else if (PX_DESC_IS_ACCESSOR(&d) && d.set == PX_UNDEFINED)
                r = violated(vm, "set", "cannot set a non-configurable property without a setter");
        }
        if (r >= 0) r = 1;
    }
    px_pop_roots(vm, 11);
    return r;
}

int px_proxy_has(PxVM *vm, PxValue proxy, PxValue key) {
    TrapCall c;
    PxDesc   d;
    PxValue  kv;
    int      r = trap_begin(vm, &c, proxy, key, "has");
    if (r != 0) return r < 0 || FORWARD(r, px_has(vm, c.target, key)) < 0 ? -1 : r;
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 2, kv, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) r = -1;
    else if (px_truthy(c.result)) r = 1;
    else {
        r = target_desc(vm, &c, &d);
        if (r > 0) {
            if (!(d.attrs & PX_ATTR_CONFIGURABLE)) r = violated(vm, "has", "cannot hide a non-configurable property");
            else if ((r = px_is_extensible(vm, c.target)) == 0)
                r = violated(vm, "has", "cannot hide a property of a non-extensible target");
        }
        if (r >= 0) r = 0;
    }
    px_pop_roots(vm, 9);
    return r;
}

int px_proxy_delete(PxVM *vm, PxValue proxy, PxValue key) {
    TrapCall c;
    PxDesc   d;
    PxValue  kv;
    int      r = trap_begin(vm, &c, proxy, key, "deleteProperty");
    if (r != 0) return r < 0 || FORWARD(r, px_delete(vm, c.target, key)) < 0 ? -1 : r;
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 2, kv, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) r = -1;
    else if (!px_truthy(c.result)) r = 0;
    else {
        r = target_desc(vm, &c, &d);
        if (r > 0) {
            if (!(d.attrs & PX_ATTR_CONFIGURABLE)) r = violated(vm, "deleteProperty", "cannot delete a non-configurable property");
            else if ((r = px_is_extensible(vm, c.target)) == 0)
                r = violated(vm, "deleteProperty", "cannot delete a property of a non-extensible target");
        }
        if (r >= 0) r = 1;
    }
    px_pop_roots(vm, 9);
    return r;
}

int px_proxy_get_own_property(PxVM *vm, PxValue proxy, PxValue key, PxDesc *out) {
    TrapCall c;
    PxDesc   td, rd;
    PxValue  kv;
    int      r = trap_begin(vm, &c, proxy, key, "getOwnPropertyDescriptor"), found, ext;
    if (r != 0) return r < 0 || FORWARD(r, px_get_own_property(vm, c.target, key, out)) < 0 ? -1 : r;
    PX_ROOT_DESC(vm, td);
    td.value = td.get = td.set = PX_UNDEFINED;
    PX_ROOT_DESC(vm, rd);
    rd.value = rd.get = rd.set = PX_UNDEFINED;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 2, kv, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) goto fail;
    if (c.result != PX_UNDEFINED && !px_is_obj(c.result)) {
        violated(vm, "getOwnPropertyDescriptor", "must return an object or undefined");
        goto fail;
    }
    found = target_desc(vm, &c, &td);
    if (found < 0) goto fail;
    if (c.result == PX_UNDEFINED) {
        if (found) {
            if (!(td.attrs & PX_ATTR_CONFIGURABLE)) {
                violated(vm, "getOwnPropertyDescriptor", "cannot hide a non-configurable property");
                goto fail;
            }
            ext = px_is_extensible(vm, c.target);
            if (ext < 0) goto fail;
            if (!ext) {
                violated(vm, "getOwnPropertyDescriptor", "cannot hide a property of a non-extensible target");
                goto fail;
            }
        }
        px_pop_roots(vm, 12);
        return 0;
    }
    ext = px_is_extensible(vm, c.target);
    if (ext < 0 || px_to_property_descriptor(vm, c.result, &rd) < 0) goto fail;
    /* CompletePropertyDescriptor: absent fields are undefined / false */
    rd.has |= PX_DESC_IS_ACCESSOR(&rd) ? (PX_DESC_GET | PX_DESC_SET) : (PX_DESC_VALUE | PX_DESC_WRITABLE);
    rd.has |= PX_DESC_ENUMERABLE | PX_DESC_CONFIGURABLE;
    if (!px_desc_compatible(vm, ext, &rd, found ? &td : NULL)) {
        violated(vm, "getOwnPropertyDescriptor", "reported a descriptor incompatible with the target's");
        goto fail;
    }
    if (!(rd.attrs & PX_ATTR_CONFIGURABLE)) {
        if (!found || (td.attrs & PX_ATTR_CONFIGURABLE)) {
            violated(vm, "getOwnPropertyDescriptor", "cannot report a configurable or missing property as non-configurable");
            goto fail;
        }
        if (PX_DESC_IS_DATA(&rd) && !(rd.attrs & PX_ATTR_WRITABLE) && (td.attrs & PX_ATTR_WRITABLE)) {
            violated(vm, "getOwnPropertyDescriptor", "cannot report a writable property as non-writable");
            goto fail;
        }
    }
    *out = rd;
    px_pop_roots(vm, 12);
    return 1;
fail:
    px_pop_roots(vm, 12);
    return -1;
}

int px_proxy_define_own_property(PxVM *vm, PxValue proxy, PxValue key, const PxDesc *desc) {
    TrapCall c;
    PxDesc   td;
    PxValue  kv, dobj;
    int      r = trap_begin(vm, &c, proxy, key, "defineProperty"), found, ext;
    int      config_false = (desc->has & PX_DESC_CONFIGURABLE) && !(desc->attrs & PX_ATTR_CONFIGURABLE);
    if (r != 0) return r < 0 || FORWARD(r, px_define_own_property(vm, c.target, key, desc)) < 0 ? -1 : r;
    PX_ROOT_DESC(vm, td);
    td.value = td.get = td.set = PX_UNDEFINED;
    dobj = px_from_property_descriptor(vm, desc);
    if (dobj == PX_EXCEPTION) goto fail;
    c.result = dobj;
    kv       = px_key_to_value(vm, key);
    c.result = kv == PX_EXCEPTION ? kv : call_trap(vm, c.handler, c.trap, c.target, 3, kv, dobj, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) goto fail;
    if (!px_truthy(c.result)) {
        px_pop_roots(vm, 9);
        return 0;
    }
    found = target_desc(vm, &c, &td);
    if (found < 0 || (ext = px_is_extensible(vm, c.target)) < 0) goto fail;
    if (!found) {
        if (!ext) {
            violated(vm, "defineProperty", "cannot add a property to a non-extensible target");
            goto fail;
        }
        if (config_false) {
            violated(vm, "defineProperty", "cannot define a missing property as non-configurable");
            goto fail;
        }
    } else {
        if (!px_desc_compatible(vm, ext, desc, &td)) {
            violated(vm, "defineProperty", "the descriptor is incompatible with the target's property");
            goto fail;
        }
        if (config_false && (td.attrs & PX_ATTR_CONFIGURABLE)) {
            violated(vm, "defineProperty", "cannot define a configurable property as non-configurable");
            goto fail;
        }
        if (PX_DESC_IS_DATA(&td) && !(td.attrs & PX_ATTR_CONFIGURABLE) && (td.attrs & PX_ATTR_WRITABLE) &&
            (desc->has & PX_DESC_WRITABLE) && !(desc->attrs & PX_ATTR_WRITABLE)) {
            violated(vm, "defineProperty", "cannot make a non-configurable property non-writable");
            goto fail;
        }
    }
    px_pop_roots(vm, 9);
    return 1;
fail:
    px_pop_roots(vm, 9);
    return -1;
}

/* The ownKeys trap's list, checked (ECMA-262 10.5.11), then filtered by
 * `flags` as px_own_keys does. */
PxVec *px_proxy_own_keys(PxVM *vm, PxValue proxy, int flags, uint32_t *count) {
    TrapCall c;
    PxDesc   d;
    PxValue  listv = PX_UNDEFINED, tkeysv = PX_UNDEFINED, outv = PX_UNDEFINED;
    PxVec   *list, *out;
    PxIdx    len, i;
    uint32_t n = 0, nt = 0, j, k, unchecked;
    int      r = trap_begin(vm, &c, proxy, PX_UNDEFINED, "ownKeys"), ext;
    *count = 0;
    if (r != 0) return r < 0 || FORWARD(list, px_own_keys(vm, c.target, flags, count)) < 0 ? NULL : list;
    PX_ROOT_DESC(vm, d);
    d.value = d.get = d.set = PX_UNDEFINED;
    PX_ROOT(vm, listv);
    PX_ROOT(vm, tkeysv);
    PX_ROOT(vm, outv);
    c.result = call_trap(vm, c.handler, c.trap, c.target, 1, PX_UNDEFINED, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) goto fail;
    if (!px_is_obj(c.result)) {
        violated(vm, "ownKeys", "must return an object");
        goto fail;
    }
    /* CreateListFromArrayLike(result, Â« String, Symbol Â»), as property keys */
    if (px_length_of(vm, c.result, &len) < 0) goto fail;
    if (len > 0xFFFFFF) {
        px_throw_error(vm, PX_RANGE_ERROR, "ownKeys trap: too many keys");
        goto fail;
    }
    list = px_vec_new(vm, len > 0 ? (uint32_t)len : 1);
    if (!list) goto fail;
    listv = px_from_ptr(list);
    for (i = 0; i < len; i++) {
        PxValue v = px_get_index(vm, c.result, i);
        if (v == PX_EXCEPTION) goto fail;
        if (!px_is_str(v) && !(px_is_ptr(v) && px_type_of(v) == PX_T_SYMBOL)) {
            violated(vm, "ownKeys", "the result may only hold strings and symbols");
            goto fail;
        }
        v = px_intern(vm, v);
        if (v == PX_EXCEPTION) goto fail;
        for (j = 0; j < n; j++)
            if (((PxVec *)px_ptr(listv))->items[j] == v) {
                violated(vm, "ownKeys", "the result has a duplicate key");
                goto fail;
            }
        ((PxVec *)px_ptr(listv))->items[n++] = v;
    }
    /* the target's non-configurable keys must all be there; with a
     * non-extensible target, exactly its keys */
    if ((ext = px_is_extensible(vm, c.target)) < 0) goto fail;
    {
        PxVec *tk = px_own_keys(vm, c.target, PX_KEYS_SYMBOLS, &nt);
        if (!tk) goto fail;
        tkeysv = px_from_ptr(tk);
    }
    unchecked = n;
    {
        /* marks: a key of the list is checked off by replacing it with 0 */
        PxVec *marks = px_vec_new(vm, n > 0 ? n : 1);
        if (!marks) goto fail;
        outv = px_from_ptr(marks);
        for (j = 0; j < n; j++) marks->items[j] = ((PxVec *)px_ptr(listv))->items[j];
    }
    for (k = 0; k < 2; k++) {
        /* pass 0: non-configurable keys, pass 1 (non-extensible target): the rest */
        if (k == 1 && ext) break;
        for (j = 0; j < nt; j++) {
            PxValue tk = px_intern(vm, ((PxVec *)px_ptr(tkeysv))->items[j]);
            uint32_t m;
            if (tk == PX_EXCEPTION) goto fail;
            c.key = tk;
            r     = target_desc(vm, &c, &d);
            if (r < 0) goto fail;
            if ((r > 0 && !(d.attrs & PX_ATTR_CONFIGURABLE)) != (k == 0)) continue;
            for (m = 0; m < n && ((PxVec *)px_ptr(outv))->items[m] != tk; m++) {}
            if (m == n) {
                violated(vm, "ownKeys", k == 0 ? "must list every non-configurable property of the target"
                                               : "must list exactly the keys of a non-extensible target");
                goto fail;
            }
            ((PxVec *)px_ptr(outv))->items[m] = 0;
            unchecked--;
        }
    }
    if (!ext && unchecked) {
        violated(vm, "ownKeys", "cannot add keys to a non-extensible target");
        goto fail;
    }
    /* the result, filtered as asked */
    out = px_vec_new(vm, n > 0 ? n : 1);
    if (!out) goto fail;
    outv = px_from_ptr(out);
    for (j = 0, k = 0; j < n; j++) {
        PxValue key = ((PxVec *)px_ptr(listv))->items[j];
        int     sym = px_is_ptr(key) && px_type_of(key) == PX_T_SYMBOL;
        if (sym ? !(flags & PX_KEYS_SYMBOLS) : (flags & PX_KEYS_NO_STRINGS) != 0) continue;
        if (flags & PX_KEYS_ENUMERABLE) {
            r = px_proxy_get_own_property(vm, c.proxy, key, &d);
            if (r < 0) goto fail;
            if (!r || !(d.attrs & PX_ATTR_ENUMERABLE)) continue;
        }
        if (!sym) {
            key = px_key_to_value(vm, key);
            if (key == PX_EXCEPTION) goto fail;
        }
        ((PxVec *)px_ptr(outv))->items[k++] = key;
    }
    *count = k;
    out    = (PxVec *)px_ptr(outv);
    px_pop_roots(vm, 12);
    return out;
fail:
    px_pop_roots(vm, 12);
    return NULL;
}

PxValue px_proxy_proto_of(PxVM *vm, PxValue proxy) {
    TrapCall c;
    PxValue  tp;
    int      r = trap_begin(vm, &c, proxy, PX_UNDEFINED, "getPrototypeOf"), ext;
    if (r != 0) return r < 0 || FORWARD(c.result, px_proto_of(vm, c.target)) < 0 ? PX_EXCEPTION : c.result;
    c.result = call_trap(vm, c.handler, c.trap, c.target, 1, PX_UNDEFINED, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result != PX_EXCEPTION && c.result != PX_NULL && !px_is_obj(c.result))
        c.result = (violated(vm, "getPrototypeOf", "must return an object or null"), PX_EXCEPTION);
    if (c.result != PX_EXCEPTION && (ext = px_is_extensible(vm, c.target)) <= 0) {
        if (ext < 0) c.result = PX_EXCEPTION;
        else if ((tp = px_proto_of(vm, c.target)) == PX_EXCEPTION) c.result = PX_EXCEPTION;
        else if (tp != c.result)
            c.result = (violated(vm, "getPrototypeOf", "must report a non-extensible target's prototype"), PX_EXCEPTION);
    }
    px_pop_roots(vm, 6);
    return c.result;
}

int px_proxy_set_proto(PxVM *vm, PxValue proxy, PxValue proto) {
    TrapCall c;
    PxValue  tp;
    int      r, ext;
    PX_ROOT(vm, proto);
    r = trap_begin(vm, &c, proxy, PX_UNDEFINED, "setPrototypeOf");
    if (r != 0) {
        px_pop_roots(vm, 1);
        return r < 0 || FORWARD(r, px_set_proto_ok(vm, c.target, proto)) < 0 ? -1 : r;
    }
    c.result = call_trap(vm, c.handler, c.trap, c.target, 2, proto, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) r = -1;
    else if (!px_truthy(c.result)) r = 0;
    else if ((ext = px_is_extensible(vm, c.target)) < 0) r = -1;
    else if (ext) r = 1;
    else if ((tp = px_proto_of(vm, c.target)) == PX_EXCEPTION) r = -1;
    else r = tp == proto ? 1 : violated(vm, "setPrototypeOf", "cannot change a non-extensible target's prototype");
    px_pop_roots(vm, 7);
    return r;
}

int px_proxy_is_extensible(PxVM *vm, PxValue proxy) {
    TrapCall c;
    int      r = trap_begin(vm, &c, proxy, PX_UNDEFINED, "isExtensible"), t;
    if (r != 0) return r < 0 || FORWARD(r, px_is_extensible(vm, c.target)) < 0 ? -1 : r;
    c.result = call_trap(vm, c.handler, c.trap, c.target, 1, PX_UNDEFINED, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION || (t = px_is_extensible(vm, c.target)) < 0) r = -1;
    else if (px_truthy(c.result) != t) r = violated(vm, "isExtensible", "must report the target's extensibility");
    else r = t;
    px_pop_roots(vm, 6);
    return r;
}

int px_proxy_prevent_extensions(PxVM *vm, PxValue proxy) {
    TrapCall c;
    int      r = trap_begin(vm, &c, proxy, PX_UNDEFINED, "preventExtensions"), t;
    if (r != 0) return r < 0 || FORWARD(r, px_prevent_extensions(vm, c.target)) < 0 ? -1 : r;
    c.result = call_trap(vm, c.handler, c.trap, c.target, 1, PX_UNDEFINED, PX_UNDEFINED, PX_UNDEFINED);
    if (c.result == PX_EXCEPTION) r = -1;
    else if (!px_truthy(c.result)) r = 0;
    else if ((t = px_is_extensible(vm, c.target)) < 0) r = -1;
    else r = t ? violated(vm, "preventExtensions", "reported success but the target is still extensible") : 1;
    px_pop_roots(vm, 6);
    return r;
}

static PxValue args_array(PxVM *vm, int argc, PxValue *argv) {
    PxValue a = px_array_new(vm, (uint32_t)argc);
    int     i;
    if (a == PX_EXCEPTION) return a;
    PX_ROOT(vm, a);
    for (i = 0; i < argc; i++)
        if (px_array_push(vm, a, argv[i]) < 0) {
            px_pop_roots(vm, 1);
            return PX_EXCEPTION;
        }
    px_pop_roots(vm, 1);
    return a;
}

/* argv must be rooted by the caller (it is, on the VM stack). */
PxValue px_proxy_call(PxVM *vm, PxValue proxy, PxValue this_val, int argc, PxValue *argv) {
    PxValue trap, target, a, r;
    if (trap_of(vm, proxy, "apply", &trap, &target) < 0) return PX_EXCEPTION;
    if (trap == PX_UNDEFINED) return px_call(vm, target, this_val, argc, argv);
    PX_ROOT(vm, trap);
    PX_ROOT(vm, target);
    PX_ROOT(vm, this_val);
    a = args_array(vm, argc, argv);
    r = a == PX_EXCEPTION ? a : call_trap(vm, as_proxy(proxy)->handler, trap, target, 3, this_val, a, PX_UNDEFINED);
    px_pop_roots(vm, 3);
    return r;
}

PxValue px_proxy_construct(PxVM *vm, PxValue proxy, int argc, PxValue *argv, PxValue new_target) {
    PxValue trap, target, a, r;
    if (trap_of(vm, proxy, "construct", &trap, &target) < 0) return PX_EXCEPTION;
    if (trap == PX_UNDEFINED) return px_construct_nt(vm, target, argc, argv, new_target == proxy ? target : new_target);
    PX_ROOT(vm, trap);
    PX_ROOT(vm, target);
    PX_ROOT(vm, new_target);
    a = args_array(vm, argc, argv);
    r = a == PX_EXCEPTION ? a : call_trap(vm, as_proxy(proxy)->handler, trap, target, 3, a, new_target, PX_UNDEFINED);
    px_pop_roots(vm, 3);
    if (r != PX_EXCEPTION && !px_is_obj(r)) return px_throw_error(vm, PX_TYPE_ERROR, "construct trap did not return an object");
    return r;
}

/* ------------------------------------------------------------ Proxy */

static PxValue make_proxy(PxVM *vm, PxValue target, PxValue handler) {
    PxProxy *p;
    if (!px_is_obj(target) || !px_is_obj(handler))
        return px_throw_error(vm, PX_TYPE_ERROR, "Proxy: the target and the handler must be objects");
    PX_ROOT(vm, target);
    PX_ROOT(vm, handler);
    p = (PxProxy *)px_obj_new(vm, PX_T_PROXY, sizeof(PxProxy), vm->proxy_proto);
    px_pop_roots(vm, 2);
    if (!p) return PX_EXCEPTION;
    p->target        = target;
    p->handler       = handler;
    p->callable      = (uint8_t)px_is_callable(target);
    p->constructable = (uint8_t)px_is_constructor(target);
    return px_from_ptr(p);
}

static PxValue proxy_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (vm->native_new_target == PX_UNDEFINED) return px_throw_error(vm, PX_TYPE_ERROR, "Proxy must be called with new");
    return make_proxy(vm, ARG(0), ARG(1));
}

static PxValue revoke_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue p = px_function_data(vm);
    (void)t;
    (void)argc;
    (void)argv;
    if (px_is_obj(p) && px_type_of(p) == PX_T_PROXY) {
        as_proxy(p)->handler = PX_NULL;
        as_proxy(p)->target  = PX_NULL;
    }
    return PX_UNDEFINED;
}

static PxValue proxy_revocable(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue p = make_proxy(vm, ARG(0), ARG(1)), o, f;
    (void)t;
    if (p == PX_EXCEPTION) return p;
    PX_ROOT(vm, p);
    o = px_object_new(vm);
    if (o == PX_EXCEPTION) goto fail;
    PX_ROOT(vm, o);
    if (px_def_value(vm, o, "proxy", p, PX_ATTR_DEFAULT) < 0) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    f = px_make_native_data(vm, revoke_fn, "", 0, p);
    if (f != PX_EXCEPTION) ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
    if (f == PX_EXCEPTION || px_def_value(vm, o, "revoke", f, PX_ATTR_DEFAULT) < 0) {
        px_pop_roots(vm, 1);
        goto fail;
    }
    px_pop_roots(vm, 2);
    return o;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

int px_proxy_init(PxVM *vm) {
    static const PxFnDef statics[] = {{"revocable", proxy_revocable, 2, 0}};
    PxValue              ctor;
    /* the private prototype placeholder: gives proxies a shape of their own */
    vm->proxy_proto = px_object_new(vm);
    if (vm->proxy_proto == PX_EXCEPTION) return -1;
    ctor = px_make_native(vm, proxy_ctor, "Proxy", 2, 0);
    if (ctor == PX_EXCEPTION) return -1;
    return px_def_value(vm, vm->global, "Proxy", ctor, PX_ATTR_HIDDEN) < 0 ||
                   px_def_fns(vm, ctor, statics, PX_COUNTOF(statics)) < 0
               ? -1
               : 0;
}
