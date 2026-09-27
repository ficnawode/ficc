#ifndef FICC_TARGET_H
#define FICC_TARGET_H

#include "ir.h"
#include "util/types.h"

typedef enum
{
    RC_GPR,
    RC_XMM,
    RC_X87,
    RC_NONE,
} RegClass;

typedef struct
{
    RegClass cls;
    u8 num_regs;
    u8 names[16];
    u8 callee_saved[16];
    u8 ncallee_saved;
    u8 align;
    bool memory_only;
    u8 fixed[16];
    u8 nfixed;
} RegBank;

#define R_X87_ST0 0

typedef struct TargetDesc TargetDesc;
struct TargetDesc
{
    const char *name;
    RegBank gpr;
    RegBank xmm;
    RegBank x87;
    u8 word_width;
    u8 frame_align;
    u8 spill_align[17];
    const u8 *gp_args;
    const u8 *fp_args;
    u8 ngp;
    u8 nfp;
    u8 (*return_reg)(const TargetDesc *, u8 width, RegClass cls);
    const u8 *clobbered_call;
    u8 nclobbered_call;
    bool (*needs_reg)(const TargetDesc *, IrOpcode op, u8 width, bool is_fp, int opnd);
    /* A value live across the instruction must avoid its implicit clobbers. */
    u16 (*instr_clobbers)(const TargetDesc *, const IrInstr *in);
    u8 frame_reg;
};

const TargetDesc *x86_64_target(void);

#endif
