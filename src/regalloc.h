#ifndef FICC_REGALLOC_H
#define FICC_REGALLOC_H

#include "liveinterval.h"
#include "util/types.h"
#include "util/vec.h"

typedef enum
{
    SEG_REG,
    SEG_MEM,
    SEG_REMAT,
} SegKind;

typedef struct
{
    u32 start;
    u32 end;
    u8 kind;
    u8 reg;
    i32 disp;
} RegSegment;

/* A vreg split at the call: pre-segment [., pos-1], post-segment [pos, .]. */
typedef struct
{
    u32 pos;
    u32 vreg;
} CallGap;

/* A vreg split at a pressure point: pre-segment [., pos-1], post-segment [pos, .]. */
typedef struct
{
    u32 pos;
    u32 vreg;
} SegGap;

/* The allocator's output, consumed by lowering and frame building. */
typedef struct
{
    LiveInterval *ivs;
    u32 n;
    u32 nvregs;
    int *phys_map;
    u32 *slot_map;
    Vec *call_sites;
    u8 saved_mask;
    u32 frame_size;

    RegSegment *segments; /* flat, ordered by (vreg, start) */
    u32 *seg_begin;       /* CSR rows, length nvregs+1 */
    u32 nsegments;
    CallGap *call_gaps; /* vregs split at a call, ordered by (pos, vreg) */
    u32 ncall_gaps;
    SegGap *seg_gaps; /* vregs split at a pressure point, ordered by (pos, vreg) */
    u32 nseg_gaps;
    u8 *has_slot;
} RegAllocation;

RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena);

RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena);

RegAllocation *regalloc_linear_ex(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                                  Arena *arena, bool allow_rbp);

typedef struct
{
    enum
    {
        LOC_REG,
        LOC_MEM,
        LOC_IMM,
        LOC_REMAT,
    } kind;
    RegClass cls;
    u8 reg;
    i32 disp;
} RegLoc;

RegLoc loc_at(const RegAllocation *alloc, IrOperand op, u32 pos);

bool regalloc_is_remat(const RegAllocation *alloc, u32 vreg);

void regalloc_set_remat_disp(RegAllocation *alloc, u32 vreg, i32 disp);

i32 regalloc_remat_disp(const RegAllocation *alloc, u32 vreg);

const CallGap *regalloc_call_gaps(const RegAllocation *alloc, u32 pos, u32 *count);

const SegGap *regalloc_seg_gaps(const RegAllocation *alloc, u32 pos, u32 *count);

#endif
