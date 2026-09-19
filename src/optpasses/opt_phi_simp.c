#include "opt_internal.h"

#include "ir.h"

/* Collapse phis whose values all agree; self entries make the value invariant. */

bool opt_pass_phi_simp(OptimizerContext *ctx)
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
                if (in->opcode != OP_PHI)
                {
                    break;
                }
                IrOperand common;
                bool have = false;
                bool conflict = false;
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    IrOperand val = in->extra.phi.entries[e].val;
                    if (!val.is_imm && !val.is_global && !val.is_func && val.u.vreg == in->result)
                    {
                        continue; /* self entry: the latch keeps the value */
                    }
                    if (have)
                    {
                        if (!opt_operand_eq(common, val))
                        {
                            conflict = true;
                        }
                    }
                    else
                    {
                        common = val;
                        have = true;
                    }
                }
                if (have && !conflict)
                {
                    opt_replace_def(ctx, f, in, common);
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