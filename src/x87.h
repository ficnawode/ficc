#ifndef FICC_X87_H
#define FICC_X87_H

#include "ir.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "x86_emit.h"

typedef struct X86LowerCtx X86LowerCtx;

/* Raw m80 byte encoders, shared with the SysV call layer (RC_X87 is memory-only). */
void x87_emit_fldt(ByteBuf *buf, X86Mem mem);
void x87_emit_fstpt(ByteBuf *buf, X86Mem mem);
void x87_emit_flds(ByteBuf *buf, X86Mem mem);
void x87_emit_fstps(ByteBuf *buf, X86Mem mem);
void x87_emit_fldl(ByteBuf *buf, X86Mem mem);
void x87_emit_fstpl(ByteBuf *buf, X86Mem mem);
void x87_emit_fild(ByteBuf *buf, u8 size, X86Mem mem);
void x87_emit_fisttp(ByteBuf *buf, u8 size, X86Mem mem);
void x87_emit_fldz(ByteBuf *buf);
void x87_emit_fchs(ByteBuf *buf);
void x87_emit_faddp(ByteBuf *buf);
void x87_emit_fsubp(ByteBuf *buf);
void x87_emit_fsubrp(ByteBuf *buf);
void x87_emit_fmulp(ByteBuf *buf);
void x87_emit_fdivp(ByteBuf *buf);
void x87_emit_fucomip(ByteBuf *buf);
void x87_emit_fstp_st0(ByteBuf *buf);

/* Width-16 IR lowering for the register-allocating backend. */
void x87_lower_itof(IrInstr *in, X86LowerCtx *ctx);
void x87_lower_ftoi(IrInstr *in, X86LowerCtx *ctx);
void x87_lower_fconv(IrInstr *in, X86LowerCtx *ctx);
void x87_lower_fbin(IrInstr *in, X86LowerCtx *ctx);
void x87_lower_fneg(IrInstr *in, X86LowerCtx *ctx);
void x87_lower_fcmp(IrInstr *in, X86LowerCtx *ctx);
void x87_load_result_to_st0(X86LowerCtx *ctx, IrInstr *in);

#endif
