#include "opt_internal.h"

#include "ir.h"

#include <string.h>

/* CFG canon: prune unreachable blocks, fold imm brconds, merge jump stubs. */

static void succs_of(IrFunction *f, IrBlock *bb, Vec *out)
{
    if (vec_size(bb->instrs) == 0)
    {
        return;
    }
    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    switch (last->opcode)
    {
        case OP_BR:
        {
            IrBlock *t = opt_block_by_label(f, last->extra.br.target_label);
            if (t)
            {
                vec_push(out, t);
            }
            break;
        }
        case OP_BRCOND:
        {
            IrBlock *t = opt_block_by_label(f, last->extra.brcond.true_label);
            IrBlock *e = opt_block_by_label(f, last->extra.brcond.false_label);
            if (t)
            {
                vec_push(out, t);
            }
            if (e && e != t)
            {
                vec_push(out, e);
            }
            break;
        }
        case OP_SWITCH:
            for (u32 c = 0; c < last->extra.sw.ncases; c++)
            {
                IrBlock *t = opt_block_by_label(f, last->extra.sw.cases[c].label);
                if (t)
                {
                    vec_push(out, t);
                }
            }
            if (last->extra.sw.default_label)
            {
                IrBlock *t = opt_block_by_label(f, last->extra.sw.default_label);
                if (t)
                {
                    vec_push(out, t);
                }
            }
            break;
        default:
            break;
    }
}

/* Blocks reachable from the entry, iterated worklist-style (no recursion). */
static u8 *reachable(IrFunction *f, Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u8 *seen = arena_alloc(arena, nblocks, sizeof(u8));
    memset(seen, 0, nblocks);
    Vec *stack = vec_new(arena);
    seen[0] = 1;
    vec_push(stack, vec_get(f->blocks, 0));
    while (vec_size(stack) > 0)
    {
        IrBlock *bb = (IrBlock *) vec_pop(stack);
        Vec *succs = vec_new(arena);
        succs_of(f, bb, succs);
        size_t n = vec_size(succs);
        for (size_t s = 0; s < n; s++)
        {
            IrBlock *t = (IrBlock *) vec_get(succs, s);
            u32 idx = opt_block_index(f, t);
            if (!seen[idx])
            {
                seen[idx] = 1;
                vec_push(stack, t);
            }
        }
    }
    return seen;
}

/* Remove the element at `i` from a pointer Vec (stable suffix shift). */
static void vec_remove_at(Vec *v, size_t i)
{
    if (i >= vec_size(v))
    {
        return;
    }
    size_t n = vec_size(v);
    for (size_t j = i; j + 1 < n; j++)
    {
        vec_set(v, j, vec_get(v, j + 1));
    }
    (void) vec_pop(v);
}

static void remove_block(IrFunction *f, IrBlock *bb)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *other = (IrBlock *) vec_get(f->blocks, b);
        if (other == bb)
        {
            continue;
        }
        size_t npred = vec_size(other->preds);
        for (size_t p = 0; p < npred; p++)
        {
            if (vec_get(other->preds, p) == bb)
            {
                opt_drop_edge(bb, other);
                break;
            }
        }
    }
    vec_remove_at(f->blocks, opt_block_index(f, bb));
}

static bool prune_unreachable(OptimizerContext *ctx, IrFunction *f)
{
    u8 *seen = reachable(f, ctx->arena);
    bool pruned = false;
    size_t n = vec_size(f->blocks);
    for (size_t k = n; k-- > 1;)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, k);
        if (!seen[k])
        {
            remove_block(f, bb);
            pruned = true;
        }
    }
    return pruned;
}

/* Fold a brcond whose condition is a constant into an unconditional br. */
static bool fold_constant_brcond(OptimizerContext *ctx, IrFunction *f)
{
    (void) ctx;
    bool changed = false;
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        if (vec_size(bb->instrs) == 0)
        {
            continue;
        }
        IrInstr *last = (IrInstr *) vec_last(bb->instrs);
        if (last->opcode != OP_BRCOND || !last->ops[0].is_imm)
        {
            continue;
        }
        const char *taken = last->ops[0].u.imm != 0 ? last->extra.brcond.true_label
                                                    : last->extra.brcond.false_label;
        const char *skipped = last->ops[0].u.imm != 0 ? last->extra.brcond.false_label
                                                      : last->extra.brcond.true_label;
        if (strcmp(taken, skipped) != 0)
        {
            IrBlock *dead = opt_block_by_label(f, skipped);
            if (dead)
            {
                opt_drop_edge(bb, dead);
            }
        }
        IrInstr *br = ir_emit_br(bb, taken);
        opt_copy_line(br, last);
        opt_erase_instr(bb, last);
        changed = true;
    }
    return changed;
}

/* Rename one label in every phi of `bb`, keeping entries == preds. */
static void phi_rename_label(IrBlock *bb, const char *old_label, const char *new_label)
{
    size_t ninstr = vec_size(bb->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        for (u32 e = 0; e < in->extra.phi.nentries; e++)
        {
            if (strcmp(in->extra.phi.entries[e].label, old_label) == 0)
            {
                in->extra.phi.entries[e].label = new_label;
            }
        }
    }
}

/* A pure jump stub: a non-entry block whose only instruction is `br Y`. */
static IrInstr *jump_stub(IrFunction *f, IrBlock *bb)
{
    if (opt_block_index(f, bb) == 0)
    {
        return NULL;
    }
    if (vec_size(bb->preds) != 1)
    {
        return NULL;
    }
    if (vec_size(bb->instrs) != 1)
    {
        return NULL;
    }
    IrInstr *only = (IrInstr *) vec_get(bb->instrs, 0);
    if (only->opcode != OP_BR)
    {
        return NULL;
    }
    return only;
}

static bool vec_contains_ptr(Vec *v, void *item)
{
    size_t n = vec_size(v);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(v, i) == item)
        {
            return true;
        }
    }
    return false;
}

/* True when one of `bb`'s phis takes a value from `label`. */
static bool phis_reference_label(IrBlock *bb, const char *label)
{
    size_t ninstr = vec_size(bb->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        for (u32 e = 0; e < in->extra.phi.nentries; e++)
        {
            if (strcmp(in->extra.phi.entries[e].label, label) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* True when `bb`'s terminator can reach more than one block. */
static bool block_has_split_exit(IrFunction *f, IrBlock *bb)
{
    if (vec_size(bb->instrs) == 0)
    {
        return false;
    }
    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    if (last->opcode != OP_BRCOND && last->opcode != OP_SWITCH)
    {
        return false; /* br / ret: a single, unambiguous successor */
    }
    if (last->opcode == OP_BRCOND)
    {
        const char *a = last->extra.brcond.true_label;
        const char *b = last->extra.brcond.false_label;
        return strcmp(a, b) != 0 && opt_block_by_label(f, a) != NULL &&
               opt_block_by_label(f, b) != NULL;
    }
    return last->extra.sw.ncases > 0 || last->extra.sw.default_label != NULL;
}

static size_t vec_index_ptr(Vec *v, void *item)
{
    size_t n = vec_size(v);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(v, i) == item)
        {
            return i;
        }
    }
    return n;
}

static bool merge_jump_stubs(OptimizerContext *ctx, IrFunction *f)
{
    (void) ctx;
    bool changed = false;
    for (;;)
    {
        IrBlock *merge = NULL;
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 1; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            IrInstr *br = jump_stub(f, bb);
            if (!br)
            {
                continue;
            }
            IrBlock *pred = (IrBlock *) vec_get(bb->preds, 0);
            IrBlock *target = opt_block_by_label(f, br->extra.br.target_label);
            if (pred == bb || !target || target == bb || vec_contains_ptr(target->preds, pred))
            {
                continue;
            }
            /* Skip: the phi copy's successor would become a block that also
               branches elsewhere, so the copy would run for the wrong target. */
            if (phis_reference_label(target, bb->label) && block_has_split_exit(f, pred))
            {
                continue;
            }
            merge = bb;
            break;
        }
        if (!merge)
        {
            return changed;
        }
        IrBlock *pred = (IrBlock *) vec_get(merge->preds, 0);
        IrInstr *br = (IrInstr *) vec_get(merge->instrs, 0);
        IrBlock *target = opt_block_by_label(f, br->extra.br.target_label);
        opt_retarget_terminator(pred, merge->label, target->label);
        phi_rename_label(target, merge->label, pred->label);
        vec_push(target->preds, pred);
        vec_remove_at(target->preds, vec_index_ptr(target->preds, merge));
        vec_remove_at(f->blocks, opt_block_index(f, merge));
        changed = true;
    }
}

bool opt_pass_cfg_clean(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        changed |= prune_unreachable(ctx, f);
        changed |= fold_constant_brcond(ctx, f);
        changed |= merge_jump_stubs(ctx, f);
        changed |= prune_unreachable(ctx, f);
    }
    return changed;
}