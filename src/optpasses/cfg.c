#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"
#include "util/assert.h"

#include <string.h>

u32 opt_block_index(IrFunction *f, IrBlock *bb)
{
    ASSERT(bb->func == f && bb->index < vec_size(f->blocks) && vec_get(f->blocks, bb->index) == bb);
    return bb->index;
}

IrBlock *opt_block_by_label(IrFunction *f, const char *label)
{
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (strcmp(bb->label, label) == 0)
        {
            return bb;
        }
    }
    return NULL;
}

StrMap *opt_label_map_build(IrFunction *f, Arena *arena)
{
    StrMap *labels = strmap_new(arena);
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        strmap_set(labels, bb->label, bb);
    }
    return labels;
}

static void push_unique_succ(Vec *succs, IrBlock *target)
{
    size_t n = vec_size(succs);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(succs, i) == target)
        {
            return;
        }
    }
    vec_push(succs, target);
}

static Vec *succs_of(StrMap *labels, IrBlock *bb, Arena *arena)
{
    Vec *succs = vec_new(arena);
    if (vec_size(bb->instrs) == 0)
    {
        return succs;
    }
    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    switch (last->opcode)
    {
        case OP_BR:
            push_unique_succ(succs, (IrBlock *) strmap_get(labels, last->extra.br.target_label));
            break;
        case OP_BRCOND:
            push_unique_succ(succs, (IrBlock *) strmap_get(labels, last->extra.brcond.true_label));
            push_unique_succ(succs, (IrBlock *) strmap_get(labels, last->extra.brcond.false_label));
            break;
        case OP_SWITCH:
            for (u32 c = 0; c < last->extra.sw.ncases; c++)
            {
                push_unique_succ(succs,
                                 (IrBlock *) strmap_get(labels, last->extra.sw.cases[c].label));
            }
            if (last->extra.sw.default_label)
            {
                push_unique_succ(succs,
                                 (IrBlock *) strmap_get(labels, last->extra.sw.default_label));
            }
            break;
        default:
            break;
    }
    return succs;
}

static void build_rpo(CfgInfo *cfg, IrFunction *f, Arena *arena)
{
    Vec *post = vec_new(arena);
    Vec *stack = vec_new(arena);

    u8 *visited = arena_alloc(arena, cfg->nblocks, sizeof(u8));
    for (u32 i = 0; i < cfg->nblocks; i++)
    {
        visited[i] = 0;
    }

    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    visited[0] = 1;
    vec_push(stack, entry);

    while (vec_size(stack) > 0)
    {
        IrBlock *bb = (IrBlock *) vec_last(stack);
        Vec *succs = cfg->succs[opt_block_index(f, bb)];

        IrBlock *next = NULL;
        size_t nsucc = vec_size(succs);
        for (size_t s = 0; s < nsucc; s++)
        {
            IrBlock *succ = (IrBlock *) vec_get(succs, s);
            if (!visited[opt_block_index(f, succ)])
            {
                next = succ;
                break;
            }
        }
        if (next)
        {
            visited[opt_block_index(f, next)] = 1;
            vec_push(stack, next);
        }
        else
        {
            vec_pop(stack);
            vec_push(post, bb);
        }
    }

    size_t npost = vec_size(post);
    for (size_t k = 0; k < npost; k++)
    {
        IrBlock *bb = (IrBlock *) vec_get(post, npost - 1 - k);
        u32 bi = opt_block_index(f, bb);
        cfg->rpo[k] = bb;
        cfg->rpo_index[bi] = (u32) k;
    }
    cfg->nreach = (u32) npost;
}

CfgInfo *opt_cfg_build(IrFunction *f, Arena *arena)
{
    size_t n = vec_size(f->blocks);
    CfgInfo *cfg = arena_alloc(arena, sizeof(CfgInfo), sizeof(void *));
    cfg->func = f;
    cfg->nblocks = (u32) n;
    cfg->succs = arena_alloc(arena, n * sizeof(Vec *), sizeof(void *));
    cfg->rpo = arena_alloc(arena, n * sizeof(IrBlock *), sizeof(void *));
    cfg->rpo_index = arena_alloc(arena, n * sizeof(u32), sizeof(u32));
    StrMap *labels = opt_label_map_build(f, arena);

    for (u32 i = 0; i < cfg->nblocks; i++)
    {
        cfg->succs[i] = succs_of(labels, (IrBlock *) vec_get(f->blocks, i), arena);
        cfg->rpo_index[i] = UINT32_MAX;
    }
    build_rpo(cfg, f, arena);
    return cfg;
}