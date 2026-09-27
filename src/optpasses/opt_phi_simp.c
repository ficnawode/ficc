#include "opt_internal.h"

#include "ir.h"

bool opt_pass_phi_simp(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        opt_repl_begin(ctx);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t i = 0; i < ninstr; i++)
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
                        continue;
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
                    opt_repl_set(ctx, in->result, common);
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
