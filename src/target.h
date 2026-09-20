#ifndef FICC_TARGET_H
#define FICC_TARGET_H

#include "ir.h"
#include "util/types.h"

/* Register classes the allocator may assign (D22.5: x87 is memory-only). */
typedef enum
{
    RC_GPR,
    RC_XMM,
    RC_X87,
    RC_NONE,
} RegClass;

/* One allocatable bank: phys ids in x86_emit numbering, spills aligned to `align`. */
typedef struct
{
    RegClass cls;
    u8 num_regs;
    u8 names[16];
    u8 callee_saved[16];
    u8 ncallee_saved;
    u8 align;         /* spill-slot alignment for a value of this class */
    bool memory_only; /* values never live in a register (RC_X87) */
    u8 fixed[16];     /* reserved from allocation: implicit operands and scratch */
    u8 nfixed;
} RegBank;

/* The x87 return carrier: element 0 of the physical x87 stack. */
#define R_X87_ST0 0

/* What the allocator and prologue may know about a target (B seam). */
typedef struct TargetDesc TargetDesc;
struct TargetDesc
{
    const char *name;
    RegBank gpr;
    RegBank xmm;
    RegBank x87;        /* memory_only = true */
    u8 word_width;      /* 8 */
    u8 frame_align;     /* 16 */
    u8 spill_align[17]; /* alignment per byte width (index 0 unused); 16-byte x87 slots */
    const u8 *gp_args;  /* RDI, RSI, RDX, RCX, R8, R9 (phys ids) */
    const u8 *fp_args;  /* XMM0..XMM7 */
    u8 ngp;
    u8 nfp;
    /* The register an aggregate/scalar return rides; %st0 for RC_X87. */
    u8 (*return_reg)(const TargetDesc *, u8 width, RegClass cls);
    const u8 *clobbered_call; /* caller-saved GPR set, for the call-crossing scan */
    u8 nclobbered_call;
    /* Whether operand `opnd` of an IR op must sit in a specific register. */
    bool (*needs_reg)(const TargetDesc *, IrOpcode op, u8 width, bool is_fp, int opnd);
};

const TargetDesc *x86_64_target(void);

#endif