#include "opt_internal.h"

#include "ir.h"

bool opt_pass_cast(OptimizerContext *ctx)
{
    IrModule *mod = ctx->mod;
    bool changed = false;
    size_t nfuncs = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        opt_repl_begin(ctx);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t i = 0; i < ninstr; i++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->result == NO_VREG)
                {
                    continue;
                }
                IrOpcode op = in->opcode;
                if (op != OP_TRUNC && op != OP_ZEXT && op != OP_SEXT)
                {
                    continue;
                }
                IrOperand src = in->ops[0];
                IrOperand result;
                bool rewrite = false;
                if (src.is_imm)
                {
                    /* Only trunc of an imm folds; an imm has no width, so
                       widening casts stay. */
                    if (op == OP_TRUNC)
                    {
                        result = ir_operand_imm(opt_normalize(src.u.imm, mod->widths[in->result],
                                                              mod->signedness[in->result]));
                        rewrite = true;
                    }
                }
                else if (!src.is_global && !src.is_func && src.u.vreg < mod->width_count &&
                         !mod->floatness[src.u.vreg] &&
                         mod->widths[src.u.vreg] == mod->widths[in->result])
                {
                    result = src;
                    rewrite = true;
                }
                if (rewrite)
                {
                    opt_repl_set(ctx, in->result, result);
                    changed = true;
                }
            }
        }
        if (opt_repl_apply(ctx, f))
        {
            changed = true;
        }
    }
    return changed;
}
