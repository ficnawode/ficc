#include "x86_lower.h"
#include "liveinterval.h"
#include "regalloc.h"
#include "target.h"
#include "type.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include "util/vec.h"
#include "x86_emit.h"
#include "x86_frame.h"
#include "x86_sysv.h"
#include "x87.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BIT_63 63

#define F32_SIGN_BIT 0x80000000
#define F64_SIGN_BIT 0x8000000000000000ULL

/* IEEE bit patterns of 2^63, added back by the u64 int→FP sequence. */
#define F32_BITS_2POW63 0x5F000000
#define F64_BITS_2POW63 0x43E0000000000000ULL

/* 0F BA /digit ib: bts = 5, btr = 6. */
#define X86_XOP_BTS 5
#define X86_XOP_BTR 6

typedef struct
{
    IrOperand src;
    u32 dst_vreg;
    bool src_is_scratch; /* the source was spilled to the cycle-break scratch slot */
} LowerPhiCopy;

/* Jump table appended to the function's .text, indexed by `val - min`. */
typedef struct
{
    size_t disp_field_off;
    u32 nentries;
    const char **targets;
} LowerSwitchTable;

#define STACK_ALIGN 16

/* Sentinel for a position slot whose byte offset has not been recorded yet. */
#define POS_UNSET UINT32_MAX

static u8 vreg_width(X86LowerCtx *ctx, u32 vreg)
{
    return ctx->mod->widths[vreg];
}

static bool fits_i32(i64 v)
{
    return v >= (i64) INT32_MIN && v <= (i64) INT32_MAX;
}

static u8 imm_load_width(i64 imm)
{
    return fits_i32(imm) ? 4 : 8;
}

static u8 operand_width(X86LowerCtx *ctx, IrOperand op)
{
    if (op.is_global || op.is_func)
    {
        return 8;
    }
    if (op.is_imm)
    {
        return imm_load_width(op.u.imm);
    }
    return vreg_width(ctx, op.u.vreg);
}

static u8 load_int_operand(X86LowerCtx *ctx, IrOperand op, bool *is_signed);
static u8 fp_imm_load_width(u8 w, i64 imm);
static void fp_operand_to_xmm(X86LowerCtx *ctx, IrOperand op, u8 w, u8 xmm);

static X86Mem rbp_mem(i32 disp)
{
    return x86_mem_rbp(disp);
}

/* Force `op` into physical `reg`, emitting an address load for globals/functions. */
static void force_to_reg(X86LowerCtx *ctx, IrOperand op, u8 reg)
{
    ByteBuf *b = ctx->buf;
    if (op.is_imm)
    {
        /* Materialize the full 64-bit value: an imm32 load zero-extends and would
           lose the sign of a negative immediate used in a 64-bit context. */
        emit_mov(b, W_QWORD, xop_reg(reg), xop_imm(op.u.imm));
        return;
    }
    if (op.is_global)
    {
        emit_global_addr_to(b, reg, op.u.global_index, ctx->global_patches, ctx->arena);
        return;
    }
    if (op.is_func)
    {
        emit_func_addr_to(b, reg, op.u.func_name, ctx->func_patches, ctx->arena);
        return;
    }
    RegLoc l = x86_lower_operand_loc(ctx, op);
    u8 w = vreg_width(ctx, op.u.vreg);
    if (l.kind == LOC_REG)
    {
        if (l.reg != reg)
        {
            emit_mov(b, w, xop_reg(reg), xop_reg(l.reg));
        }
        return;
    }
    if (l.kind == LOC_REMAT)
    {
        emit_lea(b, reg, rbp_mem(l.disp));
        return;
    }
    emit_mov(b, w, xop_reg(reg), xop_mem(rbp_mem(l.disp)));
}

/* Resolve `op` to an operand without forcing register placement. */
static X86Operand resolve(X86LowerCtx *ctx, IrOperand op, u8 scratch)
{
    if (op.is_imm)
    {
        return xop_imm(op.u.imm);
    }
    if (op.is_global)
    {
        emit_global_addr_to(ctx->buf, scratch, op.u.global_index, ctx->global_patches, ctx->arena);
        return xop_reg(scratch);
    }
    if (op.is_func)
    {
        emit_func_addr_to(ctx->buf, scratch, op.u.func_name, ctx->func_patches, ctx->arena);
        return xop_reg(scratch);
    }
    RegLoc l = x86_lower_operand_loc(ctx, op);
    if (l.kind == LOC_REG)
    {
        return xop_reg(l.reg);
    }
    if (l.kind == LOC_REMAT)
    {
        emit_lea(ctx->buf, scratch, rbp_mem(l.disp));
        return xop_reg(scratch);
    }
    return xop_mem(rbp_mem(l.disp));
}

/* A right-hand operand; an imm64 that no encoding reaches is materialized in `scratch`. */
static X86Operand resolve_rhs(X86LowerCtx *ctx, IrOperand op, u8 width, u8 scratch)
{
    if (op.is_imm && width == 8 && !fits_i32(op.u.imm))
    {
        emit_mov(ctx->buf, 8, xop_reg(scratch), xop_imm(op.u.imm));
        return xop_reg(scratch);
    }
    return resolve(ctx, op, scratch);
}

static RegLoc result_loc(X86LowerCtx *ctx, IrInstr *in)
{
    return x86_lower_operand_loc(ctx, ir_operand_vreg(in->result));
}

static void store_reg_result(X86LowerCtx *ctx, IrInstr *in, u8 width, u8 reg)
{
    RegLoc l = result_loc(ctx, in);
    if (l.kind == LOC_REG)
    {
        if (l.reg != reg)
        {
            emit_mov(ctx->buf, width, xop_reg(l.reg), xop_reg(reg));
        }
        return;
    }
    emit_mov(ctx->buf, width, xop_mem(rbp_mem(l.disp)), xop_reg(reg));
}

/* Address of the pointee in %rax; globals/functions carry their own relocations. */
static X86Mem pointer_in_rax(X86LowerCtx *ctx, IrOperand ptr)
{
    force_to_reg(ctx, ptr, R_EAX);
    return x86_mem_rax(0);
}

/* A pointer already in a register is used as the base directly; a
   spilled/immediate/global pointer is materialized in `scratch` first. */
static X86Mem mem_operand_for_ptr(X86LowerCtx *ctx, IrOperand ptr, u8 scratch)
{
    if (!ptr.is_imm && !ptr.is_global && !ptr.is_func)
    {
        RegLoc l = x86_lower_operand_loc(ctx, ptr);
        if (l.kind == LOC_REG)
        {
            return (X86Mem) {.base = l.reg, .index = NO_REG, .scale = 1, .disp = 0};
        }
    }
    force_to_reg(ctx, ptr, scratch);
    return (X86Mem) {.base = scratch, .index = NO_REG, .scale = 1, .disp = 0};
}

static void lower_binary(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    const ArithSpec *s = &arith_specs[in->opcode];
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    X86Operand rhs = resolve_rhs(ctx, in->ops[1], w, R_R11);
    bool unit = rhs.kind == XOP_IMM && (rhs.u.imm == 1 || rhs.u.imm == -1);
    if (unit && (in->opcode == OP_ADD || in->opcode == OP_SUB))
    {
        bool dec = in->opcode == OP_ADD ? rhs.u.imm == -1 : rhs.u.imm == 1;
        emit_inc_dec(ctx->buf, w, dst, dec);
    }
    else
    {
        emit_binop_rhs(ctx->buf, w, s, dst, rhs);
    }
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, w, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static void lower_unary(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    emit_unary(ctx->buf, w, dst, unary_digit[in->opcode]);
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, w, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static void lower_shift(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    if (in->ops[1].is_imm)
    {
        emit_shift_imm(ctx->buf, w, dst, shift_digit[in->opcode], (u8) in->ops[1].u.imm);
    }
    else
    {
        force_to_reg(ctx, in->ops[1], R_ECX);
        emit_shift_cl(ctx->buf, w, dst, shift_digit[in->opcode]);
    }
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, w, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static void lower_div(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    bool is_unsigned = in->opcode == OP_UDIV || in->opcode == OP_UREM;
    force_to_reg(ctx, in->ops[0], R_EAX);
    emit_cdq(ctx->buf, w, is_unsigned);
    force_to_reg(ctx, in->ops[1], R_R11);
    if (is_unsigned)
    {
        emit_div(ctx->buf, w, R_R11);
    }
    else
    {
        emit_idiv(ctx->buf, w, R_R11);
    }
    u8 src = (in->opcode == OP_SREM || in->opcode == OP_UREM) ? R_EDX : R_EAX;
    store_reg_result(ctx, in, w, src);
}

static bool operand_extend_needed(u8 width, u8 result_width)
{
    return width < result_width && width < 4;
}

/* Compare `lhs` and `rhs` into EFLAGS, shared by lower_icmp and the brcond fold. */
static void emit_icmp_cmp(X86LowerCtx *ctx, IrOperand lhs, IrOperand rhs)
{
    u8 w0 = operand_width(ctx, lhs);
    u8 w1 = operand_width(ctx, rhs);
    u8 w = MAX(w0, w1);

    if (rhs.is_imm && rhs.u.imm == 0)
    {
        if (!lhs.is_imm && !lhs.is_global && !lhs.is_func && !operand_extend_needed(w0, w))
        {
            RegLoc ll = x86_lower_operand_loc(ctx, lhs);
            if (ll.kind == LOC_REG)
            {
                emit_test_reg(ctx->buf, w, ll.reg);
                return;
            }
        }
        force_to_reg(ctx, lhs, R_EAX);
        if (!lhs.is_imm && operand_extend_needed(w0, w))
        {
            emit_movzx(ctx->buf, w0, w, R_EAX, xop_reg(R_EAX));
        }
        emit_test_reg(ctx->buf, w, R_EAX);
        return;
    }

    bool lhs_ext = !lhs.is_imm && operand_extend_needed(w0, w);
    u8 lreg = R_EAX;
    if (!lhs.is_imm && !lhs.is_global && !lhs.is_func)
    {
        RegLoc ll = x86_lower_operand_loc(ctx, lhs);
        if (!lhs_ext && ll.kind == LOC_REG)
        {
            lreg = ll.reg;
        }
        else
        {
            force_to_reg(ctx, lhs, R_EAX);
            if (lhs_ext)
            {
                emit_movzx(ctx->buf, w0, w, R_EAX, xop_reg(R_EAX));
            }
        }
    }
    else
    {
        force_to_reg(ctx, lhs, R_EAX);
    }

    if (rhs.is_imm)
    {
        if (w == 8 && !fits_i32(rhs.u.imm))
        {
            force_to_reg(ctx, rhs, R_R11);
            emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_reg(R_R11));
            return;
        }
        emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_imm(rhs.u.imm));
        return;
    }
    if (rhs.is_global || rhs.is_func)
    {
        force_to_reg(ctx, rhs, R_R11);
        emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_reg(R_R11));
        return;
    }
    RegLoc rl = x86_lower_operand_loc(ctx, rhs);
    if (!operand_extend_needed(w1, w))
    {
        if (rl.kind == LOC_REG)
        {
            emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_reg(rl.reg));
        }
        else if (w1 == w && rl.kind == LOC_MEM)
        {
            emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_mem(rbp_mem(rl.disp)));
        }
        else
        {
            force_to_reg(ctx, rhs, R_R11);
            emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_reg(R_R11));
        }
        return;
    }
    emit_movzx(ctx->buf, w1, w, R_R11,
               rl.kind == LOC_REG ? xop_reg(rl.reg) : xop_mem(rbp_mem(rl.disp)));
    emit_binop_rhs(ctx->buf, w, &cmp_spec, lreg, xop_reg(R_R11));
}

static void lower_icmp(IrInstr *in, X86LowerCtx *ctx)
{
    u8 rw = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    emit_icmp_cmp(ctx, in->ops[0], in->ops[1]);
    if (rl.kind == LOC_REG)
    {
        emit_setcc_reg(ctx->buf, icmp_cc[in->opcode], rl.reg);
        if (rw != W_BYTE)
        {
            emit_movzx(ctx->buf, W_BYTE, rw, rl.reg, xop_reg(rl.reg));
        }
        return;
    }
    emit_setcc(ctx->buf, icmp_cc[in->opcode]);
    emit_movzbl_al_eax(ctx->buf);
    store_reg_result(ctx, in, rw, R_EAX);
}

static void lower_trunc(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    if (rl.kind == LOC_REG)
    {
        force_to_reg(ctx, in->ops[0], rl.reg);
        return;
    }
    force_to_reg(ctx, in->ops[0], R_EAX);
    store_reg_result(ctx, in, w, R_EAX);
}

static void lower_zext(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    if (rl.cls == RC_XMM)
    {
        u8 mf = MF_OF(dw);
        if (in->ops[0].is_imm)
        {
            emit_mov(ctx->buf, fp_imm_load_width(dw, in->ops[0].u.imm), xop_reg(R_EAX),
                     xop_imm(in->ops[0].u.imm));
            if (rl.kind == LOC_REG)
            {
                emit_movd_to_xmm(ctx->buf, rl.reg, R_EAX, dw != W_DWORD);
            }
            else
            {
                emit_movd_to_xmm(ctx->buf, R_XMM0, R_EAX, dw != W_DWORD);
                emit_sse_store(ctx->buf, mf, rbp_mem(rl.disp), R_XMM0);
            }
            return;
        }
        u8 dst = rl.kind == LOC_REG ? rl.reg : R_XMM0;
        fp_operand_to_xmm(ctx, in->ops[0], dw, dst);
        if (rl.kind == LOC_MEM)
        {
            emit_sse_store(ctx->buf, mf, rbp_mem(rl.disp), dst);
        }
        return;
    }
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    if (!in->ops[0].is_imm)
    {
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        if (sw < 4)
        {
            emit_movzx(ctx->buf, sw, dw, dst, xop_reg(dst));
        }
    }
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, dw, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static void lower_sext(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    if (!in->ops[0].is_imm)
    {
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        emit_movsx(ctx->buf, sw, dw, dst, xop_reg(dst));
    }
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, dw, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static bool operand_is_fp_vreg(X86LowerCtx *ctx, IrOperand op)
{
    return !op.is_imm && !op.is_global && !op.is_func && ir_vreg_float(ctx->mod, op.u.vreg);
}

static u8 fp_imm_load_width(u8 w, i64 imm)
{
    return w == 4 ? 4 : imm_load_width(imm);
}

/* Move an FP operand's bits into `xmm`; a bare immediate rides a GP mov + movd/movq. */
static void fp_operand_to_xmm(X86LowerCtx *ctx, IrOperand op, u8 w, u8 xmm)
{
    ByteBuf *b = ctx->buf;
    if (op.is_imm)
    {
        emit_mov(b, fp_imm_load_width(w, op.u.imm), xop_reg(R_EAX), xop_imm(op.u.imm));
        emit_movd_to_xmm(b, xmm, R_EAX, w != W_DWORD);
        return;
    }
    RegLoc l = x86_lower_operand_loc(ctx, op);
    if (l.kind == LOC_REG)
    {
        if (l.reg != xmm)
        {
            emit_sse_op_reg(b, MF_OF(w), X86_SSE_MOV, xmm, l.reg);
        }
        return;
    }
    emit_sse_load(b, MF_OF(w), xmm, rbp_mem(l.disp));
}

static void store_fp_result(X86LowerCtx *ctx, IrInstr *in, u8 w, u8 xmm)
{
    RegLoc rl = result_loc(ctx, in);
    if (rl.kind == LOC_REG)
    {
        if (rl.reg != xmm)
        {
            emit_sse_op_reg(ctx->buf, MF_OF(w), X86_SSE_MOV, rl.reg, xmm);
        }
        return;
    }
    emit_sse_store(ctx->buf, MF_OF(w), rbp_mem(rl.disp), xmm);
}

/* u64 ≥ 2^63 to float/double: clear the top bit, convert, add 2^63 back. */
static void emit_load_2pow63(ByteBuf *buf, bool is_f32)
{
    if (is_f32)
    {
        emit_mov(buf, W_DWORD, xop_reg(R_R11), xop_imm(F32_BITS_2POW63));
        emit_movd_to_xmm(buf, R_XMM1, R_R11, false);
    }
    else
    {
        emit_mov(buf, W_QWORD, xop_reg(R_R11), xop_imm(F64_BITS_2POW63));
        emit_movd_to_xmm(buf, R_XMM1, R_R11, true);
    }
}

static void emit_bit_imm(ByteBuf *buf, u8 digit, u8 dst_reg, u8 imm)
{
    bytebuf_append(buf, rex(true, false, false, dst_reg >= 8));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_BIT_BASE);
    bytebuf_append(buf, modrm(3, digit, dst_reg));
    bytebuf_append_i8(buf, (i8) imm);
}

static void emit_itof_u64_xmm(X86LowerCtx *ctx, u8 mf, bool is_f32)
{
    ByteBuf *b = ctx->buf;
    emit_test_reg(b, W_QWORD, R_EAX);
    size_t js_field = emit_jcc_pending(b, CC_S);
    emit_cvtsi2fp(b, mf, R_XMM0, R_EAX);
    size_t jmp_field = emit_jmp_pending(b);
    size_t big_off = bytebuf_len(b);
    emit_mov(b, W_QWORD, xop_reg(R_R11), xop_reg(R_EAX));
    emit_bit_imm(b, X86_XOP_BTR, R_R11, BIT_63);
    emit_cvtsi2fp(b, mf, R_XMM0, R_R11);
    emit_load_2pow63(b, is_f32);
    emit_sse_add(b, mf, R_XMM0, R_XMM1);
    patch_rel32(b, js_field, big_off);
    patch_rel32(b, jmp_field, bytebuf_len(b));
}

/* u64 from float/double: convert, then for ≥ 2^63 subtract 2^63, re-convert, set bit 63. */
static void emit_ftoi_u64_xmm(X86LowerCtx *ctx, u8 mf, u8 sw)
{
    ByteBuf *b = ctx->buf;
    emit_cvtts2i(b, mf, R_EAX, R_XMM0, true);
    emit_load_2pow63(b, sw == W_DWORD);
    emit_sse_ucomis(b, sw, R_XMM0, R_XMM1);
    size_t done_field = emit_jcc_pending(b, CC_B);
    emit_sse_sub(b, mf, R_XMM0, R_XMM1);
    emit_cvtts2i(b, mf, R_EAX, R_XMM0, true);
    emit_bit_imm(b, X86_XOP_BTS, R_EAX, BIT_63);
    patch_rel32(b, done_field, bytebuf_len(b));
}

static void lower_itof(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    if (dw == W_LD)
    {
        x87_lower_itof(in, ctx);
        return;
    }
    bool is_signed;
    u8 sw = load_int_operand(ctx, in->ops[0], &is_signed);
    u8 mf = MF_OF(dw);
    if (sw == W_QWORD && !is_signed)
    {
        emit_itof_u64_xmm(ctx, mf, dw == W_DWORD);
    }
    else
    {
        emit_cvtsi2fp(ctx->buf, mf, R_XMM0, R_EAX);
    }
    store_fp_result(ctx, in, dw, R_XMM0);
}

static void lower_ftoi(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    bool is_signed = ir_vreg_signed(ctx->mod, in->result);
    u8 sw = in->ops[0].is_imm ? 8 : vreg_width(ctx, in->ops[0].u.vreg);
    if (sw == W_LD)
    {
        x87_lower_ftoi(in, ctx);
        return;
    }
    u8 mf = MF_OF(sw);
    fp_operand_to_xmm(ctx, in->ops[0], sw, R_XMM0);
    if (dw == W_QWORD && !is_signed)
    {
        emit_ftoi_u64_xmm(ctx, mf, sw);
    }
    else
    {
        emit_cvtts2i(ctx->buf, mf, R_EAX, R_XMM0, dw == W_QWORD || !is_signed);
    }
    store_reg_result(ctx, in, dw, R_EAX);
}

static void lower_fconv(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    u8 sw = in->ops[0].is_imm ? 8 : vreg_width(ctx, in->ops[0].u.vreg);
    if (dw == W_LD || sw == W_LD)
    {
        x87_lower_fconv(in, ctx);
        return;
    }
    u8 mf = MF_OF(sw);
    fp_operand_to_xmm(ctx, in->ops[0], sw, R_XMM0);
    emit_sse_cvt(ctx->buf, mf, R_XMM0, R_XMM0);
    store_fp_result(ctx, in, dw, R_XMM0);
}

static void lower_fbin(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    if (w == W_LD)
    {
        x87_lower_fbin(in, ctx);
        return;
    }
    u8 mf = MF_OF(w);
    const ArithSpec *s = &arith_specs[in->opcode];
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_XMM0;
    fp_operand_to_xmm(ctx, in->ops[0], w, dst);

    IrOperand rhs = in->ops[1];
    if (rhs.is_imm)
    {
        emit_mov(ctx->buf, fp_imm_load_width(w, rhs.u.imm), xop_reg(R_EAX), xop_imm(rhs.u.imm));
        emit_movd_to_xmm(ctx->buf, R_XMM1, R_EAX, w != W_DWORD);
        emit_sse_op_reg(ctx->buf, mf, s->mem, dst, R_XMM1);
    }
    else
    {
        RegLoc sl = x86_lower_operand_loc(ctx, rhs);
        if (sl.kind == LOC_REG)
        {
            emit_sse_op_reg(ctx->buf, mf, s->mem, dst, sl.reg);
        }
        else
        {
            emit_sse_op_mem(ctx->buf, mf, s->mem, dst, rbp_mem(sl.disp));
        }
    }
    if (rl.kind == LOC_MEM)
    {
        emit_sse_store(ctx->buf, mf, rbp_mem(rl.disp), dst);
    }
}

static void lower_fneg(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    if (w == W_LD)
    {
        x87_lower_fneg(in, ctx);
        return;
    }
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_XMM0;
    fp_operand_to_xmm(ctx, in->ops[0], w, dst);
    if (w == W_DWORD)
    {
        emit_mov(ctx->buf, W_DWORD, xop_reg(R_R11), xop_imm(F32_SIGN_BIT));
        emit_movd_to_xmm(ctx->buf, R_XMM1, R_R11, false);
        emit_sse_xor(ctx->buf, 0, dst, R_XMM1);
    }
    else
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_R11), xop_imm((i64) F64_SIGN_BIT));
        emit_movd_to_xmm(ctx->buf, R_XMM1, R_R11, true);
        emit_sse_xor(ctx->buf, X86_SSE_66, dst, R_XMM1);
    }
    if (rl.kind == LOC_MEM)
    {
        emit_sse_store(ctx->buf, MF_OF(w), rbp_mem(rl.disp), dst);
    }
}

/* FP compare setcc matrix; join=0 predicates need no PF (unordered) fixup. */
typedef struct
{
    u8 cc;
    u8 join; /* OP_AND/OP_OR to combine the unordered (PF) flag; 0 = none */
} FcmpSpec;

static const FcmpSpec fcmp_specs[OP_FCMP_GE + 1] = {
    [OP_FCMP_EQ] = {CC_E, OP_AND}, [OP_FCMP_NE] = {CC_NE, OP_OR},  [OP_FCMP_LT] = {CC_B, OP_AND},
    [OP_FCMP_GT] = {CC_A, 0},      [OP_FCMP_LE] = {CC_BE, OP_AND}, [OP_FCMP_GE] = {CC_AE, 0},
};

static void lower_fcmp(IrInstr *in, X86LowerCtx *ctx)
{
    u8 rw = vreg_width(ctx, in->result);
    u8 sw = in->ops[0].is_imm ? 8 : vreg_width(ctx, in->ops[0].u.vreg);
    if (sw == W_LD)
    {
        x87_lower_fcmp(in, ctx);
        return;
    }
    ByteBuf *b = ctx->buf;
    u8 mf = MF_OF(sw);
    fp_operand_to_xmm(ctx, in->ops[0], sw, R_XMM0);

    IrOperand rhs = in->ops[1];
    if (rhs.is_imm)
    {
        emit_mov(b, fp_imm_load_width(sw, rhs.u.imm), xop_reg(R_EAX), xop_imm(rhs.u.imm));
        emit_movd_to_xmm(b, R_XMM1, R_EAX, sw != W_DWORD);
    }
    else
    {
        RegLoc sl = x86_lower_operand_loc(ctx, rhs);
        if (sl.kind == LOC_REG)
        {
            if (sl.reg != R_XMM1)
            {
                emit_sse_op_reg(b, mf, X86_SSE_MOV, R_XMM1, sl.reg);
            }
        }
        else
        {
            emit_sse_load(b, mf, R_XMM1, rbp_mem(sl.disp));
        }
    }
    emit_sse_ucomis(b, sw, R_XMM0, R_XMM1);

    const FcmpSpec *spec = &fcmp_specs[in->opcode];
    emit_setcc_reg(b, spec->cc, R_EAX);
    if (spec->join)
    {
        u8 pf_cc = spec->join == OP_AND ? CC_NP : CC_P;
        emit_setcc_reg(b, pf_cc, R_R11);
        emit_binop_rhs(b, W_BYTE, &arith_specs[spec->join], R_EAX, xop_reg(R_R11));
    }
    emit_movzbl_al_eax(b);
    store_reg_result(ctx, in, rw, R_EAX);
}

static void lower_load(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    X86Mem addr = mem_operand_for_ptr(ctx, in->ops[0], R_R11);
    RegLoc rl = result_loc(ctx, in);
    if (w == W_LD)
    {
        ASSERT(rl.kind == LOC_MEM && "x87 values are memory-only");
        emit_mov16(ctx->buf, addr, rbp_mem(rl.disp));
        return;
    }
    if (rl.cls == RC_XMM)
    {
        u8 mf = MF_OF(w);
        if (rl.kind == LOC_REG)
        {
            emit_sse_load(ctx->buf, mf, rl.reg, addr);
            return;
        }
        emit_sse_load(ctx->buf, mf, R_XMM0, addr);
        emit_sse_store(ctx->buf, mf, rbp_mem(rl.disp), R_XMM0);
        return;
    }
    if (rl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, w, xop_reg(rl.reg), xop_mem(addr));
        return;
    }
    u8 tmp = addr.base == R_R11 ? R_EAX : R_R11;
    emit_mov(ctx->buf, w, xop_reg(tmp), xop_mem(addr));
    emit_mov(ctx->buf, w, xop_mem(rbp_mem(rl.disp)), xop_reg(tmp));
}

static void lower_store(IrInstr *in, X86LowerCtx *ctx)
{
    u32 w = (u32) in->ops[2].u.imm;
    IrOperand val = in->ops[0];
    if (w == W_LD)
    {
        if (val.is_imm)
        {
            ASSERT(val.u.imm == 0 && "nonzero immediate in a width-16 store");
            emit_sse_xor(ctx->buf, 0, R_XMM0, R_XMM0);
            X86Mem addr0 = mem_operand_for_ptr(ctx, in->ops[1], R_R11);
            emit_mov16_store(ctx->buf, addr0);
            return;
        }
        RegLoc sl = x86_lower_operand_loc(ctx, val);
        ASSERT(sl.kind == LOC_MEM && "x87 values are memory-only");
        X86Mem addr = mem_operand_for_ptr(ctx, in->ops[1], R_R11);
        emit_mov16(ctx->buf, rbp_mem(sl.disp), addr);
        return;
    }
    if (operand_is_fp_vreg(ctx, val))
    {
        u8 mf = MF_OF(w);
        RegLoc sl = x86_lower_operand_loc(ctx, val);
        X86Mem addr = mem_operand_for_ptr(ctx, in->ops[1], R_R11);
        if (sl.kind == LOC_REG)
        {
            emit_sse_store(ctx->buf, mf, addr, sl.reg);
            return;
        }
        emit_sse_load(ctx->buf, mf, R_XMM0, rbp_mem(sl.disp));
        emit_sse_store(ctx->buf, mf, addr, R_XMM0);
        return;
    }
    X86Mem addr = mem_operand_for_ptr(ctx, in->ops[1], R_R11);
    if (!val.is_imm && !val.is_global && !val.is_func)
    {
        RegLoc sl = x86_lower_operand_loc(ctx, val);
        if (sl.kind == LOC_REG)
        {
            emit_mov(ctx->buf, (u8) w, xop_mem(addr), xop_reg(sl.reg));
            return;
        }
    }
    u8 tmp = addr.base == R_R11 ? R_EAX : R_R11;
    force_to_reg(ctx, val, tmp);
    emit_mov(ctx->buf, (u8) w, xop_mem(addr), xop_reg(tmp));
}

static void lower_gep(IrInstr *in, X86LowerCtx *ctx)
{
    i64 stride = in->ops[2].u.imm;
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    if (in->ops[1].is_imm && in->ops[1].u.imm == 1)
    {
        X86Mem m = {.base = dst, .index = NO_REG, .scale = 1, .disp = (i32) stride};
        emit_lea(ctx->buf, dst, m);
    }
    else
    {
        force_to_reg(ctx, in->ops[1], R_R11);
        if (stride == 1 || stride == 2 || stride == 4 || stride == 8)
        {
            X86Mem scaled = {.base = dst, .index = R_R11, .scale = (u8) stride, .disp = 0};
            emit_lea(ctx->buf, dst, scaled);
        }
        else
        {
            emit_imul_imm(ctx->buf, W_QWORD, R_R11, stride);
            X86Mem scaled = {.base = dst, .index = R_R11, .scale = 1, .disp = 0};
            emit_lea(ctx->buf, dst, scaled);
        }
    }
    if (rl.kind == LOC_MEM)
    {
        emit_mov(ctx->buf, W_QWORD, xop_mem(rbp_mem(rl.disp)), xop_reg(dst));
    }
}

static void lower_alloca(IrInstr *in, X86LowerCtx *ctx)
{
    /* The frame planner reserves a static slot and the result is recomputed at
       each use (LOC_REMAT), so the definition emits nothing. */
    (void) in;
    (void) ctx;
}

static bool operand_in_reg(X86LowerCtx *ctx, IrOperand op, u8 reg)
{
    if (op.is_imm || op.is_global || op.is_func)
    {
        return false;
    }
    RegLoc l = x86_lower_operand_loc(ctx, op);
    return l.kind == LOC_REG && l.reg == reg;
}

/* rdi <- dst, rsi <- src, then rep movsb.  The two moves are ordered so a
   source still living in %rdi or %rsi is read before its register is
   overwritten (the pathological swap goes through %rax). */
static void lower_memcpy(IrInstr *in, X86LowerCtx *ctx)
{
    IrOperand dst = in->ops[0];
    IrOperand src = in->ops[1];
    if (operand_in_reg(ctx, dst, R_ESI) && operand_in_reg(ctx, src, R_EDI))
    {
        force_to_reg(ctx, src, R_EAX);
        force_to_reg(ctx, dst, R_EDI);
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_ESI), xop_reg(R_EAX));
    }
    else if (operand_in_reg(ctx, dst, R_ESI))
    {
        force_to_reg(ctx, dst, R_EDI);
        force_to_reg(ctx, src, R_ESI);
    }
    else
    {
        force_to_reg(ctx, src, R_ESI);
        force_to_reg(ctx, dst, R_EDI);
    }
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(in->ops[2].u.imm));
    bytebuf_append(ctx->buf, X86_REP);
    bytebuf_append(ctx->buf, X86_MOVSB);
}

static void lower_ret(IrInstr *in, X86LowerCtx *ctx)
{
    if (in->nops > 0)
    {
        IrOperand val = in->ops[0];
        if (type_is_fp(ctx->func->ret_type))
        {
            u8 w = val.is_imm ? (u8) type_sizeof(ctx->func->ret_type) : vreg_width(ctx, val.u.vreg);
            if (w == W_LD)
            {
                /* %st0 return: leave the value on the x87 stack, reset the counter. */
                x87_load_result_to_st0(ctx, in);
            }
            else
            {
                fp_operand_to_xmm(ctx, val, w, R_XMM0);
            }
        }
        else
        {
            force_to_reg(ctx, val, R_EAX);
        }
    }
    else
    {
        emit_xor_zero(ctx->buf, W_DWORD, R_EAX);
    }
    if (ctx->shared_epilogue)
    {
        u32 *field = arena_alloc(ctx->arena, sizeof(u32), sizeof(u32));
        *field = (u32) emit_jmp_pending(ctx->buf);
        vec_push(ctx->epilogue_jumps, field);
        return;
    }
    x86_frame_restore_callee(ctx->buf, ctx->frame);
    if (!ctx->frame->omit_fp)
    {
        bytebuf_append(ctx->buf, X86_LEAVE);
    }
    bytebuf_append(ctx->buf, X86_RET);
}

/* Emit the one epilogue shared by every `ret`; each `ret` jumped here. */
static void emit_shared_epilogue(X86LowerCtx *ctx)
{
    size_t epilogue = bytebuf_len(ctx->buf);
    x86_frame_restore_callee(ctx->buf, ctx->frame);
    if (!ctx->frame->omit_fp)
    {
        bytebuf_append(ctx->buf, X86_LEAVE);
    }
    bytebuf_append(ctx->buf, X86_RET);
    size_t n = vec_size(ctx->epilogue_jumps);
    for (size_t i = 0; i < n; i++)
    {
        u32 field = *(u32 *) vec_get(ctx->epilogue_jumps, i);
        patch_rel32(ctx->buf, field, epilogue);
    }
}

static void lower_unreachable(IrInstr *in, X86LowerCtx *ctx)
{
    (void) in;
    emit_ud2(ctx->buf);
}

static void lower_noop(IrInstr *in, X86LowerCtx *ctx)
{
    (void) in;
    (void) ctx;
}

static void lower_unsupported(IrInstr *in, X86LowerCtx *ctx)
{
    fprintf(stderr, "[x86_lower] unsupported opcode %s\n", ir_opcode_name(in->opcode));
    emit_ud2(ctx->buf);
}

static bool is_terminator(IrOpcode op)
{
    return op == OP_RET || op == OP_UNREACHABLE || op == OP_BR || op == OP_BRCOND ||
           op == OP_SWITCH;
}

static size_t block_index_of_label(X86LowerCtx *ctx, const char *label)
{
    void *v = strmap_get(ctx->label_to_index, label);
    ASSERT(v != NULL && "branch target names a real block");
    return (size_t) (uintptr_t) v - 1;
}

static void lower_br(IrInstr *in, X86LowerCtx *ctx)
{
    emit_jmp(ctx->buf, in->extra.br.target_label, ctx->block_patches, ctx->arena);
}

/* Jcc predicate pairs differ only in the low bit. */
static u8 invert_cc(u8 cc)
{
    return (u8) (cc ^ 1);
}

/* Branch on `cc`; an edge that is the next block falls through instead. */
static void emit_cond_branch(X86LowerCtx *ctx, u8 cc, const char *true_label,
                             const char *false_label)
{
    const char *next = ctx->next_label;
    if (next && strcmp(false_label, next) == 0)
    {
        emit_jcc(ctx->buf, cc, true_label, ctx->block_patches, ctx->arena);
    }
    else if (next && strcmp(true_label, next) == 0)
    {
        emit_jcc(ctx->buf, invert_cc(cc), false_label, ctx->block_patches, ctx->arena);
    }
    else
    {
        emit_jcc(ctx->buf, cc, true_label, ctx->block_patches, ctx->arena);
        emit_jmp(ctx->buf, false_label, ctx->block_patches, ctx->arena);
    }
}

/* Read the tested operand before loop-carried writes that may overwrite it. */
static void lower_brcond_test(IrInstr *in, X86LowerCtx *ctx)
{
    u8 cw = operand_width(ctx, in->ops[0]);
    if (!in->ops[0].is_imm && !in->ops[0].is_global && !in->ops[0].is_func)
    {
        RegLoc l = x86_lower_operand_loc(ctx, in->ops[0]);
        if (l.kind == LOC_REG)
        {
            emit_test_reg(ctx->buf, cw, l.reg);
            return;
        }
    }
    force_to_reg(ctx, in->ops[0], R_EAX);
    emit_test_reg(ctx->buf, cw, R_EAX);
}

static void lower_brcond_branch(IrInstr *in, X86LowerCtx *ctx, u8 cc)
{
    emit_cond_branch(ctx, cc, in->extra.brcond.true_label, in->extra.brcond.false_label);
}

static void lower_brcond(IrInstr *in, X86LowerCtx *ctx)
{
    lower_brcond_test(in, ctx);
    lower_brcond_branch(in, ctx, CC_NE);
}

static bool is_icmp(IrOpcode op)
{
    return op >= OP_ICMP_EQ && op <= OP_ICMP_SGE;
}

/* Load the switch control into %rax at its exact 64-bit semantic value. */
static u8 load_int_operand(X86LowerCtx *ctx, IrOperand op, bool *is_signed)
{
    if (op.is_imm)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), xop_imm(op.u.imm));
        *is_signed = true;
        return W_QWORD;
    }
    if (op.is_global || op.is_func)
    {
        force_to_reg(ctx, op, R_EAX);
        *is_signed = true;
        return W_QWORD;
    }
    u8 sw = vreg_width(ctx, op.u.vreg);
    *is_signed = ir_vreg_signed(ctx->mod, op.u.vreg);
    force_to_reg(ctx, op, R_EAX);
    if (sw == W_BYTE || sw == W_WORD)
    {
        if (*is_signed)
        {
            emit_movsx(ctx->buf, sw, W_QWORD, R_EAX, xop_reg(R_EAX));
        }
        else
        {
            emit_movzx(ctx->buf, sw, W_QWORD, R_EAX, xop_reg(R_EAX));
        }
    }
    else if (sw == W_DWORD && *is_signed)
    {
        emit_movsx(ctx->buf, W_DWORD, W_QWORD, R_EAX, xop_reg(R_EAX));
    }
    return sw;
}

/* Tables only when the case range is small (≤ JT_MAX_RANGE); sparse switches use a chain. */
#define JT_MAX_RANGE 256

/* Bounds-check %eax to [min,max], subtract min, jump through the full-range table.
   The check is unsigned: after `sub min`, the in-range values are 0..range and
   everything else wraps above it, so one `ja` catches both sides. */
static void emit_switch_table(X86LowerCtx *ctx, IrSwitchCase *cases, u32 n, i64 min, i64 max,
                              const char *default_label)
{
    u64 range = (u64) max - (u64) min;

    if (min != 0)
    {
        emit_binop_rhs(ctx->buf, W_DWORD, &arith_specs[OP_SUB], R_EAX, xop_imm(min));
    }
    emit_binop_rhs(ctx->buf, W_DWORD, &cmp_spec, R_EAX, xop_imm((i64) range));
    emit_jcc(ctx->buf, CC_A, default_label, ctx->block_patches, ctx->arena);

    emit_lea(ctx->buf, R_R11, (X86Mem) {.base = NO_REG, .index = NO_REG, .scale = 1, .disp = 0});
    size_t disp_field_off = bytebuf_len(ctx->buf) - 4;

    emit_movsx(ctx->buf, W_DWORD, W_QWORD, R_EAX,
               xop_mem((X86Mem) {.base = R_R11, .index = R_EAX, .scale = 4, .disp = 0}));
    emit_reg_reg(ctx->buf, arith_specs[OP_ADD].mem, R_EAX, R_R11);
    emit_jmp_reg(ctx->buf, R_EAX);

    size_t nentries = (size_t) range + 1;
    LowerSwitchTable *rec = arena_alloc(ctx->arena, sizeof(LowerSwitchTable), sizeof(void *));
    rec->disp_field_off = disp_field_off;
    rec->nentries = (u32) nentries;
    rec->targets = arena_alloc(ctx->arena, nentries * sizeof(const char *), sizeof(void *));
    for (size_t e = 0; e < nentries; e++)
    {
        rec->targets[e] = default_label;
    }
    for (u32 i = 0; i < n; i++)
    {
        size_t idx = (size_t) ((u64) cases[i].val - (u64) min);
        rec->targets[idx] = cases[i].label;
    }
    vec_push(ctx->switch_tables, rec);
}

static void emit_switch_chain(X86LowerCtx *ctx, IrSwitchCase *cases, u32 n,
                              const char *default_label)
{
    for (u32 i = 0; i < n; i++)
    {
        if (fits_i32(cases[i].val))
        {
            emit_binop_rhs(ctx->buf, W_QWORD, &cmp_spec, R_EAX, xop_imm(cases[i].val));
        }
        else
        {
            emit_mov(ctx->buf, W_QWORD, xop_reg(R_R11), xop_imm(cases[i].val));
            emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_R11);
        }
        emit_jcc(ctx->buf, CC_E, cases[i].label, ctx->block_patches, ctx->arena);
    }
    if (!(ctx->next_label && strcmp(default_label, ctx->next_label) == 0))
    {
        emit_jmp(ctx->buf, default_label, ctx->block_patches, ctx->arena);
    }
}

static void lower_switch(IrInstr *in, X86LowerCtx *ctx)
{
    u32 n = in->extra.sw.ncases;
    IrSwitchCase *cases = in->extra.sw.cases;
    const char *default_label = in->extra.sw.default_label;

    bool is_signed;
    u8 cw = load_int_operand(ctx, in->ops[0], &is_signed);

    if (n >= 2)
    {
        i64 min = cases[0].val, max = cases[0].val;
        for (u32 i = 1; i < n; i++)
        {
            min = MIN(cases[i].val, min);
            max = MAX(cases[i].val, max);
        }
        u64 range = (u64) max - (u64) min;
        if (cw <= W_DWORD && range <= JT_MAX_RANGE && fits_i32(min) && fits_i32(max))
        {
            emit_switch_table(ctx, cases, n, min, max, default_label);
            return;
        }
    }
    emit_switch_chain(ctx, cases, n, default_label);
}

typedef void (*LowerFn)(IrInstr *, X86LowerCtx *);

#define LOWER_ENTRIES(X)                                                                           \
    X(OP_RET, lower_ret)                                                                           \
    X(OP_ADD, lower_binary)                                                                        \
    X(OP_SUB, lower_binary)                                                                        \
    X(OP_MUL, lower_binary)                                                                        \
    X(OP_SDIV, lower_div)                                                                          \
    X(OP_SREM, lower_div)                                                                          \
    X(OP_UDIV, lower_div)                                                                          \
    X(OP_UREM, lower_div)                                                                          \
    X(OP_AND, lower_binary)                                                                        \
    X(OP_OR, lower_binary)                                                                         \
    X(OP_XOR, lower_binary)                                                                        \
    X(OP_SHL, lower_shift)                                                                         \
    X(OP_ASHR, lower_shift)                                                                        \
    X(OP_LSHR, lower_shift)                                                                        \
    X(OP_NEG, lower_unary)                                                                         \
    X(OP_NOT, lower_unary)                                                                         \
    X(OP_TRUNC, lower_trunc)                                                                       \
    X(OP_ZEXT, lower_zext)                                                                         \
    X(OP_SEXT, lower_sext)                                                                         \
    X(OP_ICMP_EQ, lower_icmp)                                                                      \
    X(OP_ICMP_NE, lower_icmp)                                                                      \
    X(OP_ICMP_ULT, lower_icmp)                                                                     \
    X(OP_ICMP_ULE, lower_icmp)                                                                     \
    X(OP_ICMP_UGT, lower_icmp)                                                                     \
    X(OP_ICMP_UGE, lower_icmp)                                                                     \
    X(OP_ICMP_SLT, lower_icmp)                                                                     \
    X(OP_ICMP_SLE, lower_icmp)                                                                     \
    X(OP_ICMP_SGT, lower_icmp)                                                                     \
    X(OP_ICMP_SGE, lower_icmp)                                                                     \
    X(OP_LOAD, lower_load)                                                                         \
    X(OP_STORE, lower_store)                                                                       \
    X(OP_GEP, lower_gep)                                                                           \
    X(OP_ALLOCA, lower_alloca)                                                                     \
    X(OP_MEMCPY, lower_memcpy)                                                                     \
    X(OP_BR, lower_br)                                                                             \
    X(OP_BRCOND, lower_brcond)                                                                     \
    X(OP_SWITCH, lower_switch)                                                                     \
    X(OP_UNREACHABLE, lower_unreachable)                                                           \
    X(OP_CALL, x86_sysv_lower_call)                                                                \
    X(OP_VA_START, x86_sysv_lower_va_start)                                                        \
    X(OP_VA_ARG, x86_sysv_lower_va_arg)                                                            \
    X(OP_PHI, lower_noop)                                                                          \
    X(OP_VA_END, lower_noop)                                                                       \
    X(OP_ITOF, lower_itof)                                                                         \
    X(OP_FTOI, lower_ftoi)                                                                         \
    X(OP_FCONV, lower_fconv)                                                                       \
    X(OP_FADD, lower_fbin)                                                                         \
    X(OP_FSUB, lower_fbin)                                                                         \
    X(OP_FMUL, lower_fbin)                                                                         \
    X(OP_FDIV, lower_fbin)                                                                         \
    X(OP_FNEG, lower_fneg)                                                                         \
    X(OP_FCMP_EQ, lower_fcmp)                                                                      \
    X(OP_FCMP_NE, lower_fcmp)                                                                      \
    X(OP_FCMP_LT, lower_fcmp)                                                                      \
    X(OP_FCMP_GT, lower_fcmp)                                                                      \
    X(OP_FCMP_LE, lower_fcmp)                                                                      \
    X(OP_FCMP_GE, lower_fcmp)

/* Dispatch table indexed by opcode; unlisted opcodes hit the unsupported path. */
static const LowerFn lower_fns[OP_FCMP_GE + 1] = {
#define LOWER_INIT(op, fn) [op] = fn,
    LOWER_ENTRIES(LOWER_INIT)
#undef LOWER_INIT
};

static void lower_instr(IrInstr *in, X86LowerCtx *ctx)
{
    /* A rematerialized result (a static alloca or an address derived from one)
       has no home; its uses recompute it, so the definition emits nothing. */
    if (in->result != NO_VREG && ctx->alloc->remat[in->result])
    {
        return;
    }
    LowerFn fn = lower_fns[in->opcode];
    if (!fn)
    {
        lower_unsupported(in, ctx);
        return;
    }
    fn(in, ctx);
}

static void store_reg_to_loc(X86LowerCtx *ctx, RegLoc l, u8 width, u8 reg)
{
    if (l.kind == LOC_REG)
    {
        if (l.reg != reg)
        {
            emit_mov(ctx->buf, width, xop_reg(l.reg), xop_reg(reg));
        }
        return;
    }
    emit_mov(ctx->buf, width, xop_mem(rbp_mem(l.disp)), xop_reg(reg));
}

static void store_vreg_from_reg(X86LowerCtx *ctx, u32 vreg, u8 width, u8 reg)
{
    store_reg_to_loc(ctx, x86_lower_operand_loc(ctx, ir_operand_vreg(vreg)), width, reg);
}

/* Emits `dst <- src` for an already-resolved destination location. */
static void emit_copy_to_loc(X86LowerCtx *ctx, IrOperand src, RegLoc dl, u8 dw)
{
    if (dw == W_LD)
    {
        if (src.is_imm)
        {
            ASSERT(src.u.imm == 0 && "nonzero immediate in a width-16 phi copy");
            emit_sse_xor(ctx->buf, 0, R_XMM0, R_XMM0);
            emit_mov16_store(ctx->buf, rbp_mem(dl.disp));
            return;
        }
        RegLoc sl = x86_lower_operand_loc(ctx, src);
        ASSERT(sl.kind == LOC_MEM && dl.kind == LOC_MEM && "x87 values are memory-only");
        emit_mov16(ctx->buf, rbp_mem(sl.disp), rbp_mem(dl.disp));
        return;
    }
    if (src.is_global)
    {
        emit_global_addr_to(ctx->buf, R_EAX, src.u.global_index, ctx->global_patches, ctx->arena);
        store_reg_to_loc(ctx, dl, dw, R_EAX);
        return;
    }
    if (src.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, src.u.func_name, ctx->func_patches, ctx->arena);
        store_reg_to_loc(ctx, dl, dw, R_EAX);
        return;
    }
    if (dl.cls == RC_XMM)
    {
        u8 mf = MF_OF(dw);
        if (src.is_imm)
        {
            emit_mov(ctx->buf, fp_imm_load_width(dw, src.u.imm), xop_reg(R_EAX),
                     xop_imm(src.u.imm));
            if (dl.kind == LOC_REG)
            {
                emit_movd_to_xmm(ctx->buf, dl.reg, R_EAX, dw != W_DWORD);
            }
            else
            {
                emit_movd_to_xmm(ctx->buf, R_XMM0, R_EAX, dw != W_DWORD);
                emit_sse_store(ctx->buf, mf, rbp_mem(dl.disp), R_XMM0);
            }
            return;
        }
        RegLoc sl = x86_lower_operand_loc(ctx, src);
        if (dl.kind == LOC_REG)
        {
            if (sl.kind == LOC_REG)
            {
                if (sl.reg != dl.reg)
                {
                    emit_sse_op_reg(ctx->buf, mf, X86_SSE_MOV, dl.reg, sl.reg);
                }
            }
            else
            {
                emit_sse_load(ctx->buf, mf, dl.reg, rbp_mem(sl.disp));
            }
            return;
        }
        if (sl.kind == LOC_REG)
        {
            emit_sse_store(ctx->buf, mf, rbp_mem(dl.disp), sl.reg);
            return;
        }
        emit_sse_load(ctx->buf, mf, R_XMM0, rbp_mem(sl.disp));
        emit_sse_store(ctx->buf, mf, rbp_mem(dl.disp), R_XMM0);
        return;
    }
    if (src.is_imm)
    {
        if (dl.kind == LOC_REG)
        {
            emit_mov(ctx->buf, dw, xop_reg(dl.reg), xop_imm(src.u.imm));
        }
        else
        {
            emit_mov(ctx->buf, dw, xop_reg(R_EAX), xop_imm(src.u.imm));
            emit_mov(ctx->buf, dw, xop_mem(rbp_mem(dl.disp)), xop_reg(R_EAX));
        }
        return;
    }
    RegLoc sl = x86_lower_operand_loc(ctx, src);
    if (sl.kind == LOC_REMAT)
    {
        force_to_reg(ctx, src, R_EAX);
        store_reg_to_loc(ctx, dl, dw, R_EAX);
        return;
    }
    if (dl.kind == LOC_REG)
    {
        if (sl.kind == LOC_REG)
        {
            if (sl.reg != dl.reg)
            {
                emit_mov(ctx->buf, dw, xop_reg(dl.reg), xop_reg(sl.reg));
            }
        }
        else
        {
            emit_mov(ctx->buf, dw, xop_reg(dl.reg), xop_mem(rbp_mem(sl.disp)));
        }
        return;
    }
    if (sl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, dw, xop_mem(rbp_mem(dl.disp)), xop_reg(sl.reg));
        return;
    }
    emit_mov(ctx->buf, dw, xop_reg(R_EAX), xop_mem(rbp_mem(sl.disp)));
    emit_mov(ctx->buf, dw, xop_mem(rbp_mem(dl.disp)), xop_reg(R_EAX));
}

/* A PHI edge's copy runs at its predecessor's end, so it reads the incoming
   value at its final use and defines the phi result at the same position. */
static void emit_phi_copy(X86LowerCtx *ctx, IrOperand src, u32 dst_vreg)
{
    u8 dw = vreg_width(ctx, dst_vreg);
    RegLoc dl = x86_lower_operand_loc(ctx, ir_operand_vreg(dst_vreg));
    emit_copy_to_loc(ctx, src, dl, dw);
}

/* Saves `src` into the 16-byte scratch slot used to break phi-copy cycles. */
static void emit_scratch_store(X86LowerCtx *ctx, IrOperand src)
{
    X86Mem sm = rbp_mem(ctx->scratch_disp);
    if (src.is_imm)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), xop_imm(src.u.imm));
        emit_mov(ctx->buf, W_QWORD, xop_mem(sm), xop_reg(R_EAX));
        return;
    }
    if (src.is_global)
    {
        emit_global_addr_to(ctx->buf, R_EAX, src.u.global_index, ctx->global_patches, ctx->arena);
        emit_mov(ctx->buf, W_QWORD, xop_mem(sm), xop_reg(R_EAX));
        return;
    }
    if (src.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, src.u.func_name, ctx->func_patches, ctx->arena);
        emit_mov(ctx->buf, W_QWORD, xop_mem(sm), xop_reg(R_EAX));
        return;
    }
    RegLoc sl = x86_lower_operand_loc(ctx, src);
    u8 w = vreg_width(ctx, src.u.vreg);
    if (w == W_LD)
    {
        ASSERT(sl.kind == LOC_MEM && "x87 values are memory-only");
        emit_mov16(ctx->buf, sm, rbp_mem(sl.disp));
        return;
    }
    if (sl.cls == RC_XMM)
    {
        ASSERT(sl.kind == LOC_REG);
        emit_sse_store(ctx->buf, MF_OF(w), sm, sl.reg);
        return;
    }
    if (sl.kind == LOC_REMAT)
    {
        force_to_reg(ctx, src, R_EAX);
        emit_mov(ctx->buf, w, xop_mem(sm), xop_reg(R_EAX));
        return;
    }
    if (sl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, w, xop_mem(sm), xop_reg(sl.reg));
    }
    else
    {
        emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_mem(rbp_mem(sl.disp)));
        emit_mov(ctx->buf, w, xop_mem(sm), xop_reg(R_EAX));
    }
}

/* Restores the scratch slot into the phi destination `dst_vreg`. */
static void emit_scratch_load(X86LowerCtx *ctx, u32 dst_vreg)
{
    X86Mem sm = rbp_mem(ctx->scratch_disp);
    u8 dw = vreg_width(ctx, dst_vreg);
    RegLoc dl = x86_lower_operand_loc(ctx, ir_operand_vreg(dst_vreg));
    if (dw == W_LD)
    {
        ASSERT(dl.kind == LOC_MEM && "x87 values are memory-only");
        emit_mov16(ctx->buf, rbp_mem(dl.disp), sm);
        return;
    }
    if (dl.cls == RC_XMM)
    {
        u8 mf = MF_OF(dw);
        if (dl.kind == LOC_REG)
        {
            emit_sse_load(ctx->buf, mf, dl.reg, sm);
        }
        else
        {
            emit_sse_load(ctx->buf, mf, R_XMM0, sm);
            emit_sse_store(ctx->buf, mf, rbp_mem(dl.disp), R_XMM0);
        }
        return;
    }
    if (dl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, dw, xop_reg(dl.reg), xop_mem(sm));
    }
    else
    {
        emit_mov(ctx->buf, dw, xop_reg(R_EAX), xop_mem(sm));
        emit_mov(ctx->buf, dw, xop_mem(rbp_mem(dl.disp)), xop_reg(R_EAX));
    }
}

static bool phi_operand_same(IrOperand a, IrOperand b)
{
    if (a.is_imm != b.is_imm || a.is_global != b.is_global || a.is_func != b.is_func)
    {
        return false;
    }
    if (a.is_imm)
    {
        return a.u.imm == b.u.imm;
    }
    if (a.is_global)
    {
        return a.u.global_index == b.u.global_index;
    }
    if (a.is_func)
    {
        return a.u.func_name == b.u.func_name || strcmp(a.u.func_name, b.u.func_name) == 0;
    }
    return a.u.vreg == b.u.vreg;
}

/* Emits a block's PHI copies with parallel-copy semantics. A copy whose
   destination is still a source of a pending copy must wait; when every
   remaining destination is a source (a cycle, e.g. a loop-carried swap), the
   cycle is broken through the scratch slot. */
static void emit_block_phi_copies(X86LowerCtx *ctx, size_t bi)
{
    Vec *pcs = ctx->phi_copies[bi];
    size_t n = vec_size(pcs);
    if (n == 0)
    {
        return;
    }
    LowerPhiCopy **work = arena_alloc(ctx->arena, n * sizeof(LowerPhiCopy *), sizeof(void *));
    bool *done = arena_alloc(ctx->arena, n, 1);
    memset(done, 0, n);
    for (size_t i = 0; i < n; i++)
    {
        work[i] = (LowerPhiCopy *) vec_get(pcs, i);
    }

    size_t remaining = n;
    while (remaining > 0)
    {
        size_t pick = n;
        for (size_t i = 0; i < n; i++)
        {
            if (done[i])
            {
                continue;
            }
            u32 dst = work[i]->dst_vreg;
            bool dst_is_source = false;
            for (size_t j = 0; j < n; j++)
            {
                if (done[j] || j == i || work[j]->src_is_scratch)
                {
                    continue;
                }
                IrOperand s = work[j]->src;
                if (!s.is_imm && !s.is_global && !s.is_func && s.u.vreg == dst)
                {
                    dst_is_source = true;
                    break;
                }
            }
            if (!dst_is_source)
            {
                pick = i;
                break;
            }
        }

        if (pick < n)
        {
            if (work[pick]->src_is_scratch)
            {
                emit_scratch_load(ctx, work[pick]->dst_vreg);
            }
            else
            {
                emit_phi_copy(ctx, work[pick]->src, work[pick]->dst_vreg);
            }
            done[pick] = true;
            remaining--;
            continue;
        }

        /* Cycle: spill one pending source to the scratch slot and redirect every
           pending copy that reads it. */
        size_t c = 0;
        while (done[c])
        {
            c++;
        }
        emit_scratch_store(ctx, work[c]->src);
        for (size_t i = 0; i < n; i++)
        {
            if (!done[i] && !work[i]->src_is_scratch &&
                phi_operand_same(work[i]->src, work[c]->src))
            {
                work[i]->src_is_scratch = true;
            }
        }
    }
}

static void add_phi_copies(X86LowerCtx *ctx)
{
    size_t nblocks = vec_size(ctx->func->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(ctx->func->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI || in->result == NO_VREG)
            {
                continue;
            }
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                IrPhiEntry *entry = &in->extra.phi.entries[e];
                size_t pj = block_index_of_label(ctx, entry->label);
                LowerPhiCopy *pc = arena_alloc(ctx->arena, sizeof(LowerPhiCopy), sizeof(void *));
                pc->src = entry->val;
                pc->dst_vreg = in->result;
                pc->src_is_scratch = false;
                vec_push(ctx->phi_copies[pj], pc);
            }
        }
    }
}

/* Record where `in` starts lowering; skip line-0 (pre-statement) rows. */
static void record_line_entry(X86LowerCtx *ctx, IrInstr *in)
{
    if (!ctx->debug || in->line == 0)
    {
        return;
    }
    LineEntry *le = arena_alloc(ctx->arena, sizeof(LineEntry), sizeof(void *));
    le->offset = bytebuf_len(ctx->buf);
    le->line = in->line;
    vec_push(ctx->lines, le);
}

/* Position of block-local instruction `ii`: two slots per instruction. */
static u32 pos_of(u32 base, size_t ii)
{
    return base + 2u * (u32) ii;
}

/* Fill still-unset position slots in [from, to) with `value`. */
static void fill_unset_positions(u32 *offsets, u32 from, u32 to, u32 value)
{
    for (u32 p = from; p < to; p++)
    {
        if (offsets[p] == POS_UNSET)
        {
            offsets[p] = value;
        }
    }
}

/* A brcond over a single-use icmp immediately before it compares into the flags. */
static IrInstr *foldable_brcond_icmp(IrBlock *blk, X86LowerCtx *ctx)
{
    size_t n = vec_size(blk->instrs);
    if (n < 2)
    {
        return NULL;
    }
    IrInstr *term = (IrInstr *) vec_get(blk->instrs, n - 1);
    if (term->opcode != OP_BRCOND || !ir_operand_is_vreg(term->ops[0]))
    {
        return NULL;
    }
    IrInstr *prev = (IrInstr *) vec_get(blk->instrs, n - 2);
    u32 cond = term->ops[0].u.vreg;
    if (!is_icmp(prev->opcode) || prev->result != cond || cond >= ctx->alloc->nvregs ||
        ctx->use_count[cond] != 1)
    {
        return NULL;
    }
    return prev;
}

static void emit_block_linear(IrBlock *blk, size_t bi, X86LowerCtx *ctx)
{
    u32 base = ctx->pos->block_base[bi];
    u32 bend = ctx->pos->block_end[bi];
    ctx->block_offsets[bi] = bytebuf_len(ctx->buf);
    size_t ninstr = vec_size(blk->instrs);

    IrInstr *fold_icmp = foldable_brcond_icmp(blk, ctx);

    size_t ii = 0;
    for (; ii < ninstr; ii++)
    {
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
        if (is_terminator(in->opcode))
        {
            break;
        }
        if (in != fold_icmp)
        {
            ctx->cur_pos = pos_of(base, ii);
            record_line_entry(ctx, in);
            lower_instr(in, ctx);
        }
        ctx->position_offsets[pos_of(base, ii)] = (u32) bytebuf_len(ctx->buf);
    }
    ASSERT(ii < ninstr && "every block ends in a terminator");

    IrInstr *term = (IrInstr *) vec_get(blk->instrs, ii);
    bool is_brcond = term->opcode == OP_BRCOND;
    ctx->cur_pos = bend - 1;
    if (is_brcond)
    {
        if (fold_icmp)
        {
            ctx->cur_pos = pos_of(base, ii - 1);
            emit_icmp_cmp(ctx, fold_icmp->ops[0], fold_icmp->ops[1]);
            ctx->cur_pos = bend - 1;
        }
        else
        {
            lower_brcond_test(term, ctx);
        }
    }

    emit_block_phi_copies(ctx, bi);

    record_line_entry(ctx, term);
    if (is_brcond)
    {
        u8 cc = fold_icmp ? icmp_cc[fold_icmp->opcode] : CC_NE;
        lower_brcond_branch(term, ctx, cc);
    }
    else if (term->opcode != OP_BR || !ctx->next_label ||
             strcmp(term->extra.br.target_label, ctx->next_label) != 0)
    {
        lower_instr(term, ctx);
    }
    ctx->position_offsets[pos_of(base, ii)] = (u32) bytebuf_len(ctx->buf);
    /* Gap positions (phi-copy and scheduling slots) run to the block end. */
    fill_unset_positions(ctx->position_offsets, base, bend, (u32) bytebuf_len(ctx->buf));
    ASSERT(ii + 1 == ninstr && "the terminator is the last instruction in a block");
}

static void resolve_block_patches(X86LowerCtx *ctx)
{
    size_t npatches = vec_size(ctx->block_patches);
    for (size_t pi = 0; pi < npatches; pi++)
    {
        PatchSite *site = (PatchSite *) vec_get(ctx->block_patches, pi);
        patch_rel32(ctx->buf, site->offset,
                    ctx->block_offsets[block_index_of_label(ctx, site->target)]);
    }
}

/* Jump tables follow the body; entries store target - table_base offsets. */
static void emit_switch_tables(X86LowerCtx *ctx)
{
    size_t nst = vec_size(ctx->switch_tables);
    if (nst == 0)
    {
        return;
    }
    bytebuf_align(ctx->buf, W_DWORD);
    for (size_t t = 0; t < nst; t++)
    {
        LowerSwitchTable *rec = (LowerSwitchTable *) vec_get(ctx->switch_tables, t);
        size_t table_off = bytebuf_len(ctx->buf);
        for (u32 i = 0; i < rec->nentries; i++)
        {
            size_t ti = block_index_of_label(ctx, rec->targets[i]);
            bytebuf_append_i32(ctx->buf, (i32) ((i64) ctx->block_offsets[ti] - (i64) table_off));
        }
        patch_rel32(ctx->buf, rec->disp_field_off, table_off);
    }
}

static StrMap *index_labels(IrFunction *f, Arena *arena)
{
    StrMap *m = strmap_new(arena);
    size_t nblocks = vec_size(f->blocks);
    for (size_t i = 0; i < nblocks; i++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, i);
        strmap_set(m, blk->label, (void *) (uintptr_t) (i + 1));
    }
    return m;
}

static IrInstr *block_terminator(IrBlock *blk)
{
    size_t n = vec_size(blk->instrs);
    return n ? (IrInstr *) vec_get(blk->instrs, n - 1) : NULL;
}

/* The successor a block falls through to: false edge, jump target, or default. */
static const char *fallthrough_label(IrBlock *blk)
{
    IrInstr *t = block_terminator(blk);
    if (!t)
    {
        return NULL;
    }
    switch (t->opcode)
    {
        case OP_BR:
            return t->extra.br.target_label;
        case OP_BRCOND:
            return t->extra.brcond.false_label;
        case OP_SWITCH:
            return t->extra.sw.default_label;
        default:
            return NULL;
    }
}

/* Push non-fallthrough edges (index+1) for later traces. */
static void push_cold_succs(IrBlock *blk, const char *fallthrough, X86LowerCtx *ctx, Vec *stack)
{
    IrInstr *t = block_terminator(blk);
    if (!t)
    {
        return;
    }
    if (t->opcode == OP_BRCOND)
    {
        const char *cold = t->extra.brcond.true_label;
        if (!fallthrough || strcmp(cold, fallthrough) != 0)
        {
            void *v = strmap_get(ctx->label_to_index, cold);
            if (v)
            {
                vec_push(stack, v);
            }
        }
    }
    else if (t->opcode == OP_SWITCH)
    {
        for (u32 c = 0; c < t->extra.sw.ncases; c++)
        {
            const char *l = t->extra.sw.cases[c].label;
            if (fallthrough && strcmp(l, fallthrough) == 0)
            {
                continue;
            }
            void *v = strmap_get(ctx->label_to_index, l);
            if (v)
            {
                vec_push(stack, v);
            }
        }
    }
}

/* Trace layout: follow fallthrough edges, deferring cold ones; entry stays first. */
static size_t *layout_blocks(IrFunction *f, X86LowerCtx *ctx, size_t nblocks, Arena *arena)
{
    size_t *order = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(size_t), sizeof(size_t));
    u8 *visited = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(u8), sizeof(u8));
    for (size_t i = 0; i < nblocks; i++)
    {
        visited[i] = 0;
    }
    Vec *stack = vec_new(arena);
    size_t n = 0;
    if (nblocks > 0)
    {
        vec_push(stack, (void *) (uintptr_t) 1); /* entry index 0, encoded as index+1 */
    }
    while (vec_size(stack) > 0)
    {
        size_t bi = (size_t) (uintptr_t) vec_pop(stack) - 1;
        while (bi < nblocks && !visited[bi])
        {
            visited[bi] = 1;
            order[n++] = bi;
            IrBlock *blk = (IrBlock *) vec_get(f->blocks, bi);
            const char *ft = fallthrough_label(blk);
            push_cold_succs(blk, ft, ctx, stack);
            void *v = ft ? strmap_get(ctx->label_to_index, ft) : NULL;
            if (!v)
            {
                break;
            }
            bi = (size_t) (uintptr_t) v - 1;
        }
    }
    for (size_t i = 0; i < nblocks; i++)
    {
        if (!visited[i])
        {
            order[n++] = i;
        }
    }
    return order;
}

static void count_vreg_uses(IrFunction *f, u32 nvregs, u32 *counts)
{
    for (u32 v = 0; v < nvregs; v++)
    {
        counts[v] = 0;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            for (u8 oi = 0; oi < in->nops; oi++)
            {
                if (ir_operand_is_vreg(in->ops[oi]))
                {
                    counts[in->ops[oi].u.vreg]++;
                }
            }
            if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    IrOperand v = in->extra.phi.entries[e].val;
                    if (ir_operand_is_vreg(v))
                    {
                        counts[v.u.vreg]++;
                    }
                }
            }
            else if (in->opcode == OP_CALL)
            {
                if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee))
                {
                    counts[in->extra.call.callee.u.vreg]++;
                }
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    if (ir_operand_is_vreg(in->extra.call.args[a]))
                    {
                        counts[in->extra.call.args[a].u.vreg]++;
                    }
                }
            }
        }
    }
}

static void rewrite_operand(IrOperand *op, const IrOperand *repl, const bool *has)
{
    if (ir_operand_is_vreg(*op) && has[op->u.vreg])
    {
        *op = repl[op->u.vreg];
    }
}

/* Fold `gep base, idx, stride` with a zero byte offset into `base`: the value
   flows directly, so no address instruction is emitted and a parameter's live
   range reaches the real use. Runs before liveness. */
static void canonicalize_identity_geps(IrFunction *f, u32 nvregs, Arena *arena)
{
    u32 n = nvregs ? nvregs : 1;
    IrOperand *repl = arena_alloc(arena, n * sizeof(IrOperand), _Alignof(IrOperand));
    bool *has = arena_alloc(arena, n * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        has[v] = false;
    }
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_GEP || in->result == NO_VREG || !in->ops[1].is_imm ||
                !in->ops[2].is_imm || in->ops[1].u.imm * in->ops[2].u.imm != 0)
            {
                continue;
            }
            repl[in->result] = in->ops[0];
            has[in->result] = true;
        }
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (u32 v = 0; v < nvregs; v++)
        {
            if (has[v] && ir_operand_is_vreg(repl[v]) && has[repl[v].u.vreg])
            {
                repl[v] = repl[repl[v].u.vreg];
                changed = true;
            }
        }
    }
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        Vec *keep = vec_new(arena);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode == OP_GEP && in->result != NO_VREG && has[in->result])
            {
                continue; /* the identity definition, now unused */
            }
            for (u8 oi = 0; oi < in->nops; oi++)
            {
                rewrite_operand(&in->ops[oi], repl, has);
            }
            if (in->opcode == OP_CALL)
            {
                rewrite_operand(&in->extra.call.callee, repl, has);
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    rewrite_operand(&in->extra.call.args[a], repl, has);
                }
            }
            else if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    rewrite_operand(&in->extra.phi.entries[e].val, repl, has);
                }
            }
            vec_push(keep, in);
        }
        blk->instrs = keep;
    }
}

static u32 count_opcode(IrFunction *f, IrOpcode op)
{
    u32 n = 0;
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == op)
            {
                n++;
            }
        }
    }
    return n;
}

static void lower_func(IrFunction *f, CodegenFunc *cf, IrModule *mod, Arena *arena, bool debug)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);
    Vec *global_patches = vec_new(arena);
    Vec *func_patches = vec_new(arena);
    Vec *lines = debug ? vec_new(arena) : NULL;

    const TargetDesc *target = x86_64_target();
    canonicalize_identity_geps(f, mod->next_vreg, arena);
    LiveIntervals set = liveinterval_compute(f, mod, arena);
    RegAllocation *alloc = regalloc_linear(f, &set, target, arena);
    bool omit_fp = false;
    if (x86_frame_can_omit_fp(f, debug) && alloc->frame_size == 0)
    {
        /* Nothing spills, so %rbp is free to join the register bank. */
        RegAllocation *lean = regalloc_linear_ex(f, &set, target, arena, true);
        if (lean->frame_size == 0)
        {
            alloc = lean;
            omit_fp = true;
        }
    }
    /* Reserve a 16-byte slot for breaking phi-copy cycles (parallel moves). */
    if (!omit_fp)
    {
        alloc->frame_size += 16;
    }
    LinearFrame frame = {0};
    x86_frame_plan(alloc, f, target, debug, omit_fp, &frame);
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode == OP_ALLOCA)
            {
                alloc->remat_disp[in->result] = -(i32) in->frame_off;
            }
        }
    }
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode == OP_GEP && alloc->remat[in->result] && ir_operand_is_vreg(in->ops[0]))
            {
                i64 off = in->ops[1].u.imm * in->ops[2].u.imm;
                alloc->remat_disp[in->result] = alloc->remat_disp[in->ops[0].u.vreg] + (i32) off;
            }
        }
    }
    /* Reserved slot sits below every real spill slot. */
    i32 scratch_disp = -(i32) (frame.saved_bytes + alloc->frame_size);

    size_t nblocks = vec_size(f->blocks);
    Vec **phi_copies = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(Vec *), sizeof(void *));
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        phi_copies[bi] = vec_new(arena);
    }

    /* One extra slot: a value live out of the final block records `end` as
       npositions, the position just past the function. */
    u32 *position_offsets = arena_alloc(arena, (set.pos.npositions + 1) * sizeof(u32), sizeof(u32));
    for (u32 p = 0; p <= set.pos.npositions; p++)
    {
        position_offsets[p] = POS_UNSET;
    }

    u32 *use_count = arena_alloc(arena, (set.nvregs ? set.nvregs : 1) * sizeof(u32), sizeof(u32));
    count_vreg_uses(f, set.nvregs, use_count);

    X86LowerCtx ctx = {
        .func = f,
        .mod = mod,
        .arena = arena,
        .buf = buf,
        .target = target,
        .alloc = alloc,
        .frame = &frame,
        .patches = patches,
        .block_patches = block_patches,
        .global_patches = global_patches,
        .func_patches = func_patches,
        .phi_copies = phi_copies,
        .switch_tables = vec_new(arena),
        .block_offsets =
            arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(size_t), sizeof(size_t)),
        .label_to_index = index_labels(f, arena),
        .pos = &set.pos,
        .position_offsets = position_offsets,
        .use_count = use_count,
        .lines = lines,
        .debug = debug,
        .scratch_disp = scratch_disp,
        .shared_epilogue = count_opcode(f, OP_RET) > 1,
        .epilogue_jumps = vec_new(arena),
    };

    x86_frame_emit_prologue(buf, f, mod, alloc, &frame, debug);
    u32 off_params = (u32) bytebuf_len(buf);

    add_phi_copies(&ctx);
    size_t *order = layout_blocks(f, &ctx, nblocks, arena);
    for (size_t pos = 0; pos < nblocks; pos++)
    {
        size_t bi = order[pos];
        ctx.next_label =
            (pos + 1 < nblocks) ? ((IrBlock *) vec_get(f->blocks, order[pos + 1]))->label : NULL;
        emit_block_linear((IrBlock *) vec_get(f->blocks, bi), bi, &ctx);
    }
    emit_switch_tables(&ctx);
    if (ctx.shared_epilogue)
    {
        emit_shared_epilogue(&ctx);
    }
    resolve_block_patches(&ctx);

    fill_unset_positions(position_offsets, 0, set.pos.npositions, (u32) bytebuf_len(buf));
    position_offsets[set.pos.npositions] = (u32) bytebuf_len(buf);
    u32 *live_end = arena_alloc(arena, (set.nvregs ? set.nvregs : 1) * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < set.nvregs; v++)
    {
        live_end[v] = (u32) bytebuf_len(buf);
    }
    for (u32 i = 0; i < set.n; i++)
    {
        live_end[set.ivs[i].vreg] = position_offsets[set.ivs[i].end];
    }
    size_t nparams = vec_size(f->params);
    u32 *param_stage = arena_alloc(arena, (nparams ? nparams : 1) * sizeof(u32), sizeof(u32));
    x86_frame_param_stages(f, &frame, debug, param_stage);

    cf->name = f->name;
    cf->bytes = buf;
    cf->offset = 0;
    cf->patches = patches;
    cf->global_patches = global_patches;
    cf->func_patches = func_patches;
    cf->lines = lines;
    cf->frame.off_push = frame.off_push;
    cf->frame.off_mov = frame.off_mov;
    cf->frame.off_sub = frame.off_sub;
    cf->frame.off_params = off_params;
    memcpy(cf->frame.saved_regs, frame.saved_regs, frame.nsaved);
    cf->frame.nsaved = frame.nsaved;
    cf->is_static = f->is_static;
    cf->func = f;
    cf->slot_off = alloc->slot_map;
    cf->phys_map = alloc->phys_map;
    cf->live_end = live_end;
    cf->param_stage = param_stage;
    cf->alloc = alloc;
    cf->position_offsets = debug ? position_offsets : NULL;
}

size_t x86_lower_module(CodegenModule *cm, IrModule *ir, bool debug, Arena *arena)
{
    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        lower_func(f, cf, ir, arena, debug);
        vec_push(cm->funcs, cf);
    }
    return nfuncs;
}

/* Exported views of the lowering helpers, for the SysV call/varargs layer. */
u8 x86_lower_vreg_width(X86LowerCtx *ctx, u32 vreg)
{
    return vreg_width(ctx, vreg);
}

u8 x86_lower_operand_width(X86LowerCtx *ctx, IrOperand op)
{
    return operand_width(ctx, op);
}

RegLoc x86_lower_operand_loc(X86LowerCtx *ctx, IrOperand op)
{
    return loc_at(ctx->alloc, op, ctx->cur_pos);
}

X86Mem x86_lower_rbp_mem(i32 disp)
{
    return rbp_mem(disp);
}

void x86_lower_force_to_reg(X86LowerCtx *ctx, IrOperand op, u8 reg)
{
    force_to_reg(ctx, op, reg);
}

RegLoc x86_lower_result_loc(X86LowerCtx *ctx, IrInstr *in)
{
    return result_loc(ctx, in);
}

void x86_lower_store_reg_result(X86LowerCtx *ctx, IrInstr *in, u8 width, u8 reg)
{
    store_reg_result(ctx, in, width, reg);
}

X86Mem x86_lower_pointer_in_rax(X86LowerCtx *ctx, IrOperand ptr)
{
    return pointer_in_rax(ctx, ptr);
}

void x86_lower_store_vreg_from_reg(X86LowerCtx *ctx, u32 vreg, u8 width, u8 reg)
{
    store_vreg_from_reg(ctx, vreg, width, reg);
}
