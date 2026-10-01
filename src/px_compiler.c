/* The PXJS compiler: JavaScript source to bytecode in one pass, with no
 * syntax tree. On a 32 MB machine an AST for a large bundle would cost
 * more than the bytecode it produces; a single pass needs only the token
 * in hand and a small fixed context per function being compiled.
 *
 * What makes one pass enough:
 *
 *   Declaration pre-scan. Entering a block (or function body), the
 *   compiler scans ahead to the closing brace, token by token, and
 *   declares every let/const/class/function in it (and, for a function
 *   body, every var) before compiling the first statement. So a closure
 *   that refers to a `const` declared further down resolves to the right
 *   slot, and let/const start as a "hole" that makes early reads throw
 *   (the temporal dead zone).
 *
 *   Hoisting by jump. Function declarations must exist before the first
 *   statement runs, but their bodies are compiled where they appear. A
 *   block with function declarations starts with a jump to code placed at
 *   its end that creates those closures, and jumps back.
 *
 *   Rewinding. Where JavaScript puts things in the "wrong" order for a
 *   single pass -- a finally block that must run on every exit, a
 *   destructuring target that comes before its default value, class
 *   fields that run inside the constructor -- the lexer is rewound to the
 *   saved position and the same source is compiled again, in the place
 *   it is needed.
 *
 * Closures use Lua-style upvalues: a captured local stays in its stack
 * slot while its scope is alive and is copied out ("closed") when the
 * scope ends, so capturing costs nothing until it is needed. */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "px_internal.h"
#include "px_lexer.h"

#define MAX_LOCALS  1024 /* a bundled app's top level has hundreds; slots past 255 take OP_WIDE */
#define MAX_UPVALS  250
#define MAX_LOOPS   64
#define MAX_TRYS    32
#define MAX_HOISTS  1024 /* function declarations in one block: a bundled app has hundreds at its top level */
#define MAX_DEPTH   150 /* nested expressions/statements: bounds the C stack */
#define MAX_DECLS   1024
#define MAX_PARAMS  64
#define MAX_MEMBERS 256

enum { K_VAR = 0, K_LET, K_CONST, K_PARAM, K_FUNC, K_TEMP, K_CLASS };

#define IS_LEXICAL(k) ((k) == K_LET || (k) == K_CONST || (k) == K_CLASS)

typedef struct Local {
    PxValue name;
    uint8_t kind;
    uint8_t init;     /* let/const/class known initialised here: no TDZ check */
    uint8_t captured; /* a nested function uses it: scopes must close it */
} Local;

typedef struct Upv {
    PxValue name;
    uint8_t  is_local, kind;
    uint16_t index;
} Upv;

typedef struct IntList {
    int *v;
    int  n, cap;
} IntList;

enum { LOOP_PLAIN = 0, LOOP_SWITCH, LOOP_ITER, LOOP_BLOCK, LOOP_ASYNC_ITER };

typedef struct Loop {
    int     kind;
    int     cont_target; /* -1: forward, patched from conts */
    IntList breaks, conts;
    int     nactive;
    int     ntry;
    PxValue label; /* 0: none */
} Loop;

typedef struct TryCtx {
    int      has_finally;
    LexState finally_pos;
} TryCtx;

typedef struct Hoist {
    int      is_global;
    int      slot;
    uint16_t name_cidx;
    uint16_t cidx;
} Hoist;

typedef struct FuncState {
    struct FuncState *parent;
    uint8_t          *code;
    uint32_t          len, cap;
    uint8_t          *lines;
    uint32_t          lines_len, lines_cap, last_line, last_pc;
    PxVec            *consts;
    uint32_t          nconsts;
    Local             locals[MAX_LOCALS];
    int               nactive, nslots;
    Upv               upvals[MAX_UPVALS];
    int               nupvals;
    Loop              loops[MAX_LOOPS];
    int               nloops;
    TryCtx            trys[MAX_TRYS];
    int               ntry;
    Hoist             hoists[MAX_HOISTS];
    int               nhoists;
    int               stack, max_stack;
    int               flags; /* PX_PROTO_* */
    int               nparams;
    int               args_slot;
    int               is_script;
    int               is_module;
    int               completion; /* script: slot of the completion value */
    PxValue           pending_label;
    PxValue           name;
    uint32_t          line;
    /* peephole state: where the last few instructions start, and the
     * highest position any jump lands on (code before it is fixed) */
    int               ops[6];
    int               nops;
    int               last_target;
    int               switch_floor1; /* 1 + first local of a switch body; 0: none */
    int               nics;          /* inline caches: one per property-access site */
    int               self_slot1;    /* 1 + the local of the function's own name; 0: none */
    int               ctx;           /* CTX_*: what the code may contain */
    int               var_floor;     /* locals from here up belong to nested blocks */
    int               length;        /* the function's `length` */
    /* while a parameter's default or pattern is compiled: the locals not
     * initialised yet (later parameters, and the names their patterns
     * bind), read as a ReferenceError */
    int               tdz_lo, tdz_hi, tdz_lo2, tdz_hi2;
} FuncState;

/* What a function's code may contain, for early errors. Arrows inherit
 * everything but CTX_PARAMS from the code around them. */
enum {
    CTX_AWAIT_KW   = 1,  /* `await` is a keyword: async functions, modules, static blocks */
    CTX_NO_ARGS    = 2,  /* `arguments` is an error: class field initialisers, static blocks */
    CTX_SUPER_PROP = 4,  /* super.x: methods, field initialisers, static blocks */
    CTX_SUPER_CALL = 8,  /* super(): derived class constructors */
    CTX_NEW_TARGET = 16, /* new.target: functions (not the top level) */
    CTX_PARAMS     = 32, /* compiling the parameters: no yield/await expressions */
    CTX_STATIC_BLOCK = 64, /* a class static block: no return */
    CTX_OWN          = CTX_PARAMS | CTX_STATIC_BLOCK /* not inherited by arrows */
};

/* compile_function flags beyond PX_PROTO_* (which fit 16 bits): accessor
 * parameter rules. */
#define FN_GETTER 0x10000
#define FN_SETTER 0x20000

typedef struct Compiler {
    PxVM      *vm;
    Lexer      lx;
    FuncState *fs;
    jmp_buf    jb;
    PxValue    filename;
    int        depth;
    int        no_in; /* parsing a for(...) head: `in` is not an operator */
    uint32_t   nroots;
    PxValue    name_hint;
    void      *owned[16]; /* scratch buffers freed if compilation fails */
    int        nowned;
    /* Lookahead memos, keyed by source offset. The parser looks ahead over
     * a (...) for "=>" and over a [...] or {...} for "=", and prescan reads
     * each block for its declarations; without these, nested groups and
     * nested functions would be lexed again at every level. */
    struct SkipTab {
        struct Skip *e;
        uint32_t     cap, n;
    } fn_ends,   /* a nested function body's '{' -> the state after its '}' */
        group_ends; /* a '(' '[' '{' -> the state after its closing token */
    int args_hits;  /* `arguments` tokens skip_balanced has crossed (prescan reads it) */
    int is_module;
    int param_floor1; /* 1 + the first parameter the next block_begin checks lexical names against */
    int unary_op;     /* the last unary() read an operator (-x, typeof x...): not a ** base */
    int private_in;   /* primary() read `#x` (which must be followed by `in`) */
    struct Decls *exports, *export_locals; /* a module's exported names; the locals `export {x}` names */
    PxValue atom_arguments, atom_eval; /* names strict code may not bind */
} Compiler;

typedef struct Skip {
    uint32_t key; /* the offset + 1; 0: empty slot */
    LexState after;
    uint8_t  flags; /* SKIP_*: what the group holds that a prescan may need */
} Skip;

enum { SKIP_ARGS = 1, SKIP_VARS = 2 }; /* mentions `arguments`; declares `var` (outside nested functions) */

static Skip *skip_find(struct SkipTab *t, uint32_t pos) {
    uint32_t i;
    if (!t->cap) return NULL;
    for (i = (pos * 2654435761u) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        if (t->e[i].key == pos + 1) return &t->e[i];
        if (!t->e[i].key) return NULL;
    }
}

/* Best effort: without memory the table just stays smaller. */
static void skip_add(struct SkipTab *t, uint32_t pos, LexState after, int flags) {
    uint32_t i;
    if ((t->n + 1) * 2 > t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 256, k;
        Skip    *n   = (Skip *)calloc(cap, sizeof *n);
        if (!n) return;
        for (k = 0; k < t->cap; k++)
            if (t->e[k].key) {
                for (i = ((t->e[k].key - 1) * 2654435761u) & (cap - 1); n[i].key; i = (i + 1) & (cap - 1)) {}
                n[i] = t->e[k];
            }
        free(t->e);
        t->e   = n;
        t->cap = cap;
    }
    for (i = (pos * 2654435761u) & (t->cap - 1); t->e[i].key; i = (i + 1) & (t->cap - 1))
        if (t->e[i].key == pos + 1) return;
    t->e[i].key   = pos + 1;
    t->e[i].after = after;
    t->e[i].flags = (uint8_t)flags;
    t->n++;
}

/* E_SUPER_MEMBER / E_SUPER_INDEX: super.x / super[k], with `this` (and the
 * key) on the stack where E_MEMBER / E_INDEX have the object (and key) */
typedef enum { E_VOID = 0, E_VALUE, E_LOCAL, E_UPVAL, E_GLOBAL, E_MEMBER, E_INDEX, E_SUPER_MEMBER, E_SUPER_INDEX } EKind;
#define MEMBER_LIKE(k) ((k) == E_MEMBER || (k) == E_SUPER_MEMBER)
#define INDEX_LIKE(k)  ((k) == E_INDEX || (k) == E_SUPER_INDEX)

typedef struct Exp {
    EKind   k;
    int     idx;
    uint8_t kind;
    uint8_t priv; /* E_INDEX: a private member (obj.#x) */
    PxValue name;
} Exp;

enum { BIND_DECL = 0, BIND_ASSIGN };

/* ------------------------------------------------------------ tables */

#define VAR 0
const uint8_t px_op_size[OP__COUNT] = {
#define OP(name, bytes, effect) bytes,
#include "px_opcodes.h"
#undef OP
};
const int8_t px_op_effect[OP__COUNT] = {
#define OP(name, bytes, effect) effect,
#include "px_opcodes.h"
#undef OP
};
const char *px_op_name[OP__COUNT] = {
#define OP(name, bytes, effect) #name,
#include "px_opcodes.h"
#undef OP
};
#undef VAR

/* ------------------------------------------------------------ errors */

static void fail(Compiler *C, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

static void fail(Compiler *C, const char *fmt, ...) {
    static const struct {
        const char *prefix;
        PxErrorType type;
    } k_types[] = {{"SyntaxError: ", PX_SYNTAX_ERROR}, {"RangeError: ", PX_RANGE_ERROR},
                   {"ReferenceError: ", PX_REFERENCE_ERROR}, {"TypeError: ", PX_TYPE_ERROR}};
    char        msg[300], full[400], file[64];
    const char *text = msg;
    PxErrorType type = PX_SYNTAX_ERROR;
    size_t      i;
    va_list     ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* Messages name their error type ("RangeError: ..."): that picks the
     * type, and the prefix is not repeated in the message. */
    for (i = 0; i < sizeof k_types / sizeof k_types[0]; i++)
        if (strncmp(msg, k_types[i].prefix, strlen(k_types[i].prefix)) == 0) {
            type = k_types[i].type;
            text = msg + strlen(k_types[i].prefix);
            break;
        }
    px_str_to_utf8(C->vm, C->filename, file, sizeof file);
    snprintf(full, sizeof full, "%s (%s:%u)", text, file, (unsigned)C->lx.tok.line);
    C->vm->nroots = C->nroots;
    {
        PxValue m = px_str_from_cstr(C->vm, full);
        if (m != PX_EXCEPTION) {
            PxValue e = px_error_new(C->vm, type, m);
            if (e != PX_EXCEPTION) C->vm->exception = e;
        }
    }
    longjmp(C->jb, 1);
}

static void check_alloc(Compiler *C, int ok) {
    if (!ok) {
        C->vm->nroots = C->nroots;
        longjmp(C->jb, 1); /* the allocator already set the exception */
    }
}

/* Scratch memory that a syntax error (a longjmp) must not leak. */
static void *c_alloc(Compiler *C, size_t n) {
    void *p;
    if (C->nowned >= 16) fail(C, "SyntaxError: nesting too deep");
    p = malloc(n);
    if (!p) {
        px_throw_oom(C->vm);
        check_alloc(C, 0);
    }
    C->owned[C->nowned++] = p;
    return p;
}

static void c_free(Compiler *C, void *p) {
    int i;
    for (i = C->nowned - 1; i >= 0; i--)
        if (C->owned[i] == p) {
            C->owned[i] = C->owned[--C->nowned];
            break;
        }
    free(p);
}

static void unexpected(Compiler *C) {
    if (C->lx.tok.type == T_ERROR) fail(C, "SyntaxError: %s", C->lx.tok.error);
    fail(C, "SyntaxError: unexpected %s", tok_name(C->lx.tok.type));
}

/* ------------------------------------------------------------ tokens */

#define TOK (C->lx.tok.type)

static void next(Compiler *C) {
    lex_next(&C->lx);
    if (TOK == T_ERROR) unexpected(C);
}

static void expect(Compiler *C, TokType t) {
    if (TOK != t) fail(C, "SyntaxError: expected '%s' but found '%s'", tok_name(t), tok_name(TOK));
    next(C);
}

static int in_generator(Compiler *C) { return (C->fs->flags & PX_PROTO_GENERATOR) != 0; }
static int in_async(Compiler *C) { return (C->fs->flags & PX_PROTO_ASYNC) != 0; }
static int in_async_generator(Compiler *C) {
    return (C->fs->flags & (PX_PROTO_ASYNC | PX_PROTO_GENERATOR)) == (PX_PROTO_ASYNC | PX_PROTO_GENERATOR);
}

static int is_name_tok_raw(TokType t) {
    return t == T_IDENT || t == T_OF || t == T_GET || t == T_SET || t == T_STATIC || t == T_ASYNC ||
           t == T_AWAIT || t == T_YIELD || t == T_LET;
}

static PxValue tok_atom(Compiler *C);

static int await_is_keyword(Compiler *C) { return (C->fs->ctx & CTX_AWAIT_KW) || C->is_module; }

/* An identifier token that is a reserved word of strict code: the future
 * reserved words, and keywords spelled with escapes (which stay T_IDENT). */
static int reserved_ident(Compiler *C) {
    static const char *const k_future[] = {"enum", "implements", "interface", "package", "private", "protected",
                                           "public"};
    const char *s = C->lx.tok.ident;
    uint32_t    n = C->lx.tok.ident_len, i;
    if (C->lx.tok.escaped) {
        TokType k = lex_keyword_type(s, n);
        if (k == T_AWAIT) return await_is_keyword(C);
        if (k != T_IDENT && k != T_OF && k != T_GET && k != T_SET && k != T_ASYNC) return 1;
    }
    if (n < 4 || n > 10) return 0;
    for (i = 0; i < sizeof k_future / sizeof k_future[0]; i++)
        if (strlen(k_future[i]) == n && memcmp(k_future[i], s, n) == 0) return 1;
    return 0;
}

/* An identifier here (a reference or a binding). All code is strict:
 * yield, let and static are reserved everywhere, await in modules, async
 * functions and class static blocks. */
static int is_name(Compiler *C) {
    TokType t = TOK;
    switch (t) {
    case T_IDENT: return !reserved_ident(C);
    case T_AWAIT: return !await_is_keyword(C);
    case T_OF: case T_GET: case T_SET: case T_ASYNC: return 1;
    default: return 0;
    }
}

/* The name a declaration binds (the current token, consumed): strict code
 * may not bind eval or arguments. */
static PxValue binding_name(Compiler *C) {
    PxValue name;
    if (!is_name(C)) unexpected(C);
    name = tok_atom(C);
    if (name == C->atom_eval || name == C->atom_arguments)
        fail(C, "SyntaxError: '%s' cannot be a binding name in strict mode", C->lx.tok.ident);
    next(C);
    return name;
}

/* Keywords are valid property names (a.default, {new: 1}). */
static int is_prop_name_tok(TokType t) { return t >= T_IDENT && t <= T_SET; }

static int peek_is(Compiler *C, TokType t) {
    LexState st = lex_save(&C->lx);
    int      r;
    next(C);
    r = TOK == t;
    lex_restore(&C->lx, st);
    return r;
}

static void semicolon(Compiler *C) {
    if (TOK == T_SEMI) {
        next(C);
        return;
    }
    if (TOK == T_RBRACE || TOK == T_EOF || C->lx.tok.nl_before) return;
    fail(C, "SyntaxError: expected ';' but found '%s'", tok_name(TOK));
}

static PxValue tok_atom(Compiler *C) {
    PxValue s = px_str_from_utf8(C->vm, C->lx.tok.ident, C->lx.tok.ident_len), a;
    check_alloc(C, s != PX_EXCEPTION);
    a = px_intern(C->vm, s);
    check_alloc(C, a != PX_EXCEPTION);
    return a;
}

static int tok_str_is(Compiler *C, const char *w) {
    size_t n = strlen(w), i;
    if (TOK != T_STRING || C->lx.tok.str_len != n) return 0;
    for (i = 0; i < n; i++)
        if (C->lx.tok.str[i] != (uint16_t)(unsigned char)w[i]) return 0;
    return 1;
}

static int tok_is_word(Compiler *C, const char *w) {
    size_t n = strlen(w);
    return C->lx.tok.ident && C->lx.tok.ident_len == n && memcmp(C->lx.tok.ident, w, n) == 0 &&
           is_prop_name_tok(TOK);
}

/* The current string token as a value (as_key = 0) or as a property key
 * (as_key = 1: "5" becomes the index 5). */
static PxValue tok_string_as(Compiler *C, int as_key) {
    PxValue s = px_str_new_u16(C->vm, C->lx.tok.str, C->lx.tok.str_len), a;
    check_alloc(C, s != PX_EXCEPTION);
    a = as_key ? px_intern(C->vm, s) : px_intern_literal(C->vm, s);
    check_alloc(C, a != PX_EXCEPTION);
    return a;
}

/* Skips one balanced unit: a bracketed group (templates inside handled),
 * a whole template literal, or a single token. */
/* A group's memo flags: `arguments` if it was seen inside; a `var` is
 * assumed (skip_balanced does not look for them). */
#define GROUP_FLAGS(hits0) ((C->args_hits > (hits0) ? SKIP_ARGS : 0) | SKIP_VARS)

static void skip_balanced(Compiler *C) {
    int      tmpl[64], ntmpl = 0, depth = 0;
    uint32_t open[128]; /* where the groups being crossed started */
    int      hits[128];
    TokType  prevt = T_SEMI;
    for (;;) {
        TokType t = TOK;
        if (t == T_EOF) fail(C, "SyntaxError: unexpected end of input");
        if (t == T_IDENT && prevt != T_DOT && C->lx.tok.ident_len == 9 && tok_is_word(C, "arguments")) C->args_hits++;
        prevt = t;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) {
            Skip *sk = skip_find(&C->group_ends, C->lx.tok.start);
            if (sk) { /* crossed before */
                if (sk->flags & SKIP_ARGS) C->args_hits++;
                lex_restore(&C->lx, sk->after);
                prevt = T_RPAREN;
                if (depth <= 0 && ntmpl == 0) return;
                continue;
            }
            if (depth >= 0 && depth < 128) {
                open[depth] = C->lx.tok.start;
                hits[depth] = C->args_hits;
            }
            depth++;
        } else if (t == T_RPAREN || t == T_RBRACKET) {
            depth--;
            next(C);
            if (depth >= 0 && depth < 128) skip_add(&C->group_ends, open[depth], lex_save(&C->lx), GROUP_FLAGS(hits[depth]));
            if (depth <= 0 && ntmpl == 0) return;
            continue;
        } else if (t == T_RBRACE) {
            if (ntmpl > 0 && tmpl[ntmpl - 1] == depth) {
                lex_template_continue(&C->lx);
                if (TOK == T_ERROR) unexpected(C);
                if (C->lx.tok.template_tail) ntmpl--;
                next(C);
                if (depth == 0 && ntmpl == 0) return;
                continue;
            }
            depth--;
            next(C);
            if (depth >= 0 && depth < 128) skip_add(&C->group_ends, open[depth], lex_save(&C->lx), GROUP_FLAGS(hits[depth]));
            if (depth <= 0 && ntmpl == 0) return;
            continue;
        } else if (t == T_TEMPLATE && !C->lx.tok.template_tail) {
            if (ntmpl >= 64) fail(C, "SyntaxError: templates nested too deeply");
            tmpl[ntmpl++] = depth;
        }
        next(C);
        if (depth <= 0 && ntmpl == 0) return;
    }
}

/* Skips an expression up to a ',' ')' ']' '}' or ';' at its own level. */
static void skip_expression(Compiler *C) {
    while (TOK != T_COMMA && TOK != T_RPAREN && TOK != T_RBRACKET && TOK != T_RBRACE && TOK != T_SEMI &&
           TOK != T_EOF)
        skip_balanced(C);
}

/* ------------------------------------------------------------ emitting */

static void grow(Compiler *C, uint8_t **buf, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return;
    {
        uint32_t ncap = *cap ? *cap * 2 : 256;
        uint8_t *n;
        while (ncap < need) ncap *= 2;
        n = (uint8_t *)realloc(*buf, ncap);
        if (!n) {
            px_throw_oom(C->vm);
            check_alloc(C, 0);
        }
        *buf = n;
        *cap = ncap;
    }
}

static void emit_byte(Compiler *C, uint8_t b) {
    FuncState *fs = C->fs;
    grow(C, &fs->code, &fs->cap, fs->len + 1);
    fs->code[fs->len++] = b;
}

static void adjust(Compiler *C, int effect) {
    FuncState *fs = C->fs;
    fs->stack += effect;
    if (fs->stack > fs->max_stack) fs->max_stack = fs->stack;
}

static void put_varint(Compiler *C, uint32_t v) {
    FuncState *fs = C->fs;
    do {
        grow(C, &fs->lines, &fs->lines_cap, fs->lines_len + 1);
        fs->lines[fs->lines_len++] = (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0));
        v >>= 7;
    } while (v);
}

/* The line table: (pc delta, line delta) pairs, recorded when the line
 * changes. Error stacks read it back; it costs a few bytes per line. */
static void mark_line(Compiler *C) {
    FuncState *fs   = C->fs;
    uint32_t   line = C->lx.tok.line;
    int32_t    dl;
    if (line == fs->last_line) return;
    dl = (int32_t)line - (int32_t)fs->last_line;
    put_varint(C, fs->len - fs->last_pc);
    put_varint(C, ((uint32_t)dl << 1) ^ (uint32_t)(dl >> 31)); /* zigzag */
    fs->last_line = line;
    fs->last_pc   = fs->len;
}

static void note_op(FuncState *fs) {
    if (fs->nops == PX_COUNTOF(fs->ops)) {
        memmove(fs->ops, fs->ops + 1, sizeof fs->ops - sizeof fs->ops[0]);
        fs->nops--;
    }
    fs->ops[fs->nops++] = (int)fs->len;
}

/* The k-th instruction back (1 = the last): its opcode, or -1. */
static int op_back(FuncState *fs, int k) {
    if (k > fs->nops) return -1;
    return fs->code[fs->ops[fs->nops - k]];
}

static int operand_back(FuncState *fs, int k) { return fs->code[fs->ops[fs->nops - k] + 1]; }

/* POP after a store to a local, the commonest statement ending there is.
 *   GET_LOCAL x; INC|DEC; SET_LOCAL x; POP                -> INC|DEC_LOCAL x  (++x;)
 *   GET_LOCAL x; TO_NUMERIC; DUP; INC|DEC; PUT_LOCAL x; POP -> INC|DEC_LOCAL x  (x++;)
 *   SET_LOCAL x; POP                                        -> PUT_LOCAL x
 * Safe only when no jump lands after the first instruction rewritten
 * (fs->last_target), and no line-table entry points inside it. */
static int peephole_pop(Compiler *C) {
    FuncState *fs = C->fs;
    int        last = op_back(fs, 1), n = 0, x, start;
    if (fs->last_target >= (int)fs->len || fs->nops == 0) return 0;
    if (last == OP_SET_LOCAL && op_back(fs, 2) >= 0 && (op_back(fs, 2) == OP_INC || op_back(fs, 2) == OP_DEC) &&
        op_back(fs, 3) == OP_GET_LOCAL && operand_back(fs, 3) == operand_back(fs, 1))
        n = 3;
    else if (last == OP_PUT_LOCAL && (op_back(fs, 2) == OP_INC || op_back(fs, 2) == OP_DEC) &&
             op_back(fs, 3) == OP_DUP && op_back(fs, 4) == OP_TO_NUMERIC && op_back(fs, 5) == OP_GET_LOCAL &&
             operand_back(fs, 5) == operand_back(fs, 1))
        n = 5;
    if (n) {
        start = fs->ops[fs->nops - n];
        if (start >= fs->last_target && (int)fs->last_pc <= start) {
            int inc = op_back(fs, 2) == OP_INC;
            x       = operand_back(fs, 1);
            fs->len = (uint32_t)start;
            fs->nops -= n;
            adjust(C, -1); /* the sequence left one value, which the POP took */
            note_op(fs);
            emit_byte(C, (uint8_t)(inc ? OP_INC_LOCAL : OP_DEC_LOCAL));
            emit_byte(C, (uint8_t)x);
            return 1;
        }
    }
    if (last == OP_SET_LOCAL) {
        fs->code[fs->ops[fs->nops - 1]] = OP_PUT_LOCAL;
        adjust(C, -1);
        return 1;
    }
    if (last == OP_WIDE && fs->code[fs->ops[fs->nops - 1] + 1] == OP_SET_LOCAL) {
        fs->code[fs->ops[fs->nops - 1] + 1] = OP_PUT_LOCAL;
        adjust(C, -1);
        return 1;
    }
    return 0;
}

static void emit_op(Compiler *C, PxOp op) {
    if (op == OP_POP && peephole_pop(C)) return;
    note_op(C->fs);
    emit_byte(C, (uint8_t)op);
    adjust(C, px_op_effect[op]);
}

static void emit_u16(Compiler *C, int v);
static void emit_op_u16(Compiler *C, PxOp op, int v);

/* GET_PROP / GET_PROP_KEEP / SET_PROP: the key's constant, then the
 * site's inline cache (0xFFFF: none left, the site runs uncached). */
static void emit_prop(Compiler *C, PxOp op, int cidx) {
    FuncState *fs = C->fs;
    emit_op_u16(C, op, cidx);
    emit_u16(C, fs->nics < 0xFFFF ? fs->nics++ : 0xFFFF);
}

/* The current position, as a jump target: code before it stays as is. */
static int label_here(Compiler *C) {
    C->fs->last_target = (int)C->fs->len;
    return (int)C->fs->len;
}

static void emit_op_u8(Compiler *C, PxOp op, int v) {
    emit_op(C, op);
    emit_byte(C, (uint8_t)v);
}

static void emit_u16(Compiler *C, int v) {
    emit_byte(C, (uint8_t)(v & 0xFF));
    emit_byte(C, (uint8_t)((v >> 8) & 0xFF));
}

static void emit_op_u16(Compiler *C, PxOp op, int v) {
    emit_op(C, op);
    emit_u16(C, v);
}

/* An instruction on local `slot`: its one-byte operand, or past 255 the
 * OP_WIDE prefix with the slot in two bytes (a bundled app's top level has
 * hundreds of locals). The fusions in peephole_pop see OP_WIDE and leave
 * it alone. */
static void emit_local(Compiler *C, PxOp op, int slot) {
    if (slot < 256) {
        emit_op_u8(C, op, slot);
        return;
    }
    note_op(C->fs);
    emit_byte(C, (uint8_t)OP_WIDE);
    emit_byte(C, (uint8_t)op);
    emit_u16(C, slot);
    adjust(C, px_op_effect[op]);
}

/* Returns the position of the 16-bit offset, to patch. */
static void emit_u16(Compiler *C, int v);

static int emit_jump(Compiler *C, PxOp op) {
    FuncState *fs = C->fs;
    int        last;
    /* LT..GE; JUMP_IF_FALSE -> one fused instruction, when no jump lands
     * between the two and no line-table entry points there */
    if (op == OP_JUMP_IF_FALSE && fs->nops > 0 && fs->last_target < (int)fs->len &&
        (int)fs->last_pc <= fs->ops[fs->nops - 1]) {
        last = op_back(fs, 1);
        if (last == OP_LT || last == OP_LE || last == OP_GT || last == OP_GE || last == OP_SEQ || last == OP_SNE) {
            fs->code[fs->ops[fs->nops - 1]] =
                (uint8_t)(last == OP_LT ? OP_LT_JUMP_IF_FALSE : last == OP_LE ? OP_LE_JUMP_IF_FALSE
                          : last == OP_GT ? OP_GT_JUMP_IF_FALSE : last == OP_GE ? OP_GE_JUMP_IF_FALSE
                          : last == OP_SEQ ? OP_SEQ_JUMP_IF_FALSE : OP_SNE_JUMP_IF_FALSE);
            emit_u16(C, 0);
            adjust(C, -1); /* the compare's result, which the jump takes */
            return (int)fs->len - 2;
        }
    }
    emit_op_u16(C, op, 0);
    return (int)fs->len - 2;
}

static void patch_to(Compiler *C, int at, int target) {
    int off = target - (at + 2);
    if (target > C->fs->last_target) C->fs->last_target = target;
    if (off < -32768 || off > 32767) fail(C, "RangeError: function too large (jump out of range)");
    C->fs->code[at]     = (uint8_t)(off & 0xFF);
    C->fs->code[at + 1] = (uint8_t)((off >> 8) & 0xFF);
}

static void patch_here(Compiler *C, int at) { patch_to(C, at, (int)C->fs->len); }

static void emit_back(Compiler *C, PxOp op, int target) {
    int at = emit_jump(C, op);
    patch_to(C, at, target);
}

static void list_add(Compiler *C, IntList *l, int v) {
    if (l->n == l->cap) {
        int  cap = l->cap ? l->cap * 2 : 8;
        int *n   = (int *)realloc(l->v, (size_t)cap * sizeof(int));
        if (!n) {
            px_throw_oom(C->vm);
            check_alloc(C, 0);
        }
        l->v   = n;
        l->cap = cap;
    }
    l->v[l->n++] = v;
}

/* ------------------------------------------------------------ constants */

static int add_const(Compiler *C, PxValue v) {
    FuncState *fs = C->fs;
    uint32_t   i;
    for (i = 0; i < fs->nconsts; i++) {
        PxValue c = fs->consts->items[i];
        if (c == v) return (int)i;
        if (px_is_ptr(c) && px_is_ptr(v) && px_type_of(c) == PX_T_NUMBER && px_type_of(v) == PX_T_NUMBER &&
            memcmp(&((PxNumber *)px_ptr(c))->d, &((PxNumber *)px_ptr(v))->d, sizeof(double)) == 0)
            return (int)i;
    }
    if (fs->nconsts >= 65535) fail(C, "RangeError: too many constants in one function");
    if (!fs->consts || fs->nconsts >= fs->consts->cap) {
        PxVec *g;
        PX_ROOT(C->vm, v);
        g = px_vec_grow(C->vm, fs->consts, fs->nconsts + 1);
        px_pop_roots(C->vm, 1);
        check_alloc(C, g != NULL);
        fs->consts = g;
    }
    fs->consts->items[fs->nconsts] = v;
    return (int)fs->nconsts++;
}

static void emit_number(Compiler *C, double d) {
    if (d == (double)(int32_t)d && !(d == 0 && 1 / d < 0)) {
        int32_t i = (int32_t)d;
        if (i >= -128 && i <= 127) {
            emit_op_u8(C, OP_INT8, (uint8_t)(int8_t)i);
            return;
        }
        if (i >= -32768 && i <= 32767) {
            emit_op_u16(C, OP_INT16, (uint16_t)(int16_t)i);
            return;
        }
    }
    {
        PxValue v = px_number(C->vm, d);
        check_alloc(C, v != PX_EXCEPTION);
        emit_op_u16(C, OP_CONST, add_const(C, v));
    }
}

static PxValue number_key(Compiler *C, double d) {
    PxValue s = px_number_to_string(C->vm, d, 10), k;
    check_alloc(C, s != PX_EXCEPTION);
    k = px_intern(C->vm, s);
    check_alloc(C, k != PX_EXCEPTION);
    return k;
}

/* ------------------------------------------------------------ scopes */

static int find_local(FuncState *fs, PxValue name) {
    int i;
    for (i = fs->nactive - 1; i >= 0; i--)
        if (fs->locals[i].name == name) return i;
    return -1;
}

/* THROW_REF with a message; its %s is name (0: nothing) */
static void emit_throw_ref(Compiler *C, const char *fmt, PxValue name) {
    char    buf[160], nm[64] = "";
    PxValue m;
    if (name) px_str_to_utf8(C->vm, name, nm, sizeof nm);
    snprintf(buf, sizeof buf, fmt, nm);
    m = px_str_from_cstr(C->vm, buf);
    check_alloc(C, m != PX_EXCEPTION);
    PX_ROOT(C->vm, m);
    emit_op_u16(C, OP_THROW_REF, add_const(C, m));
    px_pop_roots(C->vm, 1);
}

static int declare_local(Compiler *C, PxValue name, int kind) {
    FuncState *fs = C->fs;
    if (fs->nactive >= MAX_LOCALS) fail(C, "RangeError: too many local variables in one function");
    fs->locals[fs->nactive].name     = name;
    fs->locals[fs->nactive].kind     = (uint8_t)kind;
    fs->locals[fs->nactive].init     = 0;
    fs->locals[fs->nactive].captured = 0;
    fs->nactive++;
    if (fs->nactive > fs->nslots) fs->nslots = fs->nactive;
    return fs->nactive - 1;
}

static int declare_temp(Compiler *C) { return declare_local(C, 0, K_TEMP); }

static int add_upval(Compiler *C, FuncState *fs, PxValue name, int is_local, int index, int kind) {
    int i;
    for (i = 0; i < fs->nupvals; i++)
        if (fs->upvals[i].is_local == is_local && fs->upvals[i].index == index) return i;
    if (fs->nupvals >= MAX_UPVALS) fail(C, "RangeError: too many captured variables in one function");
    fs->upvals[fs->nupvals].name     = name;
    fs->upvals[fs->nupvals].is_local = (uint8_t)is_local;
    fs->upvals[fs->nupvals].index    = (uint16_t)index;
    fs->upvals[fs->nupvals].kind     = (uint8_t)kind;
    return fs->nupvals++;
}

static int find_upval(Compiler *C, FuncState *fs, PxValue name, int *kind) {
    int i, l, u;
    if (!fs->parent) return -1;
    for (i = 0; i < fs->nupvals; i++)
        if (fs->upvals[i].name == name) {
            *kind = fs->upvals[i].kind;
            return i;
        }
    l = find_local(fs->parent, name);
    if (l >= 0) {
        *kind                           = fs->parent->locals[l].kind;
        fs->parent->locals[l].captured = 1;
        return add_upval(C, fs, name, 1, l, *kind);
    }
    u = find_upval(C, fs->parent, name, kind);
    if (u >= 0) return add_upval(C, fs, name, 0, u, *kind);
    return -1;
}

static void resolve(Compiler *C, PxValue name, Exp *e) {
    int kind = K_VAR, i;
    e->name  = name;
    i        = find_local(C->fs, name);
    if (i >= 0) {
        e->k    = E_LOCAL;
        e->idx  = i;
        e->kind = C->fs->locals[i].kind;
        return;
    }
    i = find_upval(C, C->fs, name, &kind);
    if (i >= 0) {
        e->k    = E_UPVAL;
        e->idx  = i;
        e->kind = (uint8_t)kind;
        return;
    }
    e->k   = E_GLOBAL;
    e->idx = add_const(C, name);
}

/* An identifier reference in an expression. */
static void ident_ref(Compiler *C, PxValue name, Exp *e) {
    if (name == C->atom_arguments && (C->fs->ctx & CTX_NO_ARGS))
        fail(C, "SyntaxError: 'arguments' is not allowed in class field initializers or static blocks");
    resolve(C, name, e);
}

/* ------------------------------------------------------------ expressions */

/* A let/const/class local needs the TDZ check unless it is known to be
 * initialised at this point of the code (see init_name). */
static int needs_tdz(Compiler *C, const Exp *e) { return IS_LEXICAL(e->kind) && !C->fs->locals[e->idx].init; }

static void discharge(Compiler *C, Exp *e) {
    switch (e->k) {
    case E_LOCAL:
        if ((e->idx >= C->fs->tdz_lo && e->idx < C->fs->tdz_hi) || (e->idx >= C->fs->tdz_lo2 && e->idx < C->fs->tdz_hi2))
            emit_throw_ref(C, "'%s' is used before its initialisation", e->name);
        else
            emit_local(C, needs_tdz(C, e) ? OP_GET_LOCAL_CHECK : OP_GET_LOCAL, e->idx);
        break;
    case E_UPVAL: emit_op_u8(C, IS_LEXICAL(e->kind) ? OP_GET_UPVAL_CHECK : OP_GET_UPVAL, e->idx); break;
    case E_GLOBAL: emit_op_u16(C, OP_GET_GLOBAL, e->idx); break;
    case E_MEMBER: emit_prop(C, OP_GET_PROP, e->idx); break;
    case E_INDEX: emit_op(C, e->priv ? OP_GET_PRIVATE : OP_GET_ELEM); break;
    case E_SUPER_MEMBER: emit_op_u16(C, OP_GET_SUPER_RECV, e->idx); break;
    case E_SUPER_INDEX: emit_op(C, OP_GET_SUPER_ELEM_RECV); break;
    case E_VOID: emit_op(C, OP_UNDEF); break;
    case E_VALUE: break;
    }
    e->k = E_VALUE;
}

/* Stores the value on top of the stack into e, leaving the value. */
static void store(Compiler *C, Exp *e) {
    switch (e->k) {
    case E_LOCAL:
    case E_UPVAL:
        if (e->kind == K_CONST) {
            emit_op_u16(C, OP_THROW_CONST, add_const(C, e->name));
            return;
        }
        if (e->k == E_LOCAL) emit_local(C, needs_tdz(C, e) ? OP_SET_LOCAL_CHECK : OP_SET_LOCAL, e->idx);
        else emit_op_u8(C, IS_LEXICAL(e->kind) ? OP_SET_UPVAL_CHECK : OP_SET_UPVAL, e->idx);
        break;
    case E_GLOBAL: emit_op_u16(C, OP_SET_GLOBAL, e->idx); break;
    case E_MEMBER: emit_prop(C, OP_SET_PROP, e->idx); break;
    case E_INDEX: emit_op(C, e->priv ? OP_SET_PRIVATE : OP_SET_ELEM); break;
    case E_SUPER_MEMBER: emit_op_u16(C, OP_SET_SUPER, e->idx); break;
    case E_SUPER_INDEX: emit_op(C, OP_SET_SUPER_ELEM); break;
    default: fail(C, "SyntaxError: invalid assignment target");
    }
    e->k = E_VALUE;
}

static int is_lvalue(const Exp *e) { return e->k >= E_LOCAL; }

/* An assignment (=, op=, ++, --, a destructuring target) to e: strict
 * code may not assign to eval or arguments. */
static void check_target(Compiler *C, const Exp *e, const char *what) {
    if (!is_lvalue(e)) fail(C, "SyntaxError: invalid %s", what);
    if ((e->k == E_LOCAL || e->k == E_UPVAL || e->k == E_GLOBAL) &&
        (e->name == C->atom_eval || e->name == C->atom_arguments))
        fail(C, "SyntaxError: cannot assign to eval or arguments in strict mode");
}

/* Loads e's current value without consuming what it needs for a store:
 * MEMBER keeps its object, INDEX its object and key. */
static void load_keep(Compiler *C, Exp *e) {
    Exp t = *e;
    if (MEMBER_LIKE(e->k)) emit_op(C, OP_DUP);
    else if (e->k == E_INDEX) {
        if (!e->priv) emit_op(C, OP_ELEM_KEY); /* the key converted once, for the load and the store */
        emit_op(C, OP_DUP2);
    } else if (e->k == E_SUPER_INDEX) {
        emit_op(C, OP_TO_PROPKEY);
        emit_op(C, OP_DUP2);
    }
    discharge(C, &t);
}

static void assign_expr(Compiler *C, Exp *e);
static void close_from(Compiler *C, int from);
static void expr(Compiler *C);
static void unary(Compiler *C, Exp *e);
static void statement(Compiler *C);

/* The body of if/else/while/do/for/label: a lexical declaration is not
 * allowed there (it would declare into the enclosing scope a binding that
 * may never be initialised). */
static int async_ahead(Compiler *C);

static void substatement(Compiler *C) {
    TokType t = C->lx.tok.type;
    if (t == T_LET || t == T_CONST || t == T_CLASS)
        fail(C, "SyntaxError: a lexical declaration cannot be the body of a statement (wrap it in { })");
    if (t == T_FUNCTION || (t == T_ASYNC && async_ahead(C) == 2))
        fail(C, "SyntaxError: a function declaration cannot be the body of a statement in strict mode");
    statement(C);
}
static void compile_function(Compiler *C, PxValue name, int flags);
static void class_def(Compiler *C, PxValue name, int inner_binding);
static void bind_target(Compiler *C, int mode, int kind);
static void block_statement(Compiler *C);
static void import_declaration(Compiler *C);
static void export_declaration(Compiler *C);

static void enter(Compiler *C) {
    if (++C->depth > MAX_DEPTH) fail(C, "RangeError: nesting too deep");
}
static void leave(Compiler *C) { C->depth--; }

static void expr_value(Compiler *C) {
    Exp e;
    assign_expr(C, &e);
    discharge(C, &e);
}

static int binop_prec(TokType t, int no_in) {
    switch (t) {
    case T_NULLISH: return 1;
    case T_OROR: return 2;
    case T_ANDAND: return 3;
    case T_PIPE: return 4;
    case T_CARET: return 5;
    case T_AMP: return 6;
    case T_EQ: case T_NE: case T_SEQ: case T_SNE: return 7;
    case T_LT: case T_GT: case T_LE: case T_GE: case T_INSTANCEOF: return 8;
    case T_IN: return no_in ? -1 : 8;
    case T_SHL: case T_SAR: case T_SHR: return 9;
    case T_PLUS: case T_MINUS: return 10;
    case T_STAR: case T_SLASH: case T_PERCENT: return 11;
    case T_STARSTAR: return 12;
    default: return -1;
    }
}

static PxOp binop_code(TokType t) {
    switch (t) {
    case T_PIPE: return OP_BOR;
    case T_CARET: return OP_BXOR;
    case T_AMP: return OP_BAND;
    case T_EQ: return OP_EQ;
    case T_NE: return OP_NE;
    case T_SEQ: return OP_SEQ;
    case T_SNE: return OP_SNE;
    case T_LT: return OP_LT;
    case T_GT: return OP_GT;
    case T_LE: return OP_LE;
    case T_GE: return OP_GE;
    case T_INSTANCEOF: return OP_INSTANCEOF;
    case T_IN: return OP_IN;
    case T_SHL: return OP_SHL;
    case T_SAR: return OP_SAR;
    case T_SHR: return OP_SHR;
    case T_PLUS: return OP_ADD;
    case T_MINUS: return OP_SUB;
    case T_STAR: return OP_MUL;
    case T_SLASH: return OP_DIV;
    case T_PERCENT: return OP_MOD;
    case T_STARSTAR: return OP_POW;
    default: return OP_NOP;
    }
}

/* Returns the short-circuit operators combined at this level (1: && or
 * ||, 2: ??): the two kinds may not be mixed without parentheses. */
static int binary(Compiler *C, Exp *e, int min_prec) {
    int ops = 0, priv_in = 0;
    enter(C);
    unary(C, e);
    if (C->private_in) {
        /* #x in obj: only as the left operand of a relational `in` */
        C->private_in = 0;
        priv_in       = 1;
        if (TOK != T_IN || C->no_in || min_prec > 8) fail(C, "SyntaxError: unexpected private name");
    }
    if (TOK == T_STARSTAR && C->unary_op) fail(C, "SyntaxError: a unary expression cannot be the base of **");
    for (;;) {
        TokType t    = TOK;
        int     prec = binop_prec(t, C->no_in);
        if (prec < 0 || prec < min_prec) break;
        next(C);
        discharge(C, e);
        if (t == T_ANDAND || t == T_OROR || t == T_NULLISH) {
            Exp r;
            int j = emit_jump(C, t == T_ANDAND ? OP_JUMP_IF_FALSE_KEEP
                                 : t == T_OROR ? OP_JUMP_IF_TRUE_KEEP
                                               : OP_JUMP_IF_NOT_NULLISH_KEEP);
            adjust(C, -1); /* the fall-through path pops it */
            ops |= binary(C, &r, prec + 1) | (t == T_NULLISH ? 2 : 1);
            if (ops == 3) fail(C, "SyntaxError: ?? cannot be mixed with && or || without parentheses");
            discharge(C, &r);
            patch_here(C, j);
        } else {
            Exp r;
            binary(C, &r, t == T_STARSTAR ? prec : prec + 1);
            discharge(C, &r);
            emit_op(C, priv_in ? OP_HAS_PRIVATE : binop_code(t));
            priv_in = 0;
        }
        e->k = E_VALUE;
    }
    leave(C);
    return ops;
}

static void cond_expr(Compiler *C, Exp *e) {
    binary(C, e, 1);
    if (TOK == T_QUESTION) {
        int jelse, jend, saved_no_in = C->no_in;
        next(C);
        discharge(C, e);
        jelse    = emit_jump(C, OP_JUMP_IF_FALSE);
        C->no_in = 0;
        expr_value(C);
        C->no_in = saved_no_in;
        jend     = emit_jump(C, OP_JUMP);
        adjust(C, -1);
        expect(C, T_COLON);
        patch_here(C, jelse);
        expr_value(C);
        patch_here(C, jend);
        e->k = E_VALUE;
    }
}

/* ( ... ) => or name => ahead? (the current token is '(' or a name) */
static int arrow_ahead(Compiler *C) {
    LexState st = lex_save(&C->lx);
    int      r;
    if (TOK == T_LPAREN) skip_balanced(C);
    else next(C);
    r = TOK == T_ARROW && !C->lx.tok.nl_before;
    lex_restore(&C->lx, st);
    return r;
}

/* async x => / async (...) => / async function ahead? Returns 1 for an
 * async arrow, 2 for an async function, 0 otherwise. */
static int async_ahead(Compiler *C) {
    LexState st = lex_save(&C->lx);
    int      r = 0;
    next(C);
    if (!C->lx.tok.nl_before) {
        if (TOK == T_FUNCTION) r = 2;
        else if (TOK == T_LPAREN || is_name_tok_raw(TOK)) r = arrow_ahead(C) ? 1 : 0;
    }
    lex_restore(&C->lx, st);
    return r;
}

static void template_literal(Compiler *C) {
    emit_op_u16(C, OP_CONST, add_const(C, tok_string_as(C, 0)));
    while (!C->lx.tok.template_tail) {
        next(C);
        expr(C);
        emit_op(C, OP_TO_STRING);
        emit_op(C, OP_ADD);
        if (TOK != T_RBRACE) fail(C, "SyntaxError: expected '}' in template literal");
        lex_template_continue(&C->lx);
        if (TOK == T_ERROR) unexpected(C);
        if (C->lx.tok.str_len) {
            emit_op_u16(C, OP_CONST, add_const(C, tok_string_as(C, 0)));
            emit_op(C, OP_ADD);
        }
    }
    next(C);
}

/* tag`a${x}b`: the strings array, then the substitutions, as arguments.
 * Returns the argument count; the caller has set up [this fn]. */
/* The raw text of the current template chunk (for .raw): the source
 * between the opening ` or } and the closing ` or ${, line breaks
 * normalised to LF, escapes left as written. */
static PxValue template_raw(Compiler *C, int first) {
    const Token *t     = &C->lx.tok;
    uint32_t     start = t->start + (first ? 1u : 0u), end = t->start + t->len - (t->template_tail ? 1u : 2u), i, n = 0;
    char        *buf;
    PxValue      s;
    if (end < start) end = start;
    buf = (char *)c_alloc(C, end - start + 1);
    for (i = start; i < end; i++) {
        char c = C->lx.src[i];
        if (c == '\r') {
            if (i + 1 < end && C->lx.src[i + 1] == '\n') i++;
            c = '\n';
        }
        buf[n++] = c;
    }
    s = px_str_from_utf8(C->vm, buf, n);
    c_free(C, buf);
    check_alloc(C, s != PX_EXCEPTION);
    return s;
}

static int tagged_template(Compiler *C) {
    PxVec   *parts;
    PxValue  pv;
    int      at, argc = 1, n = 0, i;
    PxValue  tmp[256], raw[256];

    emit_op_u16(C, OP_TEMPLATE_OBJ, 0);
    at = (int)C->fs->len - 2;
    for (;;) {
        if (n >= 256) fail(C, "SyntaxError: template literal too long");
        raw[n] = template_raw(C, n == 0);
        PX_ROOT(C->vm, raw[n]);
        tmp[n] = tok_string_as(C, 0);
        PX_ROOT(C->vm, tmp[n]);
        n++;
        if (C->lx.tok.template_tail) break;
        next(C);
        expr_value(C);
        argc++;
        if (TOK != T_RBRACE) fail(C, "SyntaxError: expected '}' in template literal");
        lex_template_continue(&C->lx);
        if (TOK == T_ERROR) unexpected(C);
    }
    next(C);
    /* cooked strings, then raw ones */
    parts = px_vec_new(C->vm, (uint32_t)(2 * n));
    check_alloc(C, parts != NULL);
    for (i = 0; i < n; i++) {
        parts->items[i]     = tmp[i];
        parts->items[n + i] = raw[i];
    }
    px_pop_roots(C->vm, 2 * n);
    pv = px_from_ptr(parts);
    PX_ROOT(C->vm, pv);
    {
        int idx               = add_const(C, pv);
        C->fs->code[at]       = (uint8_t)(idx & 0xFF);
        C->fs->code[at + 1]   = (uint8_t)(idx >> 8);
    }
    px_pop_roots(C->vm, 1);
    return argc;
}

/* Call arguments after '('. Returns argc, or -1 when there was a spread:
 * then the arguments are one array on the stack. */
static int arguments(Compiler *C) {
    int argc = 0, packed = 0;
    next(C); /* ( */
    while (TOK != T_RPAREN) {
        if (TOK == T_ELLIPSIS) {
            next(C);
            if (!packed) {
                emit_op_u8(C, OP_PACK_ARRAY, argc);
                adjust(C, 1 - argc);
                packed = 1;
            }
            expr_value(C);
            emit_op(C, OP_APPEND_SPREAD);
        } else {
            expr_value(C);
            if (packed) emit_op(C, OP_APPEND);
            else if (++argc > 255) fail(C, "SyntaxError: too many arguments");
        }
        if (TOK != T_RPAREN) expect(C, T_COMMA);
    }
    next(C);
    return packed ? -1 : argc;
}

static void emit_call(Compiler *C, int argc) {
    if (argc < 0) {
        emit_op(C, OP_CALL_ARRAY);
    } else {
        emit_op_u8(C, OP_CALL, argc);
        adjust(C, -(argc + 1));
    }
}

static void array_literal(Compiler *C) {
    next(C);
    emit_op(C, OP_NEW_ARRAY);
    while (TOK != T_RBRACKET) {
        if (TOK == T_COMMA) {
            next(C);
            emit_op(C, OP_APPEND_HOLE);
            continue;
        }
        if (TOK == T_ELLIPSIS) {
            next(C);
            expr_value(C);
            emit_op(C, OP_APPEND_SPREAD);
        } else {
            expr_value(C);
            emit_op(C, OP_APPEND);
        }
        if (TOK != T_RBRACKET) expect(C, T_COMMA);
    }
    next(C);
}

/* Parses a property key (name, string, number, [computed]). For static
 * keys returns the key; for computed keys emits the key's value and
 * returns 0. plain_name: set when the key was a bare identifier. */
static PxValue property_key(Compiler *C, int *plain_name) {
    PxValue key = 0;
    if (plain_name) *plain_name = 0;
    if (TOK == T_PRIVATE && plain_name) unexpected(C); /* only class members have private names */
    if (TOK == T_PRIVATE) {
        /* #name: the key is the class's private symbol, a local */
        Exp e;
        resolve(C, tok_atom(C), &e);
        if (e.k == E_GLOBAL) fail(C, "SyntaxError: undeclared private name");
        discharge(C, &e);
        next(C);
        return 0;
    }
    if (is_prop_name_tok(TOK)) {
        if (plain_name) *plain_name = is_name(C);
        key = tok_atom(C);
        next(C);
    } else if (TOK == T_STRING) {
        key = tok_string_as(C, 1);
        next(C);
    } else if (TOK == T_NUMBER) {
        key = number_key(C, C->lx.tok.num);
        next(C);
    } else if (TOK == T_LBRACKET) {
        int saved = C->no_in;
        next(C);
        C->no_in = 0;
        expr_value(C);
        C->no_in = saved;
        expect(C, T_RBRACKET);
    } else {
        unexpected(C);
    }
    return key;
}

/* A method's name from its (non-computed) key: "x", "1", "#x", or "get x"
 * and "set x" for accessors. */
static PxValue method_name(Compiler *C, int kind, PxValue key) {
    PxValue k = px_key_to_value(C->vm, key), p, s;
    check_alloc(C, k != PX_EXCEPTION);
    if ((kind & 3) == PX_MK_METHOD) return k;
    PX_ROOT(C->vm, k);
    p = px_str_from_cstr(C->vm, (kind & 3) == PX_MK_GET ? "get " : "set ");
    check_alloc(C, p != PX_EXCEPTION);
    PX_ROOT(C->vm, p);
    s = px_str_concat(C->vm, p, k);
    check_alloc(C, s != PX_EXCEPTION);
    s = px_intern_literal(C->vm, s);
    check_alloc(C, s != PX_EXCEPTION);
    px_pop_roots(C->vm, 2);
    return s;
}

/* Is the current word (async, get, set, static) a modifier, that is, is a
 * member name after it? *nl: a line break comes first. */
static int modifier_ahead(Compiler *C, int *nl) {
    LexState st = lex_save(&C->lx);
    TokType  w = TOK, t;
    next(C);
    t   = TOK;
    *nl = C->lx.tok.nl_before;
    lex_restore(&C->lx, st);
    /* a name follows; `*` after async and static; `{` (a block) after static */
    return is_prop_name_tok(t) || t == T_STRING || t == T_NUMBER || t == T_LBRACKET || t == T_PRIVATE ||
           (t == T_STAR && (w == T_ASYNC || w == T_STATIC)) || (t == T_LBRACE && w == T_STATIC);
}

/* Method-ish member header: [async] [*] [get|set] key. Emits a computed
 * key if there is one. Sets *flags (PX_PROTO_*) and *kind (PX_MK_*). */
static PxValue member_header(Compiler *C, int *flags, int *kind, int *computed) {
    PxValue key;
    int     nl;
    *flags = 0;
    *kind  = PX_MK_METHOD;
    if (TOK == T_ASYNC && modifier_ahead(C, &nl) && !nl) {
        next(C);
        *flags |= PX_PROTO_ASYNC;
    }
    if (TOK == T_STAR) {
        next(C);
        *flags |= PX_PROTO_GENERATOR;
    }
    if (!*flags && (TOK == T_GET || TOK == T_SET) && modifier_ahead(C, &nl)) {
        *kind = TOK == T_GET ? PX_MK_GET : PX_MK_SET;
        *flags |= TOK == T_GET ? FN_GETTER : FN_SETTER;
        next(C);
    }
    key       = property_key(C, NULL);
    *computed = key == 0;
    return key;
}

static void object_literal(Compiler *C) {
    int seen_proto = 0;
    next(C);
    emit_op(C, OP_NEW_OBJECT);
    while (TOK != T_RBRACE) {
        PxValue key;
        int     plain = 0, flags = 0, kind = PX_MK_METHOD, computed = 0;
        if (TOK == T_ELLIPSIS) {
            next(C);
            expr_value(C);
            emit_op(C, OP_COPY_PROPS);
            if (TOK != T_RBRACE) expect(C, T_COMMA);
            continue;
        }
        if (TOK == T_STAR) {
            key = member_header(C, &flags, &kind, &computed);
        } else {
            /* A plain `name` (shorthand, name: value, name() {}) or a
             * method header with modifiers. */
            LexState st = lex_save(&C->lx);
            key         = property_key(C, &plain);
            computed    = key == 0;
            if (!computed && (TOK == T_COLON || TOK == T_COMMA || TOK == T_RBRACE || TOK == T_LPAREN ||
                              TOK == T_ASSIGN)) {
                /* simple key: nothing more to read */
            } else if (!computed) {
                lex_restore(&C->lx, st);
                key = member_header(C, &flags, &kind, &computed);
                plain = 0;
            }
        }
        if (!computed) PX_ROOT(C->vm, key);
        if (TOK == T_LPAREN) {
            PxValue fname = computed ? PX_UNDEFINED : method_name(C, kind, key);
            PX_ROOT(C->vm, fname);
            compile_function(C, fname, PX_PROTO_METHOD | flags);
            px_pop_roots(C->vm, 1);
            if (computed) emit_op_u8(C, OP_DEFINE_METHOD_ELEM, kind | PX_MK_ENUM);
            else {
                emit_op_u16(C, OP_DEFINE_METHOD, add_const(C, key));
                emit_byte(C, (uint8_t)(kind | PX_MK_ENUM));
            }
        } else if (TOK == T_COLON && !computed && key == C->vm->atom[PX_ATOM___proto__]) {
            /* __proto__: v sets the prototype (Annex B), once per literal */
            if (kind != PX_MK_METHOD || flags) unexpected(C);
            if (seen_proto) fail(C, "SyntaxError: duplicate __proto__ in an object literal");
            seen_proto = 1;
            next(C);
            expr_value(C);
            emit_op(C, OP_SET_PROTO);
        } else {
            if (kind != PX_MK_METHOD || flags) unexpected(C);
            if (TOK == T_COLON) {
                next(C);
                C->name_hint = computed ? PX_UNDEFINED : key;
                expr_value(C);
            } else if (plain && (TOK == T_COMMA || TOK == T_RBRACE)) {
                Exp e;
                ident_ref(C, key, &e);
                discharge(C, &e);
            } else {
                unexpected(C);
            }
            if (computed) emit_op(C, OP_DEFINE_ELEM);
            else emit_op_u16(C, OP_DEFINE_FIELD, add_const(C, key));
        }
        if (!computed) px_pop_roots(C->vm, 1);
        if (TOK != T_RBRACE) expect(C, T_COMMA);
    }
    next(C);
}

static int in_derived_ctor(Compiler *C) { return (C->fs->flags & PX_PROTO_DERIVED) != 0; }

static void emit_this(Compiler *C) { emit_op(C, in_derived_ctor(C) ? OP_THIS_CHECK : OP_THIS); }

/* super(...), super.x, super[x], super.m(...) */
static void super_expr(Compiler *C, Exp *e) {
    next(C); /* super */
    e->k = E_VALUE;
    if (TOK == T_LPAREN) {
        int argc;
        if (!in_derived_ctor(C)) fail(C, "SyntaxError: super() is only valid in a derived class constructor");
        mark_line(C);
        argc = arguments(C);
        if (argc < 0) emit_op(C, OP_SUPER_CALL_ARRAY);
        else {
            emit_op_u8(C, OP_SUPER_CALL, argc);
            adjust(C, 1 - argc);
        }
        emit_op(C, OP_INIT_FIELDS);
        return;
    }
    if (!(C->fs->ctx & CTX_SUPER_PROP)) fail(C, "SyntaxError: 'super' keyword unexpected here");
    if (TOK == T_DOT) {
        PxValue key;
        next(C);
        if (!is_prop_name_tok(TOK)) unexpected(C);
        key = tok_atom(C);
        next(C);
        if (TOK == T_LPAREN) {
            int argc;
            emit_this(C);
            emit_op_u16(C, OP_GET_SUPER, add_const(C, key));
            argc = arguments(C);
            emit_call(C, argc);
        } else { /* a reference: read, assigned or deleted by the caller */
            emit_this(C);
            e->k    = E_SUPER_MEMBER;
            e->idx  = add_const(C, key);
            e->priv = 0;
        }
        return;
    }
    if (TOK == T_LBRACKET) {
        next(C);
        {
            int is_call;
            LexState st = lex_save(&C->lx);
            skip_expression(C);
            expect(C, T_RBRACKET);
            is_call = TOK == T_LPAREN;
            lex_restore(&C->lx, st);
            emit_this(C);
            expr(C);
            expect(C, T_RBRACKET);
            if (is_call) {
                emit_op(C, OP_GET_SUPER_ELEM);
                emit_call(C, arguments(C));
            } else {
                e->k    = E_SUPER_INDEX;
                e->priv = 0;
            }
        }
        return;
    }
    fail(C, "SyntaxError: 'super' keyword unexpected here");
}

static void primary(Compiler *C, Exp *e) {
    e->k    = E_VALUE;
    e->priv = 0;
    switch (TOK) {
    case T_NUMBER:
        emit_number(C, C->lx.tok.num);
        next(C);
        break;
    case T_STRING:
        emit_op_u16(C, OP_CONST, add_const(C, tok_string_as(C, 0)));
        next(C);
        break;
    case T_TEMPLATE: template_literal(C); break;
    case T_THIS:
        emit_this(C);
        next(C);
        break;
    case T_NULL:
        emit_op(C, OP_NULL);
        next(C);
        break;
    case T_TRUE:
        emit_op(C, OP_TRUE);
        next(C);
        break;
    case T_FALSE:
        emit_op(C, OP_FALSE);
        next(C);
        break;
    case T_LPAREN: {
        /* (x) stays a reference: (x) = 1 and (a.b)++ are valid. An
         * anonymous function directly inside takes the name being
         * assigned: x = (function () {}) names it x. */
        int     saved = C->no_in;
        PxValue hint  = C->name_hint;
        C->name_hint  = PX_UNDEFINED;
        next(C);
        C->no_in     = 0;
        C->name_hint = hint;
        assign_expr(C, e);
        C->name_hint = PX_UNDEFINED;
        if (TOK == T_COMMA) {
            discharge(C, e);
            while (TOK == T_COMMA) {
                next(C);
                emit_op(C, OP_POP);
                expr_value(C);
            }
        }
        C->no_in = saved;
        expect(C, T_RPAREN);
        return;
    }
    case T_LBRACKET: array_literal(C); break;
    case T_LBRACE: object_literal(C); break;
    case T_FUNCTION: {
        PxValue name  = PX_UNDEFINED;
        int     flags = 0;
        next(C);
        if (TOK == T_STAR) {
            next(C);
            flags |= PX_PROTO_GENERATOR;
        }
        if (TOK == T_AWAIT && !C->is_module) {
            /* a function expression's name is in its own scope, where
             * await is a name (unless the code is a module) */
            name = tok_atom(C);
            next(C);
        } else if (TOK != T_LPAREN) {
            name = binding_name(C);
        } else if (px_is_ptr(C->name_hint)) {
            name = C->name_hint;
        }
        PX_ROOT(C->vm, name);
        compile_function(C, name, flags);
        px_pop_roots(C->vm, 1);
        break;
    }
    case T_CLASS: {
        PxValue name = PX_UNDEFINED;
        int     named = 0;
        next(C);
        if (TOK != T_LBRACE && TOK != T_EXTENDS) {
            name  = binding_name(C);
            named = 1;
        } else if (px_is_ptr(C->name_hint)) {
            name = C->name_hint;
        }
        PX_ROOT(C->vm, name);
        class_def(C, name, named);
        px_pop_roots(C->vm, 1);
        break;
    }
    case T_REGEXP:
    case T_SLASH:
    case T_SLASH_ASSIGN: {
        PxValue pat, fl;
        if (TOK != T_REGEXP) lex_regex(&C->lx);
        if (TOK == T_ERROR) unexpected(C);
        pat = tok_string_as(C, 0);
        PX_ROOT(C->vm, pat);
        fl = px_str_from_utf8(C->vm, C->lx.tok.ident, C->lx.tok.ident_len);
        check_alloc(C, fl != PX_EXCEPTION);
        fl = px_intern_literal(C->vm, fl);
        check_alloc(C, fl != PX_EXCEPTION);
        PX_ROOT(C->vm, fl);
        /* An invalid pattern or flags is an early error: compile it once
         * now (the regexp compiler's SyntaxError ends the compilation). */
        if (px_regexp_create(C->vm, pat, fl) == PX_EXCEPTION) {
            C->vm->nroots = C->nroots;
            longjmp(C->jb, 1);
        }
        emit_op_u16(C, OP_REGEXP, add_const(C, pat));
        emit_u16(C, add_const(C, fl));
        px_pop_roots(C->vm, 2);
        next(C);
        break;
    }
    case T_SUPER: super_expr(C, e); break;
    case T_PRIVATE: {
        /* #x in obj (binary() checks the `in`) */
        Exp pe;
        resolve(C, tok_atom(C), &pe);
        if (pe.k == E_GLOBAL) fail(C, "SyntaxError: undeclared private name");
        discharge(C, &pe);
        next(C);
        C->private_in = 1;
        break;
    }
    case T_NEW: fail(C, "SyntaxError: unexpected new");
    case T_IMPORT: fail(C, "SyntaxError: import() is resolved by the PSPX build, not by the engine");
    default:
        if (is_name(C)) {
            PxValue name = tok_atom(C);
            next(C);
            ident_ref(C, name, e);
            break;
        }
        unexpected(C);
    }
}

/* Short-circuit targets of a ?. chain, by how many values were on the
 * stack for the chain when it jumped (1: the object, 2: this+function). */
typedef struct Chain {
    int jumps[2][32]; /* [0]: one value, [1]: two (fixed: no heap memory to lose on a syntax error) */
    int n[2];
} Chain;

static void chain_add(Compiler *C, Chain *ch, int two, int at) {
    if (ch->n[two] >= 32) fail(C, "SyntaxError: optional chain too long");
    ch->jumps[two][ch->n[two]++] = at;
}

static void chain_end(Compiler *C, Exp *e, Chain *ch) {
    int i, jend, depth;
    if (ch->n[0] == 0 && ch->n[1] == 0) return;
    discharge(C, e);
    depth = C->fs->stack;
    jend  = emit_jump(C, OP_JUMP);
    for (i = 0; i < ch->n[1]; i++) patch_here(C, ch->jumps[1][i]);
    if (ch->n[1]) emit_op(C, OP_POP);
    for (i = 0; i < ch->n[0]; i++) patch_here(C, ch->jumps[0][i]);
    emit_op(C, OP_POP);
    emit_op(C, OP_UNDEF);
    patch_here(C, jend);
    C->fs->stack = depth;
    e->k = E_VALUE;
}

/* Sets up [this fn] for a call on e. */
static void call_setup(Compiler *C, Exp *e) {
    switch (e->k) {
    case E_MEMBER: emit_prop(C, OP_GET_PROP_KEEP, e->idx); break;
    case E_INDEX: emit_op(C, e->priv ? OP_GET_PRIVATE_KEEP : OP_GET_ELEM_KEEP); break;
    case E_SUPER_MEMBER: /* [this] -> [this fn] */
        emit_op(C, OP_DUP);
        emit_op_u16(C, OP_GET_SUPER_RECV, e->idx);
        break;
    case E_SUPER_INDEX: /* [this key] -> [this fn] */
        emit_op(C, OP_OVER);
        emit_op(C, OP_SWAP);
        emit_op(C, OP_GET_SUPER_ELEM_RECV);
        break;
    case E_VALUE:
        emit_op(C, OP_UNDEF);
        emit_op(C, OP_SWAP);
        break;
    default:
        emit_op(C, OP_UNDEF);
        discharge(C, e);
        break;
    }
    e->k = E_VALUE;
}

static void member_tail(Compiler *C, Exp *e, int allow_call) {
    Chain ch;
    memset(&ch, 0, sizeof ch);
    for (;;) {
        if (TOK == T_DOT) {
            PxValue name;
            next(C);
            if (TOK == T_PRIVATE) {
                Exp pe;
                discharge(C, e);
                resolve(C, tok_atom(C), &pe);
                if (pe.k == E_GLOBAL) fail(C, "SyntaxError: undeclared private name");
                discharge(C, &pe);
                next(C);
                e->k    = E_INDEX;
                e->priv = 1;
                continue;
            }
            if (!is_prop_name_tok(TOK)) unexpected(C);
            discharge(C, e);
            name    = tok_atom(C);
            e->k    = E_MEMBER;
            e->priv = 0;
            e->idx  = add_const(C, name);
            next(C);
        } else if (TOK == T_LBRACKET) {
            int saved = C->no_in;
            e->priv   = 0;
            next(C);
            discharge(C, e);
            C->no_in = 0;
            expr(C);
            C->no_in = saved;
            expect(C, T_RBRACKET);
            e->k = E_INDEX;
        } else if (TOK == T_LPAREN && allow_call) {
            mark_line(C);
            call_setup(C, e);
            emit_call(C, arguments(C));
        } else if (TOK == T_OPTCHAIN) {
            if (!allow_call) fail(C, "SyntaxError: an optional chain cannot be constructed with new");
            next(C);
            if (TOK == T_LPAREN) {
                call_setup(C, e);
                chain_add(C, &ch, 1, emit_jump(C, OP_JUMP_IF_NULLISH));
                emit_call(C, arguments(C));
            } else {
                discharge(C, e);
                chain_add(C, &ch, 0, emit_jump(C, OP_JUMP_IF_NULLISH));
                e->priv = 0;
                if (TOK == T_LBRACKET) {
                    next(C);
                    expr(C);
                    expect(C, T_RBRACKET);
                    e->k = E_INDEX;
                } else if (TOK == T_PRIVATE) {
                    Exp pe;
                    resolve(C, tok_atom(C), &pe);
                    if (pe.k == E_GLOBAL) fail(C, "SyntaxError: undeclared private name");
                    discharge(C, &pe);
                    next(C);
                    e->k    = E_INDEX;
                    e->priv = 1;
                } else if (TOK == T_TEMPLATE) {
                    fail(C, "SyntaxError: a template cannot follow an optional chain");
                } else {
                    if (!is_prop_name_tok(TOK)) unexpected(C);
                    e->k   = E_MEMBER;
                    e->idx = add_const(C, tok_atom(C));
                    next(C);
                }
            }
        } else if (TOK == T_TEMPLATE && allow_call) {
            int argc;
            if (ch.n[0] || ch.n[1]) fail(C, "SyntaxError: a template cannot follow an optional chain");
            call_setup(C, e);
            argc = tagged_template(C);
            emit_op_u8(C, OP_CALL, argc);
            adjust(C, -(argc + 1));
        } else {
            break;
        }
    }
    chain_end(C, e, &ch);
}

static void new_expr(Compiler *C, Exp *e) {
    int argc = 0;
    next(C); /* new */
    if (TOK == T_DOT) {
        next(C);
        if (!tok_is_word(C, "target") || C->lx.tok.escaped) unexpected(C);
        if (!(C->fs->ctx & CTX_NEW_TARGET)) fail(C, "SyntaxError: new.target is only valid in functions");
        next(C);
        emit_op(C, OP_NEW_TARGET);
        e->k = E_VALUE;
        return;
    }
    if (TOK == T_NEW) new_expr(C, e);
    else primary(C, e);
    member_tail(C, e, 0);
    discharge(C, e);
    mark_line(C);
    if (TOK == T_LPAREN) argc = arguments(C);
    if (argc < 0) emit_op(C, OP_NEW_ARRAY_ARGS);
    else {
        emit_op_u8(C, OP_NEW, argc);
        adjust(C, -argc);
    }
    e->k = E_VALUE;
}

static void lhs_expr(Compiler *C, Exp *e) {
    if (TOK == T_NEW) new_expr(C, e);
    else primary(C, e);
    member_tail(C, e, 1);
}

static void inc_dec_prefix(Compiler *C, Exp *e, int inc) {
    check_target(C, e, "increment/decrement target");
    load_keep(C, e);
    emit_op(C, inc ? OP_INC : OP_DEC);
    store(C, e);
}

static void postfix(Compiler *C, Exp *e) {
    lhs_expr(C, e);
    if ((TOK == T_INC || TOK == T_DEC) && !C->lx.tok.nl_before) {
        int inc = TOK == T_INC;
        check_target(C, e, "increment/decrement target");
        next(C);
        load_keep(C, e);
        emit_op(C, OP_TO_NUMERIC);
        emit_op(C, OP_DUP);
        if (MEMBER_LIKE(e->k)) emit_op(C, OP_ROT3);
        else if (INDEX_LIKE(e->k)) emit_op(C, OP_ROT4);
        emit_op(C, inc ? OP_INC : OP_DEC);
        store(C, e);
        emit_op(C, OP_POP);
        e->k = E_VALUE;
    }
}

/* The operand of a unary operator, as a value (`-#x in o` is invalid). */
static void unary_operand(Compiler *C, Exp *e) {
    if (C->private_in) unexpected(C);
    discharge(C, e);
}

static void unary(Compiler *C, Exp *e) {
    TokType t = TOK;
    enter(C);
    switch (t) {
    case T_BANG:
    case T_TILDE:
    case T_MINUS:
    case T_PLUS:
        next(C);
        if (t == T_MINUS && TOK == T_NUMBER) {
            /* Fold -literal: loops count down from -1 more than you'd think.
             * (Not -1.5.toFixed(): the member access binds first.) */
            double d = C->lx.tok.num;
            next(C);
            if (TOK != T_DOT && TOK != T_LBRACKET && TOK != T_LPAREN && TOK != T_TEMPLATE && TOK != T_OPTCHAIN) {
                emit_number(C, -d);
                e->k = E_VALUE;
                break;
            }
            emit_number(C, d);
            e->k = E_VALUE;
            member_tail(C, e, 1);
        } else {
            unary(C, e);
        }
        unary_operand(C, e);
        emit_op(C, t == T_BANG ? OP_NOT : t == T_TILDE ? OP_BNOT : t == T_MINUS ? OP_NEG : OP_PLUS);
        break;
    case T_TYPEOF:
        next(C);
        unary(C, e);
        if (C->private_in) unexpected(C);
        if (e->k == E_GLOBAL) {
            emit_op_u16(C, OP_GET_GLOBAL_TYPEOF, e->idx);
            e->k = E_VALUE;
        } else {
            discharge(C, e);
        }
        emit_op(C, OP_TYPEOF);
        break;
    case T_VOID:
        next(C);
        unary(C, e);
        unary_operand(C, e);
        emit_op(C, OP_POP);
        emit_op(C, OP_UNDEF);
        break;
    case T_DELETE:
        next(C);
        unary(C, e);
        if (C->private_in) unexpected(C);
        if (e->k == E_INDEX && e->priv) fail(C, "SyntaxError: private fields cannot be deleted");
        if (e->k == E_MEMBER) emit_op_u16(C, OP_DELETE_PROP, e->idx);
        else if (e->k == E_INDEX) emit_op(C, OP_DELETE_ELEM);
        else if (e->k == E_SUPER_MEMBER || e->k == E_SUPER_INDEX) {
            /* after `this` and the key expression, before ToPropertyKey */
            if (e->k == E_SUPER_INDEX) emit_op(C, OP_POP);
            emit_op(C, OP_POP);
            emit_throw_ref(C, "cannot delete a property of super%s", 0);
        }
        else if (e->k == E_VALUE) {
            emit_op(C, OP_POP);
            emit_op(C, OP_TRUE);
        } else fail(C, "SyntaxError: delete of an unqualified identifier");
        break;
    case T_INC:
    case T_DEC:
        next(C);
        unary(C, e);
        if (C->private_in) unexpected(C);
        inc_dec_prefix(C, e, t == T_INC);
        e->k       = E_VALUE;
        C->unary_op = 0; /* an update expression: ++x ** 2 is fine */
        leave(C);
        return;
    case T_AWAIT:
        if (!in_async(C)) {
            if (await_is_keyword(C)) fail(C, "SyntaxError: await is only valid in async functions");
            postfix(C, e);
            C->unary_op = 0;
            leave(C);
            return;
        }
        if (C->fs->ctx & CTX_PARAMS) fail(C, "SyntaxError: await is not allowed in parameters");
        next(C);
        unary(C, e);
        unary_operand(C, e);
        emit_op(C, OP_AWAIT);
        break;
    default:
        postfix(C, e);
        C->unary_op = 0;
        leave(C);
        return;
    }
    e->k        = E_VALUE;
    C->unary_op = 1;
    leave(C);
}

static PxOp compound_op(TokType t) {
    switch (t) {
    case T_PLUS_ASSIGN: return OP_ADD;
    case T_MINUS_ASSIGN: return OP_SUB;
    case T_STAR_ASSIGN: return OP_MUL;
    case T_SLASH_ASSIGN: return OP_DIV;
    case T_PERCENT_ASSIGN: return OP_MOD;
    case T_STARSTAR_ASSIGN: return OP_POW;
    case T_SHL_ASSIGN: return OP_SHL;
    case T_SAR_ASSIGN: return OP_SAR;
    case T_SHR_ASSIGN: return OP_SHR;
    case T_AMP_ASSIGN: return OP_BAND;
    case T_PIPE_ASSIGN: return OP_BOR;
    case T_CARET_ASSIGN: return OP_BXOR;
    default: return OP_NOP;
    }
}

/* a &&= b, a ||= b, a ??= b: the store happens only when needed. */
static void logical_assign(Compiler *C, Exp *e, TokType t) {
    int jkeep, jend;
    PxOp op = t == T_ANDAND_ASSIGN ? OP_JUMP_IF_FALSE_KEEP
            : t == T_OROR_ASSIGN   ? OP_JUMP_IF_TRUE_KEEP
                                   : OP_JUMP_IF_NOT_NULLISH_KEEP;
    load_keep(C, e);
    jkeep = emit_jump(C, op);
    adjust(C, -1);
    if (e->k == E_LOCAL || e->k == E_UPVAL || e->k == E_GLOBAL) C->name_hint = e->name;
    expr_value(C);
    store(C, e);
    jend = emit_jump(C, OP_JUMP);
    patch_here(C, jkeep);
    /* the kept value is on top of whatever the store needed */
    if (MEMBER_LIKE(e->k)) {
        emit_op(C, OP_SWAP);
        emit_op(C, OP_POP);
    } else if (INDEX_LIKE(e->k)) {
        emit_op(C, OP_ROT3);
        emit_op(C, OP_POP);
        emit_op(C, OP_POP);
    }
    patch_here(C, jend);
}

static void yield_expr(Compiler *C) {
    next(C); /* yield */
    if (TOK == T_STAR && !C->lx.tok.nl_before) { /* yield \n * x is yield, then a stray * */
        /* the generator's driver runs the whole delegation (px_iter.c,
         * px_asyncgen.c) and resumes here with its completion */
        next(C);
        expr_value(C);
        emit_op(C, OP_DELEGATE);
        return;
    }
    if (TOK == T_RPAREN || TOK == T_RBRACKET || TOK == T_RBRACE || TOK == T_COMMA || TOK == T_SEMI ||
        TOK == T_COLON || TOK == T_EOF || C->lx.tok.nl_before)
        emit_op(C, OP_UNDEF);
    else
        expr_value(C);
    /* an async generator's yield awaits its operand first */
    if (in_async_generator(C)) emit_op(C, OP_AWAIT);
    emit_op(C, OP_YIELD);
}

/* [ ... ] = / { ... } = ahead? */
static int pattern_assign_ahead(Compiler *C) {
    LexState st = lex_save(&C->lx);
    int      r;
    skip_balanced(C);
    r = TOK == T_ASSIGN;
    lex_restore(&C->lx, st);
    return r;
}

static void arrow_function(Compiler *C, int flags) {
    PxValue hint = px_is_ptr(C->name_hint) ? C->name_hint : PX_UNDEFINED;
    PX_ROOT(C->vm, hint);
    compile_function(C, hint, PX_PROTO_ARROW | flags);
    px_pop_roots(C->vm, 1);
}

static void assign_expr(Compiler *C, Exp *e) {
    TokType t;
    PxValue hint;
    enter(C);
    hint         = C->name_hint;
    C->name_hint = PX_UNDEFINED;
    e->k         = E_VALUE;
    if (TOK == T_YIELD && in_generator(C)) {
        if (C->fs->ctx & CTX_PARAMS) fail(C, "SyntaxError: yield is not allowed in generator parameters");
        yield_expr(C);
        leave(C);
        return;
    }
    if (TOK == T_ASYNC) {
        int a = async_ahead(C);
        if (a == 1) {
            next(C);
            C->name_hint = hint;
            arrow_function(C, PX_PROTO_ASYNC);
            C->name_hint = PX_UNDEFINED;
            leave(C);
            return;
        }
        if (a == 2) {
            PxValue name = px_is_ptr(hint) ? hint : PX_UNDEFINED;
            int     flags = PX_PROTO_ASYNC;
            next(C);
            next(C); /* function */
            if (TOK == T_STAR) {
                next(C);
                flags |= PX_PROTO_GENERATOR;
            }
            if (TOK != T_LPAREN) {
                /* an async function expression's own name is in its scope,
                 * where await is a keyword */
                if (TOK == T_AWAIT) unexpected(C);
                name = binding_name(C);
            }
            PX_ROOT(C->vm, name);
            compile_function(C, name, flags);
            px_pop_roots(C->vm, 1);
            member_tail(C, e, 1);
            leave(C);
            return;
        }
    }
    if ((is_name(C) || TOK == T_LPAREN) && arrow_ahead(C)) {
        C->name_hint = hint;
        arrow_function(C, 0);
        C->name_hint = PX_UNDEFINED;
        leave(C);
        return;
    }
    if ((TOK == T_LBRACKET || TOK == T_LBRACE) && pattern_assign_ahead(C)) {
        /* Destructuring assignment: the value first, then the pattern. */
        LexState pat = lex_save(&C->lx), after;
        skip_balanced(C);
        expect(C, T_ASSIGN);
        expr_value(C);
        after = lex_save(&C->lx);
        emit_op(C, OP_DUP);
        lex_restore(&C->lx, pat);
        bind_target(C, BIND_ASSIGN, K_VAR);
        lex_restore(&C->lx, after);
        leave(C);
        return;
    }
    if (TOK == T_FUNCTION || TOK == T_CLASS || TOK == T_LPAREN) C->name_hint = hint;
    cond_expr(C, e);
    C->name_hint = PX_UNDEFINED;
    t            = TOK;
    if (t == T_ASSIGN) {
        check_target(C, e, "assignment target");
        next(C);
        if (e->k == E_LOCAL || e->k == E_UPVAL || e->k == E_GLOBAL) C->name_hint = e->name;
        expr_value(C);
        store(C, e);
    } else if (compound_op(t) != OP_NOP) {
        check_target(C, e, "assignment target");
        next(C);
        load_keep(C, e);
        expr_value(C);
        emit_op(C, compound_op(t));
        store(C, e);
    } else if (t == T_ANDAND_ASSIGN || t == T_OROR_ASSIGN || t == T_NULLISH_ASSIGN) {
        check_target(C, e, "assignment target");
        next(C);
        logical_assign(C, e, t);
        e->k = E_VALUE;
    }
    leave(C);
}

static void expr(Compiler *C) {
    expr_value(C);
    while (TOK == T_COMMA) {
        next(C);
        emit_op(C, OP_POP);
        expr_value(C);
    }
}

/* ------------------------------------------------------------ patterns */

/* `var name` inside blocks: an error if a let/const/class or a block-level
 * function of an enclosing block has the name, since the var would be
 * hoisted through it. (Clashes at the function's own level are found by
 * the pre-scan.) */
static void check_var_conflict(Compiler *C, PxValue name) {
    FuncState *fs = C->fs;
    int        i;
    for (i = fs->nactive - 1; i >= fs->var_floor; i--)
        if (fs->locals[i].name == name && (IS_LEXICAL(fs->locals[i].kind) || fs->locals[i].kind == K_FUNC))
            fail(C, "SyntaxError: var redeclares a lexical declaration");
}

/* Stores the value on top of the stack into a declared name (consuming
 * it): initialisation, so no TDZ check and const is allowed. */
static void init_name(Compiler *C, PxValue name, int kind) {
    Exp        e;
    FuncState *fs = C->fs;
    resolve(C, name, &e);
    if (e.k == E_LOCAL) {
        emit_local(C, OP_PUT_LOCAL, e.idx);
        /* From here on in this function the binding is initialised: the
         * code after a declaration only runs after the declaration ran.
         * Except in a switch body, where a case label can jump past a
         * declaration into code after it. (Nested functions keep their
         * checks: a hoisted function can run before the declaration.) */
        if (!fs->switch_floor1 || e.idx < fs->switch_floor1 - 1) fs->locals[e.idx].init = 1;
    } else if (e.k == E_GLOBAL) {
        emit_op_u16(C, kind == K_VAR ? OP_SET_GLOBAL : OP_DEF_GLOBAL, e.idx);
        if (kind == K_VAR) emit_op(C, OP_POP);
    } else {
        e.kind = K_VAR;
        store(C, &e);
        emit_op(C, OP_POP);
    }
}

/* value -> (default applied) value, when the lexer is at '='. hint names
 * an anonymous function default (NamedEvaluation). */
static void apply_default(Compiler *C, PxValue hint) {
    int skip;
    next(C); /* = */
    emit_op(C, OP_DUP);
    emit_op(C, OP_UNDEF);
    emit_op(C, OP_SEQ);
    skip = emit_jump(C, OP_JUMP_IF_FALSE);
    emit_op(C, OP_POP);
    C->name_hint = hint;
    expr_value(C);
    patch_here(C, skip);
}

/* Where a destructuring element's value comes from: the pattern's
 * iterator or object is in a local (so that an assignment target's
 * reference can be evaluated before the value is read, as the spec
 * orders it). */
enum { SRC_ITER, SRC_ITER_REST, SRC_PROP, SRC_ELEM, SRC_REST };

typedef struct Source {
    int        how;    /* SRC_* */
    int        slot;   /* the iterator, or the object */
    int        kslot;  /* SRC_ELEM: the local with the computed key */
    int        kcidx;  /* SRC_PROP: the key's constant */
    const int *ex;     /* SRC_REST: the excluded keys: constants, or ~local for computed ones */
    int        nex;
} Source;

static void emit_source(Compiler *C, const Source *s) {
    int i;
    switch (s->how) {
    case SRC_ITER: emit_local(C, OP_ITER_STEP_AT, s->slot); break;
    case SRC_ITER_REST: emit_local(C, OP_ITER_REST_AT, s->slot); break;
    case SRC_PROP:
        emit_local(C, OP_GET_LOCAL, s->slot);
        emit_prop(C, OP_GET_PROP, s->kcidx);
        break;
    case SRC_ELEM:
        emit_local(C, OP_GET_LOCAL, s->slot);
        emit_local(C, OP_GET_LOCAL, s->kslot);
        emit_op(C, OP_GET_ELEM);
        break;
    default:
        emit_local(C, OP_GET_LOCAL, s->slot);
        emit_op(C, OP_NEW_ARRAY);
        for (i = 0; i < s->nex; i++) {
            if (s->ex[i] >= 0) emit_op_u16(C, OP_CONST, s->ex[i]);
            else emit_local(C, OP_GET_LOCAL, ~s->ex[i]);
            emit_op(C, OP_APPEND);
        }
        emit_op(C, OP_OBJ_REST);
        break;
    }
}

/* In an assignment, is the '[' or '{' here a nested pattern, or the start
 * of a target expression such as `{}[k]`? */
static int nested_pattern_ahead(Compiler *C) {
    LexState st = lex_save(&C->lx);
    int      r;
    skip_balanced(C);
    r = TOK == T_ASSIGN || TOK == T_COMMA || TOK == T_RBRACKET || TOK == T_RBRACE;
    lex_restore(&C->lx, st);
    return r;
}

/* One element: a target with an optional `= default`, its value read
 * from src. A nested pattern takes the value (with the default applied);
 * a declared name is bound; an assignment target's reference (a.b, a[k])
 * is evaluated first, then the value is read, defaulted and stored. */
static void bind_element(Compiler *C, int mode, int kind, const Source *src, int allow_default) {
    if ((TOK == T_LBRACE || TOK == T_LBRACKET) && (mode == BIND_DECL || nested_pattern_ahead(C))) {
        LexState tstart = lex_save(&C->lx), after;
        skip_balanced(C);
        emit_source(C, src);
        if (TOK == T_ASSIGN) {
            if (!allow_default) unexpected(C);
            apply_default(C, PX_UNDEFINED);
        }
        after = lex_save(&C->lx);
        lex_restore(&C->lx, tstart);
        bind_target(C, mode, kind);
        lex_restore(&C->lx, after);
        return;
    }
    if (mode == BIND_DECL) {
        PxValue name = binding_name(C);
        PX_ROOT(C->vm, name);
        if (kind == K_VAR) check_var_conflict(C, name);
        emit_source(C, src);
        if (TOK == T_ASSIGN) {
            if (!allow_default) unexpected(C);
            apply_default(C, name);
        }
        init_name(C, name, kind);
        px_pop_roots(C->vm, 1);
    } else {
        Exp e;
        int bare = is_name(C); /* a plain identifier names an anonymous function default */
        lhs_expr(C, &e);
        check_target(C, &e, "destructuring target");
        if (TOK != T_ASSIGN && TOK != T_COMMA && TOK != T_RBRACKET && TOK != T_RBRACE) unexpected(C);
        emit_source(C, src);
        if (TOK == T_ASSIGN) {
            if (!allow_default) unexpected(C);
            apply_default(C, bare && !MEMBER_LIKE(e.k) && !INDEX_LIKE(e.k) ? e.name : PX_UNDEFINED);
        }
        store(C, &e);
        emit_op(C, OP_POP);
    }
}

static void bind_object_pattern(Compiler *C, int mode, int kind) {
    int    ex[64], nex = 0, slot, ktemps = 0;
    Source src;
    next(C); /* { */
    emit_op(C, OP_REQUIRE_OBJ);
    slot = declare_temp(C);
    emit_local(C, OP_PUT_LOCAL, slot);
    src.slot = slot;
    while (TOK != T_RBRACE) {
        if (nex >= 64) fail(C, "SyntaxError: too many properties in a destructuring pattern");
        if (TOK == T_ELLIPSIS) {
            /* ...rest: a name, or (assigning) a simple target; last */
            next(C);
            if (TOK == T_LBRACE || TOK == T_LBRACKET) unexpected(C);
            src.how = SRC_REST;
            src.ex  = ex;
            src.nex = nex;
            bind_element(C, mode, kind, &src, 0);
            if (TOK != T_RBRACE) fail(C, "SyntaxError: a rest property must be last");
            break;
        }
        if (TOK == T_LBRACKET) {
            /* [key]: evaluated now, kept for the rest property */
            property_key(C, NULL);
            emit_op(C, OP_TO_PROPKEY);
            src.how   = SRC_ELEM;
            src.kslot = declare_temp(C);
            ktemps++;
            emit_local(C, OP_PUT_LOCAL, src.kslot);
            ex[nex++] = ~src.kslot;
            expect(C, T_COLON);
            bind_element(C, mode, kind, &src, 1);
        } else {
            LexState kpos = lex_save(&C->lx);
            int      plain;
            PxValue  key = property_key(C, &plain);
            src.how      = SRC_PROP;
            src.kcidx    = add_const(C, key);
            ex[nex++]    = src.kcidx;
            if (TOK == T_COLON) {
                next(C);
                bind_element(C, mode, kind, &src, 1);
            } else {
                /* shorthand {a} / {a = 1}: the key is the target */
                if (!plain || (TOK != T_ASSIGN && TOK != T_COMMA && TOK != T_RBRACE))
                    fail(C, "SyntaxError: invalid destructuring target");
                lex_restore(&C->lx, kpos);
                bind_element(C, mode, kind, &src, 1);
            }
        }
        if (TOK != T_RBRACE) expect(C, T_COMMA);
    }
    next(C);
    C->fs->nactive -= 1 + ktemps; /* the temporaries, declared last */
}

/* The iterator is closed when the pattern is done with it; when an
 * exception (or a generator's return()) leaves the pattern, a handler
 * closes it on the way out. */
static void bind_array_pattern(Compiler *C, int mode, int kind) {
    FuncState *fs = C->fs;
    int        slot, handler, jend, base;
    Source     src;
    next(C); /* [ */
    emit_op(C, OP_ITER_START);
    slot = declare_temp(C);
    emit_local(C, OP_PUT_LOCAL, slot);
    base     = fs->stack;
    handler  = emit_jump(C, OP_TRY);
    src.slot = slot;
    while (TOK != T_RBRACKET) {
        if (TOK == T_COMMA) {
            next(C);
            emit_local(C, OP_ITER_STEP_AT, slot);
            emit_op(C, OP_POP);
            continue;
        }
        if (TOK == T_ELLIPSIS) {
            next(C);
            src.how = SRC_ITER_REST;
            bind_element(C, mode, kind, &src, 0);
            if (TOK != T_RBRACKET) fail(C, "SyntaxError: a rest element must be last");
            break;
        }
        src.how = SRC_ITER;
        bind_element(C, mode, kind, &src, 1);
        if (TOK != T_RBRACKET) expect(C, T_COMMA);
    }
    next(C);
    emit_op(C, OP_END_TRY);
    emit_local(C, OP_GET_LOCAL, slot);
    emit_op(C, OP_ITER_CLOSE);
    jend = emit_jump(C, OP_JUMP);
    patch_here(C, handler);
    fs->stack = base + 1; /* the exception */
    emit_local(C, OP_GET_LOCAL, slot);
    emit_op(C, OP_ITER_CLOSE_ABRUPT);
    fs->stack = base;
    patch_here(C, jend);
    fs->nactive--; /* the iterator's temporary */
}

/* Consumes the value on the stack into the target at the lexer. */
static void bind_target(Compiler *C, int mode, int kind) {
    enter(C);
    if (TOK == T_LBRACE) bind_object_pattern(C, mode, kind);
    else if (TOK == T_LBRACKET) bind_array_pattern(C, mode, kind);
    else if (mode == BIND_DECL) {
        PxValue name = binding_name(C);
        if (kind == K_VAR) check_var_conflict(C, name);
        init_name(C, name, kind);
    } else {
        Exp e;
        lhs_expr(C, &e);
        check_target(C, &e, "destructuring target");
        switch (e.k) {
        case E_MEMBER: /* [v obj] -> [obj v] */
        case E_SUPER_MEMBER:
            emit_op(C, OP_SWAP);
            store(C, &e);
            emit_op(C, OP_POP);
            break;
        case E_INDEX: /* [v obj key] -> [obj key v] */
        case E_SUPER_INDEX:
            emit_op(C, OP_ROT3);
            emit_op(C, OP_ROT3);
            store(C, &e);
            emit_op(C, OP_POP);
            break;
        case E_LOCAL:
        case E_UPVAL:
        case E_GLOBAL:
            store(C, &e);
            emit_op(C, OP_POP);
            break;
        default: fail(C, "SyntaxError: invalid destructuring target");
        }
    }
    leave(C);
}

/* ------------------------------------------------------------ pre-scan */

/* The names a block (or a parameter list, a for head...) declares, in
 * order: (name, kind) pairs in a heap vector, rooted while in use. Big
 * lists (a bundle's top level) get a hash index for the duplicate check. */
typedef struct Decls {
    PxVec   *v;
    PxValue  root;  /* v, as a GC root */
    int32_t *index; /* pair numbers by name hash, -1: empty; NULL while small */
    int      n, slots;
    int      block; /* a block, not a function body: its functions are lexical */
} Decls;

#define DECL_NAME(d, i) ((d)->v->items[2 * (i)])
#define DECL_KIND(d, i) px_smi((d)->v->items[2 * (i) + 1])
#define DECLS_HASHED    16 /* from this many names on, look them up by hash */

/* Callers take vm->nroots before and restore it after decls_free. */
static Decls *decls_new(Compiler *C, int block) {
    Decls *d = (Decls *)c_alloc(C, sizeof(Decls));
    memset(d, 0, sizeof *d);
    d->block = block;
    d->v     = px_vec_new(C->vm, 2 * 8);
    check_alloc(C, d->v != NULL);
    d->root = px_from_ptr(d->v);
    PX_ROOT(C->vm, d->root);
    return d;
}

static void decls_free(Compiler *C, Decls *d) {
    if (d->index) c_free(C, d->index);
    c_free(C, d);
}

static uint32_t decl_hash(PxValue name) { return (uint32_t)(name >> 3) * 2654435761u; }

static void decls_reindex(Compiler *C, Decls *d) {
    int i;
    if (d->index) c_free(C, d->index);
    d->slots = 64;
    while (d->slots < 4 * d->n) d->slots *= 2;
    d->index = (int32_t *)c_alloc(C, (size_t)d->slots * sizeof(int32_t));
    memset(d->index, 0xFF, (size_t)d->slots * sizeof(int32_t));
    for (i = 0; i < d->n; i++) {
        uint32_t h = decl_hash(DECL_NAME(d, i)) & (uint32_t)(d->slots - 1);
        while (d->index[h] >= 0) h = (h + 1) & (uint32_t)(d->slots - 1);
        d->index[h] = i;
    }
}

static int decl_find(Decls *d, PxValue name) {
    int i;
    if (!d->index) {
        for (i = 0; i < d->n; i++)
            if (DECL_NAME(d, i) == name) return i;
        return -1;
    }
    {
        uint32_t h = decl_hash(name) & (uint32_t)(d->slots - 1);
        for (; d->index[h] >= 0; h = (h + 1) & (uint32_t)(d->slots - 1))
            if (DECL_NAME(d, d->index[h]) == name) return d->index[h];
        return -1;
    }
}

static void decl_add(Compiler *C, Decls *d, PxValue name, int kind) {
    int i = decl_find(d, name);
    if (i >= 0) {
        int old = DECL_KIND(d, i);
        /* in a block, function declarations are lexical too */
        if (IS_LEXICAL(kind) || IS_LEXICAL(old) || (d->block && kind == K_FUNC && old == K_FUNC) ||
            (d->block && (kind == K_FUNC || old == K_FUNC) && (kind == K_VAR || old == K_VAR)))
            fail(C, "SyntaxError: duplicate declaration");
        if (kind == K_FUNC) d->v->items[2 * i + 1] = px_from_smi(K_FUNC);
        return;
    }
    if (2 * (uint32_t)(d->n + 1) > d->v->cap) {
        PxVec *g;
        PX_ROOT(C->vm, name);
        g = px_vec_grow(C->vm, d->v, 4 * (uint32_t)(d->n + 1));
        px_pop_roots(C->vm, 1);
        check_alloc(C, g != NULL);
        d->v    = g;
        d->root = px_from_ptr(g);
    }
    d->v->items[2 * d->n]     = name;
    d->v->items[2 * d->n + 1] = px_from_smi(kind);
    d->n++;
    if (d->n == DECLS_HASHED || (d->index && 2 * d->n > d->slots)) decls_reindex(C, d);
    else if (d->index) {
        uint32_t h = decl_hash(name) & (uint32_t)(d->slots - 1);
        while (d->index[h] >= 0) h = (h + 1) & (uint32_t)(d->slots - 1);
        d->index[h] = d->n - 1;
    }
}

static void scan_target(Compiler *C, Decls *d, int kind);

/* The names a destructuring pattern binds, without compiling it. */
static void scan_pattern(Compiler *C, Decls *d, int kind) {
    if (TOK == T_LBRACE) {
        next(C);
        while (TOK != T_RBRACE && TOK != T_EOF) {
            if (TOK == T_ELLIPSIS) {
                next(C);
                scan_target(C, d, kind);
            } else {
                int     shorthand_ok = is_name_tok_raw(TOK) || TOK == T_IDENT;
                PxValue key          = 0;
                if (TOK == T_LBRACKET) skip_balanced(C);
                else {
                    if (shorthand_ok) key = tok_atom(C);
                    next(C);
                }
                if (TOK == T_COLON) {
                    next(C);
                    scan_target(C, d, kind);
                } else if (key) {
                    decl_add(C, d, key, kind);
                }
                if (TOK == T_ASSIGN) {
                    next(C);
                    skip_expression(C);
                }
            }
            if (TOK == T_COMMA) next(C);
        }
        next(C);
    } else if (TOK == T_LBRACKET) {
        next(C);
        while (TOK != T_RBRACKET && TOK != T_EOF) {
            if (TOK == T_COMMA) {
                next(C);
                continue;
            }
            if (TOK == T_ELLIPSIS) next(C);
            scan_target(C, d, kind);
            if (TOK == T_ASSIGN) {
                next(C);
                skip_expression(C);
            }
            if (TOK == T_COMMA) next(C);
        }
        next(C);
    }
}

static void scan_target(Compiler *C, Decls *d, int kind) {
    if (TOK == T_LBRACE || TOK == T_LBRACKET) scan_pattern(C, d, kind);
    else if (is_name_tok_raw(TOK)) {
        decl_add(C, d, tok_atom(C), kind);
        next(C);
    } else {
        skip_balanced(C);
    }
}

/* Is a '(' ... ')' '{' at this point a function body? Only when what came
 * before the '(' is not a statement keyword. */
static int is_block_keyword(TokType t) {
    return t == T_IF || t == T_WHILE || t == T_FOR || t == T_SWITCH || t == T_CATCH || t == T_WITH;
}

static int ends_expression(TokType t) {
    return t == T_IDENT || t == T_NUMBER || t == T_STRING || t == T_TEMPLATE || t == T_RPAREN ||
           t == T_RBRACKET || t == T_RBRACE || t == T_THIS || t == T_TRUE || t == T_FALSE || t == T_NULL ||
           t == T_INC || t == T_DEC || is_name_tok_raw(t);
}

/* Skips the rest of a declarator list: initialisers, and further
 * declarators, whose names it records. */
static void scan_declarators(Compiler *C, Decls *d, int kind) {
    TokType p = T_IDENT;
    for (;;) {
        TokType u = TOK;
        if (u == T_EOF || u == T_SEMI || u == T_RBRACE || u == T_RPAREN) break;
        if (C->lx.tok.nl_before && ends_expression(p) && u != T_COMMA && binop_prec(u, 0) < 0 && u != T_ASSIGN &&
            u != T_DOT && u != T_QUESTION && u != T_LPAREN && u != T_LBRACKET && u != T_OPTCHAIN)
            break;
        if (u == T_IN || u == T_OF) break; /* for (let x of ...) */
        if (u == T_COMMA) {
            next(C);
            if (TOK == T_LBRACE || TOK == T_LBRACKET) scan_pattern(C, d, kind);
            else if (is_name_tok_raw(TOK)) {
                decl_add(C, d, tok_atom(C), kind);
                next(C);
            }
            p = T_IDENT;
            continue;
        }
        p = u;
        skip_balanced(C);
        if (u == T_LPAREN || u == T_LBRACKET || u == T_LBRACE) p = T_RPAREN;
    }
}

/* Scans from the current token to the end of the block (the '}' that
 * closes it, or the end of input for a script), without compiling, and
 * collects declarations:
 *   lexical (let/const/class/function) at the block's own level;
 *   var at any level, when `want_vars` (function bodies), except inside
 *   nested functions.
 * Also reports whether `arguments` is used (outside nested non-arrow
 * functions). Restores the lexer afterwards. */
static void prescan(Compiler *C, Decls *d, int want_vars, int *uses_arguments) {
    LexState st = lex_save(&C->lx);
    int      braces = 0, parens = 0, brackets = 0;
    int      tmpl[64], ntmpl = 0;
    TokType  prev = T_SEMI, before_paren = T_SEMI;
    int      paren_owner[64];
    int      nparen_owner = 0;
    int      fn_depth[64], fn_arrow[64]; /* brace depths at which nested function bodies started */
    uint32_t fn_start[64];
    int      fn_args[64], args_seen = 0;
    int      nfn = 0, nfn_plain = 0;
    uint32_t bopen[128];            /* where the open braces started */
    int      bargs[128], bvars[128]; /* args_seen, vars_seen there */
    int      bnfn[128];              /* nfn there: a block inside a nested function is not recorded */
    int      vars_seen = 0;
    int      hits0 = C->args_hits;
    int      case_label = 0, qdepth = 0; /* inside `case ...:` (switch bodies) */
    int      async_stmt = 0;              /* the last `async` started a statement */

    for (;;) {
        TokType t = TOK;
        if (t == T_ASYNC)
            async_stmt = prev == T_SEMI || prev == T_RBRACE || prev == T_LBRACE || prev == T_EXPORT ||
                         prev == T_DEFAULT || C->lx.tok.nl_before;
        if (C->args_hits != hits0) {
            /* `arguments` inside a declarator's initializer (skipped over) */
            hits0 = C->args_hits;
            args_seen++;
            if (uses_arguments && nfn_plain == 0) *uses_arguments = 1;
        }
        if (t == T_EOF) break;
        if (braces == 0 && parens == 0 && brackets == 0 && nfn == 0) {
            /* a switch body: a statement starts after `case x:` (whose
             * expression may hold ?: of its own) */
            if (t == T_CASE || (t == T_DEFAULT && prev != T_EXPORT)) {
                case_label = 1;
                qdepth     = 0;
            } else if (case_label && t == T_QUESTION) {
                qdepth++;
            } else if (case_label && t == T_COLON) {
                if (qdepth == 0) {
                    case_label = 0;
                    prev       = T_SEMI;
                    next(C);
                    continue;
                }
                qdepth--;
            }
        }
        if (t == T_RBRACE) {
            if (ntmpl > 0 && tmpl[ntmpl - 1] == braces) {
                lex_template_continue(&C->lx);
                if (TOK == T_ERROR) unexpected(C);
                if (C->lx.tok.template_tail) ntmpl--;
                prev = T_TEMPLATE;
                next(C);
                continue;
            }
            if (braces == 0) break;
            braces--;
            prev = t;
            next(C);
            if (braces < 128 && bnfn[braces] == 0)
                skip_add(&C->group_ends, bopen[braces], lex_save(&C->lx),
                         (args_seen > bargs[braces] ? SKIP_ARGS : 0) | (vars_seen > bvars[braces] ? SKIP_VARS : 0));
            if (nfn > 0 && fn_depth[nfn - 1] == braces) {
                nfn--;
                if (!fn_arrow[nfn]) nfn_plain--;
                skip_add(&C->fn_ends, fn_start[nfn], lex_save(&C->lx), args_seen > fn_args[nfn] ? SKIP_ARGS : 0);
            }
            continue;
        }
        if (t == T_TEMPLATE && !C->lx.tok.template_tail) {
            if (ntmpl >= 64) fail(C, "SyntaxError: templates nested too deeply");
            tmpl[ntmpl++] = braces;
        }
        if (t == T_IDENT && C->lx.tok.ident_len == 9 && prev != T_DOT && tok_is_word(C, "arguments")) {
            args_seen++;
            if (uses_arguments && nfn_plain == 0) *uses_arguments = 1;
        }
        if (t == T_LPAREN) {
            if (nparen_owner < 64) paren_owner[nparen_owner++] = prev;
            parens++;
        } else if (t == T_RPAREN) {
            if (parens > 0) parens--;
            before_paren = nparen_owner > 0 ? (TokType)paren_owner[--nparen_owner] : T_SEMI;
        } else if (t == T_LBRACKET) {
            brackets++;
        } else if (t == T_RBRACKET) {
            if (brackets > 0) brackets--;
        } else if (t == T_LBRACE) {
            /* A function body starts here after `=>`, or after `(...)` that
             * followed a name (function f(), method(), get x()). */
            int is_arrow = prev == T_ARROW;
            int is_fn    = is_arrow ||
                        (prev == T_RPAREN && !is_block_keyword(before_paren) && before_paren != T_SEMI);
            if (is_fn) {
                Skip *sk = skip_find(&C->fn_ends, C->lx.tok.start);
                if (sk) {
                    /* crossed before: of its content only an arrow's `arguments` matters here */
                    if ((sk->flags & SKIP_ARGS) && is_arrow && uses_arguments && nfn_plain == 0) *uses_arguments = 1;
                    if (sk->flags & SKIP_ARGS) args_seen++;
                    lex_restore(&C->lx, sk->after);
                    prev = T_RBRACE;
                    continue;
                }
            }
            if (!is_fn && nfn == 0) {
                /* a nested block or object crossed before: skipped, unless it
                 * holds a `var` this prescan collects or `arguments` it reports */
                Skip *sk = skip_find(&C->group_ends, C->lx.tok.start);
                if (sk && !((sk->flags & SKIP_VARS) && want_vars) && !((sk->flags & SKIP_ARGS) && uses_arguments)) {
                    lex_restore(&C->lx, sk->after);
                    prev = T_RBRACE;
                    continue;
                }
            }
            if (braces < 128) {
                bopen[braces] = C->lx.tok.start;
                bargs[braces] = args_seen;
                bvars[braces] = vars_seen;
                bnfn[braces]  = nfn;
            }
            if (is_fn && nfn < 64) {
                fn_arrow[nfn]   = is_arrow;
                fn_start[nfn]   = C->lx.tok.start;
                fn_args[nfn]    = args_seen;
                fn_depth[nfn++] = braces;
                if (!is_arrow) nfn_plain++;
            }
            braces++;
        } else if (t == T_VAR && nfn == 0 && !(vars_seen++, want_vars)) {
            /* counted for the group tables; not this prescan's to collect */
        } else if (nfn == 0 && ((t == T_VAR && want_vars) ||
                                ((t == T_LET || t == T_CONST || t == T_CLASS) && braces == 0 && parens == 0 &&
                                 brackets == 0))) {
            int kind = t == T_VAR ? K_VAR : t == T_LET ? K_LET : t == T_CONST ? K_CONST : K_CLASS;
            next(C);
            if (TOK == T_LBRACE || TOK == T_LBRACKET) {
                if (kind == K_CLASS) {
                    prev = t;
                    continue;
                }
                scan_pattern(C, d, kind);
            } else if (is_name_tok_raw(TOK)) {
                decl_add(C, d, tok_atom(C), kind);
                next(C);
            } else {
                prev = t;
                continue;
            }
            if (kind == K_CLASS) {
                prev = T_IDENT;
                continue;
            }
            scan_declarators(C, d, kind);
            prev = T_IDENT;
            continue;
        } else if (t == T_IMPORT && nfn == 0 && braces == 0 && parens == 0 && brackets == 0 &&
                   C->fs->is_module && !C->fs->parent) {
            /* import bindings are module-scope constants */
            next(C);
            if (TOK == T_LPAREN || TOK == T_DOT) {
                prev = T_IMPORT;
                continue;
            }
            while (TOK != T_STRING && TOK != T_EOF && !tok_is_word(C, "from")) {
                if (TOK == T_STAR) {
                    next(C); /* as */
                    next(C);
                    decl_add(C, d, tok_atom(C), K_CONST);
                    next(C);
                } else if (TOK == T_LBRACE) {
                    next(C);
                    while (TOK != T_RBRACE && TOK != T_EOF) {
                        PxValue name = TOK == T_STRING ? 0 : tok_atom(C);
                        next(C);
                        if (tok_is_word(C, "as")) {
                            next(C);
                            name = tok_atom(C);
                            next(C);
                        }
                        if (name) decl_add(C, d, name, K_CONST);
                        if (TOK == T_COMMA) next(C);
                    }
                    next(C);
                } else if (is_name_tok_raw(TOK)) {
                    decl_add(C, d, tok_atom(C), K_CONST);
                    next(C);
                } else {
                    next(C);
                }
                if (TOK == T_COMMA) next(C);
            }
            prev = T_IMPORT;
            continue;
        } else if (t == T_FUNCTION && nfn == 0 && braces == 0 && parens == 0 && brackets == 0 &&
                   (prev == T_SEMI || prev == T_RBRACE || prev == T_LBRACE || (prev == T_ASYNC && async_stmt) ||
                    prev == T_EXPORT || prev == T_DEFAULT || C->lx.tok.nl_before)) {
            /* A function declaration at the block's own level. */
            next(C);
            if (TOK == T_STAR) next(C);
            if (is_name_tok_raw(TOK)) decl_add(C, d, tok_atom(C), K_FUNC);
            prev = T_FUNCTION;
            continue;
        }
        prev = t;
        next(C);
    }
    lex_restore(&C->lx, st);
}

/* ------------------------------------------------------------ blocks */

typedef struct Block {
    int nactive;
    int hoist_start;
    int hoist_jump;
    int resume;
} Block;

static void block_begin(Compiler *C, Block *b, int is_function_body, int *uses_arguments) {
    FuncState *fs     = C->fs;
    uint32_t   roots0 = C->vm->nroots;
    /* functions are lexical in blocks, and at a module's top level */
    Decls *d = decls_new(C, !is_function_body || (fs->is_module && !fs->parent));
    int    i, has_fn = 0;

    b->nactive     = fs->nactive;
    b->hoist_start = fs->nhoists;
    b->hoist_jump  = -1;
    prescan(C, d, is_function_body, uses_arguments);
    if (C->param_floor1) {
        /* a function body or catch block may not redeclare a parameter
         * lexically (a function body's own functions are var-like) */
        int floor = C->param_floor1 - 1, j;
        C->param_floor1 = 0;
        for (i = 0; i < d->n; i++) {
            if (!IS_LEXICAL(DECL_KIND(d, i)) && !(DECL_KIND(d, i) == K_FUNC && !is_function_body)) continue;
            for (j = floor; j < b->nactive; j++)
                if (fs->locals[j].name == DECL_NAME(d, i) && j != fs->self_slot1 - 1 && j != fs->args_slot)
                    fail(C, "SyntaxError: redeclaration of a parameter");
        }
    }
    for (i = 0; i < d->n; i++) {
        int     kind = DECL_KIND(d, i);
        PxValue name = DECL_NAME(d, i);
        if (kind == K_FUNC) has_fn = 1;
        if (fs->is_script && is_function_body && (kind == K_VAR || kind == K_FUNC)) {
            /* Script top level: var and function are global object
             * properties, created before the first statement runs. */
            if (kind == K_VAR) {
                emit_op(C, OP_UNDEF);
                emit_op_u16(C, OP_DEF_GLOBAL, add_const(C, name));
            }
            continue;
        }
        if (kind == K_VAR) {
            /* a parameter already, or declared twice; a var named like the
             * function itself is a new binding that shadows it */
            int l = find_local(fs, name);
            if (l >= 0 && l != fs->self_slot1 - 1) continue;
        }
        {
            int slot = declare_local(C, name, kind);
            if (IS_LEXICAL(kind)) emit_local(C, OP_INIT_HOLE, slot);
        }
    }
    decls_free(C, d);
    C->vm->nroots = roots0; /* the names now live in locals/consts, which are roots */
    if (has_fn) {
        b->hoist_jump = emit_jump(C, OP_JUMP);
        b->resume     = label_here(C);
    }
}

static void block_end(Compiler *C, Block *b) {
    FuncState *fs = C->fs;
    if (b->hoist_jump >= 0) {
        int over = emit_jump(C, OP_JUMP), i;
        patch_here(C, b->hoist_jump);
        for (i = b->hoist_start; i < fs->nhoists; i++) {
            Hoist *h = &fs->hoists[i];
            emit_op_u16(C, OP_CLOSURE, h->cidx);
            if (h->is_global) emit_op_u16(C, OP_DEF_GLOBAL, h->name_cidx);
            else emit_local(C, OP_PUT_LOCAL, h->slot);
        }
        emit_back(C, OP_JUMP, b->resume);
        patch_here(C, over);
    }
    fs->nhoists = b->hoist_start;
    close_from(C, b->nactive);
    fs->nactive = b->nactive;
}

/* CLOSE_UPVALS for locals >= from, if a nested function captured one:
 * nothing else can have an open upvalue into them. */
static void close_from(Compiler *C, int from) {
    FuncState *fs = C->fs;
    int        i;
    for (i = from; i < fs->nactive; i++)
        if (fs->locals[i].captured) {
            emit_local(C, OP_CLOSE_UPVALS, from);
            return;
        }
}

static void scope_begin(Compiler *C, Block *b) {
    b->nactive     = C->fs->nactive;
    b->hoist_jump  = -1;
    b->hoist_start = C->fs->nhoists;
}

static void block_statement(Compiler *C) {
    Block b;
    next(C); /* { */
    block_begin(C, &b, 0, NULL);
    while (TOK != T_RBRACE) {
        if (TOK == T_EOF) fail(C, "SyntaxError: expected '}'");
        statement(C);
    }
    block_end(C, &b);
    next(C);
}

/* ------------------------------------------------------------ functions */

static void funcstate_free(FuncState *fs) {
    int i;
    free(fs->code);
    free(fs->lines);
    for (i = 0; i < fs->nloops; i++) {
        free(fs->loops[i].breaks.v);
        free(fs->loops[i].conts.v);
    }
    free(fs);
}

static FuncState *funcstate_new(Compiler *C, PxValue name, int flags) {
    FuncState *fs = (FuncState *)calloc(1, sizeof(FuncState));
    if (!fs) {
        px_throw_oom(C->vm);
        check_alloc(C, 0);
    }
    fs->parent    = C->fs;
    fs->name      = name;
    fs->flags     = flags;
    fs->line      = C->lx.tok.line;
    fs->last_line = fs->line;
    fs->args_slot = -1;
    if (flags & PX_PROTO_ARROW) fs->ctx = fs->parent->ctx & ~CTX_OWN;
    else if (fs->parent)
        fs->ctx = CTX_NEW_TARGET | ((flags & PX_PROTO_METHOD) ? CTX_SUPER_PROP : 0) |
                  ((flags & PX_PROTO_DERIVED) ? CTX_SUPER_CALL : 0);
    if (flags & PX_PROTO_ASYNC) fs->ctx |= CTX_AWAIT_KW;
    C->fs         = fs;
    return fs;
}

static PxProto *funcstate_finish(Compiler *C) {
    FuncState *fs = C->fs;
    PxVM      *vm = C->vm;
    PxProto   *p;
    PxBytes   *code, *lines, *ud, *ics = NULL;
    PxValue    pv, cv, lv, iv = 0;
    uint8_t    udesc[3 * MAX_UPVALS];
    int        i;

    for (i = 0; i < fs->nupvals; i++) {
        udesc[3 * i]     = fs->upvals[i].is_local;
        udesc[3 * i + 1] = (uint8_t)(fs->upvals[i].index & 0xFF);
        udesc[3 * i + 2] = (uint8_t)(fs->upvals[i].index >> 8);
    }
    code = px_bytes_new(vm, fs->code, fs->len);
    check_alloc(C, code != NULL);
    cv = px_from_ptr(code);
    PX_ROOT(vm, cv);
    lines = px_bytes_new(vm, fs->lines, fs->lines_len);
    check_alloc(C, lines != NULL);
    lv = px_from_ptr(lines);
    PX_ROOT(vm, lv);
    ud = px_bytes_new(vm, udesc, (uint32_t)(3 * fs->nupvals));
    check_alloc(C, ud != NULL);
    pv = px_from_ptr(ud);
    PX_ROOT(vm, pv);
    if (fs->nics > 0) {
        ics = px_bytes_new(vm, NULL, (uint32_t)fs->nics * (uint32_t)sizeof(PxIC)); /* zeroed: empty */
        check_alloc(C, ics != NULL);
        iv = px_from_ptr(ics);
    }
    PX_ROOT(vm, iv);
    p = (PxProto *)px_alloc(vm, PX_T_PROTO, sizeof(PxProto));
    check_alloc(C, p != NULL);
    px_pop_roots(vm, 4);
    p->code       = code;
    p->lines      = lines;
    p->upval_desc = ud;
    p->ics        = ics;
    p->consts     = fs->consts;
    p->name       = fs->name;
    p->filename   = C->filename;
    p->nparams    = (uint16_t)fs->nparams;
    p->length     = (uint8_t)fs->length;
    p->nlocals    = (uint16_t)fs->nslots;
    /* +4: the conditional-branch paths are counted approximately; a small
     * margin keeps the estimate an upper bound. */
    p->max_stack = (uint16_t)(fs->max_stack + 4);
    p->nupvals   = (uint8_t)fs->nupvals;
    p->flags     = (uint16_t)fs->flags;
    p->args_slot = (uint8_t)(fs->args_slot >= 0 ? fs->args_slot : 0);
    p->self_slot = (uint8_t)(fs->self_slot1 ? fs->self_slot1 - 1 : 0xFF);
    p->line      = fs->line;
    C->fs        = fs->parent;
    funcstate_free(fs);
    return p;
}

static void emit_closure(Compiler *C, PxProto *p) {
    PxValue pv = px_from_ptr(p);
    PX_ROOT(C->vm, pv);
    emit_op_u16(C, OP_CLOSURE, add_const(C, pv));
    px_pop_roots(C->vm, 1);
}

typedef struct Param {
    int      slot;
    int      pattern;
    int      has_default;
    LexState pos, dflt;
} Param;

/* The local that receives the arguments object (PX_PROTO_ARGUMENTS). */
static void declare_arguments(Compiler *C) {
    FuncState *fs = C->fs;
    if (fs->args_slot >= 0 || find_local(fs, C->atom_arguments) >= 0) return;
    fs->args_slot = declare_local(C, C->atom_arguments, K_VAR);
    fs->flags |= PX_PROTO_ARGUMENTS;
}

/* Parameters from after '(' to after ')'. Simple names and patterns get a
 * slot each, in order; then the names patterns bind are declared; then
 * the defaults and patterns are compiled, left to right, by rewinding. */
static int parameters(Compiler *C) {
    FuncState *fs = C->fs;
    Param      ps[MAX_PARAMS];
    int        n = 0, rest = -1, i, simple = 1, length = -1, hits0 = C->args_hits;
    int        names_before[MAX_PARAMS + 1], first_name = 0; /* pattern names of params < i */
    uint32_t   roots0;
    LexState   after;
    Decls     *d;

    while (TOK != T_RPAREN) {
        Param *p;
        int    is_rest = 0;
        if (n >= MAX_PARAMS) fail(C, "SyntaxError: too many parameters");
        p = &ps[n];
        memset(p, 0, sizeof *p);
        if (TOK == T_ELLIPSIS) {
            next(C);
            is_rest = 1;
        }
        p->pos = lex_save(&C->lx);
        if (TOK == T_LBRACE || TOK == T_LBRACKET) {
            p->pattern = 1;
            p->slot    = declare_local(C, 0, K_PARAM);
            skip_balanced(C);
            simple     = 0;
        } else {
            PxValue name = binding_name(C);
            if (find_local(fs, name) >= 0) fail(C, "SyntaxError: duplicate parameter name");
            p->slot = declare_local(C, name, K_PARAM);
        }
        if (TOK == T_ASSIGN) {
            if (is_rest) fail(C, "SyntaxError: a rest parameter cannot have a default");
            next(C);
            p->has_default = 1;
            p->dflt        = lex_save(&C->lx);
            skip_expression(C);
            simple = 0;
            if (length < 0) length = n;
        }
        if (is_rest) {
            simple = 0;
            if (length < 0) length = n;
        }
        if (is_rest) {
            rest = n;
            n++;
            if (TOK != T_RPAREN) fail(C, "SyntaxError: the rest parameter must be last");
            break;
        }
        n++;
        if (TOK != T_RPAREN) expect(C, T_COMMA);
    }
    next(C); /* ) */
    after       = lex_save(&C->lx);
    fs->nparams = rest >= 0 ? rest : n;
    fs->length  = length >= 0 ? length : n;
    if (rest >= 0) fs->flags |= PX_PROTO_REST;
    if (C->args_hits > hits0 && !(fs->flags & PX_PROTO_ARROW)) declare_arguments(C);

    roots0 = C->vm->nroots;
    d      = decls_new(C, 0);
    for (i = 0; i < n; i++) {
        names_before[i] = (int)d->n;
        if (!ps[i].pattern) continue;
        lex_restore(&C->lx, ps[i].pos);
        scan_pattern(C, d, K_LET); /* lexical for the scan: a name twice is an error */
    }
    names_before[n] = (int)d->n;
    first_name      = fs->nactive;
    for (i = 0; i < d->n; i++) {
        if (find_local(fs, DECL_NAME(d, i)) >= 0) fail(C, "SyntaxError: duplicate parameter name");
        declare_local(C, DECL_NAME(d, i), K_VAR);
    }
    decls_free(C, d);
    C->vm->nroots = roots0;
    /* A named function expression sees its own name, in the defaults too.
     * Filled in by the call itself (push_frame), not by code. */
    if (!(fs->flags & PX_PROTO_ARROW) && px_is_ptr(fs->name) &&
        !(fs->flags & (PX_PROTO_METHOD | PX_PROTO_CLASS_CTOR)) && find_local(fs, fs->name) < 0) {
        int self_slot              = declare_local(C, fs->name, K_CONST);
        fs->locals[self_slot].init = 1;
        fs->self_slot1             = self_slot + 1;
    }
    fs->ctx |= CTX_PARAMS;
    for (i = 0; i < n; i++) {
        fs->tdz_lo  = ps[i].slot;
        fs->tdz_hi  = ps[n - 1].slot + 1;
        fs->tdz_lo2 = first_name + names_before[i];
        fs->tdz_hi2 = first_name + names_before[n];
        if (ps[i].has_default) {
            int skip;
            emit_local(C, OP_GET_LOCAL, ps[i].slot);
            emit_op(C, OP_UNDEF);
            emit_op(C, OP_SEQ);
            skip = emit_jump(C, OP_JUMP_IF_FALSE);
            lex_restore(&C->lx, ps[i].dflt);
            if (!ps[i].pattern) C->name_hint = fs->locals[ps[i].slot].name;
            expr_value(C);
            emit_local(C, OP_PUT_LOCAL, ps[i].slot);
            patch_here(C, skip);
        }
        if (ps[i].pattern) {
            emit_local(C, OP_GET_LOCAL, ps[i].slot);
            lex_restore(&C->lx, ps[i].pos);
            bind_target(C, BIND_DECL, K_VAR);
        }
    }
    fs->tdz_lo = fs->tdz_hi = fs->tdz_lo2 = fs->tdz_hi2 = 0;
    fs->ctx &= ~CTX_PARAMS;
    lex_restore(&C->lx, after);
    return simple;
}

/* The directive prologue of a body whose parameters are not all plain
 * names: "use strict" is an error there. The lexer is left where it was. */
static void check_directives(Compiler *C) {
    LexState st = lex_save(&C->lx);
    while (TOK == T_STRING) {
        const char *raw = C->lx.src + C->lx.tok.start;
        int use_strict  = C->lx.tok.len == 12 && (memcmp(raw, "\"use strict\"", 12) == 0 ||
                                                 memcmp(raw, "'use strict'", 12) == 0);
        next(C);
        if (TOK != T_SEMI && TOK != T_RBRACE && !C->lx.tok.nl_before) break; /* "a" + b: not a directive */
        if (use_strict) fail(C, "SyntaxError: \"use strict\" is not allowed in a function with non-simple parameters");
        if (TOK == T_SEMI) next(C);
    }
    lex_restore(&C->lx, st);
}

/* Parses parameters and body with the lexer at '(' (or at the single
 * parameter name of an arrow), compiles them into a new function, and
 * emits CLOSURE for it in the enclosing function. */
static void compile_function(Compiler *C, PxValue name, int flags) {
    FuncState *fs;
    int        is_arrow = (flags & PX_PROTO_ARROW) != 0, simple = 1;
    Block      b;

    fs = funcstate_new(C, name, flags);
    if (is_arrow && TOK != T_LPAREN) {
        declare_local(C, binding_name(C), K_PARAM);
        fs->nparams = fs->length = 1;
    } else {
        expect(C, T_LPAREN);
        simple = parameters(C);
    }
    /* an arrow's parameters are in the await context around it, its body
     * only if the arrow is async (ConciseBody is parsed [~Await]) */
    if (is_arrow && !(flags & PX_PROTO_ASYNC)) fs->ctx &= ~CTX_AWAIT_KW;
    if ((flags & FN_GETTER) && (fs->nparams || (fs->flags & PX_PROTO_REST)))
        fail(C, "SyntaxError: a getter takes no parameters");
    if ((flags & FN_SETTER) && (fs->nparams != 1 || (fs->flags & PX_PROTO_REST)))
        fail(C, "SyntaxError: a setter takes exactly one parameter");
    /* A named function expression sees its own name. */
    if (!is_arrow && px_is_ptr(name) && !(flags & (PX_PROTO_METHOD | PX_PROTO_CLASS_CTOR)) && find_local(fs, name) < 0) {
        /* filled in by the call itself (push_frame), not by code */
        int self_slot                  = declare_local(C, name, K_CONST);
        fs->locals[self_slot].init     = 1;
        fs->self_slot1                 = self_slot + 1;
    }
    if ((flags & PX_PROTO_CLASS_CTOR) && !(flags & PX_PROTO_DERIVED)) emit_op(C, OP_INIT_FIELDS);
    if (is_arrow) {
        expect(C, T_ARROW);
        if (TOK != T_LBRACE) {
            expr_value(C);
            emit_op(C, OP_RETURN);
            emit_closure(C, funcstate_finish(C));
            return;
        }
    }
    if (TOK != T_LBRACE) fail(C, "SyntaxError: expected '{' before function body");
    next(C); /* { */
    if (!simple) check_directives(C);
    {
        int uses_arguments = 0;
        C->param_floor1    = 1;
        block_begin(C, &b, 1, is_arrow ? NULL : &uses_arguments);
        if (uses_arguments) declare_arguments(C);
        fs->var_floor = fs->nactive;
        /* the call binds a generator's parameters, then returns it here */
        if (fs->flags & PX_PROTO_GENERATOR) emit_op(C, OP_GEN_START);
    }
    while (TOK != T_RBRACE) {
        if (TOK == T_EOF) fail(C, "SyntaxError: expected '}' at the end of the function");
        statement(C);
    }
    next(C);
    emit_op(C, OP_RETURN_UNDEF);
    block_end(C, &b);
    emit_closure(C, funcstate_finish(C));
}

/* ------------------------------------------------------------ classes */

typedef struct Member {
    LexState start;  /* the first token (`static`, a modifier or the name) */
    LexState head;   /* after `static`: the modifiers and the name */
    uint8_t  is_static, is_field, is_ctor, is_block, computed;
    uint8_t  kind;   /* PX_MK_METHOD, _GET, _SET */
    int16_t  priv;   /* the private name's index, or -1 */
    int16_t  slot;   /* a local: a computed field's key, or an instance private method */
} Member;

/* What a class declares under each private name, for the duplicate check:
 * only a getter and a setter of the same staticness may share one. */
enum { PRIV_FIELD = 1, PRIV_METHOD = 2, PRIV_GET = 4, PRIV_SET = 8, PRIV_STATIC = 16 };

#define MAX_PRIVATE 128

/* The end of a class field: a ';', the '}', or a line break (ASI). */
static void field_end(Compiler *C) {
    if (TOK != T_SEMI && TOK != T_RBRACE && !C->lx.tok.nl_before)
        fail(C, "SyntaxError: expected ';' after a class field");
}

/* Walks the class body once, recording each member, and reports the early
 * errors of class bodies. The lexer is just past '{' and is left at '}'. */
static int scan_members(Compiler *C, Member *m, PxValue *privs, uint8_t *pinfo, int *npriv) {
    int n = 0, nl, has_ctor = 0;
    while (TOK != T_RBRACE) {
        Member *cur;
        int     fn = 0, is_ctor_name = 0, is_proto_name = 0;
        if (TOK == T_EOF) fail(C, "SyntaxError: expected '}' at the end of the class body");
        if (TOK == T_SEMI) {
            next(C);
            continue;
        }
        if (n >= MAX_MEMBERS) fail(C, "SyntaxError: too many class members");
        cur = &m[n++];
        memset(cur, 0, sizeof *cur);
        cur->priv  = -1;
        cur->slot  = -1;
        cur->start = lex_save(&C->lx);
        if (TOK == T_STATIC && modifier_ahead(C, &nl)) {
            cur->is_static = 1;
            next(C);
            if (TOK == T_LBRACE) {
                cur->is_block = 1;
                skip_balanced(C);
                continue;
            }
        }
        cur->head = lex_save(&C->lx);
        /* modifiers */
        if (TOK == T_ASYNC && modifier_ahead(C, &nl) && !nl) {
            fn = 1;
            next(C);
        }
        if (TOK == T_STAR) {
            fn = 1;
            next(C);
        }
        if (!fn && (TOK == T_GET || TOK == T_SET) && modifier_ahead(C, &nl)) {
            cur->kind = TOK == T_GET ? PX_MK_GET : PX_MK_SET;
            next(C);
        }
        /* the name */
        if (TOK == T_PRIVATE) {
            PxValue pn = tok_atom(C);
            int     k, what;
            if (C->lx.tok.ident_len == 12 && memcmp(C->lx.tok.ident, "#constructor", 12) == 0)
                fail(C, "SyntaxError: #constructor is not a valid private name");
            for (k = 0; k < *npriv; k++)
                if (privs[k] == pn) break;
            if (k == *npriv) {
                if (*npriv >= MAX_PRIVATE) fail(C, "SyntaxError: too many private names in one class");
                privs[(*npriv)++] = pn;
                PX_ROOT(C->vm, privs[k]);
                pinfo[k] = 0;
            }
            next(C);
            what = TOK != T_LPAREN ? PRIV_FIELD
                 : cur->kind == PX_MK_GET ? PRIV_GET
                 : cur->kind == PX_MK_SET ? PRIV_SET
                                          : PRIV_METHOD;
            if (pinfo[k] && !((pinfo[k] & ~PRIV_STATIC) == (what == PRIV_GET ? PRIV_SET : PRIV_GET) &&
                              (what == PRIV_GET || what == PRIV_SET) &&
                              !(pinfo[k] & PRIV_STATIC) == !cur->is_static))
                fail(C, "SyntaxError: duplicate private name");
            pinfo[k] |= (uint8_t)(what | (cur->is_static ? PRIV_STATIC : 0));
            cur->priv = (int16_t)k;
        } else if (TOK == T_LBRACKET) {
            cur->computed = 1;
            skip_balanced(C);
        } else if (is_prop_name_tok(TOK) || TOK == T_STRING || TOK == T_NUMBER) {
            is_ctor_name  = tok_is_word(C, "constructor") || tok_str_is(C, "constructor");
            is_proto_name = tok_is_word(C, "prototype") || tok_str_is(C, "prototype");
            next(C);
        } else {
            unexpected(C);
        }
        if (is_proto_name && cur->is_static)
            fail(C, "SyntaxError: a class may not have a static member named prototype");
        if (TOK == T_LPAREN) {
            if (is_ctor_name && !cur->is_static) {
                if (fn || cur->kind != PX_MK_METHOD)
                    fail(C, "SyntaxError: a class constructor may not be an accessor, a generator or async");
                if (has_ctor) fail(C, "SyntaxError: a class may only have one constructor");
                has_ctor     = 1;
                cur->is_ctor = 1;
            }
            skip_balanced(C); /* params */
            if (TOK != T_LBRACE) expect(C, T_LBRACE);
            skip_balanced(C); /* body */
        } else {
            if (fn || cur->kind != PX_MK_METHOD) unexpected(C);
            if (is_ctor_name) fail(C, "SyntaxError: a class field may not be named constructor");
            cur->is_field = 1;
            if (TOK == T_ASSIGN) {
                next(C);
                while (TOK != T_SEMI && TOK != T_RBRACE && TOK != T_EOF) {
                    skip_balanced(C);
                    if (C->lx.tok.nl_before && TOK != T_DOT && binop_prec(TOK, 0) < 0 && TOK != T_QUESTION &&
                        TOK != T_LPAREN && TOK != T_LBRACKET && TOK != T_OPTCHAIN && TOK != T_COMMA)
                        break;
                }
            }
            field_end(C);
            if (TOK == T_SEMI) next(C);
        }
    }
    return n;
}

/* An upvalue of the function being compiled for local `slot` of the one
 * around it: the class members' hidden locals, which have no name. */
static int capture_local(Compiler *C, int slot) {
    C->fs->parent->locals[slot].captured = 1;
    return add_upval(C, C->fs, 0, 1, slot, K_VAR);
}

static void compile_thunk(Compiler *C, LexState pos, int is_block, PxValue name);

/* Defines field mb on the object on top of the stack (which stays):
 * instance fields inside the initialiser, on `this`; static ones in the
 * class definition, on the class, their value made by a thunk method. */
static void define_field(Compiler *C, Member *mb, PxValue *privs, int is_static) {
    PxValue key = 0, hint = PX_UNDEFINED;
    lex_restore(&C->lx, mb->head);
    if (mb->priv >= 0) {
        Exp pe;
        resolve(C, privs[mb->priv], &pe);
        discharge(C, &pe);
        hint = privs[mb->priv];
        next(C);
    } else if (mb->computed) {
        if (is_static) emit_local(C, OP_GET_LOCAL, mb->slot);
        else emit_op_u8(C, OP_GET_UPVAL, capture_local(C, mb->slot));
        skip_balanced(C);
    } else {
        key = property_key(C, NULL);
        PX_ROOT(C->vm, key);
        hint = method_name(C, PX_MK_METHOD, key);
    }
    PX_ROOT(C->vm, hint);
    if (TOK == T_ASSIGN) {
        next(C);
        if (is_static) {
            emit_op(C, key ? OP_DUP : OP_OVER); /* this: the class */
            emit_op(C, OP_DUP);                 /* its home */
            compile_thunk(C, lex_save(&C->lx), 0, hint);
            emit_op(C, OP_SET_HOME);
            emit_op_u8(C, OP_CALL, 0);
            adjust(C, -1);
        } else {
            C->name_hint = hint;
            expr_value(C);
            field_end(C);
        }
    } else {
        emit_op(C, OP_UNDEF);
    }
    if (mb->priv >= 0) emit_op(C, OP_DEFINE_PRIVATE);
    else if (mb->computed) emit_op(C, OP_DEFINE_ELEM);
    else emit_op_u16(C, OP_DEFINE_FIELD, add_const(C, key));
    px_pop_roots(C->vm, key ? 2 : 1);
    emit_op(C, OP_POP);
}

/* A thunk method: `return <expression at pos>` with this = the object it
 * is called on (static fields, static blocks). */
static void compile_thunk(Compiler *C, LexState pos, int is_block, PxValue name) {
    FuncState *fs = funcstate_new(C, PX_UNDEFINED, PX_PROTO_METHOD);
    fs->ctx |= CTX_NO_ARGS | (is_block ? CTX_AWAIT_KW | CTX_STATIC_BLOCK : 0);
    lex_restore(&C->lx, pos);
    if (is_block) {
        /* a static block is a function body of its own (its vars are its own) */
        Block b;
        next(C); /* { */
        block_begin(C, &b, 1, NULL);
        fs->var_floor = fs->nactive;
        while (TOK != T_RBRACE) {
            if (TOK == T_EOF) fail(C, "SyntaxError: expected '}'");
            statement(C);
        }
        next(C);
        emit_op(C, OP_RETURN_UNDEF);
        block_end(C, &b);
    } else {
        C->name_hint = name;
        expr_value(C);
        field_end(C);
        emit_op(C, OP_RETURN);
    }
    emit_closure(C, funcstate_finish(C));
}

/* class [name] [extends expr] { members }: leaves the constructor on the
 * stack. With inner_binding, `name` is visible (as a constant) inside. */
static void class_def(Compiler *C, PxValue name, int inner_binding) {
    FuncState *fs = C->fs;
    Member    *m;
    int        n, i, derived = 0, ctor = -1, inner = -1, has_init = 0, npriv = 0;
    LexState   end;
    Block      scope;
    PxValue    privs[MAX_PRIVATE];
    uint8_t    pinfo[MAX_PRIVATE];

    scope_begin(C, &scope);
    if (inner_binding && px_is_ptr(name)) {
        inner = declare_local(C, name, K_CONST);
        emit_local(C, OP_INIT_HOLE, inner);
    }
    if (TOK == T_EXTENDS) {
        Exp e;
        next(C);
        lhs_expr(C, &e);
        discharge(C, &e);
        derived = 1;
    } else {
        emit_op(C, OP_HOLE);
    }
    if (TOK != T_LBRACE) expect(C, T_LBRACE);
    next(C);
    m = (Member *)c_alloc(C, sizeof(Member) * MAX_MEMBERS);
    {
        uint32_t roots0 = C->vm->nroots;
        int      k;
        n   = scan_members(C, m, privs, pinfo, &npriv);
        end = lex_save(&C->lx); /* at '}' */
        /* one private symbol per #name, made where the class is defined */
        for (k = 0; k < npriv; k++) {
            int       slot = declare_local(C, privs[k], K_CONST);
            PxValue   desc;
            PxString *ps = px_str_flat(C->vm, privs[k]);
            check_alloc(C, ps != NULL);
            desc = px_str_slice(C->vm, privs[k], 1, ps->len);
            check_alloc(C, desc != PX_EXCEPTION);
            desc = px_intern_literal(C->vm, desc);
            check_alloc(C, desc != PX_EXCEPTION);
            emit_op_u16(C, OP_NEW_PRIVATE, add_const(C, desc));
            emit_local(C, OP_PUT_LOCAL, slot);
        }
        C->vm->nroots = roots0; /* the names are locals now */
    }
    for (i = 0; i < n; i++) {
        if (m[i].is_ctor) ctor = i;
        if (!m[i].is_static && (m[i].is_field || m[i].priv >= 0)) has_init = 1;
    }

    /* the constructor */
    {
        int flags = PX_PROTO_CLASS_CTOR | PX_PROTO_METHOD | (derived ? PX_PROTO_DERIVED : 0);
        if (ctor >= 0) {
            lex_restore(&C->lx, m[ctor].start);
            next(C); /* constructor */
            compile_function(C, name, flags);
        } else {
            /* constructor(...args) { super(...args) }  or  constructor() {} */
            FuncState *cfs = funcstate_new(C, name, flags);
            if (derived) {
                cfs->flags |= PX_PROTO_REST;
                declare_local(C, 0, K_PARAM);
                emit_local(C, OP_GET_LOCAL, 0);
                emit_op(C, OP_SUPER_CALL_ARRAY);
                emit_op(C, OP_INIT_FIELDS);
                emit_op(C, OP_POP);
            } else {
                emit_op(C, OP_INIT_FIELDS);
            }
            emit_op(C, OP_RETURN_UNDEF);
            emit_closure(C, funcstate_finish(C));
        }
    }
    emit_op(C, OP_CLASS); /* parent ctor -> ctor proto */

    /* The elements, in source order: methods and accessors are defined,
     * computed field names evaluated (and kept in a local), private
     * methods made. Field values and static blocks come later. */
    for (i = 0; i < n; i++) {
        Member *mb = &m[i];
        int     flags, kind, computed;
        PxValue key;
        if (i == ctor || mb->is_block) continue;
        lex_restore(&C->lx, mb->head);
        if (mb->is_field) {
            if (mb->computed) {
                property_key(C, NULL);
                emit_op(C, OP_TO_PROPKEY);
                mb->slot = (int16_t)declare_temp(C);
                emit_local(C, OP_PUT_LOCAL, mb->slot);
            }
            continue;
        }
        if (mb->priv >= 0) {
            /* a private method or accessor: made once, here; the initialiser
             * adds instance ones to each object, static ones go on the class
             * now. Its home (for super) is the prototype or the class. */
            PxValue fname = method_name(C, mb->kind, privs[mb->priv]);
            PX_ROOT(C->vm, fname);
            member_header(C, &flags, &kind, &computed); /* pushes the private symbol */
            emit_op(C, OP_POP);
            if (mb->is_static) {
                emit_op(C, OP_OVER);
                emit_local(C, OP_GET_LOCAL, find_local(fs, privs[mb->priv]));
                emit_op(C, OP_OVER);
            } else {
                emit_op(C, OP_DUP);
            }
            compile_function(C, fname, PX_PROTO_METHOD | flags);
            px_pop_roots(C->vm, 1);
            emit_op(C, OP_SET_HOME);
            if (mb->is_static) {
                emit_op_u8(C, OP_ADD_PRIVATE_METHOD, mb->kind);
                emit_op(C, OP_POP);
            } else {
                mb->slot = (int16_t)declare_temp(C);
                emit_local(C, OP_PUT_LOCAL, mb->slot);
            }
            continue;
        }
        emit_op(C, mb->is_static ? OP_OVER : OP_DUP); /* the target: ctor or proto */
        key = member_header(C, &flags, &kind, &computed);
        if (!computed) {
            PxValue fname;
            PX_ROOT(C->vm, key);
            fname = method_name(C, kind, key);
            PX_ROOT(C->vm, fname);
            compile_function(C, fname, PX_PROTO_METHOD | flags);
            emit_op_u16(C, OP_DEFINE_METHOD, add_const(C, key));
            emit_byte(C, (uint8_t)kind);
            px_pop_roots(C->vm, 2);
        } else {
            compile_function(C, PX_UNDEFINED, PX_PROTO_METHOD | flags);
            emit_op_u8(C, OP_DEFINE_METHOD_ELEM, kind);
        }
        emit_op(C, OP_POP);
    }

    /* The instance initialiser, run on every new object (INIT_FIELDS):
     * private methods first, then the fields in order. */
    if (has_init) {
        funcstate_new(C, PX_UNDEFINED, PX_PROTO_METHOD)->ctx |= CTX_NO_ARGS;
        for (i = 0; i < n; i++) {
            Member *mb = &m[i];
            Exp     pe;
            if (mb->is_static || mb->is_field || mb->priv < 0) continue;
            emit_op(C, OP_THIS);
            resolve(C, privs[mb->priv], &pe);
            discharge(C, &pe);
            emit_op_u8(C, OP_GET_UPVAL, capture_local(C, mb->slot));
            emit_op_u8(C, OP_ADD_PRIVATE_METHOD, mb->kind);
            emit_op(C, OP_POP);
        }
        for (i = 0; i < n; i++) {
            if (m[i].is_static || !m[i].is_field) continue;
            emit_op(C, OP_THIS);
            define_field(C, &m[i], privs, 0);
        }
        emit_op(C, OP_RETURN_UNDEF);
        emit_closure(C, funcstate_finish(C));
        emit_op(C, OP_SET_FIELDS);
    }

    /* The class's own name is bound (static code may use it); then static
     * fields and blocks run, in source order. */
    if (inner >= 0) {
        emit_op(C, OP_OVER);
        emit_local(C, OP_PUT_LOCAL, inner);
    }
    for (i = 0; i < n; i++) {
        if (!m[i].is_static || !(m[i].is_field || m[i].is_block)) continue;
        emit_op(C, OP_OVER); /* the class */
        if (m[i].is_block) {
            lex_restore(&C->lx, m[i].start);
            next(C); /* static */
            emit_op(C, OP_DUP);
            compile_thunk(C, lex_save(&C->lx), 1, PX_UNDEFINED);
            emit_op(C, OP_SET_HOME);
            emit_op_u8(C, OP_CALL, 0);
            adjust(C, -1);
            emit_op(C, OP_POP);
            continue;
        }
        define_field(C, &m[i], privs, 1);
    }
    c_free(C, m);
    emit_op(C, OP_POP); /* proto */
    block_end(C, &scope);
    lex_restore(&C->lx, end);
    next(C); /* } */
}

/* ------------------------------------------------------------ statements */

static void var_declarations(Compiler *C, int kind) {
    for (;;) {
        if (TOK == T_LBRACE || TOK == T_LBRACKET) {
            /* pattern = value */
            LexState pat = lex_save(&C->lx), after;
            skip_balanced(C);
            expect(C, T_ASSIGN);
            expr_value(C);
            after = lex_save(&C->lx);
            lex_restore(&C->lx, pat);
            bind_target(C, BIND_DECL, kind);
            lex_restore(&C->lx, after);
        } else {
            PxValue name = binding_name(C);
            PX_ROOT(C->vm, name);
            if (kind == K_VAR) check_var_conflict(C, name);
            if (TOK == T_ASSIGN) {
                next(C);
                C->name_hint = name;
                expr_value(C);
                init_name(C, name, kind);
            } else if (kind == K_CONST) {
                fail(C, "SyntaxError: missing initializer in const declaration");
            } else if (kind != K_VAR) {
                emit_op(C, OP_UNDEF);
                init_name(C, name, kind);
            }
            px_pop_roots(C->vm, 1);
        }
        if (TOK != T_COMMA) return;
        next(C);
    }
}

static Loop *loop_push(Compiler *C, int kind, int cont_target) {
    FuncState *fs = C->fs;
    Loop      *l;
    if (fs->nloops >= MAX_LOOPS) fail(C, "SyntaxError: loops nested too deeply");
    l = &fs->loops[fs->nloops++];
    memset(l, 0, sizeof *l);
    l->kind           = kind;
    l->cont_target    = cont_target;
    l->nactive        = fs->nactive;
    l->ntry           = fs->ntry;
    l->label          = fs->pending_label;
    fs->pending_label = 0;
    return l;
}

static void loop_pop(Compiler *C, int break_target) {
    FuncState *fs = C->fs;
    Loop      *l  = &fs->loops[fs->nloops - 1];
    int        i;
    for (i = 0; i < l->breaks.n; i++) patch_to(C, l->breaks.v[i], break_target);
    free(l->breaks.v);
    free(l->conts.v);
    fs->nloops--;
}

static void compile_finally_at(Compiler *C, LexState pos) {
    LexState here = lex_save(&C->lx);
    lex_restore(&C->lx, pos);
    block_statement(C);
    lex_restore(&C->lx, here);
}

/* Leaves try blocks top-1 .. down_to (running their finally code, each
 * compiled as it runs: with its try popped and only the loops around it
 * visible to its own break/continue). */
static void unwind_trys(Compiler *C, int top, int down_to, int nloops) {
    FuncState *fs = C->fs;
    int        i;
    for (i = top - 1; i >= down_to; i--) {
        TryCtx t = fs->trys[i];
        emit_op(C, OP_END_TRY);
        if (t.has_finally) {
            int saved_try = fs->ntry, saved_loops = fs->nloops;
            fs->ntry      = i;
            fs->nloops    = nloops;
            compile_finally_at(C, t.finally_pos);
            fs->ntry   = saved_try;
            fs->nloops = saved_loops;
        }
    }
}

/* Before a break/continue to loop `to_loop`, or a return (to_loop -1):
 * leaves everything in between, innermost first -- try blocks (finally
 * code runs) and for-of/for-in loops (their iterators are closed, which
 * runs a generator's finally blocks, and popped). With keep_top, a value
 * (the return value) stays on top of the stack throughout. Returns the
 * number of iterators popped: the jump that follows leaves the code the
 * compiler goes on with still expecting them. */
static int unwind_to(Compiler *C, int to_loop, int nactive, int keep_top) {
    FuncState *fs   = C->fs;
    int        ntry = fs->ntry, i, closed = 0;
    for (i = fs->nloops - 1; i > to_loop; i--) {
        Loop *x = &fs->loops[i];
        unwind_trys(C, ntry, x->ntry, i + 1);
        ntry = x->ntry;
        if (x->kind == LOOP_ITER) {
            if (keep_top) emit_op(C, OP_SWAP);
            emit_op(C, OP_ITER_CLOSE);
            closed++;
        } else if (x->kind == LOOP_ASYNC_ITER) {
            if (keep_top) emit_op(C, OP_SWAP);
            emit_op(C, OP_ASYNC_CLOSE); /* await it.return() */
            emit_op(C, OP_AWAIT);
            emit_op(C, OP_POP);
            closed++;
        }
    }
    unwind_trys(C, ntry, to_loop >= 0 ? fs->loops[to_loop].ntry : 0, to_loop + 1);
    close_from(C, nactive);
    return closed;
}

static void if_statement(Compiler *C) {
    int jelse;
    next(C);
    expect(C, T_LPAREN);
    expr(C);
    expect(C, T_RPAREN);
    jelse = emit_jump(C, OP_JUMP_IF_FALSE);
    substatement(C);
    if (TOK == T_ELSE) {
        int jend = emit_jump(C, OP_JUMP);
        next(C);
        patch_here(C, jelse);
        substatement(C);
        patch_here(C, jend);
    } else {
        patch_here(C, jelse);
    }
}

static void while_statement(Compiler *C) {
    int top, exit;
    next(C);
    top = label_here(C);
    expect(C, T_LPAREN);
    expr(C);
    expect(C, T_RPAREN);
    exit = emit_jump(C, OP_JUMP_IF_FALSE);
    loop_push(C, LOOP_PLAIN, top);
    substatement(C);
    emit_back(C, OP_LOOP, top);
    patch_here(C, exit);
    loop_pop(C, (int)C->fs->len);
}

static void do_statement(Compiler *C) {
    int   top, i;
    Loop *l;
    next(C);
    top = label_here(C);
    loop_push(C, LOOP_PLAIN, -1);
    substatement(C);
    l = &C->fs->loops[C->fs->nloops - 1];
    for (i = 0; i < l->conts.n; i++) patch_here(C, l->conts.v[i]);
    expect(C, T_WHILE);
    expect(C, T_LPAREN);
    expr(C);
    expect(C, T_RPAREN);
    {
        int exit = emit_jump(C, OP_JUMP_IF_FALSE);
        emit_back(C, OP_LOOP, top);
        patch_here(C, exit);
    }
    if (TOK == T_SEMI) next(C);
    loop_pop(C, (int)C->fs->len);
}

/* for (<target> of|in expr) body; the lexer is at the target. decl_kind
 * is -1 for an existing target (assignment). */
static void for_in_of(Compiler *C, int decl_kind, int is_await) {
    FuncState *fs = C->fs;
    LexState   target = lex_save(&C->lx);
    int        top, done, cont, brk, i, is_of, first_slot = fs->nactive, jend = -1;
    Loop      *l;

    if (TOK == T_LBRACE || TOK == T_LBRACKET) skip_balanced(C);
    else
        while (TOK != T_OF && TOK != T_IN && TOK != T_EOF) skip_balanced(C);
    is_of = TOK == T_OF;
    if (is_await && !is_of) fail(C, "SyntaxError: for await needs `of`");
    if (decl_kind >= 0 && decl_kind != K_VAR) {
        /* let/const: fresh bindings, declared inside the loop's scope and
         * in their TDZ while the expression runs (for (let x of x) throws) */
        LexState here   = lex_save(&C->lx);
        uint32_t roots0 = C->vm->nroots;
        Decls   *d      = decls_new(C, 1);
        lex_restore(&C->lx, target);
        scan_target(C, d, decl_kind);
        for (i = 0; i < d->n; i++) emit_local(C, OP_INIT_HOLE, declare_local(C, DECL_NAME(d, i), decl_kind));
        decls_free(C, d);
        C->vm->nroots = roots0;
        lex_restore(&C->lx, here);
    }
    next(C); /* of / in */
    if (is_of) expr_value(C);
    else expr(C);
    expect(C, T_RPAREN);
    emit_op(C, is_await ? OP_GET_ASYNC_ITER : is_of ? OP_FOR_OF : OP_FOR_IN);
    top = label_here(C);
    if (is_await) {
        /* [it] -> it.next() -> await -> done? -> value */
        emit_op(C, OP_DUP);
        emit_prop(C, OP_GET_PROP_KEEP, add_const(C, C->vm->atom[PX_ATOM_next]));
        emit_call(C, 0);
        emit_op(C, OP_AWAIT);
        emit_op(C, OP_DUP);
        emit_prop(C, OP_GET_PROP, add_const(C, C->vm->atom[PX_ATOM_done]));
        done = emit_jump(C, OP_JUMP_IF_TRUE);
        emit_prop(C, OP_GET_PROP, add_const(C, C->vm->atom[PX_ATOM_value]));
    } else {
        done = emit_jump(C, OP_ITER_NEXT);
    }
    {
        LexState here = lex_save(&C->lx);
        lex_restore(&C->lx, target);
        bind_target(C, decl_kind >= 0 ? BIND_DECL : BIND_ASSIGN, decl_kind >= 0 ? decl_kind : K_VAR);
        if (TOK != T_OF && TOK != T_IN) unexpected(C); /* one binding, no initialiser */
        lex_restore(&C->lx, here);
    }
    loop_push(C, is_await ? LOOP_ASYNC_ITER : LOOP_ITER, -1);
    substatement(C);
    l    = &fs->loops[fs->nloops - 1];
    cont = label_here(C);
    for (i = 0; i < l->conts.n; i++) patch_to(C, l->conts.v[i], cont);
    close_from(C, first_slot);
    emit_back(C, OP_LOOP, top);
    brk = label_here(C);
    if (is_await) {
        /* break path: await it.return() */
        emit_op(C, OP_ASYNC_CLOSE);
        emit_op(C, OP_AWAIT);
        emit_op(C, OP_POP);
        jend = emit_jump(C, OP_JUMP);
        loop_pop(C, brk);
        /* done path: [it, result] */
        patch_here(C, done);
        adjust(C, 2);
        emit_op(C, OP_POP);
        emit_op(C, OP_POP);
        patch_here(C, jend);
        return;
    }
    emit_op(C, OP_ITER_CLOSE); /* the iterator, on the break path: closed, as the spec says */
    adjust(C, 1);
    loop_pop(C, brk);
    patch_here(C, done);
    adjust(C, -1); /* the iterator is gone on both paths */
}

/* ( [let|const|var] x of|in ...) ahead? */
static int for_in_of_ahead(Compiler *C) {
    LexState st = lex_save(&C->lx);
    int      r;
    if (TOK == T_LBRACE || TOK == T_LBRACKET) skip_balanced(C);
    else {
        int n = 0;
        while (TOK != T_OF && TOK != T_IN && TOK != T_SEMI && TOK != T_ASSIGN && TOK != T_EOF && n < 64) {
            skip_balanced(C);
            n++;
        }
    }
    r = TOK == T_OF || TOK == T_IN;
    lex_restore(&C->lx, st);
    return r;
}

/* Skips a for-loop update expression up to (not past) its ')'. Returns 1
 * if it may create a function, which could capture a loop variable. */
static int skip_for_update(Compiler *C) {
    int tmpl[64], ntmpl = 0, depth = 0, fn = 0;
    for (;;) {
        TokType t = TOK;
        if (t == T_EOF) fail(C, "SyntaxError: unexpected end of input");
        if (t == T_FUNCTION || t == T_ARROW || t == T_CLASS || t == T_LBRACE) fn = 1; /* { : object methods */
        if (t == T_RPAREN && depth == 0 && ntmpl == 0) return fn;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET) depth--;
        else if (t == T_RBRACE) {
            if (ntmpl > 0 && tmpl[ntmpl - 1] == depth) {
                lex_template_continue(&C->lx);
                if (TOK == T_ERROR) unexpected(C);
                if (C->lx.tok.template_tail) ntmpl--;
                next(C);
                continue;
            }
            depth--;
        } else if (t == T_TEMPLATE && !C->lx.tok.template_tail) {
            if (ntmpl >= 64) fail(C, "SyntaxError: templates nested too deeply");
            tmpl[ntmpl++] = depth;
        }
        next(C);
    }
}

/* for (init; test; update) body
 *
 * Laid out as  init; L: test; JUMP_IF_FALSE out; body; [continue:] update;
 * LOOP L  -- one jump per iteration. The update comes after the body in
 * the code but before it in the source, so it is skipped, and compiled
 * after the body by rewinding the lexer. */
static void for_statement(Compiler *C) {
    FuncState *fs = C->fs;
    Block      b;
    int        cond, exit = -1, cont, loop_var_start, i, update_fn, has_update, is_await = 0;
    LexState   update_pos;
    Loop      *l;

    next(C);
    if (TOK == T_AWAIT) {
        if (!in_async(C)) fail(C, "SyntaxError: for await is only valid in async functions and modules");
        is_await = 1;
        next(C);
    }
    expect(C, T_LPAREN);
    scope_begin(C, &b);
    loop_var_start = fs->nactive;
    if (TOK == T_VAR || TOK == T_LET || TOK == T_CONST) {
        int kind = TOK == T_VAR ? K_VAR : TOK == T_LET ? K_LET : K_CONST;
        next(C);
        if (for_in_of_ahead(C)) {
            for_in_of(C, kind, is_await);
            block_end(C, &b);
            return;
        }
        if (kind != K_VAR) {
            /* Declare every name in the head first, then initialise. */
            LexState s2     = lex_save(&C->lx);
            uint32_t roots0 = C->vm->nroots;
            Decls   *d      = decls_new(C, 1);
            if (TOK == T_LBRACE || TOK == T_LBRACKET) scan_pattern(C, d, kind);
            else if (is_name_tok_raw(TOK)) {
                decl_add(C, d, tok_atom(C), kind);
                next(C);
            }
            scan_declarators(C, d, kind);
            for (i = 0; i < d->n; i++) emit_local(C, OP_INIT_HOLE, declare_local(C, DECL_NAME(d, i), kind));
            decls_free(C, d);
            C->vm->nroots = roots0;
            lex_restore(&C->lx, s2);
        }
        C->no_in = 1;
        var_declarations(C, kind);
        C->no_in = 0;
    } else if (TOK != T_SEMI) {
        if (TOK == T_ASYNC && !is_await && peek_is(C, T_OF))
            fail(C, "SyntaxError: for (async of ...) is ambiguous: use for ((async) of ...)");
        if (for_in_of_ahead(C)) {
            for_in_of(C, -1, is_await);
            block_end(C, &b);
            return;
        }
        C->no_in = 1;
        expr(C);
        C->no_in = 0;
        emit_op(C, OP_POP);
    }
    if (is_await) fail(C, "SyntaxError: for await needs `of`");
    expect(C, T_SEMI);
    cond = label_here(C);
    if (TOK != T_SEMI) {
        expr(C);
        exit = emit_jump(C, OP_JUMP_IF_FALSE);
    }
    expect(C, T_SEMI);
    update_pos = lex_save(&C->lx);
    has_update = TOK != T_RPAREN;
    update_fn  = skip_for_update(C);
    expect(C, T_RPAREN);
    l = loop_push(C, LOOP_PLAIN, -1);
    substatement(C);
    l    = &fs->loops[fs->nloops - 1];
    cont = label_here(C);
    for (i = 0; i < l->conts.n; i++) patch_to(C, l->conts.v[i], cont);
    /* Each iteration gets its own copy of the loop variables: closures
     * made in the body keep the value of their iteration. Needed only if
     * something captured one; the update is not compiled yet, so a
     * function there counts as a capture. */
    if (update_fn) {
        if (fs->nactive > loop_var_start) emit_local(C, OP_CLOSE_UPVALS, loop_var_start);
    } else {
        close_from(C, loop_var_start);
    }
    if (has_update) {
        LexState after = lex_save(&C->lx);
        lex_restore(&C->lx, update_pos);
        expr(C);
        emit_op(C, OP_POP);
        if (TOK != T_RPAREN) unexpected(C);
        lex_restore(&C->lx, after);
    }
    emit_back(C, OP_LOOP, cond);
    if (exit >= 0) patch_here(C, exit);
    loop_pop(C, label_here(C));
    block_end(C, &b);
}

static void switch_statement(Compiler *C) {
    FuncState *fs = C->fs;
    Block      scope, b;
    int        tmp, test_fail = -1, fall = -1, default_body = -1, have_body = 0, saved_floor;

    next(C);
    expect(C, T_LPAREN);
    expr(C);
    expect(C, T_RPAREN);
    scope_begin(C, &scope);
    tmp = declare_temp(C);
    emit_local(C, OP_PUT_LOCAL, tmp);
    if (TOK != T_LBRACE) expect(C, T_LBRACE);
    next(C);
    saved_floor = fs->switch_floor1;
    if (!fs->switch_floor1) fs->switch_floor1 = fs->nactive + 1;
    block_begin(C, &b, 0, NULL);
    loop_push(C, LOOP_SWITCH, -1);
    while (TOK != T_RBRACE) {
        if (TOK == T_CASE) {
            next(C);
            if (have_body) fall = emit_jump(C, OP_JUMP);
            if (test_fail >= 0) patch_here(C, test_fail);
            emit_local(C, OP_GET_LOCAL, tmp);
            expr(C);
            emit_op(C, OP_SEQ);
            test_fail = emit_jump(C, OP_JUMP_IF_FALSE);
            if (fall >= 0) {
                patch_here(C, fall);
                fall = -1;
            }
            expect(C, T_COLON);
        } else if (TOK == T_DEFAULT) {
            next(C);
            if (default_body >= 0) fail(C, "SyntaxError: more than one default clause");
            expect(C, T_COLON);
            default_body = label_here(C);
        } else {
            fail(C, "SyntaxError: expected case or default");
        }
        have_body = 1;
        while (TOK != T_CASE && TOK != T_DEFAULT && TOK != T_RBRACE) {
            if (TOK == T_EOF) fail(C, "SyntaxError: expected '}'");
            statement(C);
        }
    }
    next(C);
    {
        int end_jump = emit_jump(C, OP_JUMP);
        if (test_fail >= 0) patch_here(C, test_fail);
        if (default_body >= 0) emit_back(C, OP_JUMP, default_body);
        patch_here(C, end_jump);
    }
    loop_pop(C, label_here(C));
    block_end(C, &b);
    fs->switch_floor1 = saved_floor;
    block_end(C, &scope);
}

static void try_statement(Compiler *C) {
    FuncState *fs = C->fs;
    LexState   start, finally_pos;
    int        has_catch = 0, has_finally = 0, h1, h2 = -1, jnormal, jnormal2 = -1, base_stack, jfilter = -1;
    memset(&finally_pos, 0, sizeof finally_pos);

    next(C);
    if (TOK != T_LBRACE) expect(C, T_LBRACE);
    /* Look ahead for catch and finally: code in the try block that leaves
     * it early (return, break) has to run the finally block first. */
    start = lex_save(&C->lx);
    skip_balanced(C);
    if (TOK == T_CATCH) {
        has_catch = 1;
        next(C);
        if (TOK == T_LPAREN) skip_balanced(C);
        if (TOK != T_LBRACE) expect(C, T_LBRACE);
        skip_balanced(C);
    }
    if (TOK == T_FINALLY) {
        has_finally = 1;
        next(C);
        if (TOK != T_LBRACE) expect(C, T_LBRACE);
        finally_pos = lex_save(&C->lx);
    }
    if (!has_catch && !has_finally) fail(C, "SyntaxError: try without catch or finally");
    lex_restore(&C->lx, start);

    base_stack = fs->stack;
    if (fs->ntry >= MAX_TRYS) fail(C, "SyntaxError: try blocks nested too deeply");
    fs->trys[fs->ntry].has_finally = has_finally;
    fs->trys[fs->ntry].finally_pos = finally_pos;
    fs->ntry++;
    h1 = emit_jump(C, OP_TRY);
    block_statement(C);
    emit_op(C, OP_END_TRY);
    fs->ntry--;
    jnormal = emit_jump(C, OP_JUMP);

    if (has_catch) {
        Block b;
        patch_here(C, h1);
        fs->stack = base_stack + 1; /* the exception */
        if (fs->stack > fs->max_stack) fs->max_stack = fs->stack;
        /* generator.return() is not an exception to catch: it goes to the
         * finally block (patched below), or on out */
        jfilter = emit_jump(C, OP_CATCH_FILTER);
        /* locals of the abandoned try block may be captured */
        emit_local(C, OP_CLOSE_UPVALS, fs->nactive);
        next(C); /* catch */
        scope_begin(C, &b);
        if (TOK == T_LPAREN) {
            next(C);
            if (TOK == T_LBRACE || TOK == T_LBRACKET) {
                LexState st     = lex_save(&C->lx);
                uint32_t roots0 = C->vm->nroots;
                Decls   *d      = decls_new(C, 1);
                int      i;
                scan_pattern(C, d, K_LET);
                for (i = 0; i < d->n; i++) declare_local(C, DECL_NAME(d, i), K_VAR);
                decls_free(C, d);
                C->vm->nroots = roots0;
                lex_restore(&C->lx, st);
                bind_target(C, BIND_DECL, K_VAR);
            } else {
                int slot = declare_local(C, binding_name(C), K_VAR);
                emit_local(C, OP_PUT_LOCAL, slot);
            }
            expect(C, T_RPAREN);
            C->param_floor1 = b.nactive + 1; /* the block may not redeclare them */
        } else {
            emit_op(C, OP_POP);
        }
        if (has_finally) {
            /* After the binding: the handler must restore the stack
             * without the caught exception on it. */
            fs->trys[fs->ntry].has_finally = 1;
            fs->trys[fs->ntry].finally_pos = finally_pos;
            fs->ntry++;
            h2 = emit_jump(C, OP_TRY);
        }
        block_statement(C);
        if (has_finally) {
            emit_op(C, OP_END_TRY);
            fs->ntry--;
        }
        block_end(C, &b);
        if (has_finally) jnormal2 = emit_jump(C, OP_JUMP);
    }
    patch_here(C, jnormal);
    if (jnormal2 >= 0) patch_here(C, jnormal2);
    if (has_finally) {
        int      jend, tmp;
        LexState after;
        fs->stack = base_stack;
        /* normal path */
        if (TOK != T_FINALLY) expect(C, T_FINALLY);
        next(C);
        block_statement(C);
        after = lex_save(&C->lx);
        jend  = emit_jump(C, OP_JUMP);
        /* exceptional path: the exception is on the stack */
        patch_here(C, has_catch ? h2 : h1);
        if (jfilter >= 0) patch_here(C, jfilter);
        fs->stack = base_stack + 1;
        if (fs->stack > fs->max_stack) fs->max_stack = fs->stack;
        emit_local(C, OP_CLOSE_UPVALS, fs->nactive);
        tmp = declare_temp(C);
        emit_local(C, OP_PUT_LOCAL, tmp);
        compile_finally_at(C, finally_pos);
        emit_local(C, OP_GET_LOCAL, tmp);
        emit_op(C, OP_THROW);
        fs->nactive--;
        patch_here(C, jend);
        fs->stack = base_stack;
        lex_restore(&C->lx, after);
    } else {
        fs->stack = base_stack;
    }
}

static void function_declaration(Compiler *C, int flags) {
    FuncState *fs = C->fs;
    PxValue    name;
    int        cidx;
    next(C); /* function */
    if (TOK == T_STAR) {
        next(C);
        flags |= PX_PROTO_GENERATOR;
    }
    if (TOK == T_LPAREN) fail(C, "SyntaxError: function declarations need a name");
    name = binding_name(C);
    PX_ROOT(C->vm, name);
    /* Compile the body now; the closure itself is created by the hoisting
     * code at block start, so take the CLOSURE back out. */
    compile_function(C, name, flags);
    fs->len -= 3;
    fs->nops = 0; /* the CLOSURE taken out was the last instruction noted */
    label_here(C);
    adjust(C, -1);
    cidx = fs->code[fs->len + 1] | (fs->code[fs->len + 2] << 8);
    if (fs->nhoists >= MAX_HOISTS) fail(C, "RangeError: too many function declarations in one block");
    {
        Hoist *h    = &fs->hoists[fs->nhoists++];
        int    slot = find_local(fs, name);
        h->cidx     = (uint16_t)cidx;
        if (slot >= 0) {
            h->is_global = 0;
            h->slot      = slot;
        } else {
            h->is_global = 1;
            h->name_cidx = (uint16_t)add_const(C, name);
        }
    }
    px_pop_roots(C->vm, 1);
}

/* import x from "m" / import * as ns from "m" / import { a, b as c } from
 * "m" / import "m". The bindings were declared (as constants) by the
 * module's pre-scan; here they are initialised from the host resolver's
 * namespace object. */
/* A contextual keyword of module syntax (as, from): never with escapes. */
static int tok_is_kw(Compiler *C, const char *w) { return tok_is_word(C, w) && !C->lx.tok.escaped; }

/* ModuleExportName: an IdentifierName, or a string without lone
 * surrogates. Returns the name as a key; *is_str tells which. */
static PxValue export_name(Compiler *C, int *is_str) {
    PxValue name;
    *is_str = TOK == T_STRING;
    if (TOK == T_STRING) {
        const uint16_t *s = C->lx.tok.str;
        uint32_t        n = C->lx.tok.str_len, i;
        for (i = 0; i < n; i++) {
            if (s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) i++;
            else if (s[i] >= 0xD800 && s[i] <= 0xDFFF) fail(C, "SyntaxError: an export name must be well-formed");
        }
        name = tok_string_as(C, 1);
    } else if (is_prop_name_tok(TOK)) {
        name = tok_atom(C);
    } else {
        unexpected(C);
    }
    next(C);
    return name;
}

/* The module's exported names, for the duplicate check. */
static void add_export(Compiler *C, PxValue name) {
    if (decl_find(C->exports, name) >= 0) fail(C, "SyntaxError: duplicate export");
    decl_add(C, C->exports, name, K_VAR);
}

static void import_declaration(Compiler *C) {
    FuncState *fs = C->fs;
    LexState   clause;
    int        has_clause = 0, tmp;
    PxValue    spec;
    next(C); /* import */
    clause = lex_save(&C->lx);
    if (TOK != T_STRING) {
        has_clause = 1;
        while (TOK != T_EOF && TOK != T_SEMI && !tok_is_kw(C, "from")) skip_balanced(C);
        if (!tok_is_kw(C, "from")) fail(C, "SyntaxError: expected 'from' in import");
        next(C);
    }
    if (TOK != T_STRING) fail(C, "SyntaxError: expected a module specifier string");
    spec = tok_string_as(C, 0);
    emit_op_u16(C, OP_IMPORT, add_const(C, spec));
    next(C);
    if (tok_is_word(C, "with") || TOK == T_WITH) fail(C, "SyntaxError: import attributes are not supported");
    semicolon(C);
    if (!has_clause) {
        emit_op(C, OP_POP);
        return;
    }
    {
        LexState after = lex_save(&C->lx);
        int      first = 1;
        tmp            = declare_temp(C);
        emit_local(C, OP_PUT_LOCAL, tmp);
        lex_restore(&C->lx, clause);
        for (;;) {
            if (TOK == T_STAR) {
                next(C);
                if (!tok_is_kw(C, "as")) fail(C, "SyntaxError: expected 'as'");
                next(C);
                emit_local(C, OP_GET_LOCAL, tmp);
                init_name(C, binding_name(C), K_CONST);
            } else if (TOK == T_LBRACE) {
                next(C);
                while (TOK != T_RBRACE) {
                    PxValue  imported, local;
                    LexState at = lex_save(&C->lx);
                    int      is_str;
                    imported    = export_name(C, &is_str);
                    PX_ROOT(C->vm, imported);
                    if (tok_is_kw(C, "as")) {
                        next(C);
                    } else {
                        /* `{ a }`: the name is the binding too */
                        if (is_str) fail(C, "SyntaxError: expected 'as' after a string import name");
                        lex_restore(&C->lx, at);
                    }
                    local = binding_name(C);
                    PX_ROOT(C->vm, local);
                    emit_local(C, OP_GET_LOCAL, tmp);
                    emit_prop(C, OP_GET_PROP, add_const(C, imported));
                    init_name(C, local, K_CONST);
                    px_pop_roots(C->vm, 2);
                    if (TOK != T_RBRACE) expect(C, T_COMMA);
                }
                next(C);
            } else if (first && TOK != T_LBRACE) {
                /* default import */
                emit_local(C, OP_GET_LOCAL, tmp);
                emit_prop(C, OP_GET_PROP, add_const(C, C->vm->atom[PX_ATOM_default]));
                init_name(C, binding_name(C), K_CONST);
                if (TOK != T_COMMA) break;
                next(C);
                if (TOK != T_STAR && TOK != T_LBRACE) unexpected(C);
                first = 0;
                continue;
            } else {
                unexpected(C);
            }
            break;
        }
        if (!tok_is_kw(C, "from")) unexpected(C);
        fs->nactive--; /* the temporary */
        lex_restore(&C->lx, after);
    }
}

/* export <declaration> / export default <expression> / export { ... }:
 * a PSPX app's entry module has nobody importing it, so exports only
 * declare; `export default` evaluates its expression. */
static void export_declaration(Compiler *C) {
    int is_str;
    next(C); /* export */
    if (TOK == T_DEFAULT) {
        next(C);
        add_export(C, C->vm->atom[PX_ATOM_default]);
        if (TOK == T_FUNCTION || TOK == T_CLASS || (TOK == T_ASYNC && async_ahead(C) == 2)) {
            /* named: a declaration; anonymous: a declaration too (no `;`,
             * no call after it), whose function is named "default" */
            LexState st    = lex_save(&C->lx);
            int      named = 0, flags = 0;
            if (TOK == T_ASYNC) next(C);
            next(C); /* function / class */
            if (TOK == T_STAR) next(C);
            named = TOK != T_LPAREN && TOK != T_LBRACE && TOK != T_EXTENDS;
            lex_restore(&C->lx, st);
            if (named) {
                statement(C);
                return;
            }
            if (TOK == T_CLASS) {
                next(C);
                class_def(C, C->vm->atom[PX_ATOM_default], 0);
            } else {
                if (TOK == T_ASYNC) {
                    flags |= PX_PROTO_ASYNC;
                    next(C);
                }
                next(C); /* function */
                if (TOK == T_STAR) {
                    flags |= PX_PROTO_GENERATOR;
                    next(C);
                }
                compile_function(C, C->vm->atom[PX_ATOM_default], flags);
            }
            emit_op(C, OP_POP);
            return;
        }
        expr_value(C);
        emit_op(C, OP_POP);
        semicolon(C);
        return;
    }
    if (TOK == T_STAR) {
        /* export * [as name] from "m" */
        next(C);
        if (tok_is_kw(C, "as")) {
            next(C);
            add_export(C, export_name(C, &is_str));
        }
        if (!tok_is_kw(C, "from")) unexpected(C);
        next(C);
        if (TOK != T_STRING) unexpected(C);
        next(C);
        semicolon(C);
        return;
    }
    if (TOK == T_LBRACE) {
        /* export { a, b as c } [from "m"]: without `from`, the local names
         * must be declared in the module (checked at its end) */
        LexState list = lex_save(&C->lx);
        int      from = 0;
        skip_balanced(C);
        if (tok_is_kw(C, "from")) {
            from = 1;
            next(C);
            if (TOK != T_STRING) unexpected(C);
            next(C);
        }
        semicolon(C);
        {
            LexState after = lex_save(&C->lx);
            lex_restore(&C->lx, list);
            next(C); /* { */
            while (TOK != T_RBRACE) {
                PxValue local = export_name(C, &is_str);
                if (!from) {
                    if (is_str) fail(C, "SyntaxError: a string cannot name a local export");
                    decl_add(C, C->export_locals, local, K_VAR);
                }
                if (tok_is_kw(C, "as")) {
                    next(C);
                    add_export(C, export_name(C, &is_str));
                } else {
                    add_export(C, local);
                }
                if (TOK != T_RBRACE) expect(C, T_COMMA);
            }
            lex_restore(&C->lx, after);
        }
        return;
    }
    /* export var / let / const / function / class: the names it declares */
    {
        LexState st     = lex_save(&C->lx);
        uint32_t roots0 = C->vm->nroots;
        Decls   *d      = decls_new(C, 0);
        int      i;
        if (TOK == T_VAR || TOK == T_LET || TOK == T_CONST) {
            next(C);
            scan_target(C, d, K_VAR);
            scan_declarators(C, d, K_VAR);
        } else if (TOK == T_FUNCTION || TOK == T_CLASS || (TOK == T_ASYNC && async_ahead(C) == 2)) {
            if (TOK == T_ASYNC) next(C);
            next(C);
            if (TOK == T_STAR) next(C);
            if (is_name_tok_raw(TOK) || TOK == T_IDENT) decl_add(C, d, tok_atom(C), K_VAR);
        } else {
            unexpected(C);
        }
        for (i = 0; i < d->n; i++) add_export(C, DECL_NAME(d, i));
        decls_free(C, d);
        C->vm->nroots = roots0;
        lex_restore(&C->lx, st);
    }
    statement(C);
}

static Loop *find_loop(Compiler *C, PxValue label, int is_break) {
    FuncState *fs = C->fs;
    int        i;
    for (i = fs->nloops - 1; i >= 0; i--) {
        Loop *l = &fs->loops[i];
        if (label) {
            if (l->label == label) {
                if (!is_break && (l->kind == LOOP_SWITCH || l->kind == LOOP_BLOCK))
                    fail(C, "SyntaxError: continue target is not a loop");
                return l;
            }
        } else if (l->kind != LOOP_BLOCK && (is_break || l->kind != LOOP_SWITCH)) {
            return l;
        }
    }
    return NULL;
}

static void statement(Compiler *C) {
    FuncState *fs = C->fs;
    enter(C);
    mark_line(C);
    switch (TOK) {
    case T_LBRACE: block_statement(C); break;
    case T_VAR:
    case T_CONST: {
        int kind = TOK == T_VAR ? K_VAR : K_CONST;
        next(C);
        var_declarations(C, kind);
        semicolon(C);
        break;
    }
    case T_LET: {
        LexState st = lex_save(&C->lx);
        next(C);
        if (TOK == T_LBRACKET || TOK == T_LBRACE || is_name_tok_raw(TOK)) {
            var_declarations(C, K_LET);
            semicolon(C);
            break;
        }
        lex_restore(&C->lx, st);
        fail(C, "SyntaxError: let is reserved");
    }
    case T_FUNCTION: function_declaration(C, 0); break;
    case T_CLASS: {
        PxValue name;
        next(C);
        if (TOK == T_LBRACE || TOK == T_EXTENDS) fail(C, "SyntaxError: class declarations need a name");
        name = binding_name(C);
        PX_ROOT(C->vm, name);
        class_def(C, name, 1);
        init_name(C, name, K_CLASS);
        px_pop_roots(C->vm, 1);
        break;
    }
    case T_IF: if_statement(C); break;
    case T_WHILE: while_statement(C); break;
    case T_DO: do_statement(C); break;
    case T_FOR: for_statement(C); break;
    case T_SWITCH: switch_statement(C); break;
    case T_TRY: try_statement(C); break;
    case T_RETURN:
        if (fs->is_script || fs->is_module || (fs->ctx & CTX_STATIC_BLOCK))
            fail(C, "SyntaxError: return outside a function");
        next(C);
        if (TOK == T_SEMI || TOK == T_RBRACE || TOK == T_EOF || C->lx.tok.nl_before) {
            emit_op(C, OP_UNDEF);
        } else {
            expr(C);
            if (in_async_generator(C)) emit_op(C, OP_AWAIT); /* `return x` awaits x there */
        }
        /* finally blocks and open iterators between here and the
         * function are dealt with first */
        {
            int closed = unwind_to(C, -1, fs->nactive, 1);
            emit_op(C, OP_RETURN);
            adjust(C, closed);
        }
        semicolon(C);
        break;
    case T_BREAK:
    case T_CONTINUE: {
        int     is_break = TOK == T_BREAK;
        PxValue label    = 0;
        Loop   *l;
        next(C);
        if (is_name_tok_raw(TOK) && !C->lx.tok.nl_before) {
            label = tok_atom(C);
            next(C);
        }
        l = find_loop(C, label, is_break);
        if (!l) fail(C, "SyntaxError: %s outside a loop", is_break ? "break" : "continue");
        {
            int closed = unwind_to(C, (int)(l - fs->loops), l->nactive, 0);
            if (is_break) {
                list_add(C, &l->breaks, emit_jump(C, OP_JUMP));
            } else if (l->cont_target >= 0) {
                emit_back(C, OP_LOOP, l->cont_target);
            } else {
                list_add(C, &l->conts, emit_jump(C, OP_JUMP));
            }
            adjust(C, closed);
        }
        semicolon(C);
        break;
    }
    case T_THROW:
        next(C);
        if (C->lx.tok.nl_before) fail(C, "SyntaxError: no line break is allowed after throw");
        expr(C);
        emit_op(C, OP_THROW);
        semicolon(C);
        break;
    case T_SEMI: next(C); break;
    case T_DEBUGGER:
        next(C);
        semicolon(C);
        break;
    case T_IMPORT:
    case T_EXPORT:
        /* module items: px_compile reads them at the module's top level */
        fail(C, "SyntaxError: %s is only valid at the top level of a module", tok_name(TOK));
    case T_WITH: fail(C, "SyntaxError: with is not allowed");
    default:
        if (TOK == T_ASYNC && async_ahead(C) == 2) {
            next(C);
            function_declaration(C, PX_PROTO_ASYNC);
            break;
        }
        if (is_name(C) && peek_is(C, T_COLON)) {
            /* label: statement */
            PxValue label = tok_atom(C);
            int     i;
            for (i = 0; i < fs->nloops; i++)
                if (fs->loops[i].label == label) fail(C, "SyntaxError: duplicate label");
            next(C);
            next(C);
            if (TOK == T_FOR || TOK == T_WHILE || TOK == T_DO) {
                fs->pending_label = label;
                statement(C);
            } else {
                Loop *l = loop_push(C, LOOP_BLOCK, -1);
                l->label = label;
                substatement(C);
                loop_pop(C, (int)fs->len);
            }
            break;
        }
        expr(C);
        if (fs->is_script && fs->nloops == 0) emit_local(C, OP_PUT_LOCAL, fs->completion);
        else emit_op(C, OP_POP);
        semicolon(C);
        break;
    }
    leave(C);
}

/* ------------------------------------------------------------ entry */

void px_compiler_mark(PxVM *vm) {
    Compiler  *C = (Compiler *)vm->compiler;
    FuncState *fs;
    int        i;
    if (!C) return;
    px_mark_value(vm, C->filename);
    px_mark_value(vm, C->name_hint);
    px_mark_value(vm, C->atom_arguments);
    px_mark_value(vm, C->atom_eval);
    for (fs = C->fs; fs; fs = fs->parent) {
        px_mark_ptr(vm, fs->consts);
        px_mark_value(vm, fs->name);
        px_mark_value(vm, fs->pending_label);
        for (i = 0; i < fs->nactive; i++) px_mark_value(vm, fs->locals[i].name);
        for (i = 0; i < fs->nupvals; i++) px_mark_value(vm, fs->upvals[i].name);
        for (i = 0; i < fs->nloops; i++) px_mark_value(vm, fs->loops[i].label);
    }
}

PxProto *px_compile(PxVM *vm, const char *src, size_t len, const char *filename, int module) {
    Compiler *C;
    PxProto  *p = NULL;
    Block     b;

    if (len > 0x7FFFFFFFu) {
        px_throw_error(vm, PX_RANGE_ERROR, "source too large");
        return NULL;
    }
    C = (Compiler *)calloc(1, sizeof(Compiler));
    if (!C) {
        px_throw_oom(vm);
        return NULL;
    }
    C->vm        = vm;
    C->nroots    = vm->nroots;
    C->name_hint = PX_UNDEFINED;
    C->filename  = px_str_from_cstr(vm, filename ? filename : "<eval>");
    if (C->filename == PX_EXCEPTION) {
        free(C);
        return NULL;
    }
    vm->compiler = C;
    lex_init(&C->lx, src, (uint32_t)len);
    C->is_module = module;
    if (setjmp(C->jb) == 0) {
        FuncState *fs;
        C->atom_arguments = px_intern_cstr(vm, "arguments");
        check_alloc(C, C->atom_arguments != PX_EXCEPTION);
        C->atom_eval = px_intern_cstr(vm, "eval");
        check_alloc(C, C->atom_eval != PX_EXCEPTION);
        fs            = funcstate_new(C, PX_UNDEFINED, PX_PROTO_SCRIPT | (module ? PX_PROTO_ASYNC : 0));
        fs->is_script = !module;
        fs->is_module = module;
        if (!module) fs->completion = declare_temp(C);
        next(C);
        if (module) {
            C->exports       = decls_new(C, 0);
            C->export_locals = decls_new(C, 0);
        }
        block_begin(C, &b, 1, NULL);
        fs->var_floor = fs->nactive;
        while (TOK != T_EOF) {
            if (module && TOK == T_EXPORT) export_declaration(C);
            else if (module && TOK == T_IMPORT && !peek_is(C, T_LPAREN) && !peek_is(C, T_DOT))
                import_declaration(C);
            else statement(C);
        }
        if (module) {
            /* export { x }: x must be declared in the module */
            int i;
            for (i = 0; i < C->export_locals->n; i++)
                if (find_local(fs, DECL_NAME(C->export_locals, i)) < 0)
                    fail(C, "SyntaxError: export of a name the module does not declare");
            decls_free(C, C->exports);
            decls_free(C, C->export_locals);
            vm->nroots = C->nroots; /* their roots, the only ones left */
        }
        block_end(C, &b);
        if (module) emit_op(C, OP_RETURN_UNDEF);
        else {
            emit_local(C, OP_GET_LOCAL, fs->completion);
            emit_op(C, OP_RETURN);
        }
        p = funcstate_finish(C);
    } else {
        while (C->fs) {
            FuncState *parent = C->fs->parent;
            funcstate_free(C->fs);
            C->fs = parent;
        }
        while (C->nowned > 0) free(C->owned[--C->nowned]);
        p = NULL;
    }
    lex_free(&C->lx);
    free(C->fn_ends.e);
    free(C->group_ends.e);
    vm->nroots   = C->nroots;
    vm->compiler = NULL;
    free(C);
    return p;
}
