/* PXJS internals: value encoding, heap cells, the VM. See docs/engine.md.
 *
 * VALUE ENCODING (one 32-bit word)
 *
 *   ...xxxxxxx1   small integer (31 bits, two's complement, value >> 1)
 *   ...xxxxx000   pointer to a heap cell (cells are 8-byte aligned); 0 is
 *                 never a valid value
 *   ...nnnnn010   special: undefined, null, false, true, hole, exception
 *
 * Doubles live in heap cells (PxNumber) and only exist for values a small
 * integer cannot hold. The Allegrex FPU has no double precision -- every
 * double operation is a libgcc software routine -- so arithmetic on small
 * integers never goes near one.
 *
 * HEAP
 *
 * One arena, allocated once. Every cell starts with a 32-bit header:
 *
 *   bits 0-5   cell type (PxType)
 *   bit  6     GC mark
 *   bit  7     spare
 *   bits 8-31  size in 8-byte units, header included
 *
 * The collector is mark-sweep and never moves anything, so a C pointer to
 * a live cell stays valid; rooting is only about keeping cells alive. */
#ifndef PX_INTERNAL_H
#define PX_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pxjs.h"

/* ------------------------------------------------------------ values */

#define px_is_smi(v)    (((v) & 1u) != 0)
#define px_smi(v)       ((int32_t)(v) >> 1)
#define px_from_smi(i)  ((PxValue)(((uint32_t)(int32_t)(i) << 1) | 1u))
#define PX_SMI_MIN      (-(1 << 30))
#define PX_SMI_MAX      ((1 << 30) - 1)
#define px_is_ptr(v)    (((v) & 7u) == 0 && (v) != 0)
#define px_is_special(v) (((v) & 7u) == 2u)
#define px_ptr(v)       ((void *)(uintptr_t)(v))
#define px_from_ptr(p)  ((PxValue)(uintptr_t)(p))

/* ------------------------------------------------------------ cells */

typedef enum PxType {
    PX_T_FREE = 0,
    PX_T_NUMBER,
    PX_T_STRING,
    PX_T_ROPE,
    PX_T_SYMBOL,
    PX_T_SHAPE,
    PX_T_VEC,
    PX_T_DICT,
    PX_T_BYTES,
    PX_T_PROTO,  /* a compiled function (template) */
    PX_T_UPVAL,
    PX_T_ITER,
    PX_T_ACCESSOR, /* a getter/setter pair stored in a property slot */
    /* objects: every type from here on starts with PxObject */
    PX_T_OBJECT,
    PX_T_ARRAY,
    PX_T_CLOSURE,
    PX_T_NATIVE,
    PX_T_BOUND,
    PX_T_ERROR,
    PX_T_BOXED,  /* new Number/String/Boolean */
    PX_T_GENERATOR, /* generator objects and running async functions */
    PX_T_PROMISE,
    PX_T_ITEROBJ,   /* array/string/map/set iterator objects */
    PX_T_MAP,       /* Map, Set, WeakMap, WeakSet */
    PX_T_DATE,
    PX_T_REGEXP,
    PX_T_ARRAYBUFFER,
    PX_T_TYPEDARRAY,
    PX_T_DATAVIEW,
    PX_T_PROXY,
    PX_T_COUNT
} PxType;

#define PX_HDR_MARK       0x40u
#define PX_HDR(type, units) ((uint32_t)(type) | ((uint32_t)(units) << 8))
#define px_hdr_type(h)    ((PxType)((h) & 0x3Fu))
#define px_hdr_units(h)   ((h) >> 8)

typedef struct PxCell {
    uint32_t hdr;
} PxCell;

static inline PxType px_type_of(PxValue v) { return px_hdr_type(((PxCell *)px_ptr(v))->hdr); }
static inline int px_is_obj(PxValue v) { return px_is_ptr(v) && px_type_of(v) >= PX_T_OBJECT; }

typedef struct PxNumber {
    uint32_t hdr;
    uint32_t pad;
    double   d;
} PxNumber;

/* A flat string: Latin-1 (one byte per code unit) when every code unit is
 * < 256, else UTF-16. `len` is in UTF-16 code units, as JS sees it. */
typedef struct PxString {
    uint32_t hdr;
    uint32_t len;
    uint32_t hash; /* 0 = not computed yet */
    uint8_t  wide;
    uint8_t  interned;
    uint8_t  is_index; /* interned: 1 if it spells an array index (never stored: keys use SMIs) */
    uint8_t  pad;
    /* data follows: uint8_t[len] or uint16_t[len] */
} PxString;

#define px_str_l1(s)  ((uint8_t *)((PxString *)(s) + 1))
#define px_str_u16(s) ((uint16_t *)((PxString *)(s) + 1))

/* Inline: every string scan (hashing, comparing, parsing) calls it per
 * code unit. */
static inline uint16_t px_str_at(const PxString *s, uint32_t i) {
    return s->wide ? px_str_u16(s)[i] : px_str_l1(s)[i];
}

/* A concatenation not yet copied: `a + b` for long strings costs one cell,
 * and the copy happens once, when the characters are first needed. */
typedef struct PxRope {
    uint32_t hdr;
    uint32_t len;
    PxValue  left, right;
    uint32_t depth;
    uint8_t  wide;
    uint8_t  pad[3];
} PxRope;

typedef struct PxSymbol {
    uint32_t hdr;
    PxValue  description; /* string or undefined */
    uint32_t is_private;  /* engine-internal keys: never listed */
    uint32_t pad;
} PxSymbol;

typedef struct PxObject PxObject;

/* Hidden classes. A shape is one step of a transition tree: `parent` plus
 * the property `key` stored at slot `count - 1`. Objects with the same
 * prototype and the same keys added in the same order share a shape. The
 * child links are weak: a shape no object uses any more is collected. */
typedef struct PxShape {
    uint32_t        hdr;
    struct PxShape *parent;
    PxObject       *proto;
    PxValue         key;
    uint16_t        count;
    uint8_t         attrs;  /* of `key` */
    uint8_t         flags;  /* PX_SHAPE_* */
    struct PxShape *first_child;
    struct PxShape *next_sibling;
    /* A hashed index of every key in the chain, for shapes with many keys
     * that are looked up often (prototypes, big config objects): malloc'd
     * outside the JS heap, built on demand, freed with the shape. */
    struct PxShapeTable *table;
    uint16_t             lookups; /* until the table is built */
    uint16_t             pad;
    uint32_t             pad2;
} PxShape;

typedef struct PxShapeTable {
    uint32_t mask;
    struct {
        PxValue  key; /* 0: empty */
        uint16_t index;
        uint8_t  attrs;
        uint8_t  pad;
    } e[1];
} PxShapeTable;

void px_shape_table_free(PxShape *s);

#define PX_SHAPE_DICT 0x01 /* owned by one object in dictionary mode */

#define PX_ATTR_WRITABLE     0x01
#define PX_ATTR_ENUMERABLE   0x02
#define PX_ATTR_CONFIGURABLE 0x04
#define PX_ATTR_DEFAULT      (PX_ATTR_WRITABLE | PX_ATTR_ENUMERABLE | PX_ATTR_CONFIGURABLE)
#define PX_ATTR_HIDDEN       (PX_ATTR_WRITABLE | PX_ATTR_CONFIGURABLE) /* builtins: not enumerable */
#define PX_ATTR_ACCESSOR     0x08 /* the slot holds a PxAccessor */

typedef struct PxAccessor {
    uint32_t hdr;
    PxValue  get, set; /* functions or undefined */
} PxAccessor;

typedef struct PxVec {
    uint32_t hdr;
    uint32_t cap;
    PxValue  items[];
} PxVec;

/* An insertion-ordered hash table (dictionary-mode objects, big shapes'
 * key index). Entries are appended; deletes leave a tombstone key of 0. */
typedef struct PxDictEntry {
    PxValue key;
    PxValue value;
    uint32_t attrs;
} PxDictEntry;

typedef struct PxDict {
    uint32_t    hdr;
    uint32_t    count; /* live entries */
    uint32_t    used;  /* entries appended, including tombstones */
    uint32_t    cap;   /* entries capacity; the index has 2*cap slots */
    PxDictEntry entries[];
    /* then: int32_t index[2 * cap] */
} PxDict;

typedef struct PxBytes {
    uint32_t hdr;
    uint32_t len;
    uint8_t  data[];
} PxBytes;

#define PX_INLINE_SLOTS 4

struct PxObject {
    uint32_t hdr;
    PxShape *shape;
    PxVec   *slots; /* overflow slots, or a PxDict in dictionary mode */
    uint32_t flags; /* PX_OBJ_* */
    PxValue  inline_slots[PX_INLINE_SLOTS];
};

#define PX_OBJ_NOT_EXTENSIBLE 0x01
#define PX_OBJ_CLASS_CTOR     0x02 /* class constructor: not callable without new */
#define PX_OBJ_ARROW          0x04 /* no own `this`, no prototype */
#define PX_OBJ_PROTO_MADE     0x08 /* the lazy `prototype` has been created */
#define PX_OBJ_IS_PROTOTYPE   0x10 /* some shape uses this as its prototype */
#define PX_OBJ_NOT_CTOR       0x20 /* a native that is not a constructor (built-in methods) */
/* A function's `length` and `name` are answered from its PxProto/PxNative
 * until they are redefined (then they are ordinary own properties) or
 * deleted (these flags). */
#define PX_OBJ_NO_LENGTH      0x40
#define PX_OBJ_NO_NAME        0x80
#define PX_OBJ_LENGTH_RO      0x100 /* an array whose length is not writable */

typedef struct PxArray {
    PxObject obj;
    PxVec   *elems; /* dense elements [0, length) where length <= cap */
    uint32_t length;
    uint32_t pad;
} PxArray;

/* A compiled function. */
typedef struct PxProto {
    uint32_t hdr;
    PxBytes *code;
    PxVec   *consts; /* strings, numbers, nested PxProtos */
    PxBytes *lines;  /* (pc delta, line delta) pairs, varint */
    PxBytes *upval_desc; /* per upvalue: byte is_local, byte index */
    PxBytes *ics;        /* PxIC per property-access site, or NULL */
    PxValue  name;
    PxValue  filename;
    uint16_t nparams;
    uint16_t nlocals;
    uint16_t max_stack;
    uint16_t flags; /* PX_PROTO_* */
    uint8_t  nupvals;
    uint8_t  args_slot; /* PX_PROTO_ARGUMENTS: the local that gets `arguments` */
    uint8_t  self_slot; /* the local holding the function itself (its own name), or 0xFF */
    uint8_t  length;    /* the `length` property: parameters before the first default or rest */
    uint32_t line;
} PxProto;

/* An inline cache: what the last lookup at one `obj.name` site found.
 * Valid while vm->shape_epoch is unchanged: a collection may free a shape
 * and a new one reuse its address, so every collection invalidates all
 * caches at once (the next lookup at each site refills it). */
typedef struct PxIC {
    PxShape *shape;        /* the receiver's */
    PxShape *holder_shape; /* NULL: own property; else its prototype's shape */
    uint32_t epoch;        /* 0: empty */
    uint16_t slot;
    uint16_t pad;
} PxIC;

#define PX_PROTO_ARROW  0x01
#define PX_PROTO_METHOD 0x02 /* object-literal method: no prototype */
#define PX_PROTO_SCRIPT 0x04
#define PX_PROTO_CLASS_CTOR 0x08 /* a class constructor: new only */
#define PX_PROTO_DERIVED    0x10 /* ...of a class with `extends`: `this` comes from super() */
#define PX_PROTO_GENERATOR  0x20
#define PX_PROTO_ASYNC      0x40
#define PX_PROTO_ARGUMENTS  0x80  /* uses `arguments` */
#define PX_PROTO_REST       0x100 /* last parameter is ...rest (in slot nparams) */

typedef struct PxUpval {
    uint32_t        hdr;
    PxValue        *v;     /* the stack slot while open, &closed after */
    PxValue         closed;
    struct PxUpval *next;  /* open list, sorted by stack address, highest first */
} PxUpval;

typedef struct PxClosure {
    PxObject obj;
    PxProto *proto;
    PxValue  this_val; /* arrows: the captured `this` */
    PxValue  home;     /* methods: the object they were defined on (super) */
    PxUpval *upvals[];
} PxClosure;

typedef struct PxNative {
    PxObject   obj;
    PxNativeFn fn;
    PxValue    name;
    int16_t    length;
    int16_t    magic;
    PxValue    data; /* per-function data (vm->native_data during the call) */
} PxNative;

typedef struct PxBound {
    PxObject obj;
    PxValue  target;
    PxValue  this_val;
    PxVec   *args; /* may be NULL */
    uint32_t nargs;
} PxBound;

typedef struct PxBoxed {
    PxObject obj;
    PxValue  value;
    uint32_t pad;
} PxBoxed;

/* A suspended function: a generator, or an async function waiting on
 * await. Its frame -- this, callee, locals and operand stack -- is copied
 * off the VM stack into `saved` while it is not running, and copied back
 * (anywhere on the stack) to resume. Upvalues that pointed into the frame
 * are detached into their own cells meanwhile and re-attached on resume,
 * so closures and the generator keep seeing the same variables. */
typedef struct PxGen {
    PxObject          obj;
    struct PxClosure *fn;
    PxVec            *saved;
    PxVec            *handlers; /* (pc offset, sp offset) per active try, as small ints */
    PxVec            *upvals;   /* (upvalue, slot offset) pairs */
    PxValue           this_val, new_target;
    union {
        PxValue promise; /* async function: the promise the call returned */
        PxValue deleg;   /* generator: [iterator, next] while a yield* delegates, else undefined */
    };
    PxValue           ret_value; /* return(v) in progress: v */
    PxVec            *queue;     /* async generator: (mode, value, promise) requests */
    uint32_t          qlen;      /* requests queued */
    uint32_t          nsaved, nhandlers, nupvals;
    uint32_t          pc_off;
    uint16_t          argc;
    uint8_t           state; /* PX_GEN_* */
    uint8_t           is_async;  /* 1: async function, 2: async generator */
    uint8_t           busy;      /* async generator: running or awaiting */
} PxGen;

enum { PX_GEN_START = 0, PX_GEN_SUSPENDED, PX_GEN_RUNNING, PX_GEN_DONE };

/* generator.return(v) on a suspended generator: thrown at its yield so its
 * finally blocks run (catch blocks let it through, see OP_CATCH_FILTER);
 * when it leaves the generator, the generator completes with v. Never
 * visible to JavaScript. */
#define PX_RETURN_MARK PX_MAKE_SPECIAL(6)

typedef struct PxPromise {
    PxObject obj;
    PxValue  result;
    PxVec   *reactions; /* per reaction: on-fulfilled, on-rejected, derived promise */
    uint32_t nreactions;
    uint8_t  state;     /* PX_PROMISE_* */
    uint8_t  handled;
    uint16_t pad;
} PxPromise;

enum { PX_PROMISE_PENDING = 0, PX_PROMISE_FULFILLED, PX_PROMISE_REJECTED };

typedef struct PxIterObj {
    PxObject obj;
    PxValue  target;
    uint32_t index;
    uint8_t  kind; /* PX_IT_* */
    uint8_t  done;
    uint8_t  is_set; /* PX_IT_MAP_*: a Set's iterator (brand checks still work once done) */
    uint8_t  pad;
} PxIterObj;

enum {
    PX_IT_ARRAY_VALUES = 0,
    PX_IT_ARRAY_KEYS,
    PX_IT_ARRAY_ENTRIES,
    PX_IT_STRING,
    PX_IT_MAP_ENTRIES,
    PX_IT_MAP_KEYS,
    PX_IT_MAP_VALUES,
    PX_IT_ASYNC_FROM_SYNC, /* for await / yield* over a sync iterable: target is [iterator, next] */
    PX_IT_REGEXP_STRING    /* String.prototype.matchAll: target is [regexp, string] (px_regexp.c) */
};

/* Map and Set: an insertion-ordered hash table under SameValueZero.
 * entries holds (key, value) pairs; a deleted pair has key PX_HOLE and
 * stays in place, so iterators that are part-way through keep their
 * position. index maps hash slots to pair numbers (int32, -1 empty). */
typedef struct PxMap {
    PxObject obj;
    PxVec   *entries;
    PxBytes *index;
    uint32_t count, used, cap;
    uint32_t kind;  /* PX_MAP_* */
    PxValue  extra; /* FinalizationRegistry: the cleanup callback */
} PxMap;

enum {
    PX_MAP_MAP = 0, PX_MAP_SET, PX_MAP_WEAKMAP, PX_MAP_WEAKSET,
    PX_MAP_WEAKREF, /* one entry: target -> target */
    PX_MAP_FINREG   /* FinalizationRegistry: target -> [held, token, held, token...] */
};

typedef struct PxProxy {
    PxObject obj;
    PxValue  target;  /* null once revoked */
    PxValue  handler; /* null once revoked */
    uint8_t  callable, constructable;
    uint16_t pad;
} PxProxy;

typedef struct PxDate {
    PxObject obj;
    double   time; /* ms since the epoch, NaN for an invalid date */
} PxDate;

typedef struct PxRegExp {
    PxObject obj;
    PxValue  source;
    PxBytes *prog;
    uint32_t flags;   /* PX_RE_* */
    uint32_t ngroups; /* capture groups, including group 0 */
    PxValue  names;   /* group names: array of strings/undefined, or undefined */
    uint32_t pad;
} PxRegExp;

typedef struct PxArrayBuffer {
    PxObject obj;
    PxBytes *data;
    uint32_t len;
    uint32_t pad;
} PxArrayBuffer;

/* Uint8Array & co, and DataView (kind unused). */
typedef struct PxTyped {
    PxObject obj;
    PxValue  buffer; /* the PxArrayBuffer */
    uint32_t offset; /* in bytes */
    uint32_t length; /* in elements (DataView: bytes) */
    uint32_t kind;   /* PX_TA_* */
    uint32_t pad;
} PxTyped;

enum {
    PX_TA_INT8 = 0, PX_TA_UINT8, PX_TA_UINT8C, PX_TA_INT16, PX_TA_UINT16, PX_TA_INT32, PX_TA_UINT32,
    PX_TA_FLOAT32, PX_TA_FLOAT64, PX_TA_KINDS
};

/* A view's length now: 0 once its buffer is detached (data NULL). */
static inline uint32_t px_typed_length(const PxTyped *t) {
    return ((const PxArrayBuffer *)px_ptr(t->buffer))->data ? t->length : 0;
}
PxValue px_typed_get(PxVM *vm, PxTyped *t, uint32_t i); /* i < px_typed_length(t) */
int     px_typed_set(PxVM *vm, PxValue ta, uint32_t i, PxValue v);
int     px_typed_init(PxVM *vm);
/* Typed arrays as integer-indexed exotic objects (ECMA-262 10.4.5): is a
 * key a CanonicalNumericIndexString? 1 yes (small integers are), 0 no,
 * -1 exception. A numeric key names an element only if it is a small
 * integer below px_typed_length(); any other numeric key names nothing,
 * and is never looked up further along the prototype chain. */
int     px_typed_numeric_key(PxVM *vm, PxValue ta, PxValue key);
int     px_array_buffer_detach(PxValue buffer); /* -1 if not an ArrayBuffer */

#define PX_RE_GLOBAL      0x01
#define PX_RE_IGNORECASE  0x02
#define PX_RE_MULTILINE   0x04
#define PX_RE_DOTALL      0x08
#define PX_RE_UNICODE     0x10
#define PX_RE_STICKY      0x20
#define PX_RE_HASINDICES  0x40

/* for-in / for-of iteration state. */
typedef struct PxIter {
    uint32_t hdr;
    uint32_t kind; /* PX_ITK_* */
    PxValue  target; /* the object, or (protocol) the iterator object */
    PxVec   *keys;
    PxValue  next_fn; /* protocol: the iterator's next method */
    uint32_t index;
    uint32_t count;
    uint32_t done;
    uint32_t pad;
} PxIter;

enum { PX_ITK_KEYS = 0, PX_ITK_ARRAY, PX_ITK_STRING, PX_ITK_PROTOCOL };

/* ------------------------------------------------------------ the VM */

typedef struct PxFrame {
    PxClosure     *fn;
    const uint8_t *pc;
    PxValue       *base; /* locals[0] */
    PxValue       *callee; /* where the result goes: the `this` slot of the call */
    PxValue        this_val;
    PxValue        new_target;
    struct PxGen  *gen; /* the generator this frame runs, or NULL */
    uint16_t       argc;
    uint8_t        is_construct;
    uint8_t        is_boundary; /* return to native code when this frame returns */
} PxFrame;

typedef struct PxHandler {
    uint32_t       frame;
    PxValue       *sp;
    const uint8_t *pc;
} PxHandler;

enum {
    PX_PROTO_OBJECT = 0,
    PX_PROTO_FUNCTION,
    PX_PROTO_ARRAY,
    PX_PROTO_STRING,
    PX_PROTO_NUMBER,
    PX_PROTO_BOOLEAN,
    PX_PROTO_SYMBOL,
    PX_PROTO_GENOBJ, /* %GeneratorPrototype% */
    PX_PROTO_ASYNCGENOBJ, /* %AsyncGeneratorPrototype% */
    PX_PROTO_PROMISE,
    PX_PROTO_ITERATOR,       /* %IteratorPrototype%: [Symbol.iterator]() { return this } */
    PX_PROTO_ARRAY_ITERATOR, /* %ArrayIteratorPrototype% */
    PX_PROTO_MAP,
    PX_PROTO_SET,
    PX_PROTO_WEAKMAP,
    PX_PROTO_WEAKSET,
    PX_PROTO_WEAKREF, /* PX_PROTO_MAP + PX_MAP_WEAKREF */
    PX_PROTO_FINREG,  /* PX_PROTO_MAP + PX_MAP_FINREG */
    PX_PROTO_DATE,
    PX_PROTO_REGEXP,
    PX_PROTO_ARRAYBUFFER,
    PX_PROTO_TYPEDARRAY, /* %TypedArray%.prototype */
    PX_PROTO_DATAVIEW,
    PX_PROTO_TA_FIRST,   /* Int8Array.prototype ... Float64Array.prototype */
    PX_PROTO_TA_LAST = PX_PROTO_TA_FIRST + 8,
    PX_PROTO_ASYNC_ITERATOR,  /* %AsyncIteratorPrototype% */
    PX_PROTO_ASYNC_FROM_SYNC, /* %AsyncFromSyncIteratorPrototype% */
    PX_PROTO_MAP_ITERATOR,
    PX_PROTO_SET_ITERATOR,
    PX_PROTO_STRING_ITERATOR,
    PX_PROTO_GENFN,      /* %GeneratorFunction.prototype% */
    PX_PROTO_ASYNCGENFN, /* %AsyncGeneratorFunction.prototype% */
    PX_PROTO_ASYNCFN,    /* %AsyncFunction.prototype% */
    PX_PROTO_ERROR, /* + PxErrorType */
    PX_PROTO_COUNT = PX_PROTO_ERROR + PX_ERROR_TYPES
};

/* Interned property names the engine itself uses. */
#define PX_ATOM_LIST(X)                                                                              \
    X(length) X(prototype) X(constructor) X(name) X(message) X(stack) X(toString) X(valueOf)        \
    X(undefined) X(object) X(boolean) X(number) X(string) X(function) X(symbol) X(default)          \
    X(__proto__) X(Error) X(anonymous) X(next) X(done) X(value) X(then) X(get) X(set) X(raw)          \
    X(writable) X(enumerable) X(configurable) X(return) X(throw) X(lastIndex) X(index) X(input)    \
    X(groups) X(empty)

enum {
#define X(n) PX_ATOM_##n,
    PX_ATOM_LIST(X)
#undef X
    PX_ATOM_COUNT
};

#define PX_SIZE_CLASSES 32 /* free lists for cells of 8..256 bytes */
#define PX_MAX_ROOTS 2048
#define PX_MAX_HANDLERS 256

struct PxVM {
    /* heap */
    uint8_t *arena;
    uint8_t *arena_end;
    uint8_t *bump;
    PxCell  *free_small[PX_SIZE_CLASSES];
    PxCell  *free_large;
    size_t   bytes_since_gc;
    size_t   live_after_gc;
    size_t   gc_threshold;
    size_t   gc_count;
    size_t   live_cells;
    uint64_t last_gc_us;
    int      in_gc;
    int      gc_stress;
    uint32_t alloc_count, fail_alloc_at; /* failure injection (px_set_alloc_failure) */
    uint32_t shape_epoch; /* see PxIC */
    PxValue *mark_stack;
    uint32_t mark_cap, mark_top;
    int      mark_overflow;
    uint64_t (*now_us)(void);
#ifdef PX_PROFILE
    PxProfile prof;
#endif
    /* global variable lookups: where a name was last found in the global
     * object's dictionary. Only a hint -- checked against the entry's key
     * every time -- so nothing ever invalidates it. */
    struct {
        PxValue  name;
        uint32_t index;
    } global_hint[64];

    /* interning: open addressing over interned strings (weak) */
    PxString **atoms;
    uint32_t   atoms_cap, atoms_count;
    PxValue    atom[PX_ATOM_COUNT];

    /* prototype -> root shape (weak on both sides) */
    PxShape **root_shapes;
    uint32_t  root_cap, root_count;

    /* execution */
    PxValue  *stack, *stack_end, *sp;
    PxFrame  *frames;
    uint32_t  nframes, max_frames;
    PxUpval  *open_upvals;
    PxHandler handlers[PX_MAX_HANDLERS];
    uint32_t  nhandlers;
    uint32_t  native_depth, max_native_depth;
    int       interrupt_counter;
    PxInterruptFn interrupt;
    void     *interrupt_opaque;
    int       uncatchable;
    int       native_magic; /* the magic of the native function being called */
    PxValue   native_callee; /* and the function itself */
    PxValue   native_data;   /* and its data */
    PxValue   native_new_target; /* and new.target (undefined: called, not constructed) */
    int       suspended;     /* run() returned because a generator yielded */
    int       suspend_await; /* ...at: PX_SUSPEND_* (the generators' drivers tell them apart) */

    /* the job (microtask) queue: records of PX_JOB_WORDS values */
    PxValue  *jobs;
    uint32_t  jobs_head, jobs_count, jobs_cap;
    PxRejectionTracker rejection_tracker;
    void     *rejection_opaque;

    /* handles: long-lived roots for the host (free slots chain as SMIs) */
    PxValue  *handles;
    uint32_t  handles_cap, handles_used;
    int32_t   handle_free;

    PxModuleResolver resolver;
    void            *resolver_opaque;

    /* roots */
    PxValue *roots[PX_MAX_ROOTS];
    uint32_t nroots;
    PxValue  global;
    PxValue  protos[PX_PROTO_COUNT];
    PxValue  ctors[PX_PROTO_COUNT];
    PxValue  exception;
    PxValue  oom_error; /* preallocated: throwing it allocates nothing */
    /* weak collections found by the current marking (see px_heap.c) */
    struct PxMap **weak_maps;
    uint32_t       nweak, weak_cap;
    /* native finalizers (px_set_finalizer): called when their object dies */
    struct PxFinalizer *finalizers;
    uint32_t            nfinal, final_cap;
    /* WeakRef targets kept alive until the current job ends (the spec's
     * KeptAlive list): an array, or undefined */
    PxValue kept;
    PxValue proxy_proto; /* private: gives proxies a shape no other object has */
    PxValue  sym_fields; /* private symbol: a class's field initialiser */
    PxValue  sym_iterator; /* Symbol.iterator */
    /* the original Array.prototype[Symbol.iterator] and array iterator
     * next(): while they are in place, for-of and destructuring read
     * arrays directly */
    PxValue  array_values, array_iter_next;
    PxValue  sym_async_iterator;
    PxValue  sym_has_instance;
    PxValue  sym_to_primitive;
    PxValue  sym_to_string_tag;
    PxValue  sym_species;
    PxValue  sym_is_concat_spreadable;
    PxValue  sym_unscopables;
    PxValue  throw_type_error; /* %ThrowTypeError% */
    PxValue  sym_match, sym_match_all, sym_replace, sym_search, sym_split; /* the RegExp protocol (px_regexp.c) */
    void    *compiler;  /* the compiler while it runs (its values are roots) */

    char error_text[1024];
};

/* ------------------------------------------------------------ heap */

void    *px_alloc(PxVM *vm, PxType type, size_t bytes); /* NULL + OOM thrown */
int      px_heap_init(PxVM *vm, size_t bytes);
void     px_heap_free(PxVM *vm);
void     px_collect(PxVM *vm);
PxVec   *px_vec_new(PxVM *vm, uint32_t cap);
PxVec   *px_vec_grow(PxVM *vm, PxVec *v, uint32_t need); /* returns v itself if big enough */
PxBytes *px_bytes_new(PxVM *vm, const void *data, uint32_t len);
size_t   px_cell_bytes(const void *cell);

/* ------------------------------------------------------------ strings */

PxValue  px_str_new_l1(PxVM *vm, const uint8_t *s, uint32_t len);
PxValue  px_str_new_u16(PxVM *vm, const uint16_t *s, uint32_t len);
PxValue  px_str_from_utf8(PxVM *vm, const char *s, size_t len);
PxValue  px_str_from_cstr(PxVM *vm, const char *s);
PxValue  px_str_concat(PxVM *vm, PxValue a, PxValue b);
PxString *px_str_flat(PxVM *vm, PxValue s); /* NULL on OOM */
uint32_t px_str_len(PxValue s);
int      px_is_str(PxValue v);
int      px_str_eq(PxVM *vm, PxValue a, PxValue b); /* -1 on OOM */
int      px_str_cmp(PxVM *vm, PxValue a, PxValue b, int *out);
PxValue  px_str_slice(PxVM *vm, PxValue s, uint32_t start, uint32_t end);
PxValue  px_intern(PxVM *vm, PxValue str); /* key form: interned string or SMI index */
PxValue  px_intern_cstr(PxVM *vm, const char *s);
/* The key form of text[0..len) (8-bit, or 16-bit when wide), allocating
 * only when the name is not interned yet (JSON.parse's property names). */
PxValue  px_intern_chars(PxVM *vm, const void *text, int wide, uint32_t len);
PxValue  px_intern_literal(PxVM *vm, PxValue str); /* dedup a string, never an index */
size_t   px_str_to_utf8(PxVM *vm, PxValue s, char *dst, size_t cap); /* returns needed length */
PxValue  px_number_to_string(PxVM *vm, double d, int radix);
double   px_string_to_number(PxVM *vm, PxValue s);

#include "px_dtoa.h"
void     px_atoms_sweep(PxVM *vm);

/* ------------------------------------------------------------ objects */

PxObject *px_obj_new(PxVM *vm, PxType type, size_t bytes, PxValue proto);
PxValue   px_object_new(PxVM *vm); /* plain, Object.prototype */
PxValue   px_array_new(PxVM *vm, uint32_t cap);
PxValue   px_error_new(PxVM *vm, PxErrorType type, PxValue message);
PxShape  *px_root_shape(PxVM *vm, PxObject *proto);
void      px_root_shapes_sweep(PxVM *vm);
void      px_shapes_unlink_dead(PxVM *vm, PxShape *dead);

/* Own lookups: slot pointer or NULL; attrs optional. */
PxValue *px_own_slot(PxVM *vm, PxObject *o, PxValue key, uint32_t *attrs);
/* Private elements (#x): own properties under private symbols, on any
 * object (a proxy's own cell included), invisible to everything else.
 * px_private_add adds an absent one, even to a non-extensible object: a
 * data property with attrs, or an accessor when get or set is given. */
PxValue *px_private_slot(PxVM *vm, PxObject *o, PxValue key, uint32_t *attrs);
int      px_private_add(PxVM *vm, PxObject *o, PxValue key, PxValue v, PxValue get, PxValue set, uint32_t attrs);
PxValue  px_get(PxVM *vm, PxValue obj, PxValue key);        /* key must be px_intern'ed */
PxValue  px_get_recv(PxVM *vm, PxValue start, PxValue key, PxValue receiver);
/* px_get / px_set through an inline cache (ic may be NULL). */
PxValue  px_get_ic(PxVM *vm, PxValue obj, PxValue key, PxIC *ic);
int      px_set_ic(PxVM *vm, PxValue obj, PxValue key, PxValue v, PxIC *ic);
/* The slot an inline-cache hit reads: inline, or in the overflow vector. */
static inline PxValue *px_slot_at(PxObject *o, uint32_t i) {
    return i < PX_INLINE_SLOTS ? &o->inline_slots[i] : &o->slots->items[i - PX_INLINE_SLOTS];
}
int      px_define_accessor(PxVM *vm, PxValue obj, PxValue key, PxValue getter, PxValue setter, uint32_t attrs);
int      px_set_proto(PxVM *vm, PxValue obj, PxValue proto); /* -1 exception (a TypeError when refused) */
int      px_set_proto_ok(PxVM *vm, PxValue obj, PxValue proto); /* [[SetPrototypeOf]]: 1 done, 0 refused, -1 exc */
PxValue  px_get_value(PxVM *vm, PxValue obj, PxValue key); /* any key value */
/* Assignment, as strict code does it: -1 exception, including a TypeError
 * when the set is refused (read-only, no setter, non-extensible). */
int      px_set(PxVM *vm, PxValue obj, PxValue key, PxValue v);
/* [[Set]] (OrdinarySet, proxies' set trap): 1 done, 0 refused, -1 exception */
int      px_set_recv(PxVM *vm, PxValue obj, PxValue key, PxValue v, PxValue receiver);
int      px_set_value(PxVM *vm, PxValue obj, PxValue key, PxValue v);
/* The engine's own definitions (built-ins, literals): no validation. */
int      px_define(PxVM *vm, PxValue obj, PxValue key, PxValue v, uint32_t attrs);
int      px_delete(PxVM *vm, PxValue obj, PxValue key); /* 1 deleted, 0 not, -1 exc */
int      px_delete_strict(PxVM *vm, PxValue obj, PxValue key); /* the delete operator: 1, or -1 (TypeError if refused) */
int      px_has(PxVM *vm, PxValue obj, PxValue key);    /* proto chain */
int      px_has_own(PxVM *vm, PxValue obj, PxValue key);
/* Own keys as strings (and symbols with PX_KEYS_SYMBOLS), in the order of
 * [[OwnPropertyKeys]]: integer keys ascending, strings, then symbols. */
#define PX_KEYS_ENUMERABLE 0x01 /* only enumerable properties */
#define PX_KEYS_SYMBOLS    0x02 /* symbols too, after the strings */
#define PX_KEYS_NO_STRINGS 0x04 /* no strings (with PX_KEYS_SYMBOLS: symbols only) */
PxVec   *px_own_keys(PxVM *vm, PxValue obj, int flags, uint32_t *count);
PxValue  px_proto_of(PxVM *vm, PxValue v);
int      px_array_push(PxVM *vm, PxValue arr, PxValue v);
int      px_array_set_length(PxVM *vm, PxArray *a, uint32_t len);
PxValue  px_key_to_value(PxVM *vm, PxValue key); /* SMI key -> string */

/* Property descriptors (ECMA-262 6.2.6) for the essential internal
 * methods. Whoever fills one keeps its values rooted. */
typedef struct PxDesc {
    uint32_t has;   /* PX_DESC_*: the fields present */
    uint32_t attrs; /* PX_ATTR_WRITABLE/ENUMERABLE/CONFIGURABLE: the values of those present */
    PxValue  value, get, set;
} PxDesc;
#define PX_DESC_VALUE        0x01
#define PX_DESC_GET          0x02
#define PX_DESC_SET          0x04
#define PX_DESC_WRITABLE     0x08
#define PX_DESC_ENUMERABLE   0x10
#define PX_DESC_CONFIGURABLE 0x20
#define PX_ROOT_DESC(vm, d)    (PX_ROOT(vm, (d).value), PX_ROOT(vm, (d).get), PX_ROOT(vm, (d).set)) /* 3 roots */
#define PX_DESC_IS_ACCESSOR(d) (((d)->has & (PX_DESC_GET | PX_DESC_SET)) != 0)
#define PX_DESC_IS_DATA(d)     (((d)->has & (PX_DESC_VALUE | PX_DESC_WRITABLE)) != 0)
/* [[GetOwnProperty]]: 1 found (a complete *d), 0 absent, -1 exception */
int      px_get_own_property(PxVM *vm, PxValue obj, PxValue key, PxDesc *d);
/* [[DefineOwnProperty]]: 1 done, 0 refused, -1 exception */
int      px_define_own_property(PxVM *vm, PxValue obj, PxValue key, const PxDesc *d);
int      px_define_or_throw(PxVM *vm, PxValue obj, PxValue key, const PxDesc *d); /* 0 / -1 */
int      px_create_data_property(PxVM *vm, PxValue obj, PxValue key, PxValue v); /* 1 / 0 / -1 */
int      px_to_property_descriptor(PxVM *vm, PxValue obj, PxDesc *d); /* 0 / -1; *d rooted by the caller */
PxValue  px_from_property_descriptor(PxVM *vm, const PxDesc *d);
int      px_prevent_extensions(PxVM *vm, PxValue obj); /* 1 / 0 / -1 */
int      px_is_extensible(PxVM *vm, PxValue obj);      /* 1 / 0 / -1 */
PxValue  px_descriptor_of(PxVM *vm, PxValue o, PxValue key); /* Object.getOwnPropertyDescriptor */
int      px_desc_compatible(PxVM *vm, int extensible, const PxDesc *d, const PxDesc *cur); /* cur NULL: absent */
int      px_same_value(PxVM *vm, PxValue a, PxValue b);
int      px_is_array(PxVM *vm, PxValue v); /* IsArray: 1 / 0 / -1 (a revoked proxy) */

/* Proxies (px_proxy.c): the object model hands them here. Each enforces
 * the invariants of its internal method. Results as for the ordinary
 * operations above (1 / 0 / -1). */
int      px_proxy_init(PxVM *vm);
PxValue  px_proxy_target(PxVM *vm, PxValue proxy); /* TypeError if revoked */
PxValue  px_proxy_get(PxVM *vm, PxValue proxy, PxValue key, PxValue receiver);
int      px_proxy_set(PxVM *vm, PxValue proxy, PxValue key, PxValue v, PxValue receiver);
int      px_proxy_has(PxVM *vm, PxValue proxy, PxValue key);
int      px_proxy_delete(PxVM *vm, PxValue proxy, PxValue key);
int      px_proxy_get_own_property(PxVM *vm, PxValue proxy, PxValue key, PxDesc *d);
int      px_proxy_define_own_property(PxVM *vm, PxValue proxy, PxValue key, const PxDesc *d);
PxVec   *px_proxy_own_keys(PxVM *vm, PxValue proxy, int flags, uint32_t *count);
PxValue  px_proxy_proto_of(PxVM *vm, PxValue proxy);
int      px_proxy_set_proto(PxVM *vm, PxValue proxy, PxValue proto);
int      px_proxy_prevent_extensions(PxVM *vm, PxValue proxy);
int      px_proxy_is_extensible(PxVM *vm, PxValue proxy);
PxValue  px_proxy_call(PxVM *vm, PxValue proxy, PxValue this_val, int argc, PxValue *argv);
PxValue  px_proxy_construct(PxVM *vm, PxValue proxy, int argc, PxValue *argv, PxValue new_target);

/* ------------------------------------------------------------ conversion */

PxValue px_to_string(PxVM *vm, PxValue v);
int     px_to_number(PxVM *vm, PxValue v, double *out);
int     px_to_int32(PxVM *vm, PxValue v, int32_t *out);
int     px_to_uint32(PxVM *vm, PxValue v, uint32_t *out);
PxValue px_to_primitive(PxVM *vm, PxValue v, int hint_string);
PxValue px_to_object(PxVM *vm, PxValue v);
int     px_truthy(PxValue v);
/* Numbers. Hot paths: inline. */
static inline int px_is_num(PxValue v) {
    return px_is_smi(v) || (px_is_ptr(v) && px_type_of(v) == PX_T_NUMBER);
}
/* v is SMI or PxNumber */
static inline double px_num(PxValue v) { return px_is_smi(v) ? (double)px_smi(v) : ((PxNumber *)px_ptr(v))->d; }

/* Doubles by their bits. The Allegrex FPU is single precision, so every
 * double operation on the PSP is a libgcc call of dozens of instructions;
 * classifying a double (is it a small integer? what is its ToInt32?)
 * needs none of them when done on the IEEE-754 bits with integer
 * instructions. */
static inline uint64_t px_dbits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

/* 1 (and *out) if d is an integer in the SMI range and not -0. */
static inline int px_dbl_to_smi(double d, int32_t *out) {
    uint64_t u = px_dbits(d), mant;
    int      e = (int)((u >> 52) & 0x7FF) - 1023, sh;
    if (e < 0) {
        if (u != 0) return 0; /* fractions, -0, denormals */
        *out = 0;
        return 1;
    }
    if (e > 29) {
        if (u != 0xC1D0000000000000ull) return 0; /* only -2^30 is in range */
        *out = PX_SMI_MIN;
        return 1;
    }
    /* d = 1.m * 2^e: an integer iff the low 52-e mantissa bits are 0 */
    mant = (u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    sh   = 52 - e;
    if (mant & ((1ull << sh) - 1)) return 0;
    *out = (u >> 63) ? -(int32_t)(uint32_t)(mant >> sh) : (int32_t)(uint32_t)(mant >> sh);
    return 1;
}

/* ToInt32 (ECMA-262 7.1.6), exactly: truncate, then modulo 2^32. */
static inline int32_t px_dbl_to_int32(double d) {
    uint64_t u = px_dbits(d), mant;
    int      e = (int)((u >> 52) & 0x7FF);
    uint32_t r;
    if (e == 0x7FF || e < 1023) return 0; /* NaN, infinities, |d| < 1 */
    e -= 1075;                            /* d = mant * 2^e, mant 53 bits */
    mant = (u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    if (e >= 32) r = 0;
    else if (e >= 0) r = (uint32_t)(mant << e);
    else r = (uint32_t)(mant >> -e);
    return (u >> 63) ? (int32_t)(0u - r) : (int32_t)r;
}

/* 1 (and *out) if v is a number whose value is an int32 exactly. */
static inline int px_num_to_int32_exact(PxValue v, int32_t *out) {
    uint64_t u, mant;
    int      e, sh;
    if (px_is_smi(v)) {
        *out = px_smi(v);
        return 1;
    }
    if (!px_is_ptr(v) || px_type_of(v) != PX_T_NUMBER) return 0;
    u = px_dbits(((PxNumber *)px_ptr(v))->d);
    e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e < 0 || e > 31) return 0; /* |d| < 1 (0 is an SMI), or too big */
    if (e == 31) {
        if (u != 0xC1E0000000000000ull) return 0; /* only -2^31 */
        *out = INT32_MIN;
        return 1;
    }
    mant = (u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    sh   = 52 - e;
    if (mant & ((1ull << sh) - 1)) return 0;
    *out = (u >> 63) ? -(int32_t)(uint32_t)(mant >> sh) : (int32_t)(uint32_t)(mant >> sh);
    return 1;
}

/* A heap number, without the SMI check (the caller knows). */
PxValue px_box_number(PxVM *vm, double d);
int     px_strict_equals(PxVM *vm, PxValue a, PxValue b);
int     px_loose_equals(PxVM *vm, PxValue a, PxValue b); /* -1 exc */
PxValue px_typeof(PxVM *vm, PxValue v);
int     px_is_callable(PxValue v);
int     px_same_value_zero(PxVM *vm, PxValue a, PxValue b);

/* ------------------------------------------------------------ execution */

PxValue px_run_proto(PxVM *vm, PxProto *proto, PxValue this_val);
PxValue px_construct(PxVM *vm, PxValue ctor, int argc, PxValue *argv);
PxValue px_construct_nt(PxVM *vm, PxValue ctor, int argc, PxValue *argv, PxValue new_target);
int     px_is_constructor(PxValue v);

/* generators and async functions (px_vm.c) */
PxValue px_gen_resume(PxVM *vm, PxGen *g, PxValue v, int mode, int *done); /* mode: PX_RESUME_* */
enum { PX_RESUME_NEXT = 0, PX_RESUME_THROW, PX_RESUME_RETURN };
/* where a generator suspended (vm->suspend_await); at PX_SUSPEND_DELEGATE
 * (yield*) the driver takes over with the iterable it returned, and
 * resumes the frame with the yield*'s completion (px_iter.c, px_asyncgen.c) */
enum { PX_SUSPEND_YIELD = 0, PX_SUSPEND_AWAIT, PX_SUSPEND_DELEGATE };
void    px_async_step(PxVM *vm, PxGen *g, PxValue v, int mode);
int     px_asyncgen_init(PxVM *vm);
PxValue px_get_async_iterator(PxVM *vm, PxValue iterable); /* for await */
PxValue px_async_iter_close(PxVM *vm, PxValue it);         /* it.return() or undefined */
PxValue px_iter_record(PxVM *vm, PxValue iterable, int async); /* [iterator, next method] */
PxValue px_get_method(PxVM *vm, PxValue v, PxValue key); /* undefined, a function, or TypeError */

/* promises and jobs (px_promise.c) */
#define PX_JOB_WORDS 4
PxValue px_promise_new(PxVM *vm);
int     px_promise_resolve(PxVM *vm, PxValue promise, PxValue value);
int     px_promise_reject(PxVM *vm, PxValue promise, PxValue reason);
PxValue px_promise_resolved(PxVM *vm, PxValue value); /* Promise.resolve(value) */
int     px_promise_then_native(PxVM *vm, PxValue promise, PxValue on_ful, PxValue on_rej);
/* PerformPromiseThen with a derived promise that the handler's result settles */
int     px_promise_then_derived(PxVM *vm, PxValue promise, PxValue on_ful, PxValue on_rej, PxValue derived);
int     px_enqueue_job(PxVM *vm, PxValue a, PxValue b, PxValue c, PxValue d);
int     px_promise_init(PxVM *vm);

/* iteration (px_iter.c) */
PxValue px_get_iterator(PxVM *vm, PxValue iterable);          /* an iterator object */
int     px_iterator_step(PxVM *vm, PxValue iter, PxValue *out); /* 1 value, 0 done, -1 exc */
int     px_iterator_close(PxVM *vm, PxValue iter);
int     px_iterator_close_throw(PxVM *vm, PxValue iter);      /* keeps the pending exception; -1 */
int     px_record_step(PxVM *vm, PxValue rec, PxValue *out);  /* px_iter_record's: 1 value, 0 done, -1 exc */
int     px_def_tag(PxVM *vm, PxValue obj, const char *tag);   /* [Symbol.toStringTag] */
int     px_def_species(PxVM *vm, PxValue ctor);               /* get [Symbol.species]() { return this } */
int     px_symbol_registered(PxVM *vm, PxValue s);            /* made by Symbol.for: 1, 0, -1 exc */
PxValue px_iter_result(PxVM *vm, PxValue value, int done);     /* { value, done } */
PxValue px_make_iterobj(PxVM *vm, PxValue target, int kind);
int     px_iter_init(PxVM *vm);
int     px_collections_init(PxVM *vm);
int     px_date_init(PxVM *vm);
int     px_regexp_init(PxVM *vm);
PxValue px_regexp_create(PxVM *vm, PxValue pattern, PxValue flags);
int     px_is_regexp(PxValue v);
/* String.prototype.match, matchAll, replace, replaceAll, search, split */
enum { PX_SM_MATCH, PX_SM_MATCH_ALL, PX_SM_REPLACE, PX_SM_REPLACE_ALL, PX_SM_SEARCH, PX_SM_SPLIT };
PxValue px_string_regexp_method(PxVM *vm, PxValue this_val, int argc, PxValue *argv, int which);
PxValue px_symbol_new(PxVM *vm, PxValue description);
PxValue px_make_native_data(PxVM *vm, PxNativeFn fn, const char *name, int length, PxValue data);
PxValue px_throw_oom(PxVM *vm);
PxValue px_make_native(PxVM *vm, PxNativeFn fn, const char *name, int length, int magic);
void    px_capture_stack(PxVM *vm, PxValue err);
int     px_check_interrupt(PxVM *vm);

/* ------------------------------------------------------------ compiler */

PxProto *px_compile(PxVM *vm, const char *src, size_t len, const char *filename, int module);
void     px_compiler_mark(PxVM *vm);

/* ------------------------------------------------------------ builtins */

int px_builtins_init(PxVM *vm);
int px_json_init(PxVM *vm);

typedef struct PxFnDef {
    const char *name;
    PxNativeFn  fn;
    int16_t     length;
    int16_t     magic;
} PxFnDef;

int     px_def_fns(PxVM *vm, PxValue obj, const PxFnDef *defs, int n);
int     px_def_value(PxVM *vm, PxValue obj, const char *name, PxValue v, uint32_t attrs);
PxValue px_arg(int argc, PxValue *argv, int i);
/* Array indices and lengths: integers up to 2^53, kept in 64-bit integers.
 * On the PSP every double operation is a libgcc call, and an array method
 * does several per element; 64-bit integer arithmetic is a few inline
 * instructions. */
typedef int64_t PxIdx;
static inline PxValue px_idx_value(PxVM *vm, PxIdx i) {
    return i >= PX_SMI_MIN && i <= PX_SMI_MAX ? px_from_smi((int32_t)i) : px_box_number(vm, (double)i);
}
int     px_length_of(PxVM *vm, PxValue obj, PxIdx *out);
int     px_web_init(PxVM *vm); /* structuredClone, atob, btoa */
/* Maps and sets for C code (structuredClone's memo, ...). kind: PX_MAP_* */
PxValue px_map_create(PxVM *vm, int kind);
int     px_map_put(PxVM *vm, PxValue map, PxValue key, PxValue value);
PxValue px_map_lookup(PxVM *vm, PxValue map, PxValue key); /* PX_HOLE: absent */
int     px_itoa(int32_t v, char *buf); /* buf: 12 bytes */
PxValue px_int_to_string(PxVM *vm, int32_t v);
PxValue px_get_index(PxVM *vm, PxValue obj, PxIdx i);
int     px_set_index(PxVM *vm, PxValue obj, PxIdx i, PxValue v);
PxValue px_json_stringify(PxVM *vm, PxValue v, PxValue replacer, PxValue space);

/* ------------------------------------------------------------ GC marking */

void px_mark_value(PxVM *vm, PxValue v);
static inline void px_mark_ptr(PxVM *vm, void *p) {
    if (p) px_mark_value(vm, px_from_ptr(p));
}

#define PX_ROOT(vm, var) px_push_root((vm), &(var))

/* Hot-path hints: rare paths out of line, tiny hot helpers inline */
#if defined(__GNUC__)
#define PX_NOINLINE      __attribute__((noinline))
#define PX_ALWAYS_INLINE inline __attribute__((always_inline))
#define PX_LIKELY(x)     __builtin_expect(!!(x), 1)
#define PX_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#else
#define PX_NOINLINE
#define PX_ALWAYS_INLINE inline
#define PX_LIKELY(x)     (x)
#define PX_UNLIKELY(x)   (x)
#endif

/* PX_PROF(statement): only in a -DPX_PROFILE build (see PxProfile) */
#ifdef PX_PROFILE
#define PX_PROF(x) do { x; } while (0)
#else
#define PX_PROF(x) ((void)0)
#endif
#define PX_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* DEFINE_METHOD kinds */
#define PX_MK_METHOD 0
#define PX_MK_GET    1
#define PX_MK_SET    2
#define PX_MK_ENUM   4 /* enumerable (object literals); class members are not */

/* ------------------------------------------------------------ bytecode */

typedef enum PxOp {
#define OP(name, bytes, effect) OP_##name,
#include "px_opcodes.h"
#undef OP
    OP__COUNT
} PxOp;

extern const uint8_t px_op_size[OP__COUNT];   /* operand bytes */
extern const int8_t  px_op_effect[OP__COUNT];
extern const char   *px_op_name[OP__COUNT];

/* The prototype a primitive's properties come from (String.prototype for
 * a string...), or NULL (undefined, null, symbols): property access on
 * primitives goes through the inline caches like an object's. */
static inline PxObject *px_prim_proto(PxVM *vm, PxValue v) {
    PxValue p;
    if (px_is_smi(v)) p = vm->protos[PX_PROTO_NUMBER];
    else if (px_is_ptr(v)) {
        PxType t = px_type_of(v);
        if (t == PX_T_STRING || t == PX_T_ROPE) p = vm->protos[PX_PROTO_STRING];
        else if (t == PX_T_NUMBER) p = vm->protos[PX_PROTO_NUMBER];
        else return NULL;
    } else if (v == PX_TRUE || v == PX_FALSE) {
        p = vm->protos[PX_PROTO_BOOLEAN];
    } else {
        return NULL;
    }
    return px_is_obj(p) ? (PxObject *)px_ptr(p) : NULL;
}

#endif
