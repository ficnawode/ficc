#include "x87.h"
#include "regalloc.h"
#include "util/assert.h"
#include "x86_lower.h"
#include <stdint.h>

#define X87_FLDT 0xDB       /* /5: fldt m80 */
#define X87_FSTPT 0xDB      /* /7: fstpt m80 (DB /7 — DD /7 is fnstsw m16) */
#define X87_FLDS 0xD9       /* /0: flds m32 */
#define X87_FSTPS 0xD9      /* /3: fstps m32 (store and pop) */
#define X87_FLDL 0xDD       /* /0: fldl m64 */
#define X87_FSTPL 0xDD      /* /3: fstpl m64 (store and pop) */
#define X87_FILD_M32 0xDB   /* /0: fild m32int */
#define X87_FILD_M64 0xDF   /* /5: fild m64int */
#define X87_FISTTP_M32 0xDB /* /1: fisttp m32int */
#define X87_FISTTP_M64 0xDD /* /1: fisttp m64int */
#define X87_DIG_5 5
#define X87_DIG_7 7
#define X87_DIG_0 0
#define X87_DIG_1 1
#define X87_DIG_2 2
#define X87_DIG_3 3

/* x87 register-form opcodes (primary byte then fixed sub-opcode byte). */
#define X87_FLDZ 0xD9EE
#define X87_FCHS 0xD9E0
#define X87_FADDP 0xDEC1
#define X87_FSUBP 0xDEE9
#define X87_FSUBRP 0xDEE1
#define X87_FMULP 0xDEC9
#define X87_FDIVP 0xDEF9
#define X87_FUCOMIP 0xDFE9
#define X87_FSTP_ST0 0xDDD8

/* 80-bit pattern of 2^63: significand 0x8000000000000000 @ exponent 0x403E. */
#define LD_EXPONENT_2POW63 0x403E
#define LD_SIGNIFICAND_2POW63 0x8000000000000000ULL

#define BIT_63 63

/* 0F BA /digit ib: bts = 5, btr = 6. */
#define X86_XOP_BTS 5
#define X86_XOP_BTR 6

static void x87_push(X86LowerCtx *ctx)
{
    ctx->fpu_depth++;
}

static void x87_pop(X86LowerCtx *ctx)
{
    ctx->fpu_depth--;
    ASSERT(ctx->fpu_depth >= 0 && "x87 stack underflow");
}

static void x87_balance(X86LowerCtx *ctx)
{
    ASSERT(ctx->fpu_depth == 0 && "x87 stack not balanced after a width-16 lowering");
}

static void x87_emit_mem(ByteBuf *buf, u8 primary, u8 digit, X86Mem m)
{
    bytebuf_append(buf, primary);
    emit_mem_operand(buf, digit, m);
}

static void x87_emit_reg(ByteBuf *buf, u16 opcode)
{
    bytebuf_append(buf, (u8) (opcode >> 8));
    bytebuf_append(buf, (u8) opcode);
}

void x87_emit_fldt(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FLDT, X87_DIG_5, m);
}

void x87_emit_fstpt(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FSTPT, X87_DIG_7, m);
}

void x87_emit_flds(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FLDS, X87_DIG_0, m);
}

void x87_emit_fstps(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FSTPS, X87_DIG_3, m);
}

void x87_emit_fldl(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FLDL, X87_DIG_0, m);
}

void x87_emit_fstpl(ByteBuf *buf, X86Mem m)
{
    x87_emit_mem(buf, X87_FSTPL, X87_DIG_3, m);
}

void x87_emit_fild(ByteBuf *buf, u8 size, X86Mem m)
{
    u8 primary = size == 4 ? X87_FILD_M32 : X87_FILD_M64;
    x87_emit_mem(buf, primary, size == 4 ? X87_DIG_0 : X87_DIG_5, m);
}

void x87_emit_fisttp(ByteBuf *buf, u8 size, X86Mem m)
{
    u8 primary = size == 4 ? X87_FISTTP_M32 : X87_FISTTP_M64;
    x87_emit_mem(buf, primary, X87_DIG_1, m);
}

void x87_emit_fldz(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FLDZ);
}

void x87_emit_fchs(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FCHS);
}

void x87_emit_faddp(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FADDP);
}

void x87_emit_fsubp(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FSUBP);
}

void x87_emit_fsubrp(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FSUBRP);
}

void x87_emit_fmulp(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FMULP);
}

void x87_emit_fdivp(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FDIVP);
}

void x87_emit_fucomip(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FUCOMIP);
}

void x87_emit_fstp_st0(ByteBuf *buf)
{
    x87_emit_reg(buf, X87_FSTP_ST0);
}

static void emit_fldt(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_fldt(ctx->buf, m);
    x87_push(ctx);
}

static void emit_fstpt(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_fstpt(ctx->buf, m);
    x87_pop(ctx);
}

static void emit_flds(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_flds(ctx->buf, m);
    x87_push(ctx);
}

static void emit_fstps(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_fstps(ctx->buf, m);
    x87_pop(ctx);
}

static void emit_fldl(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_fldl(ctx->buf, m);
    x87_push(ctx);
}

static void emit_fstpl(X86LowerCtx *ctx, X86Mem m)
{
    x87_emit_fstpl(ctx->buf, m);
    x87_pop(ctx);
}

static void emit_fild(X86LowerCtx *ctx, u8 size, X86Mem m)
{
    x87_emit_fild(ctx->buf, size, m);
    x87_push(ctx);
}

static void emit_fisttp(X86LowerCtx *ctx, u8 size, X86Mem m)
{
    x87_emit_fisttp(ctx->buf, size, m);
    x87_pop(ctx);
}

static void emit_fldz(X86LowerCtx *ctx)
{
    x87_emit_fldz(ctx->buf);
    x87_push(ctx);
}

static void emit_fchs(X86LowerCtx *ctx)
{
    x87_emit_fchs(ctx->buf);
}

/* faddp/fmulp: st(1) ← st(1) <op> st(0), pop. */
static void emit_faddp(X86LowerCtx *ctx)
{
    x87_emit_faddp(ctx->buf);
    x87_pop(ctx);
}

static void emit_fsubp(X86LowerCtx *ctx)
{
    x87_emit_fsubp(ctx->buf);
    x87_pop(ctx);
}

/* fsubrp st(1), st(0): st(1) ← st(0) − st(1), pop — the reverse of fsubp. */
static void emit_fsubrp(X86LowerCtx *ctx)
{
    x87_emit_fsubrp(ctx->buf);
    x87_pop(ctx);
}

static void emit_fmulp(X86LowerCtx *ctx)
{
    x87_emit_fmulp(ctx->buf);
    x87_pop(ctx);
}

static void emit_fdivp(X86LowerCtx *ctx)
{
    x87_emit_fdivp(ctx->buf);
    x87_pop(ctx);
}

/* fucomip st(0), st(1): unordered compare, sets ZF/CF/PF like ucomis*, pops st(0). */
static void emit_fucomip(X86LowerCtx *ctx)
{
    x87_emit_fucomip(ctx->buf);
    x87_pop(ctx);
}

static void emit_fstp_st0(X86LowerCtx *ctx)
{
    x87_emit_fstp_st0(ctx->buf);
    x87_pop(ctx);
}

static void emit_bit_imm(ByteBuf *buf, u8 digit, u8 dst_reg, u8 imm)
{
    bytebuf_append(buf, rex(true, false, false, dst_reg >= 8));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_BIT_BASE);
    bytebuf_append(buf, modrm(3, digit, dst_reg));
    bytebuf_append_i8(buf, (i8) imm);
}

static void emit_scratch_reserve(ByteBuf *buf)
{
    emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(W_LD));
}

static void emit_scratch_release(ByteBuf *buf)
{
    emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_ADD], R_ESP, xop_imm(W_LD));
}

/* Build the 80-bit 2^63 in a temporary %rsp scratch region, fldt it, release. */
static void emit_load_2pow63_ld(X86LowerCtx *ctx)
{
    ByteBuf *b = ctx->buf;
    emit_scratch_reserve(b);
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_imm((i64) LD_SIGNIFICAND_2POW63));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rsp(0)), xop_reg(R_EAX));
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_imm(0));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rsp(8)), xop_reg(R_EAX));
    emit_mov(b, W_WORD, xop_reg(R_EAX), xop_imm(LD_EXPONENT_2POW63));
    emit_mov(b, W_WORD, xop_mem(x86_mem_rsp(8)), xop_reg(R_EAX));
    emit_fldt(ctx, x86_mem_rsp(0));
    emit_scratch_release(b);
}

static X86Mem ld_operand_mem(X86LowerCtx *ctx, IrOperand op)
{
    ASSERT(ir_operand_is_vreg(op) && "long double operands are vregs");
    RegLoc l = x86_lower_operand_loc(ctx, op);
    ASSERT(l.kind == LOC_MEM && "x87 values are memory-only");
    return x86_lower_rbp_mem(l.disp);
}

static X86Mem ld_result_mem(X86LowerCtx *ctx, IrInstr *in)
{
    RegLoc l = x86_lower_result_loc(ctx, in);
    ASSERT(l.kind == LOC_MEM && "x87 results are memory-only");
    return x86_lower_rbp_mem(l.disp);
}

/* Load an integer operand into %rax, sign/zero-extended to its 64-bit value; reports signedness. */
static u8 load_int_operand(X86LowerCtx *ctx, IrOperand op, bool *is_signed)
{
    ByteBuf *b = ctx->buf;
    x86_lower_force_to_reg(ctx, op, R_EAX);
    if (op.is_imm || op.is_global || op.is_func)
    {
        *is_signed = true;
        return W_QWORD;
    }
    u8 sw = x86_lower_vreg_width(ctx, op.u.vreg);
    *is_signed = ir_vreg_signed(ctx->mod, op.u.vreg);
    if (sw == W_BYTE || sw == W_WORD)
    {
        if (*is_signed)
        {
            emit_movsx(b, sw, W_QWORD, R_EAX, xop_reg(R_EAX));
        }
        else
        {
            emit_movzx(b, sw, W_QWORD, R_EAX, xop_reg(R_EAX));
        }
    }
    else if (sw == W_DWORD && *is_signed)
    {
        emit_movsx(b, W_DWORD, W_QWORD, R_EAX, xop_reg(R_EAX));
    }
    return sw;
}

/* u64 ≥ 2^63 to long double: clear bit 63, convert, add the exact 2^63 back. */
static void emit_itof_u64_x87(X86LowerCtx *ctx, X86Mem dst)
{
    ByteBuf *b = ctx->buf;
    emit_test_reg(b, W_QWORD, R_EAX);
    size_t big_field = emit_jcc_pending(b, CC_S);
    emit_mov(b, W_QWORD, xop_mem(dst), xop_reg(R_EAX));
    emit_fild(ctx, W_QWORD, dst);
    emit_fstpt(ctx, dst);
    size_t small_done = emit_jmp_pending(b);

    size_t big_off = bytebuf_len(b);
    emit_mov(b, W_QWORD, xop_reg(R_R11), xop_reg(R_EAX));
    emit_bit_imm(b, X86_XOP_BTR, R_R11, BIT_63);
    emit_mov(b, W_QWORD, xop_mem(dst), xop_reg(R_R11));
    emit_fild(ctx, W_QWORD, dst);
    emit_load_2pow63_ld(ctx);
    emit_faddp(ctx);
    emit_fstpt(ctx, dst);

    patch_rel32(b, big_field, big_off);
    patch_rel32(b, small_done, bytebuf_len(b));
}

void x87_lower_itof(IrInstr *in, X86LowerCtx *ctx)
{
    bool is_signed;
    u8 sw = load_int_operand(ctx, in->ops[0], &is_signed);
    X86Mem dst = ld_result_mem(ctx, in);
    if (sw == W_QWORD && !is_signed)
    {
        emit_itof_u64_x87(ctx, dst);
    }
    else
    {
        emit_mov(ctx->buf, W_QWORD, xop_mem(dst), xop_reg(R_EAX));
        emit_fild(ctx, W_QWORD, dst);
        emit_fstpt(ctx, dst);
    }
    x87_balance(ctx);
}

/* u64 result from long double: values ≥ 2^63 subtract 2^63, re-truncate, set bit 63. */
static void emit_ftoi_u64_x87(X86LowerCtx *ctx, X86Mem src, X86Mem scratch)
{
    ByteBuf *b = ctx->buf;
    emit_load_2pow63_ld(ctx);
    emit_fldt(ctx, src);
    emit_fucomip(ctx);
    size_t fast_field = emit_jcc_pending(b, CC_B);

    emit_fldt(ctx, src);
    emit_fsubrp(ctx);
    emit_fisttp(ctx, W_QWORD, scratch);
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_mem(scratch));
    emit_bit_imm(b, X86_XOP_BTS, R_EAX, BIT_63);
    size_t slow_done = emit_jmp_pending(b);

    ctx->fpu_depth = 1; /* fast path re-enters with the leftover 2^63 on the stack */
    size_t fast_off = bytebuf_len(b);
    emit_fldt(ctx, src);
    emit_fisttp(ctx, W_QWORD, scratch);
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_mem(scratch));
    emit_fstp_st0(ctx);

    patch_rel32(b, fast_field, fast_off);
    patch_rel32(b, slow_done, bytebuf_len(b));
}

void x87_lower_ftoi(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = x86_lower_vreg_width(ctx, in->result);
    bool is_signed = ir_vreg_signed(ctx->mod, in->result);
    X86Mem src = ld_operand_mem(ctx, in->ops[0]);
    emit_scratch_reserve(ctx->buf);
    X86Mem scratch = x86_mem_rsp(0);
    if (dw == W_QWORD && is_signed)
    {
        emit_fldt(ctx, src);
        emit_fisttp(ctx, W_QWORD, scratch);
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), xop_mem(scratch));
    }
    else if (dw == W_QWORD)
    {
        emit_ftoi_u64_x87(ctx, src, scratch);
    }
    else
    {
        u8 win_w = is_signed ? W_DWORD : W_QWORD;
        emit_fldt(ctx, src);
        emit_fisttp(ctx, win_w, scratch);
        emit_mov(ctx->buf, win_w, xop_reg(R_EAX), xop_mem(scratch));
    }
    emit_scratch_release(ctx->buf);
    x86_lower_store_reg_result(ctx, in, dw, R_EAX);
    x87_balance(ctx);
}

static u8 fp_imm_load_width(u8 w, i64 imm)
{
    return w == 4 ? 4 : (imm >= (i64) INT32_MIN && imm <= (i64) INT32_MAX ? 4 : 8);
}

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
    emit_sse_load(b, MF_OF(w), xmm, x86_lower_rbp_mem(l.disp));
}

/* Long double ↔ float/double via the x87 stack; register homes stage through %rsp. */
void x87_lower_fconv(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = x86_lower_vreg_width(ctx, in->result);
    IrOperand src = in->ops[0];
    u8 sw = src.is_imm ? 8 : x86_lower_vreg_width(ctx, src.u.vreg);

    if (sw == W_LD)
    {
        emit_fldt(ctx, ld_operand_mem(ctx, src));
        RegLoc rl = x86_lower_result_loc(ctx, in);
        if (rl.kind == LOC_REG)
        {
            emit_scratch_reserve(ctx->buf);
            X86Mem tmp = x86_mem_rsp(0);
            if (dw == W_DWORD)
            {
                emit_fstps(ctx, tmp);
            }
            else
            {
                emit_fstpl(ctx, tmp);
            }
            emit_sse_load(ctx->buf, MF_OF(dw), rl.reg, tmp);
            emit_scratch_release(ctx->buf);
        }
        else if (dw == W_DWORD)
        {
            emit_fstps(ctx, x86_lower_rbp_mem(rl.disp));
        }
        else
        {
            emit_fstpl(ctx, x86_lower_rbp_mem(rl.disp));
        }
    }
    else
    {
        X86Mem src_mem;
        if (src.is_imm)
        {
            emit_scratch_reserve(ctx->buf);
            src_mem = x86_mem_rsp(0);
            fp_operand_to_xmm(ctx, src, sw, R_XMM0);
            emit_sse_store(ctx->buf, MF_OF(sw), src_mem, R_XMM0);
        }
        else
        {
            RegLoc sl = x86_lower_operand_loc(ctx, src);
            if (sl.kind == LOC_REG)
            {
                emit_scratch_reserve(ctx->buf);
                src_mem = x86_mem_rsp(0);
                emit_sse_store(ctx->buf, MF_OF(sw), src_mem, sl.reg);
            }
            else
            {
                src_mem = x86_lower_rbp_mem(sl.disp);
            }
        }
        if (sw == W_DWORD)
        {
            emit_flds(ctx, src_mem);
        }
        else
        {
            emit_fldl(ctx, src_mem);
        }
        emit_fstpt(ctx, ld_result_mem(ctx, in));
        if (src.is_imm || x86_lower_operand_loc(ctx, src).kind == LOC_REG)
        {
            emit_scratch_release(ctx->buf);
        }
    }
    x87_balance(ctx);
}

void x87_lower_fbin(IrInstr *in, X86LowerCtx *ctx)
{
    emit_fldt(ctx, ld_operand_mem(ctx, in->ops[0]));
    emit_fldt(ctx, ld_operand_mem(ctx, in->ops[1]));
    switch (in->opcode)
    {
        case OP_FADD:
            emit_faddp(ctx);
            break;
        case OP_FSUB:
            emit_fsubp(ctx);
            break;
        case OP_FMUL:
            emit_fmulp(ctx);
            break;
        case OP_FDIV:
            emit_fdivp(ctx);
            break;
        default:
            ASSERT(false && "unsupported long-double arithmetic opcode");
            break;
    }
    emit_fstpt(ctx, ld_result_mem(ctx, in));
    x87_balance(ctx);
}

void x87_lower_fneg(IrInstr *in, X86LowerCtx *ctx)
{
    IrOperand src = in->ops[0];
    if (src.is_imm)
    {
        ASSERT(src.u.imm == 0 && "nonzero immediate in a long-double negate");
        emit_fldz(ctx);
    }
    else
    {
        emit_fldt(ctx, ld_operand_mem(ctx, src));
    }
    emit_fchs(ctx);
    emit_fstpt(ctx, ld_result_mem(ctx, in));
    x87_balance(ctx);
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

void x87_lower_fcmp(IrInstr *in, X86LowerCtx *ctx)
{
    u8 rw = x86_lower_vreg_width(ctx, in->result);
    IrOperand rhs = in->ops[1];
    if (rhs.is_imm)
    {
        ASSERT(rhs.u.imm == 0 && "nonzero immediate in a long-double compare");
        emit_fldz(ctx);
    }
    else
    {
        emit_fldt(ctx, ld_operand_mem(ctx, rhs));
    }
    emit_fldt(ctx, ld_operand_mem(ctx, in->ops[0]));
    emit_fucomip(ctx);
    emit_fstp_st0(ctx);

    ByteBuf *b = ctx->buf;
    const FcmpSpec *spec = &fcmp_specs[in->opcode];
    emit_setcc_reg(b, spec->cc, R_EAX);
    if (spec->join)
    {
        u8 pf_cc = spec->join == OP_AND ? CC_NP : CC_P;
        emit_setcc_reg(b, pf_cc, R_R11);
        emit_binop_rhs(b, W_BYTE, &arith_specs[spec->join], R_EAX, xop_reg(R_R11));
    }
    emit_movzbl_al_eax(b);
    x86_lower_store_reg_result(ctx, in, rw, R_EAX);
    x87_balance(ctx);
}

void x87_load_result_to_st0(X86LowerCtx *ctx, IrInstr *in)
{
    IrOperand val = in->ops[0];
    if (val.is_imm)
    {
        ASSERT(val.u.imm == 0 && "nonzero immediate long-double return");
        emit_fldz(ctx);
        return;
    }
    x87_emit_fldt(ctx->buf, ld_operand_mem(ctx, val));
}
