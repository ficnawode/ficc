#include "ir.h"
#include "util/assert.h"
#include <ctype.h>
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

IrModule *ir_module_new(Arena *arena)
{
    IrModule *m = arena_alloc(arena, sizeof(IrModule), sizeof(void *));
    m->arena = arena;
    m->funcs = vec_new(arena);
    m->globals = vec_new(arena);
    m->widths = NULL;
    m->signedness = NULL;
    m->floatness = NULL;
    m->width_count = 0;
    m->width_cap = 0;
    m->next_vreg = 0;
    return m;
}

IrFunction *ir_module_add_func(IrModule *m, const char *name, Type *ret_type)
{
    IrFunction *f = arena_alloc(m->arena, sizeof(IrFunction), sizeof(void *));
    f->name = name;
    f->ret_type = ret_type;
    f->arena = m->arena;
    f->params = vec_new(m->arena);
    f->blocks = vec_new(m->arena);
    f->locals = vec_new(m->arena);
    f->cur_line = 0;
    vec_push(m->funcs, f);
    return f;
}

IrLocal *ir_func_add_local(IrFunction *f, const char *name, Type *type)
{
    IrLocal *l = arena_alloc(f->arena, sizeof(IrLocal), sizeof(void *));
    l->name = name;
    l->type = type;
    l->vregs = vec_new(f->arena);
    vec_push(f->locals, l);
    return l;
}

IrBlock *ir_func_add_block(IrFunction *f, const char *label)
{
    IrBlock *bb = arena_alloc(f->arena, sizeof(IrBlock), sizeof(void *));
    bb->label = label;
    bb->func = f;
    bb->arena = f->arena;
    bb->instrs = vec_new(f->arena);
    bb->preds = vec_new(f->arena);
    bb->sealed = false;
    bb->is_loop_header = false;
    bb->index = (u32) vec_size(f->blocks);
    vec_push(f->blocks, bb);
    return bb;
}

void ir_func_remove_block(IrFunction *f, IrBlock *bb)
{
    size_t n = vec_size(f->blocks);
    ASSERT(bb->func == f && bb->index < n && vec_get(f->blocks, bb->index) == bb);
    for (size_t i = bb->index; i + 1 < n; i++)
    {
        IrBlock *moved = (IrBlock *) vec_get(f->blocks, i + 1);
        vec_set(f->blocks, i, moved);
        moved->index = (u32) i;
    }
    vec_pop(f->blocks);
}

u32 ir_alloc_vreg(IrModule *m, u8 width, bool is_signed, bool is_float)
{
    if (m->width_count >= m->width_cap)
    {
        u32 new_cap = m->width_cap ? m->width_cap * 2 : 8;
        u8 *new_widths = arena_alloc(m->arena, new_cap, sizeof(u8));
        bool *new_signed = arena_alloc(m->arena, new_cap, sizeof(bool));
        bool *new_float = arena_alloc(m->arena, new_cap, sizeof(bool));
        if (m->widths)
        {
            memcpy(new_widths, m->widths, m->width_count);
            memcpy(new_signed, m->signedness, m->width_count);
            memcpy(new_float, m->floatness, m->width_count);
        }
        m->widths = new_widths;
        m->signedness = new_signed;
        m->floatness = new_float;
        m->width_cap = new_cap;
    }
    m->widths[m->width_count] = width;
    m->signedness[m->width_count] = is_signed;
    m->floatness[m->width_count] = is_float;
    m->width_count++;
    return m->next_vreg++;
}

u32 ir_alloc_fp_vreg(IrModule *m, u8 width)
{
    return ir_alloc_vreg(m, width, false, true);
}

bool ir_vreg_signed(const IrModule *m, u32 vreg)
{
    ASSERT(vreg < m->width_count);
    return m->signedness[vreg];
}

bool ir_vreg_float(const IrModule *m, u32 vreg)
{
    ASSERT(vreg < m->width_count);
    return m->floatness[vreg];
}

static IrInstr *instr_new(IrBlock *bb, IrOpcode opcode, u32 result, u8 nops)
{
    ASSERT(bb->func && "every block belongs to a function");
    IrInstr *ins = arena_alloc(bb->arena, sizeof(IrInstr), sizeof(void *));
    ins->opcode = opcode;
    ins->result = result;
    ins->line = bb->func->cur_line;
    ins->nops = nops;
    ins->frame_off = 0;
    vec_push(bb->instrs, ins);
    return ins;
}

IrInstr *ir_emit_ret(IrBlock *bb, IrOperand val)
{
    IrInstr *ins = instr_new(bb, OP_RET, NO_VREG, 1);
    ins->ops[0] = val;
    return ins;
}

IrInstr *ir_emit_unreachable(IrBlock *bb)
{
    return instr_new(bb, OP_UNREACHABLE, NO_VREG, 0);
}

IrInstr *ir_emit_ret_void(IrBlock *bb)
{
    return instr_new(bb, OP_RET, NO_VREG, 0);
}

/* The opcode carries the operation; only binops, icmp predicates, FP arithmetic
   and fcmp predicates belong. */
IrInstr *ir_emit_binop(IrBlock *bb, IrOpcode op, u32 dst, IrOperand lhs, IrOperand rhs)
{
    ASSERT((op >= OP_ADD && op <= OP_ASHR) || (op >= OP_ICMP_EQ && op <= OP_ICMP_SGE) ||
           (op >= OP_FADD && op <= OP_FDIV) || (op >= OP_FCMP_EQ && op <= OP_FCMP_GE));
    IrInstr *ins = instr_new(bb, op, dst, 2);
    ins->ops[0] = lhs;
    ins->ops[1] = rhs;
    return ins;
}

IrInstr *ir_emit_unary(IrBlock *bb, IrOpcode op, u32 dst, IrOperand src)
{
    ASSERT(op == OP_NEG || op == OP_NOT || op == OP_TRUNC || op == OP_ZEXT || op == OP_SEXT ||
           op == OP_ITOF || op == OP_FTOI || op == OP_FCONV || op == OP_FNEG);
    IrInstr *ins = instr_new(bb, op, dst, 1);
    ins->ops[0] = src;
    return ins;
}

IrInstr *ir_emit_call(IrBlock *bb, u32 dst, const char *name, u32 nargs, IrOperand *args)
{
    IrInstr *ins = instr_new(bb, OP_CALL, dst, 0);
    ins->extra.call.nargs = nargs;
    ins->extra.call.args = args;
    ins->extra.call.name = name;
    ins->extra.call.is_variadic = false;
    ins->extra.call.is_indirect = false;
    ins->extra.call.callee = ir_operand_imm(0);
    ins->extra.call.arg_types = NULL;
    ins->extra.call.ret_type = NULL;
    return ins;
}

/* One by-value type per argument plus the return type; records stay records. */
void ir_call_set_types(IrInstr *call, Type **arg_types, Type *ret_type)
{
    ASSERT(call->opcode == OP_CALL);
    ASSERT(arg_types != NULL && "a call carries an argument-type vector");
    call->extra.call.arg_types = arg_types;
    call->extra.call.ret_type = ret_type;
}

void ir_call_set_variadic(IrInstr *call, bool is_variadic)
{
    ASSERT(call->opcode == OP_CALL);
    call->extra.call.is_variadic = is_variadic;
}

/* Mark a call as *indirect*: the target is the runtime value of the
   `callee` operand (a function pointer), not the named symbol `name` (which is
   then ignored). Append-only — every existing direct call keeps byte-identical
   lowering. */
void ir_call_set_indirect(IrInstr *call, IrOperand callee)
{
    ASSERT(call->opcode == OP_CALL);
    call->extra.call.is_indirect = true;
    call->extra.call.callee = callee;
}

/* va_start(ap, last): write the four va_list fields; ap rides ops[0], the three offsets live in
 * extra.va_start (they overflow the 3-slot ops array). */
IrInstr *ir_emit_va_start(IrBlock *bb, IrOperand ap, i64 stack_skip, i64 gp_offset, i64 fp_offset)
{
    IrInstr *ins = instr_new(bb, OP_VA_START, NO_VREG, 1);
    ins->ops[0] = ap;
    ins->extra.va_start.stack_skip = stack_skip;
    ins->extra.va_start.gp_offset = gp_offset;
    ins->extra.va_start.fp_offset = fp_offset;
    return ins;
}

/* __builtin_va_arg(ap, type): advance the ap to the next argument and fetch
   its full 8-byte slot into the width-8 result vreg; the builder converts to
   the requested type afterwards. */
IrInstr *ir_emit_va_arg(IrBlock *bb, u32 dst, IrOperand ap)
{
    IrInstr *ins = instr_new(bb, OP_VA_ARG, dst, 1);
    ins->ops[0] = ap;
    return ins;
}

/* __builtin_va_end(ap): a no-op in both backends (kept for source symmetry
   and the future va_copy). */
IrInstr *ir_emit_va_end(IrBlock *bb, IrOperand ap)
{
    IrInstr *ins = instr_new(bb, OP_VA_END, NO_VREG, 1);
    ins->ops[0] = ap;
    return ins;
}

IrInstr *ir_emit_br(IrBlock *bb, const char *target_label)
{
    IrInstr *ins = instr_new(bb, OP_BR, NO_VREG, 0);
    ins->extra.br.target_label = target_label;
    return ins;
}

IrInstr *ir_emit_brcond(IrBlock *bb, IrOperand cond, const char *true_label,
                        const char *false_label)
{
    IrInstr *ins = instr_new(bb, OP_BRCOND, NO_VREG, 1);
    ins->ops[0] = cond;
    ins->extra.brcond.true_label = true_label;
    ins->extra.brcond.false_label = false_label;
    return ins;
}

IrInstr *ir_emit_phi(IrBlock *bb, u32 dst, u32 nentries)
{
    IrInstr *ins = instr_new(bb, OP_PHI, dst, 0);
    ins->extra.phi.nentries = nentries;
    ins->extra.phi.nfilled = 0;
    ins->extra.phi.entries = arena_alloc(bb->arena, nentries * sizeof(IrPhiEntry), sizeof(void *));
    return ins;
}

/* A PHI must precede every other instruction in its block. instr_new appends,
   so emit and then move to the front. */
IrInstr *ir_emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries)
{
    IrInstr *phi = ir_emit_phi(bb, dst, nentries);
    IrInstr *last = (IrInstr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
    return phi;
}

void ir_phi_add_entry(IrInstr *phi, IrOperand val, IrBlock *pred)
{
    ASSERT(phi->opcode == OP_PHI);
    u32 idx = phi->extra.phi.nfilled;
    ASSERT(idx < phi->extra.phi.nentries);
    phi->extra.phi.entries[idx].val = val;
    phi->extra.phi.entries[idx].label = pred->label;
    phi->extra.phi.nfilled++;
}

IrInstr *ir_emit_switch(IrBlock *bb, IrOperand val, u32 ncases, IrSwitchCase *cases,
                        const char *default_label)
{
    IrInstr *ins = instr_new(bb, OP_SWITCH, NO_VREG, 1);
    ins->ops[0] = val;
    ins->extra.sw.ncases = ncases;
    ins->extra.sw.cases = cases;
    ins->extra.sw.default_label = default_label;
    return ins;
}

IrInstr *ir_emit_load(IrBlock *bb, u32 dst, IrOperand ptr, bool is_volatile)
{
    IrInstr *ins = instr_new(bb, OP_LOAD, dst, 1);
    ins->ops[0] = ptr;
    ins->extra.mem.is_volatile = is_volatile;
    return ins;
}

IrInstr *ir_emit_store(IrBlock *bb, IrOperand val, IrOperand ptr, u32 width_bytes, bool is_volatile)
{
    IrInstr *ins = instr_new(bb, OP_STORE, NO_VREG, 3);
    ins->ops[0] = val;
    ins->ops[1] = ptr;
    ins->ops[2] = ir_operand_imm((i64) width_bytes);
    ins->extra.mem.is_volatile = is_volatile;
    return ins;
}

IrInstr *ir_emit_gep(IrBlock *bb, u32 dst, IrOperand base, IrOperand index, u32 stride)
{
    IrInstr *ins = instr_new(bb, OP_GEP, dst, 3);
    ins->ops[0] = base;
    ins->ops[1] = index;
    ins->ops[2] = ir_operand_imm((i64) stride);
    return ins;
}

IrInstr *ir_emit_alloca(IrBlock *bb, u32 dst, u32 size_bytes)
{
    IrInstr *ins = instr_new(bb, OP_ALLOCA, dst, 1);
    ins->ops[0] = ir_operand_imm((i64) size_bytes);
    return ins;
}

IrInstr *ir_emit_memcpy(IrBlock *bb, IrOperand dst, IrOperand src, u32 size_bytes)
{
    IrInstr *ins = instr_new(bb, OP_MEMCPY, NO_VREG, 3);
    ins->ops[0] = dst;
    ins->ops[1] = src;
    ins->ops[2] = ir_operand_imm((i64) size_bytes);
    return ins;
}

IrOperand ir_operand_imm(i64 val)
{
    IrOperand o;
    o.is_imm = true;
    o.is_global = false;
    o.is_func = false;
    o.u.imm = val;
    return o;
}

IrOperand ir_operand_vreg(u32 vreg)
{
    IrOperand o;
    o.is_imm = false;
    o.is_global = false;
    o.is_func = false;
    o.u.vreg = vreg;
    return o;
}

IrOperand ir_operand_global(u32 global_index)
{
    IrOperand o;
    o.is_imm = false;
    o.is_global = true;
    o.is_func = false;
    o.u.global_index = global_index;
    return o;
}

IrOperand ir_operand_func(const char *func_name)
{
    IrOperand o;
    o.is_imm = false;
    o.is_global = false;
    o.is_func = true;
    o.u.func_name = func_name;
    return o;
}

bool ir_operand_is_vreg(IrOperand op)
{
    return !op.is_imm && !op.is_global && !op.is_func;
}

static void dump_operand(IrOperand op)
{
    if (op.is_global)
    {
        printf("g%u", op.u.global_index);
    }
    else if (op.is_func)
    {
        printf("@%s", op.u.func_name);
    }
    else if (op.is_imm)
    {
        printf("%lld", (long long) op.u.imm);
    }
    else
    {
        printf("v%u", op.u.vreg);
    }
}

static void dump_phi_entries(IrInstr *ins)
{
    for (u32 e = 0; e < ins->extra.phi.nentries; e++)
    {
        printf(" [");
        dump_operand(ins->extra.phi.entries[e].val);
        printf(" <- %s]", ins->extra.phi.entries[e].label);
    }
}

static void dump_call_args(IrInstr *ins)
{
    if (ins->extra.call.is_indirect)
    {
        printf(" [@");
        dump_operand(ins->extra.call.callee);
        printf("]");
    }
    else
    {
        printf(" %s", ins->extra.call.name);
    }
    for (u32 i = 0; i < ins->extra.call.nargs; i++)
    {
        printf(", ");
        dump_operand(ins->extra.call.args[i]);
    }
    if (ins->extra.call.is_variadic)
    {
        printf(" [variadic]");
    }
}

static void dump_switch_cases(IrInstr *ins)
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

static void dump_instr(IrInstr *ins, IrModule *m)
{
    printf("    ");
    if (ins->result != NO_VREG)
    {
        printf("v%u:w%u%s = ", ins->result, m->widths[ins->result],
               m->floatness[ins->result] ? "f" : "");
    }

    const char *name = ir_opcode_name(ins->opcode) + 3;
    for (const char *p = name; *p; p++)
    {
        putchar((char) tolower((unsigned char) *p));
    }

    for (u8 oi = 0; oi < ins->nops; oi++)
    {
        printf("%s", oi == 0 ? " " : ", ");
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
        case OP_VA_START:
            printf(", stack_skip=%lld, gp_offset=%lld, fp_offset=%lld",
                   (long long) ins->extra.va_start.stack_skip,
                   (long long) ins->extra.va_start.gp_offset,
                   (long long) ins->extra.va_start.fp_offset);
            break;
        case OP_VA_END:
            printf(" ap");
            break;
        case OP_SWITCH:
            dump_switch_cases(ins);
            break;
        case OP_BR:
            printf(" %s", ins->extra.br.target_label);
            break;
        case OP_BRCOND:
            printf(", %s, %s", ins->extra.brcond.true_label, ins->extra.brcond.false_label);
            break;
        default:
            break;
    }
    printf("\n");
}

void ir_dump(IrModule *m)
{
    size_t nfuncs = vec_size(m->funcs);
    for (size_t func_i = 0; func_i < nfuncs; func_i++)
    {
        IrFunction *func = (IrFunction *) vec_get(m->funcs, func_i);
        printf("func %s -> %s {\n", func->name, type_kind_name(func->ret_type->kind));
        size_t nblocks = vec_size(func->blocks);
        for (size_t block_i = 0; block_i < nblocks; block_i++)
        {
            IrBlock *bb = (IrBlock *) vec_get(func->blocks, block_i);
            printf("  %s:\n", bb->label);
            size_t ninstrs = vec_size(bb->instrs);
            for (size_t instr_i = 0; instr_i < ninstrs; instr_i++)
            {
                IrInstr *ins = (IrInstr *) vec_get(bb->instrs, instr_i);
                dump_instr(ins, m);
            }
        }
        printf("}\n");
    }
}
