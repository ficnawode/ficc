#ifndef FICC_SPILL_H
#define FICC_SPILL_H

#include "target.h"
#include "util/arena.h"
#include "util/types.h"

typedef struct
{
    u32 start;
    u32 end;
    u32 vreg;
    u8 is16;
} SlotRange;

int spill_slot_range_cmp(const void *a, const void *b);

u32 spill_pack_slots(const TargetDesc *target, SlotRange *ranges, size_t nranges, u32 *slot_off,
                     Arena *arena);

#endif