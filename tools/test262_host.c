/* test262_host: runs one Test262 test in a fresh VM and reports how it ended.
 *
 *   test262_host [--strict] [--module] [--gc-stress] include.js ... -- test.js
 *
 * The includes (assert.js, sta.js, and whatever the test lists) run first,
 * as sloppy scripts in the global scope. Then the test runs as a script
 * (with "use strict"; in front for --strict) or as a module, and the job
 * queue is drained. The last line of output is one of
 *
 *   PXJS-RESULT ok
 *   PXJS-RESULT parse-error <ErrorName> <message>
 *   PXJS-RESULT runtime-error <ErrorName> <message>
 *
 * and async tests also print Test262:AsyncTestComplete or
 * Test262:AsyncTestFailure:... through print(), as the harness expects.
 * The Python driver (tools/test262.py) interprets all this.
 *
 * $262 provides: global, gc(), evalScript(source), detachArrayBuffer(buffer)
 * (DetachArrayBuffer, as ArrayBuffer.prototype.transfer does it), and
 * createRealm as a function that throws (PXJS has one realm; the driver
 * classifies tests that need it separately). This tool uses the engine's
 * internal API (px_compile/px_run_proto) to tell a parse-phase error from
 * a runtime one. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "px_internal.h"
#include "pxjs.h"

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t g_deadline;

static int interrupt(PxVM *vm, void *opaque) {
    (void)vm;
    (void)opaque;
    return now_us() > g_deadline;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long  n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)n + 32);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len   = (size_t)n;
    return buf;
}

static PxValue js_print(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char buf[2048];
    int  i;
    (void)t;
    for (i = 0; i < argc; i++) {
        px_to_utf8(vm, argv[i], buf, sizeof buf);
        fputs(buf, stdout);
        if (i + 1 < argc) fputc(' ', stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return PX_UNDEFINED;
}

static PxValue js_gc(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    px_gc(vm);
    return PX_UNDEFINED;
}

static PxValue js_unsupported(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_throw_error(vm, PX_TYPE_ERROR, "$262: not supported by this host");
}

static PxValue js_detach(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    if (argc < 1 || px_array_buffer_detach(argv[0]) < 0)
        return px_throw_error(vm, PX_TYPE_ERROR, "detachArrayBuffer: not an ArrayBuffer");
    return PX_NULL;
}

/* $262.evalScript(source): a host operation (ScriptEvaluation), not eval */
static PxValue js_eval_script(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char    *src;
    int      n;
    PxProto *p;
    PxValue  pv, r;
    (void)t;
    if (argc < 1) return PX_UNDEFINED;
    n = px_to_utf8(vm, argv[0], NULL, 0);
    if (n < 0) return PX_EXCEPTION;
    src = (char *)malloc((size_t)n + 1);
    if (!src) return px_throw_error(vm, PX_RANGE_ERROR, "out of memory");
    px_to_utf8(vm, argv[0], src, (size_t)n + 1);
    p = px_compile(vm, src, (size_t)n, "<evalScript>", 0);
    free(src);
    if (!p) return PX_EXCEPTION;
    pv = px_from_ptr(p);
    PX_ROOT(vm, pv);
    r = px_run_proto(vm, p, vm->global);
    px_pop_roots(vm, 1);
    return r;
}

/* The name of a thrown value: its constructor's name, else its `name`. */
static void describe(PxVM *vm, PxValue e, char *name, size_t ncap, char *msg, size_t mcap) {
    snprintf(name, ncap, "Unknown");
    msg[0] = '\0';
    PX_ROOT(vm, e);
    if (px_is_object(e)) {
        PxValue c = px_get_prop(vm, e, "constructor"), n = PX_UNDEFINED, m;
        if (c != PX_EXCEPTION && px_is_object(c)) n = px_get_prop(vm, c, "name");
        if (n == PX_EXCEPTION || !px_is_string(n)) n = px_get_prop(vm, e, "name");
        if (n != PX_EXCEPTION && px_is_string(n)) px_to_utf8(vm, n, name, ncap);
        m = px_get_prop(vm, e, "message");
        if (m != PX_EXCEPTION && px_is_string(m)) px_to_utf8(vm, m, msg, mcap);
    } else {
        snprintf(name, ncap, "NonObject");
        px_inspect(vm, e, msg, mcap);
    }
    vm->exception = PX_UNDEFINED;
    px_pop_roots(vm, 1);
    {
        char *p;
        for (p = msg; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

static void report(const char *kind, PxVM *vm) {
    char name[128], msg[512];
    describe(vm, vm->exception, name, sizeof name, msg, sizeof msg);
    printf("PXJS-RESULT %s %s %s\n", kind, name, msg);
    fflush(stdout);
}

static PxValue resolve_module(PxVM *vm, const char *spec, void *opaque) {
    (void)opaque;
    return px_throw_error(vm, PX_SYNTAX_ERROR, "cannot import \"%s\": PXJS runs one module (no module graph)", spec);
}

int main(int argc, char **argv) {
    PxConfig cfg;
    PxVM    *vm;
    int      i, strict = 0, module = 0;
    size_t   len;
    char    *src;
    PxProto *p;
    PxValue  r, g;

    px_config_default(&cfg);
    /* Some tests build multi-megabyte strings (the generated Unicode
     * property tests do); conformance, not the PSP's memory, is measured
     * here. */
    cfg.heap_bytes = 64u << 20;
    for (i = 1; i < argc && strcmp(argv[i], "--") != 0 && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (strcmp(argv[i], "--strict") == 0) strict = 1;
        else if (strcmp(argv[i], "--module") == 0) module = 1;
        else if (strcmp(argv[i], "--gc-stress") == 0) cfg.gc_stress = 1;
    }
    vm = px_new(&cfg);
    if (!vm) return 3;
    g_deadline = now_us() + 20u * 1000000u;
    px_set_interrupt(vm, interrupt, NULL);
    px_set_module_resolver(vm, resolve_module, NULL);
    px_set_global_function(vm, "print", js_print, 1);

    /* $262 */
    g = px_new_object(vm);
    PX_ROOT(vm, g);
    px_set_prop(vm, g, "global", px_global_object(vm));
    px_set_prop(vm, g, "gc", px_new_function(vm, js_gc, "gc", 0));
    px_set_prop(vm, g, "evalScript", px_new_function(vm, js_eval_script, "evalScript", 1));
    px_set_prop(vm, g, "detachArrayBuffer", px_new_function(vm, js_detach, "detachArrayBuffer", 1));
    px_set_prop(vm, g, "createRealm", px_new_function(vm, js_unsupported, "createRealm", 0));
    px_set_global_value(vm, "$262", g);
    px_pop_roots(vm, 1);

    /* the includes */
    for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
        src = read_file(argv[i], &len);
        if (!src) {
            printf("PXJS-RESULT host-error cannot read %s\n", argv[i]);
            return 2;
        }
        if (px_eval(vm, src, len, argv[i], NULL) != 0) {
            printf("PXJS-RESULT host-error include %s failed: %s\n", argv[i], px_error_text(vm));
            free(src);
            return 2;
        }
        free(src);
    }
    if (i + 1 >= argc) {
        fprintf(stderr, "usage: test262_host [--strict] [--module] [--gc-stress] include.js ... -- test.js\n");
        return 2;
    }

    /* the test */
    src = read_file(argv[i + 1], &len);
    if (!src) {
        printf("PXJS-RESULT host-error cannot read %s\n", argv[i + 1]);
        return 2;
    }
    if (strict) {
        static const char k_use[] = "\"use strict\";\n";
        memmove(src + sizeof k_use - 1, src, len + 1);
        memcpy(src, k_use, sizeof k_use - 1);
        len += sizeof k_use - 1;
    }
    p = px_compile(vm, src, len, argv[i + 1], module);
    free(src);
    if (!p) {
        report("parse-error", vm);
        px_free(vm);
        return 1;
    }
    {
        PxValue pv = px_from_ptr(p);
        PX_ROOT(vm, pv);
        r = px_run_proto(vm, p, module ? PX_UNDEFINED : vm->global);
        px_pop_roots(vm, 1);
    }
    if (r == PX_EXCEPTION) {
        report("runtime-error", vm);
        px_free(vm);
        return 1;
    }
    if (module) {
        /* the module body's promise: a rejection is the module failing */
        PX_ROOT(vm, r);
        while (px_has_jobs(vm))
            if (px_run_jobs(vm) != 0) {
                printf("PXJS-RESULT runtime-error JobError %s\n", px_error_text(vm));
                return 1;
            }
        if (px_promise_state(r) == 2) {
            vm->exception = px_promise_result(r);
            report("runtime-error", vm);
            return 1;
        }
        px_pop_roots(vm, 1);
    }
    while (px_has_jobs(vm))
        if (px_run_jobs(vm) != 0) {
            printf("PXJS-RESULT runtime-error JobError %s\n", px_error_text(vm));
            return 1;
        }
    printf("PXJS-RESULT ok\n");
    px_free(vm);
    return 0;
}
