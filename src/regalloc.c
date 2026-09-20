#include "regalloc.h"
#include "spill.h"
#include "target.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/vec.h"
#include <stdlib.h>

static void scan_call_positions(IrFunction *f, const IrPositions *pos, Vec *call_sites,
                                Arena *arena)
{
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
            u32 *p = arena_alloc(arena, sizeof(u32), sizeof(u32));
            *p = pos->block_base[b] + (u32) 2 * (u32) ii;
            vec_push(call_sites, p);
        }
    }
}

RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena)
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
        slot_map[v] = 8; /* never-live vregs keep an unused slot */
    }
    alloc->phys_map = phys_map;
    alloc->slot_map = slot_map;

    SlotRange *ranges =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(SlotRange), _Alignof(SlotRange));
    for (u32 i = 0; i < set->n; i++)
    {
        ranges[i].vreg = set->ivs[i].vreg;
        ranges[i].start = set->ivs[i].start;
        ranges[i].end = set->ivs[i].end;
        ranges[i].is16 = set->ivs[i].width == 16;
    }
    qsort(ranges, set->n, sizeof(SlotRange), spill_slot_range_cmp);
    alloc->frame_size = spill_pack_slots(x86_64_target(), ranges, set->n, slot_map, arena);

    Vec *call_sites = vec_new(arena);
    scan_call_positions(f, &set->pos, call_sites, arena);
    alloc->call_sites = call_sites;
    alloc->saved_mask = 0;
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
