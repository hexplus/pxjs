/* See px_lexer.h. Malformed input produces a T_ERROR token with a message;
 * the lexer never reads past the end of the source. */

#include "px_lexer.h"
#include "px_dtoa.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *word;
    TokType     type;
} k_keywords[] = {
    {"break", T_BREAK},       {"case", T_CASE},         {"catch", T_CATCH},     {"class", T_CLASS},
    {"const", T_CONST},       {"continue", T_CONTINUE}, {"debugger", T_DEBUGGER}, {"default", T_DEFAULT},
    {"delete", T_DELETE},     {"do", T_DO},             {"else", T_ELSE},       {"export", T_EXPORT},
    {"extends", T_EXTENDS},   {"false", T_FALSE},       {"finally", T_FINALLY}, {"for", T_FOR},
    {"function", T_FUNCTION}, {"if", T_IF},             {"import", T_IMPORT},   {"in", T_IN},
    {"instanceof", T_INSTANCEOF}, {"let", T_LET},       {"new", T_NEW},         {"null", T_NULL},
    {"of", T_OF},             {"return", T_RETURN},     {"super", T_SUPER},     {"switch", T_SWITCH},
    {"this", T_THIS},         {"throw", T_THROW},       {"true", T_TRUE},       {"try", T_TRY},
    {"typeof", T_TYPEOF},     {"var", T_VAR},           {"void", T_VOID},       {"while", T_WHILE},
    {"with", T_WITH},         {"yield", T_YIELD},       {"async", T_ASYNC},     {"await", T_AWAIT},
    {"static", T_STATIC},     {"get", T_GET},           {"set", T_SET},
};

void lex_init(Lexer *lx, const char *src, uint32_t len) {
    memset(lx, 0, sizeof *lx);
    lx->src  = src;
    lx->len  = len;
    lx->line = 1;
    /* A leading "#!" line is a comment (scripts run as executables). */
    if (len >= 2 && src[0] == '#' && src[1] == '!')
        while (lx->pos < len && src[lx->pos] != '\n') lx->pos++;
}

void lex_free(Lexer *lx) {
    free(lx->ibuf);
    free(lx->sbuf);
    lx->ibuf = NULL;
    lx->sbuf = NULL;
}

LexState lex_save(const Lexer *lx) {
    LexState st;
    st.pos  = lx->tok.ws_start;
    st.line = lx->tok.ws_line;
    st.prev = lx->tok.prev_type;
    return st;
}

void lex_restore(Lexer *lx, LexState st) {
    lx->pos       = st.pos;
    lx->line      = st.line;
    lx->last_type = st.prev;
    lex_next(lx);
}

/* After these a '/' divides; anywhere else it starts a regular
 * expression literal. Contextual words (of, get, async...) count as
 * names. '}' is taken as the end of a block, where a statement -- which
 * may be a regular expression -- can follow. */
static int regex_allowed(int prev) {
    switch (prev) {
    case T_IDENT: case T_NUMBER: case T_STRING: case T_TEMPLATE: case T_REGEXP:
    case T_RPAREN: case T_RBRACKET: case T_THIS: case T_TRUE: case T_FALSE: case T_NULL:
    case T_SUPER: case T_INC: case T_DEC: case T_OF: case T_GET: case T_SET: case T_STATIC:
    case T_ASYNC: case T_AWAIT: case T_YIELD: case T_LET:
        return 0;
    default: return 1;
    }
}

static int error(Lexer *lx, const char *msg) {
    lx->tok.type  = T_ERROR;
    lx->tok.error = msg;
    return 0;
}

static int ibuf_put(Lexer *lx, uint32_t *n, char c) {
    if (*n + 2 > lx->ibuf_cap) {
        uint32_t cap = lx->ibuf_cap ? lx->ibuf_cap * 2 : 64;
        char    *nb  = (char *)realloc(lx->ibuf, cap);
        if (!nb) return error(lx, "out of memory");
        lx->ibuf     = nb;
        lx->ibuf_cap = cap;
    }
    lx->ibuf[(*n)++] = c;
    lx->ibuf[*n]     = '\0';
    return 1;
}

static int sbuf_put(Lexer *lx, uint32_t *n, uint16_t c) {
    if (*n + 1 > lx->sbuf_cap) {
        uint32_t  cap = lx->sbuf_cap ? lx->sbuf_cap * 2 : 128;
        uint16_t *nb  = (uint16_t *)realloc(lx->sbuf, cap * sizeof(uint16_t));
        if (!nb) return error(lx, "out of memory");
        lx->sbuf     = nb;
        lx->sbuf_cap = cap;
    }
    lx->sbuf[(*n)++] = c;
    return 1;
}

static int sbuf_put_cp(Lexer *lx, uint32_t *n, uint32_t cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        return sbuf_put(lx, n, (uint16_t)(0xD800 + (cp >> 10))) && sbuf_put(lx, n, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    }
    return sbuf_put(lx, n, (uint16_t)cp);
}

/* One UTF-8 code point at lx->pos; malformed bytes read as U+FFFD. */
static uint32_t read_cp(Lexer *lx, uint32_t *size) {
    const uint8_t *s = (const uint8_t *)lx->src + lx->pos;
    uint32_t       left = lx->len - lx->pos, c = s[0], n, i, min;
    if (c < 0x80) {
        *size = 1;
        return c;
    }
    if (c >= 0xC2 && c <= 0xDF) n = 2, c &= 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, c &= 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, c &= 0x07, min = 0x10000;
    else {
        *size = 1;
        return 0xFFFD;
    }
    if (left < n) {
        *size = 1;
        return 0xFFFD;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *size = 1;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *size = 1;
        return 0xFFFD;
    }
    *size = n;
    return c;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* \uXXXX or \u{X...}; pos is just past the 'u'. */
static int read_unicode_escape(Lexer *lx, uint32_t *cp) {
    uint32_t v = 0, i;
    if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
        lx->pos++;
        for (i = 0; lx->pos < lx->len && lx->src[lx->pos] != '}'; i++, lx->pos++) {
            int h = hexval((unsigned char)lx->src[lx->pos]);
            if (h < 0 || i >= 6) return error(lx, "invalid \\u{...} escape");
            v = v * 16 + (uint32_t)h;
        }
        if (lx->pos >= lx->len || i == 0 || v > 0x10FFFF) return error(lx, "invalid \\u{...} escape");
        lx->pos++;
    } else {
        for (i = 0; i < 4; i++, lx->pos++) {
            int h = lx->pos < lx->len ? hexval((unsigned char)lx->src[lx->pos]) : -1;
            if (h < 0) return error(lx, "invalid \\u escape");
            v = v * 16 + (uint32_t)h;
        }
    }
    *cp = v;
    return 1;
}

/* px_unicode.c (see tools/gen_unicode.py): the General_Category of every
 * code point, as runs (start delta as a variable-length number, category
 * index); px_ucd_sizes[0] is its length in bytes. */
extern const uint8_t  px_ucd_gc[];
extern const uint32_t px_ucd_sizes[];

static int general_category(uint32_t cp) {
    const uint8_t *p = px_ucd_gc, *end = p + px_ucd_sizes[0];
    uint32_t       start = 0;
    int            cat   = 2; /* Cn */
    while (p < end) {
        uint32_t v = *p++;
        if (v >= 0xC0) {
            v = ((v & 0x3F) << 16) | ((uint32_t)p[0] << 8) | p[1];
            p += 2;
        } else if (v >= 0x80) {
            v = ((v & 0x3F) << 8) | p[0];
            p++;
        }
        start += v;
        if (start > cp) break;
        cat = *p++;
    }
    return cat;
}

/* The categories (indices of px_unicode.c's list) of ID_Start: Lu Ll Lt Lm
 * Lo Nl; ID_Continue adds Mn Mc Nd Pc. Identifiers are rare outside ASCII,
 * so the run table is simply scanned. */
#define GC_BIT(i)     (1u << (i))
#define ID_START_CATS (GC_BIT(5) | GC_BIT(6) | GC_BIT(7) | GC_BIT(8) | GC_BIT(9) | GC_BIT(14))
#define ID_PART_CATS  (ID_START_CATS | GC_BIT(10) | GC_BIT(12) | GC_BIT(13) | GC_BIT(16))

static int is_id_start(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    if (c == 0x2E2F) return 0; /* VERTICAL TILDE: Lm, but Pattern_Syntax */
    if (c == 0x1885 || c == 0x1886 || c == 0x2118 || c == 0x212E || c == 0x309B || c == 0x309C)
        return 1; /* Other_ID_Start */
    return (ID_START_CATS >> general_category(c)) & 1;
}

static int is_id_part(uint32_t c) {
    if (c < 0x80) return is_id_start(c) || (c >= '0' && c <= '9');
    if (c == 0x200C || c == 0x200D || c == 0xB7 || c == 0x387 || (c >= 0x1369 && c <= 0x1371) || c == 0x19DA ||
        c == 0x30FB || c == 0xFF65)
        return 1; /* ZWNJ, ZWJ, Other_ID_Continue */
    if (c == 0x2E2F) return 0;
    return is_id_start(c) || ((ID_PART_CATS >> general_category(c)) & 1);
}

/* Does an identifier (or an escape, which can only be one) start at p? */
static int id_starts_at(Lexer *lx, uint32_t p) {
    unsigned char c = (unsigned char)lx->src[p];
    uint32_t      save = lx->pos, size, cp;
    if (c < 0x80) return is_id_start(c) || c == '\\';
    lx->pos = p;
    cp      = read_cp(lx, &size);
    lx->pos = save;
    return is_id_start(cp);
}

static int put_utf8(Lexer *lx, uint32_t *n, uint32_t c) {
    if (c < 0x80) return ibuf_put(lx, n, (char)c);
    if (c < 0x800) return ibuf_put(lx, n, (char)(0xC0 | (c >> 6))) && ibuf_put(lx, n, (char)(0x80 | (c & 0x3F)));
    if (c < 0x10000)
        return ibuf_put(lx, n, (char)(0xE0 | (c >> 12))) && ibuf_put(lx, n, (char)(0x80 | ((c >> 6) & 0x3F))) &&
               ibuf_put(lx, n, (char)(0x80 | (c & 0x3F)));
    return ibuf_put(lx, n, (char)(0xF0 | (c >> 18))) && ibuf_put(lx, n, (char)(0x80 | ((c >> 12) & 0x3F))) &&
           ibuf_put(lx, n, (char)(0x80 | ((c >> 6) & 0x3F))) && ibuf_put(lx, n, (char)(0x80 | (c & 0x3F)));
}

/* Keywords by length and first letter: most identifiers are compared
 * with none or one. Built on first use. */
static int8_t k_kw[11][26][4]; /* k_keywords indices, -1 after the last */

static void kw_index(void) {
    int i;
    memset(k_kw, -1, sizeof k_kw);
    for (i = 0; i < (int)(sizeof k_keywords / sizeof k_keywords[0]); i++) {
        size_t  n = strlen(k_keywords[i].word);
        int8_t *b = k_kw[n][k_keywords[i].word[0] - 'a'];
        int     k = 0;
        while (b[k] >= 0) k++;
        b[k] = (int8_t)i;
    }
}

TokType lex_keyword_type(const char *s, uint32_t n) {
    static int    ready;
    int           c = (unsigned char)s[0], k;
    const int8_t *b;
    if (n < 2 || n > 10 || c < 'a' || c > 'z') return T_IDENT;
    if (!ready) {
        kw_index();
        ready = 1;
    }
    b = k_kw[n][c - 'a'];
    for (k = 0; k < 4 && b[k] >= 0; k++)
        if (memcmp(k_keywords[b[k]].word, s, n) == 0) return k_keywords[b[k]].type;
    return T_IDENT;
}

static void keyword(Lexer *lx, uint32_t n) { lx->tok.type = lex_keyword_type(lx->ibuf, n); }

static int ascii_id_part(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '_';
}

static void lex_ident(Lexer *lx) {
    uint32_t n = 0, size, start = lx->pos, end = lx->pos;
    int      escaped = 0;
    lx->tok.escaped = 0;
    /* the common case: plain ASCII, copied in one go */
    while (end < lx->len && ascii_id_part((unsigned char)lx->src[end])) end++;
    if (end > start && !(end < lx->len && ((unsigned char)lx->src[end] >= 0x80 || lx->src[end] == '\\'))) {
        n = end - start;
        if (n + 2 > lx->ibuf_cap) {
            uint32_t cap = lx->ibuf_cap ? lx->ibuf_cap : 64;
            char    *nb;
            while (cap < n + 2) cap *= 2;
            nb = (char *)realloc(lx->ibuf, cap);
            if (!nb) {
                error(lx, "out of memory");
                return;
            }
            lx->ibuf     = nb;
            lx->ibuf_cap = cap;
        }
        memcpy(lx->ibuf, lx->src + start, n);
        lx->ibuf[n]       = 0;
        lx->pos           = end;
        lx->tok.type      = T_IDENT;
        lx->tok.ident     = lx->ibuf;
        lx->tok.ident_len = n;
        keyword(lx, n);
        return;
    }
    for (;;) {
        uint32_t c;
        if (lx->pos >= lx->len) break;
        if (lx->src[lx->pos] == '\\') {
            if (lx->pos + 1 >= lx->len || lx->src[lx->pos + 1] != 'u') {
                error(lx, "invalid escape in identifier");
                return;
            }
            lx->pos += 2;
            if (!read_unicode_escape(lx, &c)) return;
            if (!(n == 0 ? is_id_start(c) : is_id_part(c))) {
                error(lx, "invalid escape in identifier");
                return;
            }
            escaped = 1;
        } else {
            c = read_cp(lx, &size);
            if (!(n == 0 ? is_id_start(c) : is_id_part(c))) break;
            lx->pos += size;
        }
        if (!put_utf8(lx, &n, c)) return;
    }
    if (n == 0) {
        error(lx, "unexpected character");
        lx->pos++;
        return;
    }
    lx->tok.type      = T_IDENT;
    lx->tok.ident     = lx->ibuf;
    lx->tok.ident_len = n;
    lx->tok.escaped   = escaped;
    if (!escaped) keyword(lx, n);
}

/* Digits of `radix` from p, where a '_' may only stand between two digits.
 * Decimal digits are copied into buf (for strtod); other radixes are
 * accumulated into *v. Returns the position after them, or 0 for a
 * misplaced separator; *count is the number of digits. */
static uint32_t scan_digits(Lexer *lx, uint32_t p, int radix, char *buf, uint32_t *n, uint32_t cap, double *v,
                            int *count) {
    const char *s = lx->src;
    *count        = 0;
    for (; p < lx->len; p++) {
        int h;
        if (s[p] == '_') {
            if (*count == 0 || p + 1 >= lx->len || hexval((unsigned char)s[p + 1]) < 0 ||
                hexval((unsigned char)s[p + 1]) >= radix)
                return 0;
            continue;
        }
        h = hexval((unsigned char)s[p]);
        if (h < 0 || h >= radix) break;
        if (buf) {
            if (*n + 1 < cap) buf[(*n)++] = s[p];
        } else {
            *v = *v * radix + h;
        }
        (*count)++;
    }
    return p;
}

static void lex_number(Lexer *lx) {
    const char *s = lx->src;
    uint32_t    p = lx->pos;
    char        buf[400];
    uint32_t    n = 0;
    int         count;
    double      v = 0;

    if (s[p] == '0' && p + 1 < lx->len && strchr("xXoObB", s[p + 1]) && s[p + 1]) {
        int radix = (s[p + 1] | 0x20) == 'x' ? 16 : (s[p + 1] | 0x20) == 'o' ? 8 : 2;
        p         = scan_digits(lx, p + 2, radix, NULL, NULL, 0, &v, &count);
        if (!p || !count) {
            error(lx, p ? "missing digits in number" : "misplaced numeric separator");
            return;
        }
        /* from the source text: correctly rounded past 2^53, where v is not */
        lx->tok.num = px_radix2_to_double(s + lx->pos + 2, 0, p - lx->pos - 2, radix == 16 ? 4 : radix == 8 ? 3 : 1);
    } else {
        if (s[p] == '0' && p + 1 < lx->len && ((s[p + 1] >= '0' && s[p + 1] <= '9') || s[p + 1] == '_')) {
            error(lx, "legacy octal literals are not allowed");
            return;
        }
        if (s[p] != '.') p = scan_digits(lx, p, 10, buf, &n, sizeof buf - 8, &v, &count);
        if (p && p < lx->len && s[p] == '.') {
            buf[n++] = '.';
            p        = scan_digits(lx, p + 1, 10, buf, &n, sizeof buf - 8, &v, &count);
        }
        if (p && p < lx->len && (s[p] == 'e' || s[p] == 'E')) {
            uint32_t q = p + 1;
            if (q < lx->len && (s[q] == '+' || s[q] == '-')) q++;
            if (q < lx->len && s[q] >= '0' && s[q] <= '9') {
                buf[n++] = 'e';
                if (s[p + 1] == '-') buf[n++] = '-';
                p = scan_digits(lx, q, 10, buf, &n, sizeof buf - 1, &v, &count);
            }
        }
        if (!p) {
            error(lx, "misplaced numeric separator");
            return;
        }
        /* from the source text (buf keeps only the first digits) */
        lx->tok.num = px_decimal_to_double(s + lx->pos, 0, p - lx->pos);
    }
    if (p < lx->len && s[p] == 'n') {
        error(lx, "BigInt literals are not supported");
        return;
    }
    if (p < lx->len && (id_starts_at(lx, p) || (s[p] >= '0' && s[p] <= '9'))) {
        error(lx, "identifier starts immediately after a number");
        return;
    }
    lx->pos      = p;
    lx->tok.type = T_NUMBER;
}

/* Escapes shared by strings and templates. pos is at the backslash. */
static int lex_escape(Lexer *lx, uint32_t *n) {
    uint32_t cp;
    char     c;
    lx->pos++;
    if (lx->pos >= lx->len) return error(lx, "unterminated string");
    c = lx->src[lx->pos++];
    switch (c) {
    case 'n': return sbuf_put(lx, n, '\n');
    case 't': return sbuf_put(lx, n, '\t');
    case 'r': return sbuf_put(lx, n, '\r');
    case 'b': return sbuf_put(lx, n, '\b');
    case 'f': return sbuf_put(lx, n, '\f');
    case 'v': return sbuf_put(lx, n, '\v');
    case '0':
        if (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9')
            return error(lx, "legacy octal escapes are not allowed");
        return sbuf_put(lx, n, 0);
    case 'x': {
        int h1 = lx->pos < lx->len ? hexval((unsigned char)lx->src[lx->pos]) : -1;
        int h2 = lx->pos + 1 < lx->len ? hexval((unsigned char)lx->src[lx->pos + 1]) : -1;
        if (h1 < 0 || h2 < 0) return error(lx, "invalid \\x escape");
        lx->pos += 2;
        return sbuf_put(lx, n, (uint16_t)(h1 * 16 + h2));
    }
    case 'u':
        if (!read_unicode_escape(lx, &cp)) return 0;
        return sbuf_put_cp(lx, n, cp);
    case '\r':
        if (lx->pos < lx->len && lx->src[lx->pos] == '\n') lx->pos++;
        lx->line++;
        return 1; /* line continuation */
    case '\n': lx->line++; return 1;
    default:
        if (c >= '1' && c <= '9') return error(lx, "legacy octal escapes are not allowed");
        lx->pos--;
        {
            uint32_t size;
            cp = read_cp(lx, &size);
            lx->pos += size;
            return sbuf_put_cp(lx, n, cp);
        }
    }
}

static void lex_string(Lexer *lx, char quote) {
    uint32_t n = 0, size;
    lx->pos++;
    for (;;) {
        char c;
        if (lx->pos >= lx->len) {
            error(lx, "unterminated string");
            return;
        }
        c = lx->src[lx->pos];
        if (c == quote) {
            lx->pos++;
            break;
        }
        if (c == '\n' || c == '\r') {
            error(lx, "unterminated string");
            return;
        }
        if (c == '\\') {
            if (!lex_escape(lx, &n)) return;
            continue;
        }
        {
            uint32_t cp = read_cp(lx, &size);
            lx->pos += size;
            if (!sbuf_put_cp(lx, &n, cp)) return;
        }
    }
    lx->tok.type    = T_STRING;
    lx->tok.str     = lx->sbuf;
    lx->tok.str_len = n;
}

/* A template chunk from lx->pos (just after ` or }) to ` or ${. */
static void lex_template_chunk(Lexer *lx) {
    uint32_t n = 0, size;
    for (;;) {
        char c;
        if (lx->pos >= lx->len) {
            error(lx, "unterminated template literal");
            return;
        }
        c = lx->src[lx->pos];
        if (c == '`') {
            lx->pos++;
            lx->tok.template_tail = 1;
            break;
        }
        if (c == '$' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '{') {
            lx->pos += 2;
            lx->tok.template_tail = 0;
            break;
        }
        if (c == '\\') {
            if (!lex_escape(lx, &n)) return;
            continue;
        }
        if (c == '\r') {
            /* CRLF and CR in templates read as LF. */
            lx->pos++;
            if (lx->pos < lx->len && lx->src[lx->pos] == '\n') lx->pos++;
            lx->line++;
            if (!sbuf_put(lx, &n, '\n')) return;
            continue;
        }
        if (c == '\n') lx->line++;
        {
            uint32_t cp = read_cp(lx, &size);
            lx->pos += size;
            if (!sbuf_put_cp(lx, &n, cp)) return;
        }
    }
    lx->tok.type    = T_TEMPLATE;
    lx->tok.str     = lx->sbuf;
    lx->tok.str_len = n;
}

void lex_template_continue(Lexer *lx) {
    /* The current token is the `}`; its text ends at pos. */
    lx->pos           = lx->tok.start + 1;
    lx->tok.nl_before = 0;
    lx->tok.start     = lx->pos;
    lx->tok.line      = lx->line;
    lex_template_chunk(lx);
    lx->tok.len   = lx->pos - lx->tok.start;
    lx->last_type = lx->tok.type;
}

void lex_regex(Lexer *lx) {
    uint32_t n = 0, fn = 0, size;
    int      in_class = 0;
    lx->pos = lx->tok.start + 1;
    for (;;) {
        char c;
        if (lx->pos >= lx->len || lx->src[lx->pos] == '\n' || lx->src[lx->pos] == '\r') {
            error(lx, "unterminated regular expression");
            return;
        }
        c = lx->src[lx->pos];
        if (c == '/' && !in_class) {
            lx->pos++;
            break;
        }
        if (c == '[') in_class = 1;
        else if (c == ']') in_class = 0;
        if (c == '\\') {
            /* keep escapes as written: the regex compiler reads them */
            if (!sbuf_put(lx, &n, '\\')) return;
            lx->pos++;
            if (lx->pos >= lx->len) {
                error(lx, "unterminated regular expression");
                return;
            }
        }
        {
            uint32_t cp = read_cp(lx, &size);
            lx->pos += size;
            if (!sbuf_put_cp(lx, &n, cp)) return;
        }
    }
    /* The flags are IdentifierPart characters (the compiler checks which). */
    while (lx->pos < lx->len) {
        uint32_t cp;
        if (lx->src[lx->pos] == '\\') {
            error(lx, "invalid regular expression flags");
            return;
        }
        cp = read_cp(lx, &size);
        if (!is_id_part(cp)) break;
        lx->pos += size;
        if (!put_utf8(lx, &fn, cp)) return;
    }
    if (fn == 0 && !ibuf_put(lx, &fn, 0)) return;
    lx->tok.type      = T_REGEXP;
    lx->tok.str       = lx->sbuf;
    lx->tok.str_len   = n;
    lx->tok.ident     = lx->ibuf;
    lx->tok.ident_len = fn == 1 && lx->ibuf[0] == 0 ? 0 : fn;
    lx->tok.len       = lx->pos - lx->tok.start;
    lx->last_type     = T_REGEXP;
}

/* Whitespace and comments; notes line breaks for ASI. */
static int skip_space(Lexer *lx) {
    int nl = 0;
    while (lx->pos < lx->len) {
        unsigned char c = (unsigned char)lx->src[lx->pos];
        if (c == '\n' || c == '\r') {
            nl = 1;
            if (!(c == '\r' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '\n')) lx->line++;
            lx->pos++;
        } else if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            lx->pos++;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            /* up to a line terminator: \n, \r, U+2028, U+2029 (E2 80 A8/A9) */
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n' && lx->src[lx->pos] != '\r' &&
                   !((unsigned char)lx->src[lx->pos] == 0xE2 && lx->pos + 2 < lx->len &&
                     (unsigned char)lx->src[lx->pos + 1] == 0x80 &&
                     ((unsigned char)lx->src[lx->pos + 2] & 0xFE) == 0xA8))
                lx->pos++;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            for (;;) {
                unsigned char d;
                if (lx->pos + 1 >= lx->len) {
                    lx->pos = lx->len;
                    return -1;
                }
                d = (unsigned char)lx->src[lx->pos];
                if (d == '*' && lx->src[lx->pos + 1] == '/') {
                    lx->pos += 2;
                    break;
                }
                if (d == '\n' || d == '\r' ||
                    (d == 0xE2 && lx->pos + 2 < lx->len && (unsigned char)lx->src[lx->pos + 1] == 0x80 &&
                     ((unsigned char)lx->src[lx->pos + 2] & 0xFE) == 0xA8)) {
                    nl = 1;
                    if (d == '\n') lx->line++;
                }
                lx->pos++;
            }
        } else if (c >= 0x80) {
            /* Unicode spaces and line terminators. */
            uint32_t size, cp = read_cp(lx, &size);
            if (cp == 0x2028 || cp == 0x2029) {
                nl = 1;
                lx->line++;
            } else if (!(cp == 0xA0 || cp == 0xFEFF || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
                         cp == 0x202F || cp == 0x205F || cp == 0x3000)) {
                break;
            }
            lx->pos += size;
        } else {
            break;
        }
    }
    return nl;
}

#define P1(c, t) case c: lx->tok.type = t; lx->pos += 1; break

void lex_next(Lexer *lx) {
    const char *s;
    char        c, c1, c2, c3;
    int         nl;

    lx->tok.prev_type = lx->last_type;
    lx->tok.ws_start  = lx->pos;
    lx->tok.ws_line  = lx->line;
    lx->tok.escaped  = 0;
    nl               = skip_space(lx);
    lx->tok.nl_before = nl != 0;
    lx->tok.start     = lx->pos;
    lx->tok.line      = lx->line;
    lx->tok.error     = NULL;
    if (nl < 0) {
        error(lx, "unterminated comment");
        return;
    }
    if (lx->pos >= lx->len) {
        lx->tok.type = T_EOF;
        lx->tok.len  = 0;
        return;
    }
    s  = lx->src + lx->pos;
    c  = s[0];
    c1 = lx->pos + 1 < lx->len ? s[1] : 0;
    c2 = lx->pos + 2 < lx->len ? s[2] : 0;
    c3 = lx->pos + 3 < lx->len ? s[3] : 0;

    if ((c >= '0' && c <= '9') || (c == '.' && c1 >= '0' && c1 <= '9')) {
        lex_number(lx);
    } else if (c == '"' || c == '\'') {
        lex_string(lx, c);
    } else if (c == '`') {
        lx->pos++;
        lex_template_chunk(lx);
    } else if (is_id_start((unsigned char)c) || (unsigned char)c >= 0x80 || c == '\\') {
        lex_ident(lx);
    } else {
        switch (c) {
            P1('{', T_LBRACE);
            P1('}', T_RBRACE);
            P1('(', T_LPAREN);
            P1(')', T_RPAREN);
            P1('[', T_LBRACKET);
            P1(']', T_RBRACKET);
            P1(';', T_SEMI);
            P1(',', T_COMMA);
            P1(':', T_COLON);
            P1('~', T_TILDE);
        case '#':
            if (lx->pos + 1 < lx->len && id_starts_at(lx, lx->pos + 1)) {
                lx->pos++;
                lex_ident(lx);
                if (lx->tok.type == T_ERROR) break;
                memmove(lx->ibuf + 1, lx->ibuf, lx->tok.ident_len + 1);
                lx->ibuf[0]       = '#';
                lx->tok.ident     = lx->ibuf;
                lx->tok.ident_len++;
                lx->tok.type = T_PRIVATE;
            } else {
                lx->tok.type = T_HASH;
                lx->pos++;
            }
            break;
        case '.':
            if (c1 == '.' && c2 == '.') lx->tok.type = T_ELLIPSIS, lx->pos += 3;
            else lx->tok.type = T_DOT, lx->pos += 1;
            break;
        case '?':
            if (c1 == '?' && c2 == '=') lx->tok.type = T_NULLISH_ASSIGN, lx->pos += 3;
            else if (c1 == '?') lx->tok.type = T_NULLISH, lx->pos += 2;
            else if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) lx->tok.type = T_OPTCHAIN, lx->pos += 2;
            else lx->tok.type = T_QUESTION, lx->pos += 1;
            break;
        case '=':
            if (c1 == '=' && c2 == '=') lx->tok.type = T_SEQ, lx->pos += 3;
            else if (c1 == '=') lx->tok.type = T_EQ, lx->pos += 2;
            else if (c1 == '>') lx->tok.type = T_ARROW, lx->pos += 2;
            else lx->tok.type = T_ASSIGN, lx->pos += 1;
            break;
        case '!':
            if (c1 == '=' && c2 == '=') lx->tok.type = T_SNE, lx->pos += 3;
            else if (c1 == '=') lx->tok.type = T_NE, lx->pos += 2;
            else lx->tok.type = T_BANG, lx->pos += 1;
            break;
        case '<':
            if (c1 == '<' && c2 == '=') lx->tok.type = T_SHL_ASSIGN, lx->pos += 3;
            else if (c1 == '<') lx->tok.type = T_SHL, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_LE, lx->pos += 2;
            else lx->tok.type = T_LT, lx->pos += 1;
            break;
        case '>':
            if (c1 == '>' && c2 == '>' && c3 == '=') lx->tok.type = T_SHR_ASSIGN, lx->pos += 4;
            else if (c1 == '>' && c2 == '>') lx->tok.type = T_SHR, lx->pos += 3;
            else if (c1 == '>' && c2 == '=') lx->tok.type = T_SAR_ASSIGN, lx->pos += 3;
            else if (c1 == '>') lx->tok.type = T_SAR, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_GE, lx->pos += 2;
            else lx->tok.type = T_GT, lx->pos += 1;
            break;
        case '+':
            if (c1 == '+') lx->tok.type = T_INC, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_PLUS_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_PLUS, lx->pos += 1;
            break;
        case '-':
            if (c1 == '-') lx->tok.type = T_DEC, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_MINUS_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_MINUS, lx->pos += 1;
            break;
        case '*':
            if (c1 == '*' && c2 == '=') lx->tok.type = T_STARSTAR_ASSIGN, lx->pos += 3;
            else if (c1 == '*') lx->tok.type = T_STARSTAR, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_STAR_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_STAR, lx->pos += 1;
            break;
        case '/':
            if (regex_allowed(lx->tok.prev_type)) {
                lex_regex(lx);
                lx->last_type = lx->tok.type;
                return;
            }
            if (c1 == '=') lx->tok.type = T_SLASH_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_SLASH, lx->pos += 1;
            break;
        case '%':
            if (c1 == '=') lx->tok.type = T_PERCENT_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_PERCENT, lx->pos += 1;
            break;
        case '&':
            if (c1 == '&' && c2 == '=') lx->tok.type = T_ANDAND_ASSIGN, lx->pos += 3;
            else if (c1 == '&') lx->tok.type = T_ANDAND, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_AMP_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_AMP, lx->pos += 1;
            break;
        case '|':
            if (c1 == '|' && c2 == '=') lx->tok.type = T_OROR_ASSIGN, lx->pos += 3;
            else if (c1 == '|') lx->tok.type = T_OROR, lx->pos += 2;
            else if (c1 == '=') lx->tok.type = T_PIPE_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_PIPE, lx->pos += 1;
            break;
        case '^':
            if (c1 == '=') lx->tok.type = T_CARET_ASSIGN, lx->pos += 2;
            else lx->tok.type = T_CARET, lx->pos += 1;
            break;
        default:
            error(lx, "unexpected character");
            lx->pos++;
            break;
        }
    }
    lx->tok.len   = lx->pos - lx->tok.start;
    lx->last_type = lx->tok.type;
}

const char *tok_name(TokType t) {
    static const char *const names[] = {
        "end of input", "number", "string", "template", "identifier",
        "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
        "else", "export", "extends", "false", "finally", "for", "function", "if", "import", "in",
        "instanceof", "let", "new", "null", "of", "return", "super", "switch", "this", "throw", "true",
        "try", "typeof", "var", "void", "while", "with", "yield", "async", "await", "static", "get", "set",
        "{", "}", "(", ")", "[", "]", ";", ",", ".", "...", "?", "?.", ":", "=>",
        "<", ">", "<=", ">=", "==", "!=", "===", "!==",
        "+", "-", "*", "/", "%", "**", "++", "--",
        "<<", ">>", ">>>", "&", "|", "^", "!", "~",
        "&&", "||", "??",
        "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?=",
        "#", "regular expression", "private name", "error"};
    return (unsigned)t < sizeof names / sizeof names[0] ? names[t] : "?";
}
