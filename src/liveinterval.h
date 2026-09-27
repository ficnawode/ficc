#ifndef FICC_INTERVAL_H
#define FICC_INTERVAL_H

#include "ir.h"
#include "target.h"
#include "util/arena.h"
#include "util/bitset.h"
#include "util/types.h"

typedef struct
{
    u32 vreg;
    u32 start;
    u32 end;
    u8 width;
    RegClass cls;
    int assigned_reg;
} LiveInterval;

/* block b's j-th instruction is at block_base[b] + 2j; block_end[b] is past the last. */
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
    u32 nblocks;
    Bitset **live_in;
    Bitset **live_out;
} LiveIntervals;

LiveIntervals liveinterval_compute(IrFunction *f, IrModule *mod, Arena *arena);

#endif
