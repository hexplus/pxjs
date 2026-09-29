/* The PXJS bytecode. One opcode byte, then operands; multi-byte operands
 * are little-endian and read a byte at a time (the Allegrex faults on
 * unaligned halfword loads, and bytecode has no alignment).
 *
 * OP(name, operand bytes, stack effect)  -- effect is for max-stack
 * accounting; VAR means the compiler accounts for it itself. */

#ifndef OP
#error "define OP before including px_opcodes.h"
#endif

OP(NOP, 0, 0)
/* constants */
OP(UNDEF, 0, 1)
OP(NULL, 0, 1)
OP(TRUE, 0, 1)
OP(FALSE, 0, 1)
OP(INT8, 1, 1)       /* s8 */
OP(INT16, 2, 1)      /* s16 */
OP(CONST, 2, 1)      /* u16 constant index */
OP(THIS, 0, 1)
OP(HOLE, 0, 1)
OP(CALLEE, 0, 1)     /* the running function (named function expressions) */
/* stack */
OP(POP, 0, -1)
OP(DUP, 0, 1)
OP(DUP2, 0, 2)       /* a b -> a b a b */
OP(SWAP, 0, 0)
OP(ROT3, 0, 0)       /* a b c -> c a b */
OP(ROT4, 0, 0)       /* a b c d -> d a b c */
/* variables */
OP(GET_LOCAL, 1, 1)
OP(SET_LOCAL, 1, 0)  /* keeps the value */
OP(PUT_LOCAL, 1, -1) /* pops it */
OP(GET_LOCAL_CHECK, 1, 1) /* TDZ: throws on HOLE */
OP(SET_LOCAL_CHECK, 1, 0)
OP(INIT_HOLE, 1, 0)
OP(GET_UPVAL, 1, 1)
OP(SET_UPVAL, 1, 0)
OP(GET_UPVAL_CHECK, 1, 1)
OP(SET_UPVAL_CHECK, 1, 0)
OP(GET_GLOBAL, 2, 1)     /* u16 atom const; ReferenceError if absent */
OP(GET_GLOBAL_TYPEOF, 2, 1) /* undefined if absent (typeof x) */
OP(SET_GLOBAL, 2, 0)     /* strict: ReferenceError if absent */
OP(DEF_GLOBAL, 2, -1)    /* var/function at script top level */
OP(CLOSE_UPVALS, 1, 0)   /* close upvalues for locals >= slot */
OP(THROW_CONST, 2, 0)    /* assignment to const: u16 atom */
OP(THROW_REF, 2, 1)      /* ReferenceError: u16 message constant (a parameter read too early, delete super.x) */
/* properties */
OP(GET_PROP, 4, 0)       /* obj -> value         u16 atom, u16 cache */
OP(GET_PROP_KEEP, 4, 1)  /* obj -> obj value     (method calls) */
OP(SET_PROP, 4, -1)      /* obj value -> value */
OP(GET_ELEM, 0, -1)      /* obj key -> value */
OP(GET_ELEM_KEEP, 0, 0)  /* obj key -> obj value */
OP(SET_ELEM, 0, -2)      /* obj key value -> value */
OP(DELETE_PROP, 2, 0)    /* obj -> bool */
OP(DELETE_ELEM, 0, -1)   /* obj key -> bool */
OP(NEW_OBJECT, 0, 1)
OP(NEW_ARRAY, 0, 1)
OP(DEFINE_FIELD, 2, -1)  /* obj value -> obj    u16 atom */
OP(DEFINE_ELEM, 0, -2)   /* obj key value -> obj */
OP(APPEND, 0, -1)        /* arr value -> arr */
OP(APPEND_HOLE, 0, 0)    /* arr -> arr (elision) */
/* calls */
OP(CALL, 1, VAR)         /* fn this args... -> result   u8 argc */
OP(NEW, 1, VAR)          /* ctor args... -> object */
OP(RETURN, 0, -1)
OP(RETURN_UNDEF, 0, 0)
OP(CLOSURE, 2, 1)        /* u16 const index of a function template */
/* control */
OP(JUMP, 2, 0)           /* s16 relative to the next instruction */
OP(JUMP_IF_FALSE, 2, -1)
OP(JUMP_IF_TRUE, 2, -1)
OP(JUMP_IF_FALSE_KEEP, 2, 0) /* && : jumps with the value, else pops it */
OP(JUMP_IF_TRUE_KEEP, 2, 0)  /* || */
OP(JUMP_IF_NOT_NULLISH_KEEP, 2, 0) /* ?? */
OP(LOOP, 2, 0)           /* backward jump: checks the interrupt */
OP(THROW, 0, -1)
OP(TRY, 2, 0)            /* push a handler at +s16 */
OP(CATCH_FILTER, 2, 0)   /* at a catch: a generator return passes through (to +s16, or rethrown if 0) */
OP(END_TRY, 0, 0)
OP(FOR_IN, 0, 0)         /* obj -> iterator */
OP(FOR_OF, 0, 0)         /* iterable -> iterator */
OP(ITER_NEXT, 2, 1)      /* iter -> iter value, or jump s16 (popping iter) when done */
/* operators */
OP(ADD, 0, -1)
OP(SUB, 0, -1)
OP(MUL, 0, -1)
OP(DIV, 0, -1)
OP(MOD, 0, -1)
OP(POW, 0, -1)
OP(SHL, 0, -1)
OP(SAR, 0, -1)
OP(SHR, 0, -1)
OP(BAND, 0, -1)
OP(BOR, 0, -1)
OP(BXOR, 0, -1)
OP(EQ, 0, -1)
OP(NE, 0, -1)
OP(SEQ, 0, -1)
OP(SNE, 0, -1)
OP(LT, 0, -1)
OP(LE, 0, -1)
OP(GT, 0, -1)
OP(GE, 0, -1)
OP(IN, 0, -1)
OP(INSTANCEOF, 0, -1)
OP(NEG, 0, 0)
OP(PLUS, 0, 0)
OP(NOT, 0, 0)
OP(BNOT, 0, 0)
OP(TYPEOF, 0, 0)
OP(INC, 0, 0)
OP(DEC, 0, 0)
OP(INC_LOCAL, 1, 0)      /* x++ / ++x as a statement, on a local */
OP(DEC_LOCAL, 1, 0)
OP(TO_STRING, 0, 0)
OP(TO_NUMERIC, 0, 0)
/* E2: classes, spread/rest, destructuring, generators, async */
OP(OVER, 0, 1)               /* a b -> a b a */
OP(PACK_ARRAY, 1, VAR)       /* v1..vn -> array   u8 n */
OP(DEFINE_METHOD, 3, -1)     /* obj fn -> obj     u16 key, u8 kind (PX_MK_*) */
OP(DEFINE_METHOD_ELEM, 1, -2)/* obj key fn -> obj u8 kind */
OP(CLASS, 0, 0)              /* parent|HOLE ctor -> ctor proto */
OP(SET_FIELDS, 0, -1)        /* ctor fn -> ctor */
OP(INIT_FIELDS, 0, 0)        /* run this class's field initialisers on `this` */
OP(GET_SUPER, 2, 1)          /* -> super[key]     u16 key */
OP(GET_SUPER_ELEM, 0, 0)     /* key -> super[key] */
OP(GET_SUPER_RECV, 2, 0)     /* this -> super[key]            u16 key (a super reference) */
OP(GET_SUPER_ELEM_RECV, 0, -1) /* this key -> super[key] */
OP(SET_SUPER, 2, -1)          /* this v -> v   super[key] = v   u16 key */
OP(SET_SUPER_ELEM, 0, -2)     /* this key v -> v */
OP(SUPER_CALL, 1, VAR)       /* args... -> this   u8 argc */
OP(SUPER_CALL_ARRAY, 0, 0)   /* args -> this */
OP(SUPER_FORWARD, 0, 1)      /* -> this: super(...arguments) of a default constructor */
OP(NEW_TARGET, 0, 1)
OP(THIS_CHECK, 0, 1)         /* `this` in a derived constructor: ReferenceError before super() */
OP(CALL_ARRAY, 0, -2)        /* this fn args -> result */
OP(NEW_ARRAY_ARGS, 0, -1)    /* ctor args -> object */
OP(APPEND_SPREAD, 0, -1)     /* arr iterable -> arr */
OP(COPY_PROPS, 0, -1)        /* obj src -> obj ({...src}) */
OP(OBJ_REST, 0, -1)          /* src excludedKeys -> rest */
OP(REQUIRE_OBJ, 0, 0)        /* TypeError when destructuring null/undefined */
OP(ITER_START, 0, 0)         /* iterable -> iterator */
OP(ITER_STEP, 0, 1)          /* iter -> iter value (undefined once done) */
OP(ITER_REST, 0, 1)          /* iter -> iter array */
OP(ITER_CLOSE, 0, -1)        /* iter -> */
OP(JUMP_IF_NULLISH, 2, 0)    /* ?. : jumps keeping the value */
OP(YIELD, 0, 0)              /* value -> sent value */
OP(DELEGATE, 0, 0)           /* yield*: iterable -> result (suspends; the generator's driver delegates) */
OP(GEN_START, 0, 0)          /* a generator's parameters are bound: its call returns the generator here */
OP(AWAIT, 0, 0)              /* value -> fulfilled value */
OP(GET_ASYNC_ITER, 0, 0)     /* for await: iterable -> async iterator */
OP(ASYNC_CLOSE, 0, 0)        /* for await, left early: iter -> it.return() result (then awaited) */
OP(REGEXP, 4, 1)             /* u16 pattern, u16 flags -> new RegExp */
OP(TEMPLATE_OBJ, 2, 1)       /* tagged template strings array  u16 const index */
OP(IMPORT, 2, 1)             /* -> the host resolver's namespace for a specifier  u16 const */
OP(NEW_PRIVATE, 2, 1)        /* a fresh private symbol for #name   u16 const (description) */
OP(SET_PROTO, 0, -1)         /* obj v -> obj: `__proto__: v` in a literal (objects and null only) */
/* private elements (#x), own properties keyed by a class's private symbols */
OP(GET_PRIVATE, 0, -1)       /* obj #x -> value: TypeError if obj lacks #x */
OP(GET_PRIVATE_KEEP, 0, 0)   /* obj #x -> obj value (method calls) */
OP(SET_PRIVATE, 0, -2)       /* obj #x value -> value: TypeError if missing or read-only */
OP(DEFINE_PRIVATE, 0, -2)    /* obj #x value -> obj: a private field; TypeError if already there */
OP(ADD_PRIVATE_METHOD, 1, -2)/* obj #x fn -> obj   u8 PX_MK_* kind: a private method or accessor half */
OP(HAS_PRIVATE, 0, -1)       /* #x obj -> bool: #x in obj */
OP(SET_HOME, 0, -1)          /* home fn -> fn: the object `super` means in a private method */
OP(TO_PROPKEY, 0, 0)         /* v -> ToPropertyKey(v): computed class field names */
OP(ELEM_KEY, 0, 0)           /* o k -> o ToPropertyKey(k), TypeError first if o is null/undefined: once, for o[k] op= v */
/* array destructuring, the iterator in a local */
OP(ITER_STEP_AT, 1, 1)       /* -> the next value (undefined once done)   u8 local */
OP(ITER_REST_AT, 1, 1)       /* -> an array of the remaining values       u8 local */
OP(ITER_CLOSE_ABRUPT, 0, -2) /* exc iter -> (closes iter, then throws exc: return() errors lose, except
                                after a generator's return) */
/* compare-and-branch: LT..GE followed by JUMP_IF_FALSE, fused by the
 * compiler (a loop's `i < n` test): a b -> (jump if not a OP b)  s16 offset */
OP(LT_JUMP_IF_FALSE, 2, -2)
OP(LE_JUMP_IF_FALSE, 2, -2)
OP(GT_JUMP_IF_FALSE, 2, -2)
OP(GE_JUMP_IF_FALSE, 2, -2)
OP(SEQ_JUMP_IF_FALSE, 2, -2) /* === and !== the same way (if-chains on a value) */
OP(SNE_JUMP_IF_FALSE, 2, -2)
