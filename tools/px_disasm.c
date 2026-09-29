/* A bytecode listing for the pxjs runner's --disasm: what the compiler
 * emitted, function by function. A development tool (host only): it reads
 * the engine's internals directly and is not part of the embedding API. */

#include <stdio.h>

#include "px_internal.h"

static const char *const k_names[] = {
#define OP(name, bytes, effect) #name,
#include "px_opcodes.h"
#undef OP
};
static const unsigned char k_bytes[] = {
#define OP(name, bytes, effect) bytes,
#include "px_opcodes.h"
#undef OP
};

static void print_value(PxVM *vm, PxValue v, FILE *out) {
    char buf[64];
    if (px_is_ptr(v) && px_type_of(v) == PX_T_PROTO) {
        fprintf(out, "<function>");
        return;
    }
    px_inspect(vm, v, buf, sizeof buf);
    fprintf(out, "%s", buf);
}

static void disasm_proto(PxVM *vm, PxProto *p, FILE *out, int depth) {
    const uint8_t *code = p->code->data;
    uint32_t       pc = 0, i, n = 0;
    char           name[64] = "<script>";

    if (depth > 32) return;
    if (px_is_str(p->name)) px_to_utf8(vm, p->name, name, sizeof name);
    fprintf(out, "\n== %s  (params %u, locals %u, upvals %u, max stack %u, line %lu)\n", name[0] ? name : "<anonymous>",
            p->nparams, p->nlocals, p->nupvals, p->max_stack, (unsigned long)p->line);
    while (pc < p->code->len) {
        uint8_t  op = code[pc];
        uint32_t k, operand = 0;
        if (op >= OP__COUNT) {
            fprintf(out, "  %5lu  ??? %u\n", (unsigned long)pc, op);
            break;
        }
        for (k = 0; k < k_bytes[op]; k++) operand |= (uint32_t)code[pc + 1 + k] << (8 * k);
        fprintf(out, "  %5lu  %-22s", (unsigned long)pc, k_names[op]);
        if (k_bytes[op] == 1) fprintf(out, " %lu", (unsigned long)operand);
        else if (k_bytes[op] == 2 && (op == OP_INT16 || op == OP_JUMP || op == OP_LOOP || op == OP_JUMP_IF_FALSE ||
                                      op == OP_JUMP_IF_TRUE || (op >= OP_LT_JUMP_IF_FALSE && op <= OP_SNE_JUMP_IF_FALSE)))
            fprintf(out, " %d", (int)(int16_t)operand);
        else if (k_bytes[op] > 0) fprintf(out, " %lu", (unsigned long)operand);
        if (op == OP_GET_PROP || op == OP_GET_PROP_KEEP || op == OP_SET_PROP) operand &= 0xFFFF;
        if ((op == OP_CONST || op == OP_GET_PROP || op == OP_GET_PROP_KEEP || op == OP_SET_PROP) &&
            operand < p->consts->cap) {
            fprintf(out, "  ; ");
            print_value(vm, p->consts->items[operand], out);
        }
        fputc('\n', out);
        pc += 1 + k_bytes[op];
        n++;
    }
    fprintf(out, "  (%lu instructions, %lu bytes)\n", (unsigned long)n, (unsigned long)p->code->len);
    for (i = 0; p->consts && i < p->consts->cap; i++) {
        PxValue c = p->consts->items[i];
        if (px_is_ptr(c) && px_type_of(c) == PX_T_PROTO) disasm_proto(vm, (PxProto *)px_ptr(c), out, depth + 1);
    }
}

int px_disassemble(PxVM *vm, const char *src, size_t len, const char *filename, FILE *out);

int px_disassemble(PxVM *vm, const char *src, size_t len, const char *filename, FILE *out) {
    PxProto *p = px_compile(vm, src, len, filename, 0);
    PxValue  pv;
    if (!p) return -1;
    pv = px_from_ptr(p);
    PX_ROOT(vm, pv);
    disasm_proto(vm, p, out, 0);
    px_pop_roots(vm, 1);
    return 0;
}
