#include "spill.h"
#include "util/arena.h"
#include "util/assert.h"

#define ALIGN_UP(v, align) (((v) + ((align) - 1)) & ~((align) - 1))

int spill_slot_range_cmp(const void *a, const void *b)
{
    const SlotRange *ra = (const SlotRange *) a;
    const SlotRange *rb = (const SlotRange *) b;
    if (ra->start < rb->start)
    {
        return -1;
    }
    if (ra->start > rb->start)
    {
        return 1;
    }
    if (ra->end < rb->end)
    {
        return -1;
    }
    if (ra->end > rb->end)
    {
        return 1;
    }
    if (ra->vreg < rb->vreg)
    {
        return -1;
    }
    if (ra->vreg > rb->vreg)
    {
        return 1;
    }
    return 0;
}

static u32 pack_slot_class(SlotRange *ranges, size_t nranges, bool is16, u32 base, u32 stride,
                           u32 *slot_off, Arena *arena)
{
    u32 *expire = arena_alloc(arena, nranges * sizeof(u32), sizeof(u32));
    u32 nslots = 0;
    for (size_t r = 0; r < nranges; r++)
    {
        SlotRange *ra = &ranges[r];
        if (ra->is16 != is16)
        {
            continue;
        }
        u32 slot = 0;
        bool reused = false;
        for (u32 i = 0; i < nslots; i++)
        {
            if (expire[i] < ra->start)
            {
                slot = i;
                reused = true;
                break;
            }
        }
        if (!reused)
        {
            slot = nslots++;
        }
        expire[slot] = ra->end;
        slot_off[ra->vreg] = base + (slot + 1) * stride;
    }
    return nslots;
}

u32 spill_pack_slots(const TargetDesc *target, SlotRange *ranges, size_t nranges, u32 *slot_off,
                     Arena *arena)
{
    u32 stride8 = target->spill_align[8];
    u32 stride16 = target->spill_align[16];
    u32 nslots8 = pack_slot_class(ranges, nranges, false, 0, stride8, slot_off, arena);
    u32 total8 = nslots8 * stride8;
    u32 base16 = ALIGN_UP(total8, stride16);
    u32 nslots16 = pack_slot_class(ranges, nranges, true, base16, stride16, slot_off, arena);
    if (nslots16 > 0)
    {
        return base16 + nslots16 * stride16;
    }
    return total8;
}