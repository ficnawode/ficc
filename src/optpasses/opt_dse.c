#include "opt_internal.h"

#include "ir.h"

/* Distinct allocas never alias. */
static u32 alloca_base(OptimizerContext *ctx, IrOperand op)
{
    if (!ir_operand_is_vreg(op))
    {
        return NO_VREG;
    }
    IrInstr *def = ctx->def_vreg[op.u.vreg];
    if (!def)
    {
        return NO_VREG;
    }
    if (def->opcode == OP_ALLOCA)
    {
        return op.u.vreg;
    }
    if (def->opcode == OP_GEP && ir_operand_is_vreg(def->ops[0]))
    {
        IrInstr *base = ctx->def_vreg[def->ops[0].u.vreg];
        if (base && base->opcode == OP_ALLOCA)
        {
            return def->ops[0].u.vreg;
        }
    }
    return NO_VREG;
}

static bool may_alias(u32 a, u32 b)
{
    return a == NO_VREG || b == NO_VREG || a == b;
}

static bool is_call_barrier(IrOpcode op)
{
    return op == OP_CALL || op == OP_MEMCPY || op == OP_VA_START || op == OP_VA_ARG ||
           op == OP_VA_END;
}

static bool operand_touches_alloca(OptimizerContext *ctx, IrOperand op, u32 alloca_vreg)
{
    return ir_operand_is_vreg(op) && op.u.vreg < ctx->mod->width_count &&
           alloca_base(ctx, op) == alloca_vreg;
}

/* True when `vreg` is an address derived (possibly transitively, via GEP) from
   `alloca_vreg`. */
static bool derives_from_alloca(OptimizerContext *ctx, u32 vreg, u32 alloca_vreg)
{
    u32 guard = 0;
    while (ir_operand_is_vreg(ir_operand_vreg(vreg)) && guard++ < ctx->mod->width_count)
    {
        if (vreg == alloca_vreg)
        {
            return true;
        }
        IrInstr *def = ctx->def_vreg[vreg];
        if (!def || def->opcode != OP_GEP || !ir_operand_is_vreg(def->ops[0]))
        {
            return false;
        }
        vreg = def->ops[0].u.vreg;
    }
    return false;
}

/* An alloca whose address is only ever used as a store target, never loaded
   from and never passed anywhere opaque, has no observable contents: every
   store into it is dead. */
static bool alloca_is_store_only(OptimizerContext *ctx, IrFunction *f, u32 alloca_vreg)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->opcode == OP_STORE && !in->extra.mem.is_volatile &&
                ir_operand_is_vreg(in->ops[1]) &&
                derives_from_alloca(ctx, in->ops[1].u.vreg, alloca_vreg))
            {
                continue; /* a store into the alloca is what we may kill */
            }
            for (u8 o = 0; o < in->nops; o++)
            {
                if (operand_touches_alloca(ctx, in->ops[o], alloca_vreg))
                {
                    return false;
                }
            }
            if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    if (operand_touches_alloca(ctx, in->extra.phi.entries[e].val, alloca_vreg))
                    {
                        return false;
                    }
                }
            }
            else if (in->opcode == OP_CALL)
            {
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    if (operand_touches_alloca(ctx, in->extra.call.args[a], alloca_vreg))
                    {
                        return false;
                    }
                }
                if (in->extra.call.is_indirect &&
                    operand_touches_alloca(ctx, in->extra.call.callee, alloca_vreg))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool store_target_is_store_only(OptimizerContext *ctx, IrFunction *f, IrOperand ptr)
{
    if (!ir_operand_is_vreg(ptr))
    {
        return false;
    }
    u32 base = alloca_base(ctx, ptr);
    if (base == NO_VREG)
    {
        return false;
    }
    return alloca_is_store_only(ctx, f, base);
}

bool opt_pass_dse(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        opt_make_value_analysis(ctx, f);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            Vec *dead = vec_new(ctx->scratch);
            size_t n = vec_size(bb->instrs);
            for (size_t i = 0; i < n; i++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->opcode != OP_STORE || in->extra.mem.is_volatile)
                {
                    continue;
                }
                /* A store into a never-read, never-escaping alloca is dead. */
                if (store_target_is_store_only(ctx, f, in->ops[1]))
                {
                    vec_push(dead, in);
                    changed = true;
                    continue;
                }
                u32 width = (u32) in->ops[2].u.imm;
                u32 base = alloca_base(ctx, in->ops[1]);
                for (size_t j = i + 1; j < n; j++)
                {
                    IrInstr *jn = (IrInstr *) vec_get(bb->instrs, j);
                    if (jn->opcode == OP_STORE)
                    {
                        if (jn->extra.mem.is_volatile)
                        {
                            break;
                        }
                        u32 jbase = alloca_base(ctx, jn->ops[1]);
                        if (!may_alias(base, jbase))
                        {
                            continue;
                        }
                        if ((u32) jn->ops[2].u.imm == width &&
                            opt_operand_eq(jn->ops[1], in->ops[1]))
                        {
                            vec_push(dead, in);
                            changed = true;
                        }
                        break;
                    }
                    if (jn->opcode == OP_LOAD)
                    {
                        if (jn->extra.mem.is_volatile ||
                            may_alias(base, alloca_base(ctx, jn->ops[0])))
                        {
                            break;
                        }
                        continue;
                    }
                    if (is_call_barrier(jn->opcode))
                    {
                        break;
                    }
                }
            }
            for (size_t d = 0; d < vec_size(dead); d++)
            {
                opt_erase_instr(bb, (IrInstr *) vec_get(dead, d));
            }
        }
    }
    return changed;
}
