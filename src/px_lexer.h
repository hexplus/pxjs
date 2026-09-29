/* The PXJS tokenizer. UTF-8 source in; tokens out, one at a time, with
 * save/restore so the compiler can look ahead (arrow functions) and scan a
 * block for declarations before compiling it. */
#ifndef PX_LEXER_H
#define PX_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum TokType {
    T_EOF = 0,
    T_NUMBER,
    T_STRING,
    T_TEMPLATE, /* a template chunk; tok.template_tail says whether it ended with ` */
    T_IDENT,
    /* keywords */
    T_BREAK, T_CASE, T_CATCH, T_CLASS, T_CONST, T_CONTINUE, T_DEBUGGER, T_DEFAULT, T_DELETE, T_DO,
    T_ELSE, T_EXPORT, T_EXTENDS, T_FALSE, T_FINALLY, T_FOR, T_FUNCTION, T_IF, T_IMPORT, T_IN,
    T_INSTANCEOF, T_LET, T_NEW, T_NULL, T_OF, T_RETURN, T_SUPER, T_SWITCH, T_THIS, T_THROW, T_TRUE,
    T_TRY, T_TYPEOF, T_VAR, T_VOID, T_WHILE, T_WITH, T_YIELD, T_ASYNC, T_AWAIT, T_STATIC, T_GET, T_SET,
    /* punctuators */
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA, T_DOT,
    T_ELLIPSIS, T_QUESTION, T_OPTCHAIN, T_COLON, T_ARROW,
    T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE, T_SEQ, T_SNE,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR, T_INC, T_DEC,
    T_SHL, T_SAR, T_SHR, T_AMP, T_PIPE, T_CARET, T_BANG, T_TILDE,
    T_ANDAND, T_OROR, T_NULLISH,
    T_ASSIGN, T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN, T_SLASH_ASSIGN, T_PERCENT_ASSIGN,
    T_STARSTAR_ASSIGN, T_SHL_ASSIGN, T_SAR_ASSIGN, T_SHR_ASSIGN, T_AMP_ASSIGN, T_PIPE_ASSIGN,
    T_CARET_ASSIGN, T_ANDAND_ASSIGN, T_OROR_ASSIGN, T_NULLISH_ASSIGN,
    T_HASH, /* private names: not supported, reported by the parser */
    T_REGEXP, /* only after lex_regex(): pattern in str, flags in ident */
    T_PRIVATE, /* #name: ident holds "#name" */
    T_ERROR
} TokType;

typedef struct Token {
    TokType  type;
    uint32_t start, len; /* source span */
    uint32_t line;
    uint32_t ws_start, ws_line; /* where the whitespace before it began */
    int      nl_before; /* a line break precedes this token (ASI) */
    int      template_tail;
    int      escaped; /* an identifier written with \u escapes: never a keyword */
    int      prev_type; /* the token before this one */
    double   num;
    /* identifiers (UTF-8, escapes resolved) and string/template contents
     * (UTF-16) point into the lexer's scratch buffers: valid until the
     * next token */
    const char     *ident;
    uint32_t        ident_len;
    const uint16_t *str;
    uint32_t        str_len;
    const char     *error;
} Token;

typedef struct Lexer {
    const char *src;
    uint32_t    len, pos, line;
    Token       tok;
    /* scratch */
    char     *ibuf;
    uint32_t  ibuf_cap;
    uint16_t *sbuf;
    uint32_t  sbuf_cap;
    int       last_type; /* the type of the last token produced */
} Lexer;

typedef struct LexState {
    uint32_t pos, line;
    int      prev; /* the token type before it: decides regex vs division */
} LexState;

void lex_init(Lexer *lx, const char *src, uint32_t len);
void lex_free(Lexer *lx);
/* Reads the next token into lx->tok. */
void lex_next(Lexer *lx);
/* After the `}` that closes a template substitution: reads the next
 * template chunk (the `}` is the current token). */
void lex_template_continue(Lexer *lx);
/* The current token is '/' or '/=' where an expression starts: re-reads it
 * as a regular expression literal (T_REGEXP). */
void lex_regex(Lexer *lx);
/* The state BEFORE the current token, to re-read it after lex_restore. */
LexState lex_save(const Lexer *lx);
void     lex_restore(Lexer *lx, LexState st);

const char *tok_name(TokType t);
/* The keyword token a word spells (T_IDENT if none): for identifiers
 * written with escapes, which may not spell a reserved word. */
TokType lex_keyword_type(const char *s, uint32_t n);

#endif
