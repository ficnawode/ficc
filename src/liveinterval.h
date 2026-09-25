#ifndef FICC_INTERVAL_H
#define FICC_INTERVAL_H

#include "ir.h"
#include "target.h"
#include "util/arena.h"
#include "util/bitset.h"
#include "util/types.h"

/* A value's live range in instruction-position space; `assigned_reg` is -1
   until the allocator hands out physical registers. */
typedef struct
{
    u32 vreg;
    u32 start;
    u32 end;
    u8 width;
    RegClass cls;
    int assigned_reg;
} LiveInterval;

/* Per-function position numbering: block b's j-th instruction sits at
   block_base[b] + 2j; the odd slots between instructions are phi-copy and
   scheduling gaps.  block_end[b] is the boundary past b's last instruction. */
typedef struct
{
    u32 nblocks;
    u32 npositions;
    u32 *block_base;
    u32 *block_end;
} IrPositions;

typedef struct
{
    IrPositions pos;
    u32 nvregs;
    LiveInterval *ivs; /* live vregs in ascending vreg order */
    u32 n;
    u32 nblocks;       /* block count backing live_in/live_out, 0 when none */
    Bitset **live_in;  /* per block: vregs live at block entry */
    Bitset **live_out; /* per block: vregs live at block exit */
} LiveIntervals;

LiveIntervals liveinterval_compute(IrFunction *f, IrModule *mod, Arena *arena);

#endif