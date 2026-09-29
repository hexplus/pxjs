/* pxjs: runs JavaScript files on the host with the PXJS engine.
 *
 *   pxjs [--gc-stress] [--heap KB] [--stats] file.js [file.js ...]
 *   pxjs --disasm file.js      the bytecode, instead of running it
 *   pxjs --count-allocs file.js       how many heap allocations it makes
 *   pxjs --fail-alloc N file.js       the Nth allocation fails (out of
 *                                     memory); the VM must then still work:
 *                                     a check script runs, and a collection
 *
 * Globals for scripts: print(...), console.log/info/warn/error(...),
 * assert(cond, message), assertEq(actual, expected, message) and
 * gc(). Exit code 0 when every file ran without an uncaught exception. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pxjs.h"

static PxValue js_print(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char buf[4096];
    int  i;
    (void)t;
    for (i = 0; i < argc; i++) {
        px_inspect(vm, argv[i], buf, sizeof buf);
        fputs(buf, stdout);
        if (i + 1 < argc) fputc(' ', stdout);
    }
    fputc('\n', stdout);
    fflush(stdout); /* a later hang or crash must not swallow it */
    return PX_UNDEFINED;
}

static PxValue js_assert(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char msg[512] = "assertion failed";
    (void)t;
    if (argc > 0 && argv[0] != PX_FALSE && argv[0] != PX_UNDEFINED && argv[0] != PX_NULL &&
        !(px_is_number(argv[0]) && px_get_number(argv[0]) == 0))
        return PX_UNDEFINED;
    if (argc > 1) px_to_utf8(vm, argv[1], msg, sizeof msg);
    return px_throw_error(vm, PX_ERROR, "%s", msg);
}

static PxValue js_assert_eq(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char a[512], b[512], m[256] = "";
    (void)t;
    if (argc < 2) return px_throw_error(vm, PX_ERROR, "assertEq needs two arguments");
    px_inspect(vm, argv[0], a, sizeof a);
    px_inspect(vm, argv[1], b, sizeof b);
    if (strcmp(a, b) == 0) return PX_UNDEFINED;
    if (argc > 2) px_to_utf8(vm, argv[2], m, sizeof m);
    return px_throw_error(vm, PX_ERROR, "%s%sexpected %s, got %s", m, m[0] ? ": " : "", b, a);
}

static PxValue js_gc(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    px_gc(vm);
    return PX_UNDEFINED;
}

static PxValue js_mem(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxMemStats s;
    (void)t;
    (void)argc;
    (void)argv;
    px_mem_stats(vm, &s);
    return px_number(vm, (double)s.used_bytes);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static PxValue js_now(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_number(vm, (double)now_us() / 1000.0);
}

static int g_unhandled;

static void track_rejection(PxVM *vm, PxValue promise, PxValue reason, int handled, void *opaque) {
    (void)vm;
    (void)promise;
    (void)reason;
    (void)opaque;
    /* counted, not printed: a handler attached later in the same turn
     * takes it back (handled = 1) */
    g_unhandled += handled ? -1 : 1;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long  n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
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

int px_disassemble(PxVM *vm, const char *src, size_t len, const char *filename, FILE *out);

int main(int argc, char **argv) {
    PxConfig cfg;
    int      i, failures = 0, stats = 0, disasm = 0, count_allocs = 0;
    uint32_t fail_at = 0;

    px_config_default(&cfg);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gc-stress") == 0) cfg.gc_stress = 1;
        else if (strcmp(argv[i], "--stats") == 0) stats = 1;
        else if (strcmp(argv[i], "--disasm") == 0) disasm = 1;
        else if (strcmp(argv[i], "--count-allocs") == 0) count_allocs = 1;
        else if (strcmp(argv[i], "--fail-alloc") == 0 && i + 1 < argc) fail_at = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--heap") == 0 && i + 1 < argc) cfg.heap_bytes = (size_t)atol(argv[++i]) * 1024u;
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: pxjs [--gc-stress] [--heap KB] [--stats] file.js...\n");
        return 2;
    }
    for (; i < argc; i++) {
        PxVM      *vm = px_new(&cfg);
        size_t     len;
        char      *src = read_file(argv[i], &len);
        uint64_t   t0;
        PxValue    console;
        if (!vm || !src) {
            fprintf(stderr, "%s: %s\n", argv[i], vm ? "cannot read" : "cannot create the VM");
            failures++;
            free(src);
            px_free(vm);
            continue;
        }
        if (disasm) {
            if (px_disassemble(vm, src, len, argv[i], stdout) != 0) {
                fprintf(stderr, "%s: %s\n", argv[i], px_error_text(vm));
                failures++;
            }
            free(src);
            px_free(vm);
            continue;
        }
        px_set_clock(vm, now_us);
        px_set_global_function(vm, "print", js_print, 1);
        px_set_global_function(vm, "assert", js_assert, 2);
        px_set_global_function(vm, "assertEq", js_assert_eq, 3);
        px_set_global_function(vm, "gc", js_gc, 0);
        px_set_global_function(vm, "heapUsed", js_mem, 0);
        px_set_global_function(vm, "now", js_now, 0);
        /* console.* all print */
        {
            static const char setup[] =
                "var console = { log: print, info: print, warn: print, error: print, debug: print };";
            if (px_eval(vm, setup, sizeof setup - 1, "<setup>", &console) != 0)
                fprintf(stderr, "setup: %s\n", px_error_text(vm));
        }
        t0          = now_us();
        g_unhandled = 0;
        px_set_rejection_tracker(vm, track_rejection, NULL);
        if (count_allocs || fail_at) px_set_alloc_failure(vm, count_allocs ? UINT32_MAX : fail_at);
        if (fail_at) {
            /* out of memory somewhere in the script: whatever it did, the VM
             * must still be consistent afterwards */
            static const char check[] =
                "var s = 0, a = []; for (var k = 0; k < 2000; k++) { s += k; a.push({ k: k, t: 'x' + k }); }"
                "if (s !== 1999000 || a.length !== 2000 || a[1999].t !== 'x1999') throw new Error('corrupt');";
            int rc = px_eval(vm, src, len, argv[i], NULL);
            px_set_alloc_failure(vm, 0);
            while (rc == 0 && px_has_jobs(vm))
                if (px_run_jobs(vm) != 0) break;
            px_gc(vm);
            if (px_eval(vm, check, sizeof check - 1, "<check>", NULL) != 0) {
                fprintf(stderr, "%s: after the failed allocation #%u the VM is broken: %s\n", argv[i], fail_at,
                        px_error_text(vm));
                failures++;
            }
            px_gc(vm);
            free(src);
            px_free(vm);
            continue;
        }
        if (px_eval(vm, src, len, argv[i], NULL) != 0) {
            fprintf(stderr, "%s: uncaught %s\n", argv[i], px_error_text(vm));
            failures++;
        } else {
            /* the microtask checkpoint: promises and async functions */
            while (px_has_jobs(vm))
                if (px_run_jobs(vm) != 0) {
                    fprintf(stderr, "%s: uncaught (in a job) %s\n", argv[i], px_error_text(vm));
                    failures++;
                    break;
                }
            if (g_unhandled > 0) {
                fprintf(stderr, "%s: %d unhandled promise rejection(s)\n", argv[i], g_unhandled);
                failures++;
            }
        }
        if (count_allocs) printf("allocations: %u\n", px_set_alloc_failure(vm, 0));
        if (stats) {
            PxMemStats s;
            px_mem_stats(vm, &s);
            fprintf(stderr, "%s: %.1f ms, heap %lu kB used of %lu, %lu GCs\n", argv[i], (now_us() - t0) / 1000.0,
                    (unsigned long)(s.used_bytes / 1024), (unsigned long)(s.heap_bytes / 1024),
                    (unsigned long)s.gc_count);
        }
        free(src);
        px_free(vm);
    }
    return failures ? 1 : 0;
}
