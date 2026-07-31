#include "ir.h"
#include <stdio.h>
#include <string.h>

const char *ir_opcode_name(IrOpcode op)
{
    switch (op) {
#define CASE(K)                                                                                    \
    case K:                                                                                        \
        return #K;
        IR_OPCODES(CASE)
#undef CASE
    }
    return "OP_UNKNOWN";
}

Module *ir_module_new(Arena *arena)
{
    Module *m = arena_alloc(arena, sizeof(Module), sizeof(void *));
    m->arena = arena;
    m->funcs = vec_new(arena);
    m->globals = vec_new(arena);
    m->widths = NULL;
    m->width_count = 0;
    m->width_cap = 0;
    m->next_vreg = 0;
    return m;
}

Function *ir_module_add_func(Module *m, Arena *arena, const char *name, Type *ret_type)
{
    Function *f = arena_alloc(arena, sizeof(Function), sizeof(void *));
    f->name = name;
    f->ret_type = ret_type;
    f->params = vec_new(arena);
    f->blocks = vec_new(arena);
    vec_push(m->funcs, f);
    return f;
}

Block *ir_func_add_block(Function *f, Arena *arena, const char *label)
{
    Block *bb = arena_alloc(arena, sizeof(Block), sizeof(void *));
    bb->label = label;
    bb->instrs = vec_new(arena);
    vec_push(f->blocks, bb);
    return bb;
}

u32 ir_alloc_vreg(Module *m, u8 width)
{
    if (m->width_count >= m->width_cap) {
        u32 new_cap = m->width_cap ? m->width_cap * 2 : 8;
        u8 *new_widths = arena_alloc(m->arena, new_cap, sizeof(u8));
        if (m->widths)
            memcpy(new_widths, m->widths, m->width_count);
        m->widths = new_widths;
        m->width_cap = new_cap;
    }
    m->widths[m->width_count++] = width;
    return m->next_vreg++;
}

Instr *ir_emit_ret(Block *b, Arena *arena, Operand val)
{
    Instr *i = arena_alloc(arena, sizeof(Instr), sizeof(void *));
    i->opcode = OP_RET;
    i->result = NO_VREG;
    i->nops = 1;
    i->ops[0] = val;
    vec_push(b->instrs, i);
    return i;
}

Instr *ir_emit_unreachable(Block *b, Arena *arena)
{
    Instr *i = arena_alloc(arena, sizeof(Instr), sizeof(void *));
    i->opcode = OP_UNREACHABLE;
    i->result = NO_VREG;
    i->nops = 0;
    vec_push(b->instrs, i);
    return i;
}

Operand ir_operand_imm(i64 val)
{
    Operand o;
    o.is_imm = true;
    o.u.imm = val;
    return o;
}

Operand ir_operand_vreg(u32 vreg)
{
    Operand o;
    o.is_imm = false;
    o.u.vreg = vreg;
    return o;
}

static void operand_dump(Operand o)
{
    if (o.is_imm)
        printf("imm %lld", (long long)o.u.imm);
    else
        printf("v%u", o.u.vreg);
}

void ir_dump(Module *m)
{
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++) {
        Function *f = (Function *)vec_get(m->funcs, fi);
        printf("func %s -> %s {\n", f->name, type_kind_name(f->ret_type->kind));
        size_t nblocks = vec_size(f->blocks);
        for (size_t bi = 0; bi < nblocks; bi++) {
            Block *bb = (Block *)vec_get(f->blocks, bi);
            printf("  %s:\n", bb->label);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t ii = 0; ii < ninstr; ii++) {
                Instr *in = (Instr *)vec_get(bb->instrs, ii);
                printf("    %s", ir_opcode_name(in->opcode));
                if (in->result != NO_VREG)
                    printf(" v%u =", in->result);
                for (u8 oi = 0; oi < in->nops; oi++) {
                    printf(" ");
                    operand_dump(in->ops[oi]);
                }
                printf("\n");
            }
        }
        printf("}\n");
    }
}
