/* The same runner as pxjs, on QuickJS: for comparing the engines (bench/). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int i;
    for (i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        fputs(s ? s : "?", stdout);
        JS_FreeCString(ctx, s);
        if (i + 1 < argc) fputc(' ', stdout);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}
static JSValue js_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return JS_NewFloat64(ctx, ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}
int main(int argc, char **argv) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx), r;
    FILE *f = fopen(argv[1], "rb");
    long n; char *src;
    JSMemoryUsage u;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    src = malloc(n + 1); fread(src, 1, n, f); src[n] = 0; fclose(f);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, g, "now", JS_NewCFunction(ctx, js_now, "now", 0));
    JS_FreeValue(ctx, g);
    r = JS_Eval(ctx, src, n, argv[1], 0);
    if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); const char *s = JS_ToCString(ctx, e); fprintf(stderr, "error: %s\n", s); return 1; }
    JS_FreeValue(ctx, r);
    JS_ComputeMemoryUsage(rt, &u);
    fprintf(stderr, "quickjs heap after run: %lld kB\n", (long long)(u.malloc_size / 1024));
    return 0;
}
