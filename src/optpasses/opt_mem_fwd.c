#include "opt_internal.h"

#include "ir.h"

#include <stddef.h>

typedef enum
{
    FWD_STORE,
    FWD_LOAD,
} FwdFactKind;

typedef struct
{
    FwdFactKind kind;
    IrOperand ptr;
    IrOperand val;
    u8 width;
    bool is_signed;
    bool is_float;
} FwdFact;

static void fact_clear_fields(FwdFact *fact)
{
    fact->width = 0;
    fact->is_signed = false;
    fact->is_float = false;
}

static void facts_clear(Vec *facts)
{
    while (vec_size(facts) > 0)
    {
        vec_pop(facts);
    }
}

/* A load masks to its own width and sign, so only a canonical value forwards. */
static bool store_value_reproducible(const IrModule *mod, IrOperand val, u8 w, bool is_signed)
{
    if (val.is_imm)
    {
        return opt_normalize(val.u.imm, w, is_signed) == val.u.imm;
    }
    if (val.is_global || val.is_func)
    {
        return false;
    }
    if (val.u.vreg >= mod->width_count)
    {
        return false;
    }
    u8 vw = mod->widths[val.u.vreg];
    return vw == w && ir_vreg_signed(mod, val.u.vreg) == is_signed && !mod->floatness[val.u.vreg];
}

bool opt_pass_mem_fwd(OptimizerContext *ctx)
{
    IrModule *mod = ctx->mod;
    bool changed = false;
    size_t nfuncs = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        opt_repl_begin(ctx);
        size_t nblocks = vec_size(f->blocks);
        for (size_t bi = 0; bi < nblocks; bi++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, bi);
            Vec *facts = vec_new(ctx->scratch);
            size_t i = 0;
            while (i < vec_size(bb->instrs))
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                IrOpcode op = in->opcode;
                if (op == OP_CALL || op == OP_MEMCPY || op == OP_VA_START || op == OP_VA_ARG ||
                    op == OP_VA_END)
                {
                    facts_clear(facts);
                    i++;
                    continue;
                }
                if (op == OP_STORE)
                {
                    facts_clear(facts);
                    if (in->extra.mem.is_volatile)
                    {
                        i++;
                        continue;
                    }
                    u8 w = (u8) in->ops[2].u.imm;
                    if (w != 16)
                    {
                        FwdFact *fact =
                            arena_alloc(ctx->scratch, sizeof(FwdFact), _Alignof(FwdFact));
                        fact_clear_fields(fact);
                        fact->kind = FWD_STORE;
                        fact->ptr = in->ops[1];
                        fact->val = in->ops[0];
                        fact->width = w;
                        vec_push(facts, fact);
                    }
                    i++;
                    continue;
                }
                if (op == OP_LOAD)
                {
                    if (in->extra.mem.is_volatile)
                    {
                        facts_clear(facts);
                        i++;
                        continue;
                    }
                    IrOperand ptr = in->ops[0];
                    u8 w = mod->widths[in->result];
                    bool is_signed = ir_vreg_signed(mod, in->result);
                    bool fwd = false;
                    size_t nfacts = vec_size(facts);
                    for (size_t k = 0; k < nfacts; k++)
                    {
                        FwdFact *fact = (FwdFact *) vec_get(facts, k);
                        if (!opt_operand_eq(fact->ptr, ptr))
                        {
                            continue;
                        }
                        if (fact->kind == FWD_STORE && fact->width == w &&
                            store_value_reproducible(mod, fact->val, w, is_signed))
                        {
                            opt_repl_set(ctx, in->result, fact->val);
                            changed = true;
                            fwd = true;
                            break;
                        }
                        if (fact->kind == FWD_LOAD && fact->width == w &&
                            fact->is_signed == is_signed &&
                            fact->is_float == mod->floatness[in->result])
                        {
                            opt_repl_set(ctx, in->result, fact->val);
                            changed = true;
                            fwd = true;
                            break;
                        }
                    }
                    if (fwd)
                    {
                        i++;
                        continue;
                    }
                    if (w != 16)
                    {
                        FwdFact *fact =
                            arena_alloc(ctx->scratch, sizeof(FwdFact), _Alignof(FwdFact));
                        fact_clear_fields(fact);
                        fact->kind = FWD_LOAD;
                        fact->ptr = ptr;
                        fact->val = ir_operand_vreg(in->result);
                        fact->width = w;
                        fact->is_signed = is_signed;
                        fact->is_float = mod->floatness[in->result];
                        vec_push(facts, fact);
                    }
                    i++;
                    continue;
                }
                i++;
            }
        }
        if (opt_repl_apply(ctx, f))
        {
            changed = true;
        }
    }
    return changed;
}
