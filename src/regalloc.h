#ifndef FICC_REGALLOC_H
#define FICC_REGALLOC_H

#include "liveinterval.h"
#include "util/types.h"
#include "util/vec.h"

/* One contiguous run of positions sharing a location. */
typedef enum
{
    SEG_REG,   /* in physical register `reg` */
    SEG_MEM,   /* at [rbp - slot_map[vreg]] */
    SEG_REMAT, /* recomputed from [rbp + remat_disp[vreg]] */
} SegKind;

typedef struct
{
    u32 start; /* first position, inclusive */
    u32 end;   /* last position, inclusive */
    u8 kind;   /* SegKind */
    u8 reg;    /* SEG_REG: physical register id */
} RegSegment;

/* A vreg whose location changes across the call at `pos`: the pre-segment
   holds [., pos-1], the post-segment [pos, .]. */
typedef struct
{
    u32 pos;
    u32 vreg;
} CallGap;

/* Pure data: the allocator's output, consumed by lowering and frame building. */
typedef struct
{
    LiveInterval *ivs;
    u32 n;
    u32 nvregs;
    int *phys_map;   /* vreg → physical register id, -1 when spilled */
    u32 *slot_map;   /* vreg → offset below %rbp (spilled values and RC_X87) */
    Vec *call_sites; /* Vec<u32*> — instruction positions of every OP_CALL */
    u8 saved_mask;   /* bit i set when target->gpr.callee_saved[i] is used */
    u32 frame_size;  /* packed spill bytes below %rbp */
    u8 *remat;       /* vreg → 1 when the value is recomputed at each use, not stored */
    i32 *remat_disp; /* vreg → [rbp+disp] the recomputed address loads from (remat only) */

    RegSegment *segments; /* flat, ordered by (vreg, start) */
    u32 *seg_begin;       /* CSR rows, length nvregs+1 */
    u32 nsegments;
    CallGap *call_gaps; /* vregs split at a call, ordered by (pos, vreg) */
    u32 ncall_gaps;
    u8 *has_slot; /* vreg → 1 when a packed spill slot is reserved for it */
} RegAllocation;

/* All-spilled allocation: every vreg rides its own packed spill slot, matching
   the legacy stack machine's frame exactly. */
RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena);

/* Linear scan over the target's GPR/XMM banks; reserved and caller-saved
   registers constrain the choice, RC_X87 always spills.  Deterministic. */
RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena);

/* As regalloc_linear, but %rbp joins the allocatable bank (the caller omits the
   frame pointer, so it is no longer the frame base). */
RegAllocation *regalloc_linear_ex(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                                  Arena *arena, bool allow_rbp);

/* Where an operand currently lives while lowering an instruction. */
typedef struct
{
    enum
    {
        LOC_REG,   /* in physical register `reg`, class `cls` */
        LOC_MEM,   /* at [rbp + disp] */
        LOC_IMM,   /* a constant */
        LOC_REMAT, /* recompute: an address at [rbp + disp] */
    } kind;
    RegClass cls;
    u8 reg;
    i32 disp;
} RegLoc;

/* Resolve a vreg/immediate operand at position `pos` against the allocation;
   globals and function addresses are emitted by lowering, never resolved here. */
RegLoc loc_at(const RegAllocation *alloc, IrOperand op, u32 pos);

/* The vregs whose location changes across the call at `pos`, or NULL when
   none.  `count` receives the number of entries. */
const CallGap *regalloc_call_gaps(const RegAllocation *alloc, u32 pos, u32 *count);

#endif