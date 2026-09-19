#include "opt_internal.h"

#include "ir.h"

/* Integer-only 0/1 identities; FP keeps its -0.0 and NaN cracks untouched. */

static bool is_imm_zero(IrOperand op)
{
    return op.is_imm && op.u.imm == 0;
}

static bool is_imm_one(IrOperand op)
{
    return op.is_imm && op.u.imm == 1;
}

bool opt_pass_identity(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t i = 0;
            while (i < vec_size(bb->instrs))
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                IrOpcode op = in->opcode;
                IrOperand result;
                bool rewrite = false;
                switch (op)
                {
                    case OP_ADD:
                        if (is_imm_zero(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        else if (is_imm_zero(in->ops[0]))
                        {
                            result = in->ops[1];
                            rewrite = true;
                        }
                        break;
                    case OP_SUB:
                        if (is_imm_zero(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        else if (opt_operand_eq(in->ops[0], in->ops[1]))
                        {
                            result = ir_operand_imm(0);
                            rewrite = true;
                        }
                        break;
                    case OP_OR:
                    case OP_XOR:
                        if (is_imm_zero(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        else if (is_imm_zero(in->ops[0]))
                        {
                            result = in->ops[1];
                            rewrite = true;
                        }
                        break;
                    case OP_SHL:
                    case OP_LSHR:
                    case OP_ASHR:
                        if (is_imm_zero(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        break;
                    case OP_MUL:
                        if (is_imm_one(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        else if (is_imm_one(in->ops[0]))
                        {
                            result = in->ops[1];
                            rewrite = true;
                        }
                        else if (is_imm_zero(in->ops[1]) || is_imm_zero(in->ops[0]))
                        {
                            result = ir_operand_imm(0);
                            rewrite = true;
                        }
                        break;
                    case OP_SDIV:
                    case OP_UDIV:
                        if (is_imm_one(in->ops[1]))
                        {
                            result = in->ops[0];
                            rewrite = true;
                        }
                        break;
                    case OP_SREM:
                    case OP_UREM:
                        if (is_imm_one(in->ops[1]))
                        {
                            result = ir_operand_imm(0);
                            rewrite = true;
                        }
                        break;
                    default:
                        break;
                }
                if (rewrite)
                {
                    opt_replace_def(ctx, f, in, result);
                    changed = true;
                }
                else
                {
                    i++;
                }
            }
        }
    }
    return changed;
}