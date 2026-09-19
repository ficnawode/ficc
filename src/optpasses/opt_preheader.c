#include "opt_internal.h"

#include "ir.h"

/* Loop canonical shape as a canon rule (at most one latch, one outside pred). */

bool opt_pass_preheader(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        LoopInfo *loops = opt_get_loops(ctx, f);
        if (vec_size(loops->loops) == 0)
        {
            continue;
        }
        changed |= opt_loops_canonicalize(ctx->mod, f, loops);
    }
    return changed;
}