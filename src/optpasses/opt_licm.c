#include "opt_internal.h"

#include "ir.h"
#include "util/hashmap.h"

#include <stdint.h>

/* Hoist loop-invariant pure defs to each loop's preheader. */

static u64 ptr_hash(const void *key)
{
    return (u64) (uintptr_t) key;
}

static bool ptr_eq(const void *a, const void *b)
{
    return a == b;
}

/* def_block/def_instr per vreg; params are "defined" at the entry block. */
static void build_def_maps(IrFunction *f, IrBlock **def_block, IrInstr **def_instr)
{
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    size_t nparams = vec_size(f->params);
    for (size_t p = 0; p < nparams; p++)
    {
        IrParam *param = (IrParam *) vec_get(f->params, p);
        def_block[param->vreg] = entry;
        def_instr[param->vreg] = NULL;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->result != NO_VREG)
            {
                def_block[in->result] = bb;
                def_instr[in->result] = in;
            }
        }
    }
}

static bool is_hoistable(IrInstr *in)
{
    switch (in->opcode)
    {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_LSHR:
        case OP_ASHR:
        case OP_NEG:
        case OP_NOT:
        case OP_TRUNC:
        case OP_ZEXT:
        case OP_SEXT:
        case OP_GEP:
        case OP_ICMP_EQ:
        case OP_ICMP_NE:
        case OP_ICMP_ULT:
        case OP_ICMP_ULE:
        case OP_ICMP_UGT:
        case OP_ICMP_UGE:
        case OP_ICMP_SLT:
        case OP_ICMP_SLE:
        case OP_ICMP_SGT:
        case OP_ICMP_SGE:
        case OP_ITOF:
        case OP_FTOI:
        case OP_FCONV:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FNEG:
        case OP_FCMP_EQ:
        case OP_FCMP_NE:
        case OP_FCMP_LT:
        case OP_FCMP_GT:
        case OP_FCMP_LE:
        case OP_FCMP_GE:
            return true;
        default:
            return false;
    }
}

static bool in_set(const HashSet *set, const void *item)
{
    return hashset_contains(set, item);
}

/* True when every vreg operand's def sits outside the loop body. A def this
   run will hoist stays invariant: both land in the preheader, and the RPO
   drain places defs before their uses there. */
static bool operands_invariant(const IrInstr *in, IrBlock *pre, const HashSet *loop_blocks,
                               IrBlock **def_block, IrInstr **def_instr, const HashSet *hoisted)
{
    for (u8 o = 0; o < in->nops; o++)
    {
        IrOperand op = in->ops[o];
        if (op.is_imm || op.is_global || op.is_func)
        {
            continue;
        }
        IrBlock *bb = def_block[op.u.vreg];
        if (bb == NULL || bb == pre)
        {
            continue; /* params and preheader defs run before the loop */
        }
        if (in_set(loop_blocks, bb))
        {
            IrInstr *def = def_instr[op.u.vreg];
            if (def == NULL || !in_set(hoisted, def))
            {
                return false;
            }
        }
    }
    return true;
}

/* The single predecessor of `loop`'s header outside the latches, if any. */
static IrBlock *outside_preheader(Loop *loop)
{
    size_t npred = vec_size(loop->header->preds);
    IrBlock *outside = NULL;
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(loop->header->preds, p);
        bool is_latch = false;
        for (size_t l = 0; l < vec_size(loop->latches); l++)
        {
            if ((IrBlock *) vec_get(loop->latches, l) == pred)
            {
                is_latch = true;
                break;
            }
        }
        if (is_latch)
        {
            continue;
        }
        if (outside != NULL)
        {
            return NULL; /* more than one outside pred: no unique preheader */
        }
        outside = pred;
    }
    return outside;
}

static void hoist_into(IrBlock *pre, IrInstr *in)
{
    size_t ninstr = vec_size(pre->instrs);
    opt_insert_instr(pre, (u32) ninstr - 1, in);
}

bool opt_pass_licm(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        LoopInfo *loops = opt_get_loops(ctx, f);
        size_t nloops = vec_size(loops->loops);
        if (nloops == 0)
        {
            continue;
        }

        u32 nvregs = ctx->mod->width_count;
        IrBlock **def_block = arena_alloc(ctx->arena, nvregs * sizeof(IrBlock *), sizeof(void *));
        IrInstr **def_instr = arena_alloc(ctx->arena, nvregs * sizeof(IrInstr *), sizeof(void *));
        for (u32 v = 0; v < nvregs; v++)
        {
            def_block[v] = NULL;
            def_instr[v] = NULL;
        }
        build_def_maps(f, def_block, def_instr);

        CfgInfo *cfg = opt_get_cfg(ctx, f);
        for (size_t li = 0; li < nloops; li++)
        {
            Loop *loop = (Loop *) vec_get(loops->loops, li);
            IrBlock *pre = loop->preheader ? loop->preheader : outside_preheader(loop);
            if (!pre)
            {
                continue;
            }

            HashSet *loop_blocks = hashset_new(ctx->arena, ptr_hash, ptr_eq);
            size_t nblocks = vec_size(loop->blocks);
            for (size_t bi = 0; bi < nblocks; bi++)
            {
                hashset_add(loop_blocks, vec_get(loop->blocks, bi));
            }

            HashSet *hoisted = hashset_new(ctx->arena, ptr_hash, ptr_eq);
            bool progress = true;
            while (progress)
            {
                progress = false;
                for (u32 rk = 0; rk < cfg->nreach; rk++)
                {
                    IrBlock *bb = cfg->rpo[rk];
                    if (bb == pre || !in_set(loop_blocks, bb))
                    {
                        continue;
                    }
                    size_t ninstr = vec_size(bb->instrs);
                    for (size_t j = 0; j < ninstr; j++)
                    {
                        IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                        if (in->result == NO_VREG || !is_hoistable(in) || in_set(hoisted, in))
                        {
                            continue;
                        }
                        if (operands_invariant(in, pre, loop_blocks, def_block, def_instr, hoisted))
                        {
                            hashset_add(hoisted, in);
                            progress = true;
                        }
                    }
                }
            }

            /* Drain in RPO so hoisted defs precede their hoisted uses. */
            for (u32 rk = 0; rk < cfg->nreach; rk++)
            {
                IrBlock *bb = cfg->rpo[rk];
                if (bb == pre || !in_set(loop_blocks, bb))
                {
                    continue;
                }
                size_t i = 0;
                while (i < vec_size(bb->instrs))
                {
                    IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                    if (in->result != NO_VREG && is_hoistable(in) && in_set(hoisted, in))
                    {
                        opt_erase_instr(bb, in);
                        hoist_into(pre, in);
                        changed = true;
                    }
                    else
                    {
                        i++;
                    }
                }
            }
        }
    }
    return changed;
}