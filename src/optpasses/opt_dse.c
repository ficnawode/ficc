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
