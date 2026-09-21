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
    u8 reg;        /* GP or XMM register index (SYSV_GP / SYSV_SSE) */
    u32 chunk_off; /* byte offset of this eightbyte within a by-value record */
} SysvChunk;

/* One call/parameter argument's SysV placement: a scalar (one chunk or a stack
   slot) or a by-value record (up to two register chunks, or a stack copy). */
typedef struct
{
    Type *type; /* record type, or scalar (pointer) type */
    bool is_record;
    bool register_passed; /* record chunks ride registers */
    bool on_stack;        /* the value rides the caller stack area */
    bool is_x87_stack;    /* bare long double: 16-byte aligned stack slot */
    u32 stack_off;        /* byte offset in the outgoing/incoming argument area */
    u32 stack_size;       /* bytes consumed on the stack (0 for register args) */
    u8 nchunks;
    SysvChunk chunks[2];
} SysvArgPlan;

/* Plan arguments in declaration order; `types[i]` is the by-value type (records
   stay records, scalars are their promoted type).  Returns the raw stack bytes
   used and the number of XMM registers consumed (`fp_used`, for `%al`). */
u32 sysv_plan_args(Type **types, u32 nargs, SysvArgPlan *plans, u32 *fp_used);

/* The local staging bytes a parameter needs; 0 when it points straight at the
   incoming stack area (MEMORY-class record). */
u32 sysv_param_stage_bytes(const SysvArgPlan *plan);

void x86_sysv_lower_call(IrInstr *in, X86LowerCtx *ctx);
void x86_sysv_lower_va_start(IrInstr *in, X86LowerCtx *ctx);
void x86_sysv_lower_va_arg(IrInstr *in, X86LowerCtx *ctx);

#endif
