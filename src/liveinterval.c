#include "liveinterval.h"
#include "util/assert.h"
#include "util/bitset.h"
#include "util/hashmap.h"
#include "util/vec.h"
#include <stdint.h>

typedef struct
{
    size_t nblocks;
    StrMap *label_to_index; /* label -> (index + 1); 0 is the "not found" sentinel */
} BlockTable;

static BlockTable index_blocks(IrFunction *f, Arena *arena)
{
    BlockTable bt = {0};
    bt.nblocks = vec_size(f->blocks);
    bt.label_to_index = strmap_new(arena);
    for (size_t i = 0; i < bt.nblocks; i++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, i);
        strmap_set(bt.label_to_index, blk->label, (void *) (uintptr_t) (i + 1));
    }
    return bt;
}

/* Resolve a predecessor label to its block index; false when the label is
   unknown (a phi edge to a block outside this function). */
static bool block_index_by_label(const BlockTable *bt, const char *label, size_t *out)
{
    void *v = strmap_get(bt->label_to_index, label);
    if (!v)
    {
        return false;
    }
    *out = (size_t) (uintptr_t) v - 1;
    return true;
}

static size_t block_index_of(const BlockTable *bt, const IrBlock *blk)
{
    return (size_t) (uintptr_t) strmap_get(bt->label_to_index, blk->label) - 1;
}

static u32 scan_vreg(u32 max, u32 vreg)
{
    return vreg == NO_VREG ? max : MAX(max, vreg);
}

static u32 scan_operand(u32 max, IrOperand op)
{
    return ir_operand_is_vreg(op) ? scan_vreg(max, op.u.vreg) : max;
}

static u32 scan_instr_vregs(u32 max, IrInstr *in)
{
    max = scan_vreg(max, in->result);
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        max = scan_operand(max, in->ops[oi]);
    }
    if (in->opcode == OP_CALL)
    {
        if (in->extra.call.is_indirect)
        {
            max = scan_operand(max, in->extra.call.callee);
        }
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            max = scan_operand(max, in->extra.call.args[a]);
        }
    }
    return max;
}

static u32 scan_max_vreg(IrFunction *f)
{
    u32 max_vreg = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            max_vreg = scan_instr_vregs(max_vreg, (IrInstr *) vec_get(blk->instrs, ii));
        }
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        max_vreg = scan_vreg(max_vreg, ((IrParam *) vec_get(f->params, i))->vreg);
    }
    return max_vreg;
}

static IrPositions build_positions(IrFunction *f, Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    IrPositions pos = {0};
    pos.nblocks = nblocks;
    pos.block_base = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(u32), sizeof(u32));
    pos.block_end = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(u32), sizeof(u32));
    u32 cursor = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        pos.block_base[b] = cursor;
        size_t ninstr = vec_size(((IrBlock *) vec_get(f->blocks, b))->instrs);
        cursor += (u32) ninstr * 2;
        pos.block_end[b] = cursor;
    }
    pos.npositions = cursor;
    return pos;
}
/* Per-block def/use sets plus the PHI bookkeeping that feeds the live dataflow. */
typedef struct
{
    size_t nblocks;
    u32 nvregs;
    Bitset **defs;
    Bitset **use_before_def;
    Bitset **local_defs;
} BlockSets;

static void record_use(Bitset *seen_defs, Bitset *use_before_def, IrOperand op)
{
    if (ir_operand_is_vreg(op))
    {
        u32 v = op.u.vreg;
        if (!bitset_test(seen_defs, v))
        {
            bitset_set(use_before_def, v);
        }
    }
}

static void scan_call_uses(Bitset *seen, Bitset *ubd, IrInstr *in)
{
    if (in->extra.call.is_indirect)
    {
        record_use(seen, ubd, in->extra.call.callee);
    }
    for (u32 a = 0; a < in->extra.call.nargs; a++)
    {
        record_use(seen, ubd, in->extra.call.args[a]);
    }
}

/* A PHI's copies at the predecessors' ends define its result and read its
   operands there, so the dataflow treats the phi as living in its preds. */
static void record_phi_edges(const BlockTable *bt, BlockSets *s, IrInstr *in)
{
    for (u32 e = 0; e < in->extra.phi.nentries; e++)
    {
        size_t pj;
        if (!block_index_by_label(bt, in->extra.phi.entries[e].label, &pj))
        {
            continue;
        }
        bitset_set(s->defs[pj], in->result);
    }
}

static void add_phi_edge_uses(IrFunction *f, const BlockTable *bt, BlockSets *s)
{
    size_t nblocks = bt->nblocks;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI)
            {
                continue;
            }
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                size_t pj;
                if (!block_index_by_label(bt, in->extra.phi.entries[e].label, &pj))
                {
                    continue;
                }
                IrOperand op = in->extra.phi.entries[e].val;
                if (ir_operand_is_vreg(op) && !bitset_test(s->local_defs[pj], op.u.vreg))
                {
                    bitset_set(s->use_before_def[pj], op.u.vreg);
                }
            }
        }
    }
}

static void scan_block_sets(const BlockTable *bt, BlockSets *s, IrBlock *blk, size_t b,
                            Arena *arena)
{
    Bitset *seen_defs = bitset_new(arena, s->nvregs);
    size_t ninstr = vec_size(blk->instrs);
    for (size_t ii = 0; ii < ninstr; ii++)
    {
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
        if (in->result != NO_VREG && in->opcode != OP_PHI)
        {
            bitset_set(seen_defs, in->result);
            bitset_set(s->defs[b], in->result);
            bitset_set(s->local_defs[b], in->result);
        }
        for (u8 oi = 0; oi < in->nops; oi++)
        {
            record_use(seen_defs, s->use_before_def[b], in->ops[oi]);
        }
        if (in->opcode == OP_CALL)
        {
            scan_call_uses(seen_defs, s->use_before_def[b], in);
        }
        else if (in->opcode == OP_PHI && in->result != NO_VREG)
        {
            bitset_set(seen_defs, in->result);
            bitset_set(s->defs[b], in->result);
            bitset_set(s->local_defs[b], in->result);
            record_phi_edges(bt, s, in);
        }
    }
}

static BlockSets build_block_sets(IrFunction *f, const BlockTable *bt, u32 nvregs, Arena *arena)
{
    size_t nblocks = bt->nblocks;
    BlockSets s = {0};
    s.nblocks = nblocks;
    s.nvregs = nvregs;
    s.defs = arena_alloc(arena, nblocks * sizeof(Bitset *), sizeof(void *));
    s.use_before_def = arena_alloc(arena, nblocks * sizeof(Bitset *), sizeof(void *));
    s.local_defs = arena_alloc(arena, nblocks * sizeof(Bitset *), sizeof(void *));
    for (size_t b = 0; b < nblocks; b++)
    {
        s.defs[b] = bitset_new(arena, nvregs);
        s.use_before_def[b] = bitset_new(arena, nvregs);
        s.local_defs[b] = bitset_new(arena, nvregs);
    }

    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        u32 pv = ((IrParam *) vec_get(f->params, i))->vreg;
        if (pv != NO_VREG && nblocks > 0)
        {
            bitset_set(s.defs[0], pv);
            bitset_set(s.local_defs[0], pv);
        }
    }

    for (size_t b = 0; b < nblocks; b++)
    {
        scan_block_sets(bt, &s, (IrBlock *) vec_get(f->blocks, b), b, arena);
    }
    add_phi_edge_uses(f, bt, &s);
    return s;
}

static void propagate_live_in(IrFunction *f, const BlockTable *bt, size_t b, Bitset **lvin,
                              Bitset **lvout, size_t nwords)
{
    IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
    size_t npred = vec_size(blk->preds);
    u64 *lin_w = bitset_words(lvin[b]);
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(blk->preds, p);
        u64 *lout_w = bitset_words(lvout[block_index_of(bt, pred)]);
        for (size_t wi = 0; wi < nwords; wi++)
        {
            lout_w[wi] |= lin_w[wi];
        }
    }
}

static bool update_block_liveness(const BlockSets *s, size_t b, Bitset **lvin, Bitset **lvout,
                                  size_t nwords)
{
    bool changed = false;
    u64 *lout_w = bitset_words(lvout[b]);
    u64 *def_w = bitset_words(s->defs[b]);
    u64 *ubd_w = bitset_words(s->use_before_def[b]);
    u64 *lin_w = bitset_words(lvin[b]);
    for (size_t wi = 0; wi < nwords; wi++)
    {
        u64 nb = (lout_w[wi] & ~def_w[wi]) | ubd_w[wi];
        if (nb & ~lin_w[wi])
        {
            changed = true;
        }
        lin_w[wi] |= nb;
    }
    return changed;
}

typedef struct
{
    Bitset **lvin;
    Bitset **lvout;
} Liveness;

static Liveness solve_liveness(IrFunction *f, const BlockTable *bt, const BlockSets *s,
                               Arena *arena)
{
    size_t nblocks = bt->nblocks;
    Bitset **lvin = arena_alloc(arena, nblocks * sizeof(Bitset *), sizeof(void *));
    Bitset **lvout = arena_alloc(arena, nblocks * sizeof(Bitset *), sizeof(void *));
    for (size_t b = 0; b < nblocks; b++)
    {
        lvin[b] = bitset_new(arena, s->nvregs);
        lvout[b] = bitset_new(arena, s->nvregs);
    }
    size_t nwords = bitset_nwords(lvin[0]);
    for (;;)
    {
        bool changed = false;
        for (size_t b = 0; b < nblocks; b++)
        {
            propagate_live_in(f, bt, b, lvin, lvout, nwords);
        }
        for (size_t b = 0; b < nblocks; b++)
        {
            changed |= update_block_liveness(s, b, lvin, lvout, nwords);
        }
        if (!changed)
        {
            break;
        }
    }
    Liveness live = {.lvin = lvin, .lvout = lvout};
    return live;
}

static unsigned lowest_set_bit(u64 v)
{
    unsigned k = 0;
    while ((v & 1ULL) == 0)
    {
        v >>= 1;
        ++k;
    }
    return k;
}

static void walk_live_bits(const Bitset *bs, u32 nvregs, void (*fn)(u32 v, void *ud), void *ud)
{
    const u64 *words = bitset_words((Bitset *) bs);
    size_t nwords = bitset_nwords(bs);
    for (size_t wi = 0; wi < nwords; wi++)
    {
        u64 bits = words[wi];
        while (bits)
        {
            u32 k = lowest_set_bit(bits);
            bits &= bits - 1;
            u32 v = (u32) wi * 64 + k;
            if (v >= nvregs)
            {
                continue;
            }
            fn(v, ud);
        }
    }
}

typedef struct
{
    u32 *start;
    u32 *end;
    u32 value;
} ExtendCtx;

static void extend_start_at(u32 v, void *ud)
{
    ExtendCtx *cx = (ExtendCtx *) ud;
    if (cx->value < cx->start[v])
    {
        cx->start[v] = cx->value;
    }
}

static void extend_end_at(u32 v, void *ud)
{
    ExtendCtx *cx = (ExtendCtx *) ud;
    if (cx->value > cx->end[v])
    {
        cx->end[v] = cx->value;
    }
}

static u8 vreg_width_of(IrModule *mod, u32 vreg)
{
    if (vreg >= mod->width_count)
    {
        return 8;
    }
    return mod->widths[vreg] == 16 ? 16 : 8;
}

static RegClass vreg_class_of(IrModule *mod, u32 vreg)
{
    if (vreg_width_of(mod, vreg) == 16)
    {
        return RC_X87;
    }
    if (vreg < mod->width_count && mod->floatness[vreg])
    {
        return RC_XMM;
    }
    return RC_GPR;
}

static void record_use_pos(Bitset *live, u32 *end, u32 p, IrOperand op)
{
    if (ir_operand_is_vreg(op))
    {
        u32 v = op.u.vreg;
        if (!bitset_test(live, v))
        {
            bitset_set(live, v);
            if (p > end[v])
            {
                end[v] = p;
            }
        }
    }
}

static void init_ranges(IrFunction *f, u32 *start, u32 *end, u32 nvregs)
{
    for (u32 v = 0; v < nvregs; v++)
    {
        start[v] = UINT32_MAX;
        end[v] = 0;
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        u32 pv = ((IrParam *) vec_get(f->params, i))->vreg;
        if (pv != NO_VREG)
        {
            start[pv] = 0;
        }
    }
}

typedef struct
{
    const IrPositions *pos;
    const BlockTable *bt;
    u32 nvregs;
    u32 *start;
    u32 *end;
} IntervalCtx;

/* Phi copies run at each predecessor's end: they write the phi result and
   read its operands at the block-end gap (the odd position after the last
   instruction).  The pred seeds let a copy-only operand, never used by a
   real instruction, still register its def position. */
static Bitset **phi_copy_marks(IntervalCtx *cx, IrFunction *f, Arena *arena)
{
    Bitset **copy_uses = arena_alloc(arena, cx->bt->nblocks * sizeof(Bitset *), sizeof(void *));
    for (size_t b = 0; b < cx->bt->nblocks; b++)
    {
        copy_uses[b] = bitset_new(arena, cx->nvregs);
    }
    for (size_t b = 0; b < cx->bt->nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI || in->result == NO_VREG)
            {
                continue;
            }
            if (cx->pos->block_base[b] < cx->start[in->result])
            {
                cx->start[in->result] = cx->pos->block_base[b];
            }
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                IrPhiEntry *entry = &in->extra.phi.entries[e];
                size_t pj;
                if (!block_index_by_label(cx->bt, entry->label, &pj))
                {
                    continue;
                }
                u32 cp = cx->pos->block_end[pj] > cx->pos->block_base[pj]
                             ? cx->pos->block_end[pj] - 1
                             : cx->pos->block_base[pj];
                if (cp < cx->start[in->result])
                {
                    cx->start[in->result] = cp;
                }
                if (cp > cx->end[in->result])
                {
                    cx->end[in->result] = cp;
                }
                if (ir_operand_is_vreg(entry->val))
                {
                    u32 ov = entry->val.u.vreg;
                    if (cp > cx->end[ov])
                    {
                        cx->end[ov] = cp;
                    }
                    bitset_set(copy_uses[pj], ov);
                }
            }
        }
    }
    return copy_uses;
}

/* Backward scan of one block: seed `work` from the block's live-out values
   plus its phi-copy reads, walk last-to-first so each vreg's end lands on its
   final use, and pin defs; live-in/live-out values extend across the block. */
static void refine_block(IntervalCtx *cx, IrFunction *f, size_t b, Bitset **lvin, Bitset **lvout,
                         Bitset *copy_uses, Bitset *work)
{
    size_t nwords = bitset_nwords(work);
    u64 *work_w = bitset_words(work);
    u64 *lout_w = bitset_words(lvout[b]);
    u64 *uses_w = bitset_words(copy_uses);
    for (size_t wi = 0; wi < nwords; wi++)
    {
        work_w[wi] = lout_w[wi] | uses_w[wi];
    }
    IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
    size_t ninstr = vec_size(blk->instrs);
    for (size_t ii = ninstr; ii > 0; ii--)
    {
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii - 1);
        u32 p = cx->pos->block_base[b] + (u32) 2 * (u32) (ii - 1);
        u32 p_use = (ii == ninstr) ? cx->pos->block_end[b] - 1 : p;
        for (u8 oi = 0; oi < in->nops; oi++)
        {
            record_use_pos(work, cx->end, p_use, in->ops[oi]);
        }
        if (in->opcode == OP_CALL)
        {
            if (in->extra.call.is_indirect)
            {
                record_use_pos(work, cx->end, p_use, in->extra.call.callee);
            }
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                record_use_pos(work, cx->end, p_use, in->extra.call.args[a]);
            }
        }
        if (in->result != NO_VREG && in->opcode != OP_PHI)
        {
            if (p < cx->start[in->result])
            {
                cx->start[in->result] = p;
            }
            if (bitset_test(work, in->result))
            {
                bitset_clear(work, in->result);
            }
            else if (p > cx->end[in->result])
            {
                cx->end[in->result] = p;
            }
        }
    }
    ExtendCtx ext = {.start = cx->start, .end = cx->end};
    ext.value = cx->pos->block_base[b];
    walk_live_bits(lvin[b], cx->nvregs, extend_start_at, &ext);
    ext.value = cx->pos->block_end[b];
    walk_live_bits(lvout[b], cx->nvregs, extend_end_at, &ext);
}

static LiveIntervals collect_intervals(const IntervalCtx *cx, IrModule *mod, Arena *arena)
{
    LiveInterval *ivs =
        arena_alloc(arena, cx->nvregs * sizeof(LiveInterval), _Alignof(LiveInterval));
    u32 n = 0;
    for (u32 v = 0; v < cx->nvregs; v++)
    {
        if (cx->start[v] == UINT32_MAX)
        {
            continue;
        }
        ivs[n].vreg = v;
        ivs[n].start = cx->start[v];
        ivs[n].end = cx->end[v];
        ivs[n].width = vreg_width_of(mod, v);
        ivs[n].cls = vreg_class_of(mod, v);
        ivs[n].assigned_reg = -1;
        n++;
    }
    LiveIntervals set = {.pos = *cx->pos, .nvregs = cx->nvregs, .ivs = ivs, .n = n};
    return set;
}

LiveIntervals liveinterval_compute(IrFunction *f, IrModule *mod, Arena *arena)
{
    IrPositions pos = build_positions(f, arena);
    u32 nvregs = scan_max_vreg(f) + 1;
    BlockTable bt = index_blocks(f, arena);
    if (bt.nblocks == 0)
    {
        return (LiveIntervals) {.pos = pos, .nvregs = nvregs};
    }

    u32 *start = arena_alloc(arena, nvregs * sizeof(u32), sizeof(u32));
    u32 *end = arena_alloc(arena, nvregs * sizeof(u32), sizeof(u32));
    init_ranges(f, start, end, nvregs);
    IntervalCtx cx = {.pos = &pos, .bt = &bt, .nvregs = nvregs, .start = start, .end = end};

    Bitset **copy_uses = phi_copy_marks(&cx, f, arena);

    BlockSets bs = build_block_sets(f, &bt, nvregs, arena);
    Liveness liveness = solve_liveness(f, &bt, &bs, arena);

    Bitset *work = bitset_new(arena, nvregs);
    for (size_t b = 0; b < bt.nblocks; b++)
    {
        refine_block(&cx, f, b, liveness.lvin, liveness.lvout, copy_uses[b], work);
    }

    return collect_intervals(&cx, mod, arena);
}
