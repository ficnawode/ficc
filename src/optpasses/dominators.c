#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"
#include "util/assert.h"

#define NO_DOM UINT32_MAX
#define ENTRY_BLOCK 0

static u32 meet_idoms(const CfgInfo *cfg, const u32 *idom, u32 a, u32 b)
{
    while (a != b)
    {
        while (a != NO_DOM && cfg->rpo_index[a] > cfg->rpo_index[b])
        {
            a = idom[a];
        }
        while (b != NO_DOM && cfg->rpo_index[b] > cfg->rpo_index[a])
        {
            b = idom[b];
        }
        ASSERT(a != NO_DOM && b != NO_DOM);
    }
    return a;
}

static u32 first_processed_pred(const Dominators *doms, IrBlock *bb, u32 nblocks)
{
    size_t npred = vec_size(bb->preds);
    for (size_t p = 0; p < npred; p++)
    {
        u32 idx = opt_block_index(bb->func, (IrBlock *) vec_get(bb->preds, p));
        if (idx < nblocks && doms->idom[idx] != NO_DOM)
        {
            return idx;
        }
    }
    return NO_DOM;
}

Dominators *opt_doms_build(CfgInfo *cfg, Arena *arena)
{
    size_t n = cfg->nblocks;
    Dominators *doms = arena_alloc(arena, sizeof(Dominators), sizeof(void *));
    doms->nblocks = (u32) n;
    doms->idom = arena_alloc(arena, n * sizeof(u32), sizeof(u32));
    doms->depth = arena_alloc(arena, n * sizeof(u32), sizeof(u32));

    for (u32 i = 0; i < n; i++)
    {
        doms->idom[i] = NO_DOM;
        doms->depth[i] = 0;
    }
    doms->idom[ENTRY_BLOCK] = ENTRY_BLOCK;

    bool changed = true;
    int guard = 0;
    while (changed)
    {
        if (++guard > (int) n * 16 + 2048)
        {
            opt_error("dominator fixpoint failed to converge in '%s' (%zu blocks)", cfg->func->name,
                      n);
            abort();
        }
        changed = false;
        for (u32 k = cfg->nreach - 1; k > 0; k--)
        {
            IrBlock *bb = cfg->rpo[k];
            u32 bi = opt_block_index(cfg->func, bb);
            u32 new_idom = first_processed_pred(doms, bb, (u32) n);
            if (new_idom == NO_DOM)
            {
                continue;
            }
            size_t npred = vec_size(bb->preds);
            for (size_t p = 0; p < npred; p++)
            {
                u32 idx = opt_block_index(cfg->func, (IrBlock *) vec_get(bb->preds, p));
                if (idx < n && idx != new_idom && doms->idom[idx] != NO_DOM)
                {
                    new_idom = meet_idoms(cfg, doms->idom, idx, new_idom);
                }
            }
            if (doms->idom[bi] != new_idom)
            {
                doms->idom[bi] = new_idom;
                changed = true;
            }
        }
    }

    for (u32 k = 1; k < cfg->nreach; k++)
    {
        u32 bi = opt_block_index(cfg->func, cfg->rpo[k]);
        if (doms->idom[bi] != NO_DOM)
        {
            doms->depth[bi] = doms->depth[doms->idom[bi]] + 1;
        }
    }
    return doms;
}

bool opt_doms_dominates(const Dominators *doms, u32 a_idx, u32 b_idx)
{
    if (a_idx == b_idx)
    {
        return true;
    }
    if (a_idx >= doms->nblocks || b_idx >= doms->nblocks)
    {
        return false;
    }
    u32 cur = b_idx;
    while (cur != NO_DOM && cur != ENTRY_BLOCK)
    {
        if (cur == a_idx)
        {
            return true;
        }
        cur = doms->idom[cur];
    }
    return cur == a_idx;
}