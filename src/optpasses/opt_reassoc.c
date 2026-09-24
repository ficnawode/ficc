#include "opt_internal.h"

#include "ir.h"

static bool is_assoc_op(IrOpcode op)
{
    return op == OP_ADD || op == OP_SUB || op == OP_MUL;
}

/* The constant addend an ADD/SUB tree reduces to: (a ± c1) ± c2 -> a + A. */
static bool combine_addsub(IrOpcode outer, IrOpcode inner, i64 c1, i64 c2, i64 *addend)
{
    if (outer == OP_ADD)
    {
        *addend = inner == OP_ADD ? c1 + c2 : c2 - c1;
        return true;
    }
    *addend = inner == OP_ADD ? c1 - c2 : -(c1 + c2);
    return true;
}

/* The `t OP c` shape: the constant operand and the value operand. */
static bool imm_operand(IrInstr *in, IrOperand *val, i64 *c)
{
    if (in->ops[1].is_imm)
    {
        *val = in->ops[0];
        *c = in->ops[1].u.imm;
        return true;
    }
    if (in->opcode == OP_ADD && in->ops[0].is_imm)
    {
        *val = in->ops[1];
        *c = in->ops[0].u.imm;
        return true;
    }
    return false;
}

/* Fold (a OP1 c1) OP2 c2 into a single a OP c when both are constants. The
   inner definition is left in place (DCE removes it when it dies). */
bool opt_pass_reassoc(OptimizerContext *ctx)
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
            size_t ninstr = vec_size(bb->instrs);
            for (size_t i = 0; i < ninstr; i++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->result == NO_VREG || !is_assoc_op(in->opcode))
                {
                    continue;
                }
                IrOperand t;
                i64 c2 = 0;
                if (!imm_operand(in, &t, &c2) || !ir_operand_is_vreg(t))
                {
                    continue;
                }
                IrInstr *inner = ctx->def_vreg[t.u.vreg];
                if (!inner || !is_assoc_op(inner->opcode) || ctx->use_count[t.u.vreg] != 1)
                {
                    continue;
                }
                if ((in->opcode == OP_MUL) != (inner->opcode == OP_MUL))
                {
                    continue; /* multiply and add/subtract do not mix */
                }
                IrOperand a;
                i64 c1 = 0;
                if (!imm_operand(inner, &a, &c1))
                {
                    continue;
                }
                if (in->opcode == OP_MUL)
                {
                    in->opcode = OP_MUL;
                    in->ops[0] = a;
                    in->ops[1] = ir_operand_imm(c1 * c2);
                }
                else
                {
                    i64 addend = 0;
                    combine_addsub(in->opcode, inner->opcode, c1, c2, &addend);
                    in->opcode = OP_ADD;
                    in->ops[0] = a;
                    in->ops[1] = ir_operand_imm(addend);
                }
                changed = true;
            }
        }
    }
    return changed;
}
