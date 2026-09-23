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

/* Shared lowering context; x86_sysv.c drives the call layer through the
   exported helpers below. */
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
    const IrPositions *pos; /* position numbering; position_offsets maps to bytes */
    u32 *position_offsets;
    u32 *use_count;         /* operand-reference count per vreg (brcond fold) */
    const char *next_label; /* label of the block emitted next (fallthrough) */
    Vec *lines;             /* Vec<LineEntry*> when recording -g line rows, else NULL */
    bool debug;             /* record line boundaries for DWARF */
    int fpu_depth;          /* x87 stack depth; every lowering leaves it at 0 */
    i32 scratch_disp;       /* [rbp+disp] 16-byte slot used to break phi-copy cycles */
};

u8 x86_lower_vreg_width(X86LowerCtx *ctx, u32 vreg);
u8 x86_lower_operand_width(X86LowerCtx *ctx, IrOperand op);
X86Mem x86_lower_rbp_mem(i32 disp);
void x86_lower_force_to_reg(X86LowerCtx *ctx, IrOperand op, u8 reg);
RegLoc x86_lower_result_loc(X86LowerCtx *ctx, IrInstr *in);
void x86_lower_store_reg_result(X86LowerCtx *ctx, IrInstr *in, u8 width, u8 reg);
X86Mem x86_lower_pointer_in_rax(X86LowerCtx *ctx, IrOperand ptr);
void x86_lower_store_vreg_from_reg(X86LowerCtx *ctx, u32 vreg, u8 width, u8 reg);

/* Register-allocating lowering entry: fills cm->funcs from the IR module. */
size_t x86_lower_module(CodegenModule *cm, IrModule *ir, bool debug, Arena *arena);

#endif
