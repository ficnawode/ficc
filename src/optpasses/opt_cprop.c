#include "opt_internal.h"

#include "ir.h"

/* Copy elimination: a def provably equal to an operand becomes that operand. */

bool opt_pass_cprop(OptimizerContext *ctx)
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
            size_t i = 0;
            while (i < vec_size(bb->instrs))
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                IrOperand src;
                bool copy = false;
                if (in->opcode == OP_NEG && !in->ops[0].is_imm && !in->ops[0].is_global &&
                    !in->ops[0].is_func)
                {
                    IrInstr *inner = ctx->def_vreg[in->ops[0].u.vreg];
                    if (inner && inner->opcode == OP_NEG)
                    {
                        src = inner->ops[0];
                        copy = true;
                    }
                }
                else if ((in->opcode == OP_TRUNC || in->opcode == OP_ZEXT ||
                          in->opcode == OP_SEXT) &&
                         !in->ops[0].is_imm && !in->ops[0].is_global && !in->ops[0].is_func &&
                         !ctx->mod->floatness[in->ops[0].u.vreg] &&
                         ctx->mod->widths[in->ops[0].u.vreg] == ctx->mod->widths[in->result])
                {
                    src = in->ops[0];
                    copy = true;
                }
                if (copy)
                {
                    opt_replace_def(ctx, f, in, src);
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