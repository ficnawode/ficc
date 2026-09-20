#ifndef FICC_REGALLOC_H
#define FICC_REGALLOC_H

#include "liveinterval.h"
#include "util/types.h"
#include "util/vec.h"

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
} RegAllocation;

/* All-spilled allocation: every vreg rides its own packed spill slot, matching
   the legacy stack machine's frame exactly. */
RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena);

/* Linear scan over the target's GPR/XMM banks; reserved and caller-saved
   registers constrain the choice, RC_X87 always spills.  Deterministic. */
RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena);

typedef struct CodegenCtx CodegenCtx;

/* Where an operand currently lives while lowering an instruction. */
typedef struct
{
    enum
    {
        LOC_REG, /* in physical register `reg`, class `cls` */
        LOC_MEM, /* at [rbp + disp] */
        LOC_IMM, /* a constant */
    } kind;
    RegClass cls;
    u8 reg;
    i32 disp;
} RegLoc;

/* Resolve a vreg/immediate operand against the allocation; globals and
   function addresses are emitted by lowering, never resolved here. */
RegLoc loc_of(const RegAllocation *alloc, IrOperand op);

/* Force `src` into physical register `reg` of class `to`, emitting a mov (or a
   register-to-register copy / immediate load) when it is not already there. */
RegLoc coerce(const RegAllocation *alloc, CodegenCtx *ctx, RegLoc src, RegClass to, u8 reg);

#endif