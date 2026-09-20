#ifndef FICC_SPILL_H
#define FICC_SPILL_H

#include "target.h"
#include "util/arena.h"
#include "util/types.h"

/* A live range eligible for a spill slot; sorted before packing. */
typedef struct
{
    u32 start;
    u32 end;
    u32 vreg;
    u8 is16;
} SlotRange;

int spill_slot_range_cmp(const void *a, const void *b);

/* First-fit pack live ranges into non-overlapping slots below %rbp; slot_off[vreg]
   records each vreg's offset.  Returns the packed frame bytes. */
u32 spill_pack_slots(const TargetDesc *target, SlotRange *ranges, size_t nranges, u32 *slot_off,
                     Arena *arena);

#endif