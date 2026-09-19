#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An edge t -> h is a back edge iff h dominates t. A do-while's natural
   header is its body block, which dominates the cond block; the header's
   latch set is exactly the back-edge sources. */

static bool vec_has(Vec *v, void *item)
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

static void vec_remove_all(Vec *v, Vec *gone)
{
    size_t write = 0;
    size_t n = vec_size(v);
    for (size_t read = 0; read < n; read++)
    {
        void *item = vec_get(v, read);
        if (!vec_has(gone, item))
        {
            vec_set(v, write, item);
            write++;
        }
    }
    while (vec_size(v) > write)
    {
        vec_pop(v);
    }
}

static void vec_push_unique(Vec *v, void *item)
{
    if (!vec_has(v, item))
    {
        vec_push(v, item);
    }
}

static IrBlock *new_block(IrFunction *f, const char *prefix)
{
    size_t idx = vec_size(f->blocks);
    int n = snprintf(NULL, 0, "%s_%zu", prefix, idx);
    char *buf = arena_alloc(f->arena, (size_t) n + 1, sizeof(char));
    snprintf(buf, (size_t) n + 1, "%s_%zu", prefix, idx);
    return ir_func_add_block(f, buf);
}

static void retarget_succs(IrFunction *f, IrBlock *from, const char *old_label,
                           const char *new_label)
{
    (void) f;
    if (vec_size(from->instrs) == 0)
    {
        return;
    }
    IrInstr *last = (IrInstr *) vec_last(from->instrs);
    switch (last->opcode)
    {
        case OP_BR:
            if (strcmp(last->extra.br.target_label, old_label) == 0)
            {
                last->extra.br.target_label = new_label;
            }
            break;
        case OP_BRCOND:
            if (strcmp(last->extra.brcond.true_label, old_label) == 0)
            {
                last->extra.brcond.true_label = new_label;
            }
            if (strcmp(last->extra.brcond.false_label, old_label) == 0)
            {
                last->extra.brcond.false_label = new_label;
            }
            break;
        case OP_SWITCH:
            for (u32 c = 0; c < last->extra.sw.ncases; c++)
            {
                if (strcmp(last->extra.sw.cases[c].label, old_label) == 0)
                {
                    last->extra.sw.cases[c].label = new_label;
                }
            }
            if (last->extra.sw.default_label &&
                strcmp(last->extra.sw.default_label, old_label) == 0)
            {
                last->extra.sw.default_label = new_label;
            }
            break;
        default:
            break;
    }
}

static bool entry_from(Vec *gone, const char *label)
{
    size_t n = vec_size(gone);
    for (size_t i = 0; i < n; i++)
    {
        if (strcmp(((IrBlock *) vec_get(gone, i))->label, label) == 0)
        {
            return true;
        }
    }
    return false;
}

static IrOperand phi_value_for_pred(IrInstr *phi, const char *label, bool *found)
{
    *found = false;
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (strcmp(phi->extra.phi.entries[e].label, label) == 0)
        {
            *found = true;
            return phi->extra.phi.entries[e].val;
        }
    }
    return ir_operand_imm(0);
}

/* Collapse the phi entries from `gone` into one entry for `new_pred`. */
static void phi_splice_entries(IrInstr *phi, Vec *gone, IrBlock *new_pred, IrOperand new_val)
{
    u32 nkept = 0;
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (!entry_from(gone, phi->extra.phi.entries[e].label))
        {
            nkept++;
        }
    }
    IrPhiEntry *entries =
        arena_alloc(new_pred->arena, (nkept + 1) * sizeof(IrPhiEntry), sizeof(void *));
    u32 k = 0;
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (entry_from(gone, phi->extra.phi.entries[e].label))
        {
            continue;
        }
        entries[k] = phi->extra.phi.entries[e];
        k++;
    }
    entries[k].val = new_val;
    entries[k].label = new_pred->label;
    phi->extra.phi.entries = entries;
    phi->extra.phi.nentries = nkept + 1;
    phi->extra.phi.nfilled = nkept + 1;
}

static u32 phi_result_vreg(IrModule *mod, IrInstr *phi)
{
    return ir_alloc_vreg(mod, mod->widths[phi->result], mod->signedness[phi->result],
                         mod->floatness[phi->result]);
}

static IrInstr *emit_merge_phi(IrModule *mod, IrBlock *target, IrInstr *header_phi, Vec *sources)
{
    u32 dst = phi_result_vreg(mod, header_phi);
    IrInstr *mphi = ir_emit_phi_at_start(target, dst, (u32) vec_size(sources));
    for (size_t s = 0; s < vec_size(sources); s++)
    {
        IrBlock *src = (IrBlock *) vec_get(sources, s);
        bool found = false;
        IrOperand val = phi_value_for_pred(header_phi, src->label, &found);
        if (!found)
        {
            fprintf(stderr, "[opt] error: no phi entry for predecessor '%s'\n", src->label);
            exit(1);
        }
        ir_phi_add_entry(mphi, val, src);
    }
    return mphi;
}

static void insertion_sort_by_index(Vec *blocks)
{
    size_t n = vec_size(blocks);
    for (size_t i = 1; i < n; i++)
    {
        IrBlock *key = (IrBlock *) vec_get(blocks, i);
        size_t j = i;
        while (j > 0 && opt_block_index(key->func, (IrBlock *) vec_get(blocks, j - 1)) >
                            opt_block_index(key->func, key))
        {
            vec_set(blocks, j, vec_get(blocks, j - 1));
            j--;
        }
        vec_set(blocks, j, key);
    }
}

static Vec *latches_of_header(Vec *pairs, IrBlock *header)
{
    size_t n = vec_size(pairs) / 2;
    for (size_t i = 0; i < n; i++)
    {
        if ((IrBlock *) vec_get(pairs, i * 2) == header)
        {
            return (Vec *) vec_get(pairs, i * 2 + 1);
        }
    }
    return NULL;
}

/* header, latches, header, latches, ... in discovery order. */
static Vec *find_back_edges(IrFunction *f, CfgInfo *cfg, Dominators *doms, Arena *arena)
{
    Vec *pairs = vec_new(arena);
    for (u32 k = 0; k < cfg->nreach; k++)
    {
        IrBlock *t = cfg->rpo[k];
        Vec *succs = cfg->succs[opt_block_index(f, t)];
        size_t nsucc = vec_size(succs);
        for (size_t s = 0; s < nsucc; s++)
        {
            IrBlock *target = (IrBlock *) vec_get(succs, s);
            if (!opt_doms_dominates(doms, opt_block_index(f, target), opt_block_index(f, t)))
            {
                continue;
            }
            Vec *latches = latches_of_header(pairs, target);
            if (!latches)
            {
                latches = vec_new(arena);
                vec_push(pairs, target);
                vec_push(pairs, latches);
            }
            vec_push_unique(latches, t);
        }
    }
    return pairs;
}

static void build_loop_bodies(IrFunction *f, LoopInfo *loops, Arena *arena)
{
    size_t nloops = vec_size(loops->loops);
    for (size_t i = 0; i < nloops; i++)
    {
        Loop *loop = (Loop *) vec_get(loops->loops, i);
        size_t nblocks = vec_size(f->blocks);
        u8 *seen = arena_alloc(arena, nblocks, sizeof(u8));
        for (u32 bi = 0; bi < nblocks; bi++)
        {
            seen[bi] = 0;
        }
        seen[opt_block_index(f, loop->header)] = 1;
        vec_push(loop->blocks, loop->header);

        size_t nlatches = vec_size(loop->latches);
        for (size_t l = 0; l < nlatches; l++)
        {
            IrBlock *latch = (IrBlock *) vec_get(loop->latches, l);
            if (!seen[opt_block_index(f, latch)])
            {
                seen[opt_block_index(f, latch)] = 1;
                vec_push(loop->blocks, latch);
            }
        }
        for (size_t q = 0; q < vec_size(loop->blocks); q++)
        {
            IrBlock *bb = (IrBlock *) vec_get(loop->blocks, q);
            if (bb == loop->header)
            {
                continue; /* blocks above the header are outside the loop */
            }
            size_t npred = vec_size(bb->preds);
            for (size_t p = 0; p < npred; p++)
            {
                IrBlock *pred = (IrBlock *) vec_get(bb->preds, p);
                u32 idx = opt_block_index(f, pred);
                if (!seen[idx])
                {
                    seen[idx] = 1;
                    vec_push(loop->blocks, pred);
                }
            }
        }
    }
}

static void assign_outer_loops(IrFunction *f, Dominators *doms, LoopInfo *loops)
{
    size_t n = vec_size(loops->loops);
    for (size_t i = 0; i < n; i++)
    {
        Loop *l = (Loop *) vec_get(loops->loops, i);
        u32 li = opt_block_index(f, l->header);
        Loop *outer = NULL;
        u32 best_depth = UINT32_MAX;
        for (size_t j = 0; j < n; j++)
        {
            Loop *o = (Loop *) vec_get(loops->loops, j);
            if (o == l)
            {
                continue;
            }
            u32 oi = opt_block_index(f, o->header);
            if (opt_doms_dominates(doms, oi, li) && doms->depth[oi] < best_depth)
            {
                outer = o;
                best_depth = doms->depth[oi];
            }
        }
        l->outer = outer;
    }
}

LoopInfo *opt_loops_find(IrFunction *f, CfgInfo *cfg, Dominators *doms, Arena *arena)
{
    LoopInfo *info = arena_alloc(arena, sizeof(LoopInfo), sizeof(void *));
    info->loops = vec_new(arena);

    Vec *pairs = find_back_edges(f, cfg, doms, arena);
    size_t nheaders = vec_size(pairs) / 2;
    for (size_t h = 0; h < nheaders; h++)
    {
        IrBlock *header = (IrBlock *) vec_get(pairs, h * 2);
        Vec *latches = (Vec *) vec_get(pairs, h * 2 + 1);
        insertion_sort_by_index(latches);

        Loop *loop = arena_alloc(arena, sizeof(Loop), sizeof(void *));
        loop->header = header;
        loop->index = (u32) h;
        loop->latches = latches;
        loop->blocks = vec_new(arena);
        loop->preheader = NULL;
        loop->outer = NULL;
        vec_push(info->loops, loop);
    }

    build_loop_bodies(f, info, arena);
    assign_outer_loops(f, doms, info);
    return info;
}

bool opt_loops_contains(const Loop *loop, IrBlock *bb)
{
    return vec_has(loop->blocks, bb);
}

static Vec *outside_preds(IrFunction *f, Loop *loop)
{
    Vec *outside = vec_new(f->arena);
    size_t npred = vec_size(loop->header->preds);
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(loop->header->preds, p);
        if (!vec_has(loop->latches, pred))
        {
            vec_push_unique(outside, pred);
        }
    }
    return outside;
}

static bool merge_latches(IrModule *mod, IrFunction *f, Loop *loop)
{
    if (vec_size(loop->latches) <= 1)
    {
        return false;
    }
    IrBlock *merge = new_block(f, "latch_merge");
    size_t ninstr = vec_size(loop->header->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(loop->header->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        IrInstr *mphi = emit_merge_phi(mod, merge, in, loop->latches);
        phi_splice_entries(in, loop->latches, merge, ir_operand_vreg(mphi->result));
    }
    ir_emit_br(merge, loop->header->label);

    IrBlock *header = loop->header;
    size_t nlatches = vec_size(loop->latches);
    for (size_t l = 0; l < nlatches; l++)
    {
        retarget_succs(f, (IrBlock *) vec_get(loop->latches, l), header->label, merge->label);
    }
    vec_remove_all(header->preds, loop->latches);
    vec_push_unique(header->preds, merge);
    for (size_t l = 0; l < nlatches; l++)
    {
        vec_push_unique(merge->preds, (IrBlock *) vec_get(loop->latches, l));
    }

    vec_push_unique(loop->blocks, merge);
    loop->latches = vec_new(f->arena);
    vec_push(loop->latches, merge);
    return true;
}

static bool splice_preheader(IrModule *mod, IrFunction *f, Loop *loop)
{
    Vec *outside = outside_preds(f, loop);
    size_t noutside = vec_size(outside);
    if (noutside == 1)
    {
        loop->preheader = (IrBlock *) vec_get(outside, 0);
        return false;
    }
    if (noutside == 0)
    {
        return false;
    }
    IrBlock *pre = new_block(f, "preheader");
    IrBlock *header = loop->header;
    size_t ninstr = vec_size(header->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(header->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        IrInstr *pphi = emit_merge_phi(mod, pre, in, outside);
        phi_splice_entries(in, outside, pre, ir_operand_vreg(pphi->result));
    }
    ir_emit_br(pre, header->label);

    for (size_t p = 0; p < noutside; p++)
    {
        retarget_succs(f, (IrBlock *) vec_get(outside, p), header->label, pre->label);
    }
    vec_remove_all(header->preds, outside);
    vec_push_unique(header->preds, pre);
    for (size_t p = 0; p < noutside; p++)
    {
        vec_push_unique(pre->preds, (IrBlock *) vec_get(outside, p));
    }

    vec_push_unique(loop->blocks, pre);
    loop->preheader = pre;
    return true;
}

bool opt_loops_canonicalize(IrModule *mod, IrFunction *f, LoopInfo *loops)
{
    bool changed = false;
    size_t n = vec_size(loops->loops);
    for (size_t i = 0; i < n; i++)
    {
        Loop *loop = (Loop *) vec_get(loops->loops, i);
        changed |= merge_latches(mod, f, loop);
        changed |= splice_preheader(mod, f, loop);
    }
    return changed;
}

bool opt_loops_verify_shapes(LoopInfo *loops)
{
    size_t n = vec_size(loops->loops);
    for (size_t i = 0; i < n; i++)
    {
        Loop *l = (Loop *) vec_get(loops->loops, i);
        Vec *outside = outside_preds(l->header->func, l);
        size_t noutside = vec_size(outside);
        if (vec_size(l->latches) > 1)
        {
            fprintf(stderr, "[opt] loop '%s' has %zu latches\n", l->header->label,
                    vec_size(l->latches));
            return false;
        }
        if (noutside > 1)
        {
            fprintf(stderr, "[opt] loop '%s' has %zu outside predecessors\n", l->header->label,
                    noutside);
            return false;
        }
        if (noutside == 1 && l->preheader != (IrBlock *) vec_get(outside, 0))
        {
            fprintf(stderr, "[opt] loop '%s' preheader mismatch\n", l->header->label);
            return false;
        }
        if (noutside == 0 && l->preheader != NULL)
        {
            fprintf(stderr, "[opt] loop '%s' has no outside pred but a preheader\n",
                    l->header->label);
            return false;
        }
    }
    return true;
}