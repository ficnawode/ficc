#include "regalloc.h"
#include "spill.h"
#include "target.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/vec.h"
#include <stdlib.h>
#include <string.h>

#define PRESSURE_SPLIT_MAX 16

typedef struct
{
    u32 vreg;
    u32 start;
    u32 end;
    u8 kind;
    u8 reg;
    i32 disp;
} SegRec;

typedef struct
{
    u32 vreg;
    u32 end;
    u8 reg;
    SegRec *rec;
} ActiveInterval;

typedef struct
{
    u32 pos;
    u16 mask;
} ClobberPos;

typedef struct
{
    const LiveInterval *iv;
    u32 from;
    u8 depth;
} SplitTail;

typedef struct
{
    u32 pred;
    u32 succ;
} CfgEdge;

typedef struct
{
    RegAllocation *alloc;
    const RegBank *bank;
    const u32 *calls;
    u32 ncall;
    const bool *call_op;
    u16 arg_avoid;
    const ClobberPos *clob;
    u32 nclob;
    IrInstr **defs;
    const u32 *def_pos;
    const bool *call_operand;
    const IrPositions *pos;
    const RegClass *vreg_cls;
    const int *pref;
    Vec *segs;
    Vec *gaps;
    Vec *seg_gaps;
    Vec *tails;
    const IrFunction *func;
    const bool *remat;
    const u32 *use_count;
    u8 split_depth;
    ActiveInterval *active;
    u32 nactive;
    Arena *arena;
    const CfgEdge *edges;
    u32 nedges;
    Bitset **edge_live;
    const u32 *phi_max_def;
} ScanCtx;

static const LiveInterval *interval_of(const RegAllocation *alloc, u32 vreg)
{
    u32 lo = 0;
    u32 hi = alloc->n;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        u32 mv = alloc->ivs[mid].vreg;
        if (mv == vreg)
        {
            return &alloc->ivs[mid];
        }
        if (mv < vreg)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return NULL;
}

static int bank_callee_index(const RegBank *bank, u8 reg);
static const RegSegment *find_segment(const RegAllocation *alloc, u32 vreg, u32 pos);

static ClobberPos *collect_clobbers(IrFunction *f, const IrPositions *pos, const TargetDesc *target,
                                    u32 *out_n, Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u32 count = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (target->instr_clobbers(target, (IrInstr *) vec_get(blk->instrs, ii)))
            {
                count++;
            }
        }
    }
    ClobberPos *out =
        arena_alloc(arena, (count ? count : 1) * sizeof(ClobberPos), _Alignof(ClobberPos));
    u32 n = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            u16 mask = target->instr_clobbers(target, (IrInstr *) vec_get(blk->instrs, ii));
            if (mask)
            {
                out[n].pos = pos->block_base[b] + (u32) 2 * (u32) ii;
                out[n].mask = mask;
                n++;
            }
        }
    }
    *out_n = n;
    return out;
}

/* GPR ids only: the mask is not meaningful for the XMM bank, whose ids share
   the same numbers. */
static u16 clobber_avoid(const ClobberPos *clob, u32 nclob, u32 start, u32 end)
{
    u16 avoid = 0;
    for (u32 c = 0; c < nclob; c++)
    {
        if (start <= clob[c].pos && clob[c].pos <= end)
        {
            avoid |= clob[c].mask;
        }
    }
    return avoid;
}

static RegAllocation *alloc_new(const LiveIntervals *set, Arena *arena)
{
    RegAllocation *alloc = arena_alloc(arena, sizeof(RegAllocation), _Alignof(RegAllocation));
    alloc->ivs = set->ivs;
    alloc->n = set->n;
    alloc->nvregs = set->nvregs;

    int *phys_map = arena_alloc(arena, set->nvregs * sizeof(int), sizeof(int));
    u32 *slot_map = arena_alloc(arena, set->nvregs * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        phys_map[v] = -1;
        slot_map[v] = 8;
    }
    alloc->phys_map = phys_map;
    alloc->slot_map = slot_map;
    alloc->saved_mask = 0;
    u8 *has_slot = arena_alloc(arena, set->nvregs * sizeof(u8), sizeof(u8));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        has_slot[v] = 0;
    }
    alloc->has_slot = has_slot;
    return alloc;
}

static u32 *collect_call_positions(IrFunction *f, const IrPositions *pos, u32 *out_count,
                                   Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u32 count = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == OP_CALL)
            {
                count++;
            }
        }
    }
    u32 *calls = arena_alloc(arena, (count ? count : 1) * sizeof(u32), sizeof(u32));
    u32 n = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == OP_CALL)
            {
                calls[n++] = pos->block_base[b] + (u32) 2 * (u32) ii;
            }
        }
    }
    *out_count = count;
    return calls;
}

static Vec *call_site_vec(const u32 *calls, u32 ncall, Arena *arena)
{
    Vec *sites = vec_new(arena);
    for (u32 i = 0; i < ncall; i++)
    {
        u32 *p = arena_alloc(arena, sizeof(u32), sizeof(u32));
        *p = calls[i];
        vec_push(sites, p);
    }
    return sites;
}

/* A value owns a slot when it has a SEG_MEM run or a call gap; one slot serves
   every run and every gap of the vreg.  Rematerialized values never do. */
static void mark_slot_users(RegAllocation *alloc, const LiveIntervals *set)
{
    for (u32 v = 0; v < set->nvregs; v++)
    {
        alloc->has_slot[v] = 0;
    }
    for (u32 v = 0; v < set->nvregs; v++)
    {
        for (u32 s = alloc->seg_begin[v]; s < alloc->seg_begin[v + 1]; s++)
        {
            if (alloc->segments[s].kind == SEG_MEM)
            {
                alloc->has_slot[v] = 1;
                break;
            }
        }
    }
    for (u32 g = 0; g < alloc->ncall_gaps; g++)
    {
        alloc->has_slot[alloc->call_gaps[g].vreg] = 1;
    }
}

static u32 pack_spills(RegAllocation *alloc, const LiveIntervals *set, const TargetDesc *target,
                       Arena *arena)
{
    mark_slot_users(alloc, set);
    SlotRange *ranges =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(SlotRange), _Alignof(SlotRange));
    u32 n = 0;
    for (u32 i = 0; i < set->n; i++)
    {
        const LiveInterval *iv = &set->ivs[i];
        if (!alloc->has_slot[iv->vreg])
        {
            continue;
        }
        ranges[n].vreg = iv->vreg;
        ranges[n].start = iv->start;
        ranges[n].end = iv->end;
        ranges[n].is16 = iv->width == 16;
        n++;
    }
    qsort(ranges, n, sizeof(SlotRange), spill_slot_range_cmp);
    return spill_pack_slots(target, ranges, n, alloc->slot_map, arena);
}

static int seg_rec_cmp(const void *a, const void *b)
{
    const SegRec *ra = *(const SegRec *const *) a;
    const SegRec *rb = *(const SegRec *const *) b;
    if (ra->vreg != rb->vreg)
    {
        return ra->vreg < rb->vreg ? -1 : 1;
    }
    if (ra->start != rb->start)
    {
        return ra->start < rb->start ? -1 : 1;
    }
    if (ra->end != rb->end)
    {
        return ra->end < rb->end ? -1 : 1;
    }
    if (ra->kind != rb->kind)
    {
        return ra->kind < rb->kind ? -1 : 1;
    }
    return ra->reg < rb->reg ? -1 : (ra->reg > rb->reg ? 1 : 0);
}

static int call_gap_cmp(const void *a, const void *b)
{
    const CallGap *ga = (const CallGap *) a;
    const CallGap *gb = (const CallGap *) b;
    if (ga->pos != gb->pos)
    {
        return ga->pos < gb->pos ? -1 : 1;
    }
    return ga->vreg < gb->vreg ? -1 : (ga->vreg > gb->vreg ? 1 : 0);
}

static int seg_gap_cmp(const void *a, const void *b)
{
    const SegGap *ga = (const SegGap *) a;
    const SegGap *gb = (const SegGap *) b;
    if (ga->pos != gb->pos)
    {
        return ga->pos < gb->pos ? -1 : 1;
    }
    return ga->vreg < gb->vreg ? -1 : (ga->vreg > gb->vreg ? 1 : 0);
}

static void build_seg_gaps(RegAllocation *alloc, Vec *sgaps, Arena *arena)
{
    size_t n = vec_size(sgaps);
    alloc->nseg_gaps = (u32) n;
    alloc->seg_gaps = arena_alloc(arena, (n ? n : 1) * sizeof(SegGap), _Alignof(SegGap));
    for (size_t i = 0; i < n; i++)
    {
        alloc->seg_gaps[i] = *(SegGap *) vec_get(sgaps, i);
    }
    qsort(alloc->seg_gaps, n, sizeof(SegGap), seg_gap_cmp);
}

/* A vreg with no record (x87, or one that never got a range) lives in memory
   for its whole span. */
static void build_segments(RegAllocation *alloc, const LiveIntervals *set, Vec *recs, Vec *gaps,
                           Vec *sgaps, Arena *arena)
{
    size_t nrec = vec_size(recs);
    SegRec **order = arena_alloc(arena, (nrec ? nrec : 1) * sizeof(SegRec *), sizeof(void *));
    for (size_t i = 0; i < nrec; i++)
    {
        order[i] = (SegRec *) vec_get(recs, i);
    }
    qsort(order, nrec, sizeof(SegRec *), seg_rec_cmp);

    RegSegment *segs =
        arena_alloc(arena, (set->n + nrec + 1) * sizeof(RegSegment), _Alignof(RegSegment));
    u32 *begin = arena_alloc(arena, (set->nvregs + 1) * sizeof(u32), sizeof(u32));
    u32 si = 0;
    size_t ri = 0;
    size_t ivi = 0;
    for (u32 v = 0; v < set->nvregs; v++)
    {
        begin[v] = si;
        if (ivi >= set->n || set->ivs[ivi].vreg != v)
        {
            continue;
        }
        const LiveInterval *iv = &set->ivs[ivi++];
        u32 before = si;
        while (ri < nrec && order[ri]->vreg == v)
        {
            segs[si++] = (RegSegment) {order[ri]->start, order[ri]->end, order[ri]->kind,
                                       order[ri]->reg, order[ri]->disp};
            ri++;
        }
        if (si == before)
        {
            segs[si++] = (RegSegment) {iv->start, iv->end, SEG_MEM, 0, 0};
        }
    }
    begin[set->nvregs] = si;
    alloc->segments = segs;
    alloc->seg_begin = begin;
    alloc->nsegments = si;

    size_t ngap = vec_size(gaps);
    alloc->ncall_gaps = (u32) ngap;
    alloc->call_gaps = arena_alloc(arena, (ngap ? ngap : 1) * sizeof(CallGap), _Alignof(CallGap));
    for (size_t i = 0; i < ngap; i++)
    {
        alloc->call_gaps[i] = *(CallGap *) vec_get(gaps, i);
    }
    qsort(alloc->call_gaps, ngap, sizeof(CallGap), call_gap_cmp);
    build_seg_gaps(alloc, sgaps, arena);
}

/* Only the bank's own class counts: XMM register ids share the GPR numbering. */
static u8 saved_mask_of(const RegAllocation *alloc, const RegBank *bank)
{
    u8 mask = 0;
    for (u32 v = 0; v < alloc->nvregs; v++)
    {
        const LiveInterval *iv = interval_of(alloc, v);
        if (!iv || iv->cls != bank->cls)
        {
            continue;
        }
        for (u32 s = alloc->seg_begin[v]; s < alloc->seg_begin[v + 1]; s++)
        {
            if (alloc->segments[s].kind != SEG_REG)
            {
                continue;
            }
            int idx = bank_callee_index(bank, alloc->segments[s].reg);
            if (idx >= 0)
            {
                mask |= (u8) (1u << idx);
            }
        }
    }
    return mask;
}

RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena)
{
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);
    build_segments(alloc, set, vec_new(arena), vec_new(arena), vec_new(arena), arena);
    alloc->frame_size = pack_spills(alloc, set, x86_64_target(), arena);
    return alloc;
}

static int interval_order_cmp(const void *a, const void *b)
{
    const LiveInterval *ia = *(const LiveInterval *const *) a;
    const LiveInterval *ib = *(const LiveInterval *const *) b;
    if (ia->start != ib->start)
    {
        return ia->start < ib->start ? -1 : 1;
    }
    if (ia->vreg != ib->vreg)
    {
        return ia->vreg < ib->vreg ? -1 : 1;
    }
    return 0;
}

static const LiveInterval **sorted_intervals(const LiveIntervals *set, Arena *arena)
{
    const LiveInterval **order =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(LiveInterval *), sizeof(void *));
    for (u32 i = 0; i < set->n; i++)
    {
        order[i] = &set->ivs[i];
    }
    qsort(order, set->n, sizeof(LiveInterval *), interval_order_cmp);
    return order;
}

static int bank_callee_index(const RegBank *bank, u8 reg)
{
    for (u8 i = 0; i < bank->ncallee_saved; i++)
    {
        if (bank->callee_saved[i] == reg)
        {
            return i;
        }
    }
    return -1;
}

static bool bank_allows(const RegBank *bank, u8 reg)
{
    for (u8 i = 0; i < bank->nfixed; i++)
    {
        if (bank->fixed[i] == reg)
        {
            return false;
        }
    }
    return true;
}

/* The register of a predecessor operand that a PHI copy can share with its
   result. The first already-allocated operand of the same class wins; the
   active-set check in pick_register rejects a hint whose source is still live
   past the result's start, so only a source that dies at the copy is reused. */
static int phi_coalesce_hint(const IrInstr *phi, RegClass cls, const RegClass *vreg_cls,
                             const int *phys_map)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        IrOperand v = phi->extra.phi.entries[e].val;
        if (!ir_operand_is_vreg(v))
        {
            continue;
        }
        u32 s = v.u.vreg;
        if (vreg_cls[s] == cls && phys_map[s] >= 0)
        {
            return phys_map[s];
        }
    }
    return -1;
}

/* The two-address form of these ops writes its result over `ops[0]`; the result
   can share that operand's register when the operand dies at the instruction.
   A PHI result likewise shares a predecessor operand's register. */
static int coalesce_hint(const IrInstr *def, const int *phys_map, RegClass cls,
                         const RegClass *vreg_cls)
{
    if (!def)
    {
        return -1;
    }
    switch (def->opcode)
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
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FNEG:
        case OP_GEP:
        case OP_LOAD:
        case OP_TRUNC:
        case OP_SEXT:
        case OP_ZEXT:
            if (def->nops > 0 && ir_operand_is_vreg(def->ops[0]))
            {
                return phys_map[def->ops[0].u.vreg];
            }
            return -1;
        case OP_PHI:
            return phi_coalesce_hint(def, cls, vreg_cls, phys_map);
        default:
            return -1;
    }
}

static u32 *collect_def_positions(IrFunction *f, const IrPositions *pos, u32 nvregs, Arena *arena)
{
    u32 *out = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < nvregs; v++)
    {
        out[v] = 0;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->result != NO_VREG)
            {
                out[in->result] = pos->block_base[b] + (u32) 2 * (u32) ii;
            }
        }
    }
    return out;
}

static IrInstr **collect_defs(IrFunction *f, u32 nvregs, Arena *arena)
{
    IrInstr **defs = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(IrInstr *), sizeof(void *));
    for (u32 v = 0; v < nvregs; v++)
    {
        defs[v] = NULL;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->result != NO_VREG)
            {
                defs[in->result] = in;
            }
        }
    }
    return defs;
}

static u32 instr_use_count(const IrInstr *in, u32 vreg)
{
    u32 count = 0;
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        if (ir_operand_is_vreg(in->ops[oi]) && in->ops[oi].u.vreg == vreg)
        {
            count++;
        }
    }
    if (in->opcode == OP_CALL)
    {
        if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee) &&
            in->extra.call.callee.u.vreg == vreg)
        {
            count++;
        }
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            if (ir_operand_is_vreg(in->extra.call.args[a]) && in->extra.call.args[a].u.vreg == vreg)
            {
                count++;
            }
        }
    }
    return count;
}

/* Reads of every vreg, PHI-edge operands included: a spilled value is reloaded
   at each read, so its read count is its spill cost. */
static u32 *collect_use_counts(IrFunction *f, u32 nvregs, Arena *arena)
{
    u32 *counts = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < nvregs; v++)
    {
        counts[v] = 0;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            for (u8 oi = 0; oi < in->nops; oi++)
            {
                if (ir_operand_is_vreg(in->ops[oi]))
                {
                    counts[in->ops[oi].u.vreg]++;
                }
            }
            if (in->opcode == OP_CALL)
            {
                if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee))
                {
                    counts[in->extra.call.callee.u.vreg]++;
                }
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    if (ir_operand_is_vreg(in->extra.call.args[a]))
                    {
                        counts[in->extra.call.args[a].u.vreg]++;
                    }
                }
            }
            if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    IrOperand val = in->extra.phi.entries[e].val;
                    if (ir_operand_is_vreg(val))
                    {
                        counts[val.u.vreg]++;
                    }
                }
            }
        }
    }
    return counts;
}

/* Every vreg read by a call (argument or indirect callee).  A value read at the
   call position must resolve to its pre-call home, so it is never split. */
static bool *collect_call_operands(IrFunction *f, u32 nvregs, Arena *arena)
{
    bool *op = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        op[v] = false;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_CALL)
            {
                continue;
            }
            if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee))
            {
                op[in->extra.call.callee.u.vreg] = true;
            }
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                IrOperand arg = in->extra.call.args[a];
                if (ir_operand_is_vreg(arg))
                {
                    op[arg.u.vreg] = true;
                }
            }
        }
    }
    return op;
}

static CfgEdge *collect_edges(IrFunction *f, u32 *out_n, Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u32 count = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        count += (u32) vec_size(((IrBlock *) vec_get(f->blocks, b))->preds);
    }
    CfgEdge *edges = arena_alloc(arena, (count ? count : 1) * sizeof(CfgEdge), _Alignof(CfgEdge));
    u32 n = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *succ = (IrBlock *) vec_get(f->blocks, b);
        size_t npred = vec_size(succ->preds);
        for (size_t p = 0; p < npred; p++)
        {
            edges[n].pred = ((IrBlock *) vec_get(succ->preds, p))->index;
            edges[n].succ = (u32) b;
            n++;
        }
    }
    *out_n = n;
    return edges;
}

static u32 block_index_by_label(IrFunction *f, const char *label)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        if (strcmp(((IrBlock *) vec_get(f->blocks, b))->label, label) == 0)
        {
            return (u32) b;
        }
    }
    return UINT32_MAX;
}

static u32 edge_index_of(const CfgEdge *edges, u32 nedges, u32 pred, u32 succ)
{
    for (u32 i = 0; i < nedges; i++)
    {
        if (edges[i].pred == pred && edges[i].succ == succ)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Vregs live on each CFG edge, including PHI operands read at the predecessor's
   end (which are not live-in to the successor). */
static Bitset **collect_edge_live(IrFunction *f, const LiveIntervals *set, const CfgEdge *edges,
                                  u32 nedges, Arena *arena)
{
    Bitset **live = arena_alloc(arena, (nedges ? nedges : 1) * sizeof(Bitset *), sizeof(void *));
    for (u32 i = 0; i < nedges; i++)
    {
        live[i] = bitset_new(arena, set->nvregs);
        bitset_or(live[i], set->live_in[edges[i].succ]);
    }
    size_t nblocks = vec_size(f->blocks);
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
                IrOperand val = in->extra.phi.entries[e].val;
                if (!ir_operand_is_vreg(val))
                {
                    continue;
                }
                u32 pred = block_index_by_label(f, in->extra.phi.entries[e].label);
                u32 ei = edge_index_of(edges, nedges, pred, (u32) b);
                if (ei != UINT32_MAX)
                {
                    bitset_set(live[ei], val.u.vreg);
                }
            }
        }
    }
    return live;
}

/* The last position at which a PHI result is defined (its latest predecessor
   end); a split at or before it would cut the definition. */
static u32 *collect_phi_max_def(IrFunction *f, const IrPositions *pos, u32 nvregs, Arena *arena)
{
    u32 *mx = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < nvregs; v++)
    {
        mx[v] = 0;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
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
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                u32 p = block_index_by_label(f, in->extra.phi.entries[e].label);
                if (p == UINT32_MAX)
                {
                    continue;
                }
                u32 gap = pos->block_end[p] > pos->block_base[p] ? pos->block_end[p] - 1
                                                                 : pos->block_base[p];
                if (gap > mx[in->result])
                {
                    mx[in->result] = gap;
                }
            }
        }
    }
    return mx;
}

static int pick_register(const RegBank *bank, const ActiveInterval *active, u32 nactive,
                         bool crossing, u16 avoid, int hint, u32 start)
{
    /* A coalescing hint may reuse a register still held by an interval that dies
       exactly here (end == start); any interval that lives past `start` blocks it. */
    if (hint >= 0 && bank_allows(bank, (u8) hint) && !(avoid & (u16) (1u << hint)) &&
        (!crossing || bank_callee_index(bank, (u8) hint) >= 0))
    {
        bool blocked = false;
        for (u32 a = 0; a < nactive; a++)
        {
            if (active[a].reg == (u8) hint && active[a].end > start)
            {
                blocked = true;
                break;
            }
        }
        if (!blocked)
        {
            return hint;
        }
    }

    bool used[16] = {false};
    for (u32 a = 0; a < nactive; a++)
    {
        used[active[a].reg] = true;
    }
    /* A non-crossing value prefers a caller-saved register (no prologue save);
       only fall back to a callee-saved one when the caller-saved bank is full. */
    for (u8 pass = 0; pass < 2; pass++)
    {
        for (u8 i = 0; i < bank->num_regs; i++)
        {
            u8 reg = bank->names[i];
            if (used[reg] || !bank_allows(bank, reg) || (avoid & (u16) (1u << reg)))
            {
                continue;
            }
            bool callee = bank_callee_index(bank, reg) >= 0;
            if (crossing)
            {
                if (!callee)
                {
                    continue;
                }
            }
            else if ((pass == 0) == callee)
            {
                continue; /* pass 0 takes caller-saved, pass 1 callee-saved */
            }
            return reg;
        }
        if (crossing)
        {
            break; /* no second pass */
        }
    }
    return -1;
}

/* Registers a call's argument setup writes that the allocator may otherwise
   hand out. A call operand must avoid them: another argument's move could
   otherwise clobber it before it is read. */
static u16 call_arg_avoid_mask(const RegBank *bank, const u8 *args, u8 nargs)
{
    u16 mask = 0;
    for (u8 i = 0; i < nargs; i++)
    {
        if (bank_allows(bank, args[i]))
        {
            mask |= (u16) (1u << args[i]);
        }
    }
    return mask;
}

/* A call whose arguments include a by-value record: the record's stack copy
   runs through rep movsb, clobbering %rsi/%rdi/%rcx, so every argument of such
   a call must keep off the argument lanes. Unknown types stay conservative. */
static bool call_has_record_arg(const IrInstr *in)
{
    if (!in->extra.call.arg_types)
    {
        return true;
    }
    for (u32 a = 0; a < in->extra.call.nargs; a++)
    {
        Type *t = in->extra.call.arg_types[a];
        if (t && type_is_record(t))
        {
            return true;
        }
    }
    return false;
}

/* Mark every vreg whose value arrives in (caller side) or is loaded from
   (callee side) an ABI argument register. Such a vreg must not itself be
   allocated to an argument register, or one argument's move would clobber
   another's incoming value. A scalar GP argument of a record-free call is the
   exception: lowering schedules those moves as a parallel copy, so the value
   may ride its own argument lane (see collect_arg_prefs). */
static bool *mark_arg_reg_vregs(IrFunction *f, u32 nvregs, Arena *arena)
{
    bool *op = arena_alloc(arena, nvregs * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        op[v] = false;
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        u32 pv = ((IrParam *) vec_get(f->params, i))->vreg;
        if (pv != NO_VREG)
        {
            op[pv] = true;
        }
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_CALL)
            {
                continue;
            }
            if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee))
            {
                op[in->extra.call.callee.u.vreg] = true;
            }
            bool conservative = call_has_record_arg(in);
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                IrOperand arg = in->extra.call.args[a];
                if (!ir_operand_is_vreg(arg))
                {
                    continue;
                }
                Type *t = in->extra.call.arg_types ? in->extra.call.arg_types[a] : NULL;
                if (conservative || (t && type_is_fp(t)))
                {
                    op[arg.u.vreg] = true;
                }
            }
        }
    }
    return op;
}

/* Preferred physical lane per vreg: a scalar GP argument of a record-free call
   is born in the lane it will be passed in, so its argument move is a no-op.
   Returns -1 for every other value. */
static int *collect_arg_prefs(IrFunction *f, u32 nvregs, const TargetDesc *target, Arena *arena)
{
    int *pref = arena_alloc(arena, nvregs * sizeof(int), sizeof(int));
    for (u32 v = 0; v < nvregs; v++)
    {
        pref[v] = -1;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_CALL || call_has_record_arg(in))
            {
                continue;
            }
            u32 gpi = 0;
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                Type *t = in->extra.call.arg_types ? in->extra.call.arg_types[a] : NULL;
                if (t && type_is_fp(t))
                {
                    continue; /* rides an XMM lane, not a GP one */
                }
                IrOperand arg = in->extra.call.args[a];
                if (ir_operand_is_vreg(arg) && gpi < target->ngp)
                {
                    pref[arg.u.vreg] = target->gp_args[gpi];
                }
                gpi++;
            }
        }
    }
    return pref;
}

/* When no register is free, a shorter-lived interval can take the register of
   an active interval that ends farther away and is read at most as often.
   Never evict a value with more reads than the one taking its register: that
   would trade a hot value for a colder one.  Returns the index to evict, or
   -1. */
static int pick_eviction(const ScanCtx *cx, bool crossing, u16 avoid, u32 iv_end, u32 new_uses)
{
    int best = -1;
    u32 best_end = iv_end;
    for (u32 a = 0; a < cx->nactive; a++)
    {
        u8 reg = cx->active[a].reg;
        if (avoid & (u16) (1u << reg))
        {
            continue;
        }
        if (crossing && bank_callee_index(cx->bank, reg) < 0)
        {
            continue;
        }
        if (cx->use_count[cx->active[a].vreg] > new_uses)
        {
            continue;
        }
        if (cx->active[a].end > best_end)
        {
            best_end = cx->active[a].end;
            best = (int) a;
        }
    }
    return best;
}

/* A value that lives over a call needs a callee-saved register or a slot.  A
   call's own result starts at the call position and so need not survive it. */
static bool scan_crosses(const ScanCtx *cx, const LiveInterval *iv)
{
    for (u32 i = 0; i < cx->ncall; i++)
    {
        u32 c = cx->calls[i];
        if (c < iv->start || c >= iv->end)
        {
            continue;
        }
        if (c == cx->def_pos[iv->vreg] && cx->defs[iv->vreg] &&
            cx->defs[iv->vreg]->opcode == OP_CALL)
        {
            continue; /* the call defines the value; it need not survive it */
        }
        return true;
    }
    return false;
}

/* A split at call position `c` moves the value's home at `c`.  It is sound
   only when no live CFG edge of the value straddles `c`: an edge from before
   the split to after it would deliver the wrong home, and a backward edge
   would do the same.  A PHI result must also be defined before the split. */
static bool split_allowed_at(const ScanCtx *cx, u32 v, u32 c)
{
    if (cx->defs[v] && cx->defs[v]->opcode == OP_PHI && c <= cx->phi_max_def[v])
    {
        return false;
    }
    for (u32 i = 0; i < cx->nedges; i++)
    {
        if (!bitset_test(cx->edge_live[i], v))
        {
            continue;
        }
        u32 p = cx->edges[i].pred;
        u32 s = cx->edges[i].succ;
        u32 e = cx->pos->block_end[p] > cx->pos->block_base[p] ? cx->pos->block_end[p] - 1
                                                               : cx->pos->block_base[p];
        u32 b = cx->pos->block_base[s];
        if ((e >= c) != (b > c))
        {
            return false;
        }
    }
    return true;
}

/* The uses of `v` at positions in [lo, hi).  Instruction operands, call
   arguments, and an indirect callee all count; the threshold only decides
   whether a reload pays for itself. */
static u32 uses_between(const ScanCtx *cx, u32 v, u32 lo, u32 hi)
{
    u32 count = 0;
    size_t nblocks = vec_size(cx->func->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(cx->func->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            u32 p = cx->pos->block_base[b] + (u32) 2 * (u32) ii;
            if (p < lo || p >= hi)
            {
                continue;
            }
            count += instr_use_count((IrInstr *) vec_get(blk->instrs, ii), v);
        }
    }
    return count;
}

static u32 next_call_pos(const ScanCtx *cx, u32 from)
{
    for (u32 i = 0; i < cx->ncall; i++)
    {
        if (cx->calls[i] > from)
        {
            return cx->calls[i];
        }
    }
    return 0;
}

/* The first call strictly inside (from, end) at which the value may split.
   A cut is only taken when the region it opens holds at least two uses, so
   the reload is amortized against the store/reload pair it costs. */
static u32 next_accepted_call(const ScanCtx *cx, const LiveInterval *iv, u32 from)
{
    for (u32 i = 0; i < cx->ncall; i++)
    {
        u32 c = cx->calls[i];
        if (c <= from || c >= iv->end)
        {
            continue;
        }
        u32 next = next_call_pos(cx, c);
        u32 hi = next ? next : iv->end + 1;
        if (uses_between(cx, iv->vreg, c, hi) >= 2 && split_allowed_at(cx, iv->vreg, c))
        {
            return c;
        }
    }
    return 0;
}

static bool has_call_in(const ScanCtx *cx, u32 lo, u32 hi)
{
    for (u32 i = 0; i < cx->ncall; i++)
    {
        if (cx->calls[i] > lo && cx->calls[i] < hi)
        {
            return true;
        }
    }
    return false;
}

static u16 scan_avoid(const ScanCtx *cx, const LiveInterval *iv, u32 start, u32 end)
{
    u16 avoid = cx->call_op[iv->vreg] ? cx->arg_avoid : 0;
    if (cx->bank->cls == RC_GPR)
    {
        avoid |= clobber_avoid(cx->clob, cx->nclob, start, end);
    }
    return avoid;
}

static SegRec *seg_rec_new(ScanCtx *cx, u32 vreg, u32 start, u32 end)
{
    SegRec *rec = arena_alloc(cx->arena, sizeof(SegRec), _Alignof(SegRec));
    rec->vreg = vreg;
    rec->start = start;
    rec->end = end;
    rec->kind = SEG_MEM;
    rec->reg = 0;
    rec->disp = 0;
    vec_push(cx->segs, rec);
    return rec;
}

/* A rematerialized value has no home: it is one whole-range SEG_REMAT run. */
static void push_remat_rec(ScanCtx *cx, const LiveInterval *iv)
{
    SegRec *rec = seg_rec_new(cx, iv->vreg, iv->start, iv->end);
    rec->kind = SEG_REMAT;
}

static void push_active(ScanCtx *cx, const SegRec *rec)
{
    cx->active[cx->nactive].vreg = rec->vreg;
    cx->active[cx->nactive].end = rec->end;
    cx->active[cx->nactive].reg = rec->reg;
    cx->active[cx->nactive].rec = (SegRec *) rec;
    cx->nactive++;
}

static int range_hint(const ScanCtx *cx, const LiveInterval *iv)
{
    int hint = coalesce_hint(cx->defs[iv->vreg], cx->alloc->phys_map, iv->cls, cx->vreg_cls);
    return hint >= 0 ? hint : cx->pref[iv->vreg];
}

static int assign_range(ScanCtx *cx, bool crossing, u16 avoid, int hint, SegRec *rec)
{
    int reg = pick_register(cx->bank, cx->active, cx->nactive, crossing, avoid, hint, rec->start);
    if (reg < 0)
    {
        int ev = pick_eviction(cx, crossing, avoid, rec->end, cx->use_count[rec->vreg]);
        if (ev >= 0)
        {
            reg = cx->active[ev].reg;
            cx->active[ev].rec->kind = SEG_MEM;
            cx->active[ev].rec->reg = 0;
            cx->alloc->phys_map[cx->active[ev].vreg] = -1;
            cx->active[ev] = cx->active[--cx->nactive];
        }
    }
    if (reg < 0)
    {
        return -1;
    }
    rec->kind = SEG_REG;
    rec->reg = (u8) reg;
    push_active(cx, rec);
    return reg;
}

/* The block containing the whole [start, end] range, or -1 when it spans more
   than one.  Splitting is only sound inside one block: there position order is
   execution order, so a move before an instruction always precedes it. */
static int range_block(const ScanCtx *cx, u32 start, u32 end)
{
    for (u32 b = 0; b < cx->pos->nblocks; b++)
    {
        if (start >= cx->pos->block_base[b] && start < cx->pos->block_end[b])
        {
            return end < cx->pos->block_end[b] ? (int) b : -1;
        }
    }
    return -1;
}

static void record_seg_gap(ScanCtx *cx, u32 vreg, u32 pos)
{
    SegGap *g = arena_alloc(cx->arena, sizeof(SegGap), _Alignof(SegGap));
    g->pos = pos;
    g->vreg = vreg;
    vec_push(cx->seg_gaps, g);
}

/* Whether `in` reads `vreg` (an ordinary operand, a call argument, or an
   indirect callee; a PHI's edge reads live at its predecessor boundary). */
static bool instr_uses_vreg(const IrInstr *in, u32 vreg)
{
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        if (ir_operand_is_vreg(in->ops[oi]) && in->ops[oi].u.vreg == vreg)
        {
            return true;
        }
    }
    if (in->opcode == OP_CALL)
    {
        if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee) &&
            in->extra.call.callee.u.vreg == vreg)
        {
            return true;
        }
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            if (ir_operand_is_vreg(in->extra.call.args[a]) && in->extra.call.args[a].u.vreg == vreg)
            {
                return true;
            }
        }
    }
    return false;
}

/* The first instruction (even) position after `from` and at or before `end`
   that reads `vreg`, or 0.  A value is reloaded at its next use, so the reload
   position is always the start of a real instruction. */
static u32 first_use_after(const ScanCtx *cx, u32 vreg, u32 from, u32 end)
{
    u32 best = 0;
    for (size_t b = 0; b < vec_size(cx->func->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(cx->func->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            u32 p = cx->pos->block_base[b] + (u32) 2 * (u32) ii;
            if (p <= from || p > end)
            {
                continue;
            }
            if (instr_uses_vreg((IrInstr *) vec_get(blk->instrs, ii), vreg) &&
                (best == 0 || p < best))
            {
                best = p;
            }
        }
    }
    return best;
}

/* A range that cannot get a register is split at its next use: the head stays
   in the slot and the tail is rescanned, so the value rides a register from
   that use on.  Only within one block, so the reload has a fixed position in
   the instruction stream; repeated splits advance to successive uses. */
static bool pressure_split(ScanCtx *cx, const LiveInterval *iv, SegRec *rec, u32 start, u32 end,
                           bool crossing)
{
    if (crossing || start >= end || cx->split_depth >= PRESSURE_SPLIT_MAX ||
        range_block(cx, start, end) < 0)
    {
        return false;
    }
    u32 q = first_use_after(cx, iv->vreg, start, end);
    if (q == 0)
    {
        return false;
    }
    rec->end = q - 1;
    record_seg_gap(cx, iv->vreg, q);
    SplitTail *tail = arena_alloc(cx->arena, sizeof(SplitTail), _Alignof(SplitTail));
    tail->iv = iv;
    tail->from = q;
    tail->depth = cx->split_depth + 1;
    vec_push(cx->tails, tail);
    return true;
}

static void scan_range(ScanCtx *cx, const LiveInterval *iv, u32 start, u32 end, bool crossing)
{
    SegRec *rec = seg_rec_new(cx, iv->vreg, start, end);
    int reg = assign_range(cx, crossing, scan_avoid(cx, iv, start, end), range_hint(cx, iv), rec);
    if (reg >= 0)
    {
        if (start == iv->start && end == iv->end)
        {
            cx->alloc->phys_map[iv->vreg] = reg;
        }
        return;
    }
    pressure_split(cx, iv, rec, start, end, crossing);
}

/* A value live across a call that touches its first position cannot be split:
   there is no earlier segment to store before the call. */
static bool call_at_start(const ScanCtx *cx, const LiveInterval *iv)
{
    for (u32 i = 0; i < cx->ncall; i++)
    {
        if (cx->calls[i] != iv->start)
        {
            continue;
        }
        if (cx->defs[iv->vreg] && cx->defs[iv->vreg]->opcode == OP_CALL)
        {
            continue; /* the call defines the value */
        }
        return true;
    }
    return false;
}

static void record_gap(ScanCtx *cx, u32 vreg, u32 pos)
{
    CallGap *g = arena_alloc(cx->arena, sizeof(CallGap), _Alignof(CallGap));
    g->pos = pos;
    g->vreg = vreg;
    vec_push(cx->gaps, g);
}

static void scan_interval(ScanCtx *cx, Vec *tails, const LiveInterval *iv)
{
    int hint = range_hint(cx, iv);
    bool crossing = scan_crosses(cx, iv);
    if (!crossing || cx->call_op[iv->vreg] || cx->call_operand[iv->vreg] || call_at_start(cx, iv))
    {
        scan_range(cx, iv, iv->start, iv->end, crossing);
        return;
    }
    int reg = pick_register(cx->bank, cx->active, cx->nactive, true,
                            scan_avoid(cx, iv, iv->start, iv->end), hint, iv->start);
    if (reg >= 0)
    {
        SegRec *whole = seg_rec_new(cx, iv->vreg, iv->start, iv->end);
        whole->kind = SEG_REG;
        whole->reg = (u8) reg;
        push_active(cx, whole);
        cx->alloc->phys_map[iv->vreg] = reg;
        return;
    }
    u32 call = next_accepted_call(cx, iv, iv->start);
    if (call == 0)
    {
        scan_range(cx, iv, iv->start, iv->end, true);
        return;
    }
    scan_range(cx, iv, iv->start, call - 1, has_call_in(cx, iv->start, call));
    record_gap(cx, iv->vreg, call);
    SplitTail *tail = arena_alloc(cx->arena, sizeof(SplitTail), _Alignof(SplitTail));
    tail->iv = iv;
    tail->from = call;
    tail->depth = 0;
    vec_push(tails, tail);
}

static void scan_tail(ScanCtx *cx, Vec *tails, const LiveInterval *iv, u32 from)
{
    u32 call = next_accepted_call(cx, iv, from);
    u32 end = call ? call - 1 : iv->end;
    scan_range(cx, iv, from, end, has_call_in(cx, from, call ? call : iv->end));
    if (call)
    {
        record_gap(cx, iv->vreg, call);
        SplitTail *tail = arena_alloc(cx->arena, sizeof(SplitTail), _Alignof(SplitTail));
        tail->iv = iv;
        tail->from = call;
        tail->depth = 0;
        vec_push(tails, tail);
    }
}

static u32 tail_min_index(const Vec *tails)
{
    u32 best = 0;
    for (u32 i = 1; i < (u32) vec_size(tails); i++)
    {
        const SplitTail *a = (const SplitTail *) vec_get(tails, i);
        const SplitTail *b = (const SplitTail *) vec_get(tails, best);
        if (a->from < b->from || (a->from == b->from && a->iv->vreg < b->iv->vreg))
        {
            best = i;
        }
    }
    return best;
}

static bool tail_precedes(const Vec *tails, const LiveInterval *iv)
{
    const SplitTail *t = (const SplitTail *) vec_get(tails, tail_min_index(tails));
    return t->from < iv->start || (t->from == iv->start && t->iv->vreg < iv->vreg);
}

static void expire(ScanCtx *cx, u32 pos)
{
    for (u32 a = 0; a < cx->nactive;)
    {
        if (cx->active[a].end < pos)
        {
            cx->active[a] = cx->active[--cx->nactive];
        }
        else
        {
            a++;
        }
    }
}

/* Intervals and split tails are merged in (start, vreg) order so the active
   set always reflects everything live at the range being allocated. */
static void linear_scan_class(ScanCtx *cx, const LiveInterval **order, u32 nintervals)
{
    cx->active = arena_alloc(cx->arena, (nintervals ? nintervals : 1) * sizeof(ActiveInterval),
                             _Alignof(ActiveInterval));
    cx->nactive = 0;
    Vec *tails = vec_new(cx->arena);
    cx->tails = tails;
    u32 k = 0;
    while (k < nintervals || vec_size(tails) > 0)
    {
        bool take_tail = k >= nintervals || (vec_size(tails) > 0 && tail_precedes(tails, order[k]));
        if (take_tail)
        {
            u32 ti = tail_min_index(tails);
            SplitTail t = *(SplitTail *) vec_get(tails, ti);
            vec_set(tails, ti, vec_last(tails));
            vec_pop(tails);
            expire(cx, t.from);
            cx->split_depth = t.depth;
            scan_tail(cx, tails, t.iv, t.from);
            continue;
        }
        const LiveInterval *iv = order[k++];
        if (iv->cls != cx->bank->cls)
        {
            continue;
        }
        if (cx->remat[iv->vreg])
        {
            push_remat_rec(cx, iv);
            continue;
        }
        expire(cx, iv->start);
        cx->split_depth = 0;
        scan_interval(cx, tails, iv);
    }
}

/* %rbp without the frame-pointer reservation: it joins the allocatable
   callee-saved bank. */
static RegBank bank_with_rbp_allocatable(const RegBank *bank, u8 frame_reg)
{
    RegBank out = *bank;
    u8 n = 0;
    for (u8 i = 0; i < out.nfixed; i++)
    {
        if (out.fixed[i] != frame_reg)
        {
            out.fixed[n++] = out.fixed[i];
        }
    }
    out.nfixed = n;
    return out;
}

/* Linear scan in interval-start order: assign the lowest free register, else
   spill.  Reserved registers (implicit operands and scratch) are never handed
   out, so a fixed-encoding instruction's operands can be coerced in place. */
RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena)
{
    return regalloc_linear_ex(f, set, target, arena, false);
}

RegAllocation *regalloc_linear_ex(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                                  Arena *arena, bool allow_rbp)
{
    RegBank gpr =
        allow_rbp ? bank_with_rbp_allocatable(&target->gpr, target->frame_reg) : target->gpr;
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);

    const LiveInterval **order = sorted_intervals(set, arena);
    bool *call_op = mark_arg_reg_vregs(f, set->nvregs, arena);
    IrInstr **defs = collect_defs(f, set->nvregs, arena);
    bool *remat = arena_alloc(arena, set->nvregs * sizeof(bool), _Alignof(bool));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        remat[v] = false;
    }
    for (u32 v = 0; v < set->nvregs; v++)
    {
        if (defs[v] && defs[v]->opcode == OP_ALLOCA)
        {
            remat[v] = true;
        }
    }
    /* An address formed by adding a constant to a static alloca is itself a
       constant %rbp-relative address, so rematerialize it too. */
    for (u32 v = 0; v < set->nvregs; v++)
    {
        IrInstr *d = defs[v];
        if (!d || d->opcode != OP_GEP || !d->ops[1].is_imm || !ir_operand_is_vreg(d->ops[0]))
        {
            continue;
        }
        u32 bv = d->ops[0].u.vreg;
        if (bv < set->nvregs && defs[bv] && defs[bv]->opcode == OP_ALLOCA)
        {
            remat[v] = true;
        }
    }
    RegClass *vreg_cls = arena_alloc(arena, set->nvregs * sizeof(RegClass), _Alignof(RegClass));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        vreg_cls[v] = RC_NONE;
    }
    for (u32 i = 0; i < set->n; i++)
    {
        vreg_cls[set->ivs[i].vreg] = set->ivs[i].cls;
    }
    u32 nclob = 0;
    ClobberPos *clob = collect_clobbers(f, &set->pos, target, &nclob, arena);
    int *pref = collect_arg_prefs(f, set->nvregs, target, arena);
    u16 gpr_avoid = call_arg_avoid_mask(&gpr, target->gp_args, target->ngp);
    u16 xmm_avoid = call_arg_avoid_mask(&target->xmm, target->fp_args, target->nfp);
    Vec *segs = vec_new(arena);
    Vec *gaps = vec_new(arena);
    Vec *sgaps = vec_new(arena);
    u32 nedges = 0;
    CfgEdge *edges = collect_edges(f, &nedges, arena);
    Bitset **edge_live = collect_edge_live(f, set, edges, nedges, arena);
    u32 *phi_max_def = collect_phi_max_def(f, &set->pos, set->nvregs, arena);
    ScanCtx cx = {
        .alloc = alloc,
        .calls = calls,
        .ncall = ncall,
        .call_op = call_op,
        .clob = clob,
        .nclob = nclob,
        .defs = defs,
        .def_pos = collect_def_positions(f, &set->pos, set->nvregs, arena),
        .call_operand = collect_call_operands(f, set->nvregs, arena),
        .pos = &set->pos,
        .vreg_cls = vreg_cls,
        .pref = pref,
        .segs = segs,
        .gaps = gaps,
        .seg_gaps = sgaps,
        .func = f,
        .remat = remat,
        .use_count = collect_use_counts(f, set->nvregs, arena),
        .arena = arena,
        .edges = edges,
        .nedges = nedges,
        .edge_live = edge_live,
        .phi_max_def = phi_max_def,
    };
    cx.bank = &gpr;
    cx.arg_avoid = gpr_avoid;
    linear_scan_class(&cx, order, set->n);
    cx.bank = &target->xmm;
    cx.arg_avoid = xmm_avoid;
    linear_scan_class(&cx, order, set->n);
    build_segments(alloc, set, segs, gaps, sgaps, arena);
    alloc->saved_mask = saved_mask_of(alloc, &gpr);
    alloc->frame_size = pack_spills(alloc, set, target, arena);
    return alloc;
}

const CallGap *regalloc_call_gaps(const RegAllocation *alloc, u32 pos, u32 *count)
{
    u32 lo = 0;
    u32 hi = alloc->ncall_gaps;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        if (alloc->call_gaps[mid].pos < pos)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    u32 n = 0;
    while (lo + n < alloc->ncall_gaps && alloc->call_gaps[lo + n].pos == pos)
    {
        n++;
    }
    *count = n;
    return n ? &alloc->call_gaps[lo] : NULL;
}

const SegGap *regalloc_seg_gaps(const RegAllocation *alloc, u32 pos, u32 *count)
{
    u32 lo = 0;
    u32 hi = alloc->nseg_gaps;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        if (alloc->seg_gaps[mid].pos < pos)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    u32 n = 0;
    while (lo + n < alloc->nseg_gaps && alloc->seg_gaps[lo + n].pos == pos)
    {
        n++;
    }
    *count = n;
    return n ? &alloc->seg_gaps[lo] : NULL;
}

static const RegSegment *find_segment(const RegAllocation *alloc, u32 vreg, u32 pos)
{
    for (u32 s = alloc->seg_begin[vreg]; s < alloc->seg_begin[vreg + 1]; s++)
    {
        if (pos >= alloc->segments[s].start && pos <= alloc->segments[s].end)
        {
            return &alloc->segments[s];
        }
    }
    return NULL;
}

RegLoc loc_at(const RegAllocation *alloc, IrOperand op, u32 pos)
{
    RegLoc loc = {0};
    if (op.is_imm)
    {
        loc.kind = LOC_IMM;
        return loc;
    }
    ASSERT(ir_operand_is_vreg(op) && "globals/functions resolve to addresses, not locations");
    ASSERT(op.u.vreg < alloc->nvregs && "operand vreg within the module's table");
    const LiveInterval *iv = interval_of(alloc, op.u.vreg);
    loc.cls = iv ? iv->cls : RC_GPR;
    const RegSegment *seg = find_segment(alloc, op.u.vreg, pos);
    ASSERT(seg && "a live vreg's position resolves to a segment");
    switch (seg->kind)
    {
        case SEG_REG:
            loc.kind = LOC_REG;
            loc.reg = seg->reg;
            return loc;
        case SEG_MEM:
            loc.kind = LOC_MEM;
            loc.disp = -(i32) alloc->slot_map[op.u.vreg];
            return loc;
        case SEG_REMAT:
            loc.kind = LOC_REMAT;
            loc.cls = RC_GPR;
            loc.disp = seg->disp;
            return loc;
    }
    ASSERT(false && "unknown segment kind");
    return loc;
}

static RegSegment *remat_segment(RegAllocation *alloc, u32 vreg)
{
    if (vreg >= alloc->nvregs)
    {
        return NULL;
    }
    for (u32 s = alloc->seg_begin[vreg]; s < alloc->seg_begin[vreg + 1]; s++)
    {
        if (alloc->segments[s].kind == SEG_REMAT)
        {
            return &alloc->segments[s];
        }
    }
    return NULL;
}

bool regalloc_is_remat(const RegAllocation *alloc, u32 vreg)
{
    if (vreg >= alloc->nvregs || alloc->seg_begin[vreg] >= alloc->seg_begin[vreg + 1])
    {
        return false;
    }
    return alloc->segments[alloc->seg_begin[vreg]].kind == SEG_REMAT;
}

void regalloc_set_remat_disp(RegAllocation *alloc, u32 vreg, i32 disp)
{
    RegSegment *seg = remat_segment(alloc, vreg);
    if (seg)
    {
        seg->disp = disp;
    }
}

i32 regalloc_remat_disp(const RegAllocation *alloc, u32 vreg)
{
    if (vreg >= alloc->nvregs)
    {
        return 0;
    }
    for (u32 s = alloc->seg_begin[vreg]; s < alloc->seg_begin[vreg + 1]; s++)
    {
        if (alloc->segments[s].kind == SEG_REMAT)
        {
            return alloc->segments[s].disp;
        }
    }
    return 0;
}
