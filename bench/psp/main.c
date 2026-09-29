/* PXJS vs QuickJS on the PSP: the same benchmark file (app.js, next to
 * EBOOT.PBP) run on each engine in turn, results appended to
 * ms0:/PSPX/logs/PSPX.log. Built and run (in PPSSPP) by
 * scripts/bench-psp.sh [file.js]; for a PSP-1000, copy EBOOT.PBP and the
 * script (as app.js) to ms0:/PSP/GAME/PXBENCH/.
 *
 * Only engine speed is measured here: no runtime, no bindings. Both engines
 * get `print`, `now` (milliseconds, from the same clock), `gc` (a full
 * collection), `nop()` (a native that does nothing: the JS -> native call
 * cost) and `callJS(fn, n)` (n calls of fn from C: the native -> JS cost),
 * and a 6 MB heap. The script must not depend on anything else. Before the
 * script, each engine reports its startup time and the memory its
 * built-ins take. */

#include <pspkernel.h>
#include <pspdebug.h>
#include <psppower.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pxjs.h"
#include "quickjs.h"

PSP_MODULE_INFO("PXBENCH", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(16384);

#define JS_HEAP (6u * 1024u * 1024u)
#define LOG     "ms0:/PSPX/logs/PSPX.log"

static const char *g_engine = "";

static void out(const char *line) {
    char buf[512];
    int  fd, n;
    n = snprintf(buf, sizeof buf, "%s%s\n", g_engine, line);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    pspDebugScreenPrintf("%s", buf);
    fd = sceIoOpen(LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
}

static double now_ms(void) { return (double)sceKernelGetSystemTimeWide() / 1000.0; }

/* ------------------------------------------------------------ PXJS */

static PxValue px_print(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    char line[400];
    int  i, len = 0;
    (void)t;
    line[0] = '\0';
    for (i = 0; i < argc && len + 2 < (int)sizeof line; i++) {
        if (i) line[len++] = ' ';
        px_to_utf8(vm, argv[i], line + len, sizeof line - (size_t)len);
        len += (int)strlen(line + len);
    }
    out(line);
    return PX_UNDEFINED;
}

static PxValue px_now(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_number(vm, now_ms());
}

static PxValue px_gc_fn(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    px_gc(vm);
    return PX_UNDEFINED;
}

/* nop(): the cost of a JS -> native call */
static PxValue px_nop(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)vm;
    (void)t;
    (void)argc;
    (void)argv;
    return PX_UNDEFINED;
}

/* callJS(fn, n): calls fn() n times from C, the cost of native -> JS */
static PxValue px_call_js(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    int32_t i, n;
    (void)t;
    if (argc < 2 || !px_is_int(argv[1])) return PX_UNDEFINED;
    n = px_get_int(argv[1]);
    for (i = 0; i < n; i++)
        if (px_call(vm, argv[0], PX_UNDEFINED, 0, NULL) == PX_EXCEPTION) return PX_EXCEPTION;
    return PX_UNDEFINED;
}

static void run_pxjs(const char *src, size_t len) {
    PxConfig   cfg;
    PxVM      *vm;
    PxMemStats s;
    char       line[160];
    double     t0 = now_ms();

    g_engine = "PXJS    ";
    px_config_default(&cfg);
    cfg.heap_bytes = JS_HEAP;
    vm             = px_new(&cfg);
    if (!vm) {
        out("cannot create the VM");
        return;
    }
    px_gc(vm);
    px_mem_stats(vm, &s);
    snprintf(line, sizeof line, "startup %.2f ms, %lu kB live, %lu cells", now_ms() - t0,
             (unsigned long)(s.used_bytes / 1024), (unsigned long)s.objects);
    out(line);
    px_set_global_function(vm, "print", px_print, 1);
    px_set_global_function(vm, "now", px_now, 0);
    px_set_global_function(vm, "gc", px_gc_fn, 0);
    px_set_global_function(vm, "nop", px_nop, 0);
    px_set_global_function(vm, "callJS", px_call_js, 2);
    if (px_eval(vm, src, len, "bench.js", NULL) != 0) out(px_error_text(vm));
    while (px_has_jobs(vm)) px_run_jobs(vm);
    px_mem_stats(vm, &s);
    snprintf(line, sizeof line, "total %lu ms, %lu GCs, heap %lu kB used", (unsigned long)(now_ms() - t0),
             (unsigned long)s.gc_count, (unsigned long)(s.used_bytes / 1024));
    out(line);
    px_free(vm);
}

/* ------------------------------------------------------------ QuickJS */

static JSValue q_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    char line[400];
    int  i, len = 0;
    (void)t;
    line[0] = '\0';
    for (i = 0; i < argc && len + 2 < (int)sizeof line; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (i) line[len++] = ' ';
        snprintf(line + len, sizeof line - (size_t)len, "%s", s ? s : "?");
        len += (int)strlen(line + len);
        JS_FreeCString(ctx, s);
    }
    out(line);
    return JS_UNDEFINED;
}

static JSValue q_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, now_ms());
}

static JSValue q_gc(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue q_nop(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx;
    (void)t;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue q_call_js(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int32_t i, n = 0;
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    JS_ToInt32(ctx, &n, argv[1]);
    for (i = 0; i < n; i++) {
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) return r;
        JS_FreeValue(ctx, r);
    }
    return JS_UNDEFINED;
}

static void run_quickjs(const char *src, size_t len) {
    JSRuntime    *rt;
    JSContext    *ctx;
    JSValue       g, r;
    JSMemoryUsage u;
    char          line[160];
    double        t0 = now_ms();

    g_engine = "QuickJS ";
    rt       = JS_NewRuntime();
    if (!rt) {
        out("cannot create the runtime");
        return;
    }
    JS_SetMemoryLimit(rt, JS_HEAP);
    JS_SetMaxStackSize(rt, 384 * 1024);
    ctx = JS_NewContext(rt);
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &u);
    snprintf(line, sizeof line, "startup %.2f ms, %lu kB malloc'd, %lu objects", now_ms() - t0,
             (unsigned long)(u.malloc_size / 1024), (unsigned long)u.obj_count);
    out(line);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, q_print, "print", 1));
    JS_SetPropertyStr(ctx, g, "nop", JS_NewCFunction(ctx, q_nop, "nop", 0));
    JS_SetPropertyStr(ctx, g, "callJS", JS_NewCFunction(ctx, q_call_js, "callJS", 2));
    JS_SetPropertyStr(ctx, g, "now", JS_NewCFunction(ctx, q_now, "now", 0));
    JS_SetPropertyStr(ctx, g, "gc", JS_NewCFunction(ctx, q_gc, "gc", 0));
    JS_FreeValue(ctx, g);
    r = JS_Eval(ctx, src, len, "bench.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue     e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        out(s ? s : "exception");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    for (;;) {
        JSContext *c;
        if (JS_ExecutePendingJob(rt, &c) <= 0) break;
    }
    JS_ComputeMemoryUsage(rt, &u);
    snprintf(line, sizeof line, "total %lu ms, heap %lu kB used", (unsigned long)(now_ms() - t0),
             (unsigned long)(u.malloc_size / 1024));
    out(line);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ------------------------------------------------------------ main */

static char *read_file(const char *path, size_t *len) {
    SceIoStat st;
    char     *buf;
    int       fd, n;
    if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return NULL;
    buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) return NULL;
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        free(buf);
        return NULL;
    }
    n = sceIoRead(fd, buf, (SceSize)st.st_size);
    sceIoClose(fd);
    if (n != (int)st.st_size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len   = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    char   path[256], line[128], *slash;
    char  *src;
    size_t len = 0;

    pspDebugScreenInit();
    sceIoMkdir("ms0:/PSPX", 0777);
    sceIoMkdir("ms0:/PSPX/logs", 0777);
    snprintf(line, sizeof line, "PXJS %s vs QuickJS, cpu %d MHz, bus %d MHz", PXJS_VERSION, scePowerGetCpuClockFrequency(),
             scePowerGetBusClockFrequency());
    out(line);
    snprintf(path, sizeof path, "%s", argc > 0 ? argv[0] : "ms0:/PSP/GAME/PSPX/EBOOT.PBP");
    slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "app.js");
    src = read_file(path, &len);
    if (!src) {
        snprintf(line, sizeof line, "cannot read %s", path);
        out(line);
    } else {
        run_pxjs(src, len);
        run_quickjs(src, len);
        free(src);
    }
    g_engine = "";
    out("clean exit");
    sceKernelExitGame();
    return 0;
}
