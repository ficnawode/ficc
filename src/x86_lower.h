#ifndef FICC_X86_LOWER_H
#define FICC_X86_LOWER_H

#include "codegen.h"
#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/arena.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "util/vec.h"
#include "x86_emit.h"
#include "x86_frame.h"

typedef struct
{
    u8 kind;
    u8 scale;
    i32 disp;
    IrOperand base;
    IrOperand index;
} GepFold;

typedef struct X86LowerCtx X86LowerCtx;
struct X86LowerCtx
{
    IrFunction *func;
    IrModule *mod;
    Arena *arena;
    ByteBuf *buf;
    const TargetDesc *target;
    const RegAllocation *alloc;
    const LinearFrame *frame;
    Vec *patches;
    Vec *block_patches;
    Vec *global_patches;
    Vec *func_patches;
    Vec **phi_copies;
    Vec *switch_tables;
    size_t *block_offsets;
    StrMap *label_to_index;
    const IrPositions *pos;
    u32 *position_offsets;
    u32 cur_pos;
    u32 *use_count;
    GepFold *gep_folds;
    bool *zero_extended;
    const char *next_label;
    Vec *lines;
    bool debug;
    bool flags_live;
    int fpu_depth;
    i32 scratch_disp;
    bool shared_epilogue;
    bool epilogue_follows_body;
    Vec *epilogue_jumps;
};

u8 x86_lower_vreg_width(X86LowerCtx *ctx, u32 vreg);
u8 x86_lower_operand_width(X86LowerCtx *ctx, IrOperand op);
RegLoc x86_lower_operand_loc(X86LowerCtx *ctx, IrOperand op);
X86Mem x86_lower_frame_mem(X86LowerCtx *ctx, i32 disp);
void x86_lower_force_to_reg(X86LowerCtx *ctx, IrOperand op, u8 reg);
RegLoc x86_lower_result_loc(X86LowerCtx *ctx, IrInstr *in);
void x86_lower_store_reg_result(X86LowerCtx *ctx, IrInstr *in, u8 width, u8 reg);
X86Mem x86_lower_pointer_in_rax(X86LowerCtx *ctx, IrOperand ptr);

X86Mem x86_lower_mem_for_ptr(X86LowerCtx *ctx, IrOperand ptr, u8 scratch);
void x86_lower_store_vreg_from_reg(X86LowerCtx *ctx, u32 vreg, u8 width, u8 reg);

void x86_lower_call_gaps(X86LowerCtx *ctx, bool before);

size_t x86_lower_module(CodegenModule *cm, IrModule *ir, bool debug, Arena *arena);

#endif
