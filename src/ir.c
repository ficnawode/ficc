#include "ir.h"
#include "util/assert.h"
#include <stdio.h>
#include <string.h>

const char *ir_opcode_name(IrOpcode op)
{
    switch (op)
    {
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

Function *ir_module_add_func(Module *m, const char *name, Type *ret_type)
{
    Function *f = arena_alloc(m->arena, sizeof(Function), sizeof(void *));
    f->name = name;
    f->ret_type = ret_type;
    f->arena = m->arena;
    f->params = vec_new(m->arena);
    f->blocks = vec_new(m->arena);
    vec_push(m->funcs, f);
    return f;
}

Block *ir_func_add_block(Function *f, const char *label)
{
    Block *bb = arena_alloc(f->arena, sizeof(Block), sizeof(void *));
    bb->label = label;
    bb->arena = f->arena;
    bb->instrs = vec_new(f->arena);
    bb->preds = vec_new(f->arena);
    bb->sealed = false;
    bb->is_loop_header = false;
    vec_push(f->blocks, bb);
    return bb;
}

u32 ir_alloc_vreg(Module *m, u8 width)
{
    if (m->width_count >= m->width_cap)
    {
        u32 new_cap = m->width_cap ? m->width_cap * 2 : 8;
        u8 *new_widths = arena_alloc(m->arena, new_cap, sizeof(u8));
        if (m->widths)
        {
            memcpy(new_widths, m->widths, m->width_count);
        }
        m->widths = new_widths;
        m->width_cap = new_cap;
    }
    m->widths[m->width_count++] = width;
    return m->next_vreg++;
}

static Instr *instr_new(Block *bb, IrOpcode opcode, u32 result, u8 nops)
{
    Instr *ins = arena_alloc(bb->arena, sizeof(Instr), sizeof(void *));
    ins->opcode = opcode;
    ins->result = result;
    ins->nops = nops;
    vec_push(bb->instrs, ins);
    return ins;
}

Instr *ir_emit_ret(Block *bb, Operand val)
{
    Instr *ins = instr_new(bb, OP_RET, NO_VREG, 1);
    ins->ops[0] = val;
    return ins;
}

Instr *ir_emit_unreachable(Block *bb)
{
    return instr_new(bb, OP_UNREACHABLE, NO_VREG, 0);
}

Instr *ir_emit_ret_void(Block *bb)
{
    return instr_new(bb, OP_RET, NO_VREG, 0);
}

static Instr *emit_binop(Block *bb, IrOpcode opcode, u32 dst, Operand lhs, Operand rhs)
{
    Instr *ins = instr_new(bb, opcode, dst, 2);
    ins->ops[0] = lhs;
    ins->ops[1] = rhs;
    return ins;
}

Instr *ir_emit_add(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_ADD, dst, lhs, rhs);
}

Instr *ir_emit_sub(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_SUB, dst, lhs, rhs);
}

Instr *ir_emit_mul(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_MUL, dst, lhs, rhs);
}

Instr *ir_emit_sdiv(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_SDIV, dst, lhs, rhs);
}

Instr *ir_emit_srem(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_SREM, dst, lhs, rhs);
}

Instr *ir_emit_and(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_AND, dst, lhs, rhs);
}

Instr *ir_emit_or(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_OR, dst, lhs, rhs);
}

Instr *ir_emit_xor(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_XOR, dst, lhs, rhs);
}

Instr *ir_emit_shl(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_SHL, dst, lhs, rhs);
}

Instr *ir_emit_ashr(Block *bb, u32 dst, Operand lhs, Operand rhs)
{
    return emit_binop(bb, OP_ASHR, dst, lhs, rhs);
}

Instr *ir_emit_neg(Block *bb, u32 dst, Operand src)
{
    Instr *ins = instr_new(bb, OP_NEG, dst, 1);
    ins->ops[0] = src;
    return ins;
}

Instr *ir_emit_not(Block *bb, u32 dst, Operand src)
{
    Instr *ins = instr_new(bb, OP_NOT, dst, 1);
    ins->ops[0] = src;
    return ins;
}

Instr *ir_emit_icmp(Block *bb, IrOpcode op, u32 dst, Operand lhs, Operand rhs)
{
    Instr *ins = instr_new(bb, op, dst, 2);
    ins->ops[0] = lhs;
    ins->ops[1] = rhs;
    return ins;
}

Instr *ir_emit_call(Block *bb, u32 dst, const char *name, u32 nargs, Operand *args)
{
    Instr *ins = instr_new(bb, OP_CALL, dst, 0);
    ins->extra.call.nargs = nargs;
    ins->extra.call.args = args;
    ins->extra.call.name = name;
    return ins;
}

Instr *ir_emit_br(Block *bb, const char *target_label)
{
    Instr *ins = instr_new(bb, OP_BR, NO_VREG, 0);
    ins->extra.br.target_label = target_label;
    return ins;
}

Instr *ir_emit_brcond(Block *bb, Operand cond, const char *true_label, const char *false_label)
{
    Instr *ins = instr_new(bb, OP_BRCOND, NO_VREG, 1);
    ins->ops[0] = cond;
    ins->extra.brcond.true_label = true_label;
    ins->extra.brcond.false_label = false_label;
    return ins;
}

Instr *ir_emit_phi(Block *bb, u32 dst, u32 nentries)
{
    Instr *ins = instr_new(bb, OP_PHI, dst, 0);
    ins->extra.phi.nentries = nentries;
    ins->extra.phi.nfilled = 0;
    ins->extra.phi.entries = arena_alloc(bb->arena, nentries * sizeof(PhiEntry), sizeof(void *));
    return ins;
}

void ir_phi_add_entry(Instr *phi, Operand val, Block *pred)
{
    ASSERT(phi->opcode == OP_PHI);
    u32 idx = phi->extra.phi.nfilled;
    ASSERT(idx < phi->extra.phi.nentries);
    phi->extra.phi.entries[idx].val = val;
    phi->extra.phi.entries[idx].label = pred->label;
    phi->extra.phi.nfilled++;
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

static void dump_operand(Operand op)
{
    if (op.is_imm)
    {
        printf("imm %lld", (long long) op.u.imm);
    }
    else
    {
        printf("v%u", op.u.vreg);
    }
}

static void dump_phi_entries(Instr *ins)
{
    for (u32 e = 0; e < ins->extra.phi.nentries; e++)
    {
        printf(" [");
        dump_operand(ins->extra.phi.entries[e].val);
        printf(" from %s]", ins->extra.phi.entries[e].label);
    }
}

static void dump_call_args(Instr *ins)
{
    printf(" %s", ins->extra.call.name);
    for (u32 i = 0; i < ins->extra.call.nargs; i++)
    {
        printf(" ");
        dump_operand(ins->extra.call.args[i]);
    }
}

static void dump_switch_cases(Instr *ins)
{
    for (u32 c = 0; c < ins->extra.sw.ncases; c++)
    {
        printf(" [%lld -> %s]", (long long) ins->extra.sw.cases[c].val,
               ins->extra.sw.cases[c].label);
    }
    if (ins->extra.sw.default_label)
    {
        printf(" default:%s", ins->extra.sw.default_label);
    }
}

static void dump_instr(Instr *ins)
{
    printf("    %s", ir_opcode_name(ins->opcode));
    if (ins->result != NO_VREG)
    {
        printf(" v%u =", ins->result);
    }
    for (u8 oi = 0; oi < ins->nops; oi++)
    {
        printf(" ");
        dump_operand(ins->ops[oi]);
    }
    switch (ins->opcode)
    {
        case OP_PHI:
            dump_phi_entries(ins);
            break;
        case OP_CALL:
            dump_call_args(ins);
            break;
        case OP_SWITCH:
            dump_switch_cases(ins);
            break;
        case OP_BR:
            printf(" %s", ins->extra.br.target_label);
            break;
        case OP_BRCOND:
            printf(" true:%s false:%s", ins->extra.brcond.true_label,
                   ins->extra.brcond.false_label);
            break;
        default:
            break;
    }
    printf("\n");
}

void ir_dump(Module *m)
{
    size_t nfuncs = vec_size(m->funcs);
    for (size_t func_i = 0; func_i < nfuncs; func_i++)
    {
        Function *func = (Function *) vec_get(m->funcs, func_i);
        printf("func %s -> %s {\n", func->name, type_kind_name(func->ret_type->kind));
        size_t nblocks = vec_size(func->blocks);
        for (size_t block_i = 0; block_i < nblocks; block_i++)
        {
            Block *bb = (Block *) vec_get(func->blocks, block_i);
            printf("  %s:\n", bb->label);
            size_t ninstrs = vec_size(bb->instrs);
            for (size_t instr_i = 0; instr_i < ninstrs; instr_i++)
            {
                Instr *ins = (Instr *) vec_get(bb->instrs, instr_i);
                dump_instr(ins);
            }
        }
        printf("}\n");
    }
}
