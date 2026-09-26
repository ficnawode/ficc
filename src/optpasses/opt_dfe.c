#include "opt_internal.h"

#include "ir.h"

#include <string.h>

/* Inlining clones a callee's body into its callers but leaves the original
   definition in the module, and codegen lowers every IrFunction.  Drop an
   internal-linkage function once nothing references it: no direct call, no
   is_func operand, no global pointer to its address, and not main.  Exported
   functions and inline definitions stay, since another translation unit may
   link against them (C11 §6.7.4). */

static bool operand_names_func(IrOperand op, const char *name)
{
    return op.is_func && strcmp(op.u.func_name, name) == 0;
}

static bool phi_references_func(IrInstr *phi, const char *name)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (operand_names_func(phi->extra.phi.entries[e].val, name))
        {
            return true;
        }
    }
    return false;
}

static bool instr_references_func(IrInstr *in, const char *name)
{
    if (in->opcode == OP_CALL)
    {
        if (!in->extra.call.is_indirect && in->extra.call.name &&
            strcmp(in->extra.call.name, name) == 0)
        {
            return true;
        }
        if (operand_names_func(in->extra.call.callee, name))
        {
            return true;
        }
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            if (operand_names_func(in->extra.call.args[a], name))
            {
                return true;
            }
        }
    }
    if (in->opcode == OP_PHI)
    {
        return phi_references_func(in, name);
    }
    for (u8 o = 0; o < in->nops; o++)
    {
        if (operand_names_func(in->ops[o], name))
        {
            return true;
        }
    }
    return false;
}

static bool func_referenced(IrModule *mod, const char *name)
{
    size_t nfuncs = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstrs = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstrs; j++)
            {
                if (instr_references_func((IrInstr *) vec_get(bb->instrs, j), name))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool global_references_func(IrModule *mod, const char *name)
{
    size_t nglobals = vec_size(mod->globals);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(mod->globals, i);
        if (!g->relocs)
        {
            continue;
        }
        size_t nrelocs = vec_size(g->relocs);
        for (size_t r = 0; r < nrelocs; r++)
        {
            GlobalReloc *rel = (GlobalReloc *) vec_get(g->relocs, r);
            if (rel->is_func && rel->func_name && strcmp(rel->func_name, name) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

static bool is_removable(IrModule *mod, IrFunction *f)
{
    if (!f->is_static || f->is_inline || strcmp(f->name, "main") == 0)
    {
        return false;
    }
    return !func_referenced(mod, f->name) && !global_references_func(mod, f->name);
}

bool opt_pass_dfe(OptimizerContext *ctx)
{
    IrModule *mod = ctx->mod;
    bool changed = false;
    for (;;)
    {
        size_t nfuncs = vec_size(mod->funcs);
        size_t found = nfuncs;
        for (size_t i = 0; i < nfuncs; i++)
        {
            if (is_removable(mod, (IrFunction *) vec_get(mod->funcs, i)))
            {
                found = i;
                break;
            }
        }
        if (found == nfuncs)
        {
            break;
        }
        for (size_t k = found; k + 1 < nfuncs; k++)
        {
            vec_set(mod->funcs, k, vec_get(mod->funcs, k + 1));
        }
        vec_pop(mod->funcs);
        changed = true;
    }
    return changed;
}
