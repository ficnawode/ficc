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

/* A call clobbers the caller-saved set at its position; a value strictly
   spanning that position must ride a callee-saved register or a slot. */
static bool crosses_call(const u32 *calls, u32 ncall, u32 start, u32 end)
{
    for (u32 i = 0; i < ncall; i++)
    {
        if (calls[i] > start && calls[i] < end)
        {
            return true;
        }
    }
    return false;
}

static int pick_register(const RegBank *bank, const ActiveInterval *active, u32 nactive,
                         bool crossing)
{
    bool used[16] = {false};
    for (u32 a = 0; a < nactive; a++)
    {
        used[active[a].reg] = true;
    }
    for (u8 i = 0; i < bank->num_regs; i++)
    {
        u8 reg = bank->names[i];
        if (used[reg] || !bank_allows(bank, reg))
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

static void linear_scan_class(RegAllocation *alloc, const LiveInterval **order, u32 nintervals,
                              const RegBank *bank, const u32 *calls, u32 ncall, u8 *saved_mask,
                              Arena *arena)
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
        int reg = pick_register(bank, active, nactive, crossing);
        if (reg < 0)
        {
            continue;
        }
        alloc->phys_map[iv->vreg] = reg;
        int callee = bank_callee_index(bank, (u8) reg);
        if (callee >= 0)
        {
            *saved_mask |= (u8) (1u << callee);
        }
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
    u8 saved_mask = 0;
    linear_scan_class(alloc, order, set->n, &target->gpr, calls, ncall, &saved_mask, arena);
    linear_scan_class(alloc, order, set->n, &target->xmm, calls, ncall, &saved_mask, arena);
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
