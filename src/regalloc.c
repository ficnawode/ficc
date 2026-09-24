#include "regalloc.h"
#include "spill.h"
#include "target.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/vec.h"
#include <stdlib.h>

typedef struct
{
    const LiveInterval *iv;
    u8 reg;
} ActiveInterval;

typedef struct
{
    u32 pos;
    u16 mask;
} ClobberPos;

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

static u32 pack_spills(const RegAllocation *alloc, const LiveIntervals *set,
                       const TargetDesc *target, Arena *arena)
{
    SlotRange *ranges =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(SlotRange), _Alignof(SlotRange));
    u32 n = 0;
    for (u32 i = 0; i < set->n; i++)
    {
        const LiveInterval *iv = &set->ivs[i];
        if (alloc->phys_map[iv->vreg] >= 0)
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

RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena)
{
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);
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

/* A value live at a call must ride a callee-saved register or a slot. The start
   bound is inclusive: an entry parameter (position 0) survives an entry call. */
static bool crosses_call(const u32 *calls, u32 ncall, u32 start, u32 end)
{
    for (u32 i = 0; i < ncall; i++)
    {
        if (calls[i] >= start && calls[i] < end)
        {
            return true;
        }
    }
    return false;
}

static int pick_register(const RegBank *bank, const ActiveInterval *active, u32 nactive,
                         bool crossing, u16 avoid)
{
    bool used[16] = {false};
    for (u32 a = 0; a < nactive; a++)
    {
        used[active[a].reg] = true;
    }
    for (u8 i = 0; i < bank->num_regs; i++)
    {
        u8 reg = bank->names[i];
        if (used[reg] || !bank_allows(bank, reg) || (avoid & (u16) (1u << reg)))
        {
            continue;
        }
        if (crossing && bank_callee_index(bank, reg) < 0)
        {
            continue;
        }
        return reg;
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

/* Mark every vreg whose value arrives in (caller side) or is loaded from
   (callee side) an ABI argument register. Such a vreg must not itself be
   allocated to an argument register, or one argument's move would clobber
   another's incoming value. */
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
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                if (ir_operand_is_vreg(in->extra.call.args[a]))
                {
                    op[in->extra.call.args[a].u.vreg] = true;
                }
            }
        }
    }
    return op;
}

static void linear_scan_class(RegAllocation *alloc, const LiveInterval **order, u32 nintervals,
                              const RegBank *bank, const u32 *calls, u32 ncall, const bool *call_op,
                              u16 arg_avoid, const ClobberPos *clob, u32 nclob, Arena *arena)
{
    ActiveInterval *active = arena_alloc(
        arena, (nintervals ? nintervals : 1) * sizeof(ActiveInterval), _Alignof(ActiveInterval));
    u32 nactive = 0;
    for (u32 k = 0; k < nintervals; k++)
    {
        const LiveInterval *iv = order[k];
        if (iv->cls != bank->cls)
        {
            continue;
        }
        for (u32 a = 0; a < nactive;)
        {
            if (active[a].iv->end < iv->start)
            {
                active[a] = active[--nactive];
            }
            else
            {
                a++;
            }
        }
        bool crossing = crosses_call(calls, ncall, iv->start, iv->end);
        u16 avoid = call_op[iv->vreg] ? arg_avoid : 0;
        if (bank->cls == RC_GPR)
        {
            avoid |= clobber_avoid(clob, nclob, iv->start, iv->end);
        }
        int reg = pick_register(bank, active, nactive, crossing, avoid);
        if (reg < 0)
        {
            continue;
        }
        alloc->phys_map[iv->vreg] = reg;
        active[nactive].iv = iv;
        active[nactive].reg = (u8) reg;
        nactive++;
    }
}

/* Linear scan in interval-start order: assign the lowest free register, else
   spill.  Reserved registers (implicit operands and scratch) are never handed
   out, so a fixed-encoding instruction's operands can be coerced in place. */
RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena)
{
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);

    const LiveInterval **order = sorted_intervals(set, arena);
    bool *call_op = mark_arg_reg_vregs(f, set->nvregs, arena);
    u32 nclob = 0;
    ClobberPos *clob = collect_clobbers(f, &set->pos, target, &nclob, arena);
    u16 gpr_avoid = call_arg_avoid_mask(&target->gpr, target->gp_args, target->ngp);
    u16 xmm_avoid = call_arg_avoid_mask(&target->xmm, target->fp_args, target->nfp);
    linear_scan_class(alloc, order, set->n, &target->gpr, calls, ncall, call_op, gpr_avoid, clob,
                      nclob, arena);
    linear_scan_class(alloc, order, set->n, &target->xmm, calls, ncall, call_op, xmm_avoid, clob,
                      nclob, arena);
    u8 saved_mask = 0;
    for (u32 v = 0; v < set->nvregs; v++)
    {
        int reg = alloc->phys_map[v];
        int callee = reg >= 0 ? bank_callee_index(&target->gpr, (u8) reg) : -1;
        if (callee >= 0)
        {
            saved_mask |= (u8) (1u << callee);
        }
    }
    alloc->saved_mask = saved_mask;
    alloc->frame_size = pack_spills(alloc, set, target, arena);
    return alloc;
}

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

RegLoc loc_of(const RegAllocation *alloc, IrOperand op)
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
    int phys = alloc->phys_map[op.u.vreg];
    if (phys >= 0)
    {
        loc.kind = LOC_REG;
        loc.reg = (u8) phys;
        return loc;
    }
    loc.kind = LOC_MEM;
    loc.disp = -(i32) alloc->slot_map[op.u.vreg];
    return loc;
}
