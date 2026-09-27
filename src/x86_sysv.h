#ifndef FICC_X86_SYSV_H
#define FICC_X86_SYSV_H

#include "ir.h"
#include "type.h"
#include "util/types.h"

typedef struct X86LowerCtx X86LowerCtx;

typedef enum
{
    SYSV_GP,
    SYSV_SSE,
} SysvSlotKind;

typedef struct
{
    SysvSlotKind kind;
    u8 reg;
    u32 chunk_off;
} SysvChunk;

typedef struct
{
    Type *type;
    bool is_record;
    bool register_passed;
    bool on_stack;
    bool is_x87_stack;
    u32 stack_off;
    u32 stack_size;
    u8 nchunks;
    SysvChunk chunks[2];
} SysvArgPlan;

u32 sysv_plan_args(Type **types, u32 nargs, SysvArgPlan *plans, u32 *fp_used);

u32 sysv_param_stage_bytes(const SysvArgPlan *plan);

void x86_sysv_lower_call(IrInstr *in, X86LowerCtx *ctx);
void x86_sysv_lower_va_start(IrInstr *in, X86LowerCtx *ctx);
void x86_sysv_lower_va_arg(IrInstr *in, X86LowerCtx *ctx);

#endif
