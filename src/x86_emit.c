#include "x86_emit.h"
#include "ir.h"
#include "util/assert.h"

static bool fits_i8(i32 v)
{
    return v >= -128 && v <= 127;
}

static u8 encode_sib_scale(u8 scale)
{
    switch (scale)
    {
        case 1:
            return 0;
        case 2:
            return 1;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            ASSERT(false && "invalid SIB scale");
            return 0;
    }
}

u8 modrm(u8 mod, u8 reg, u8 rm)
{
    return (mod << 6) | ((reg & 7) << 3) | (rm & 7);
}

u8 rex(bool w, bool r, bool x, bool b)
{
    return 0x40 | (w ? 0x08 : 0) | (r ? 0x04 : 0) | (x ? 0x02 : 0) | (b ? 0x01 : 0);
}

static u8 rex_mem(bool w, bool r, X86Mem m)
{
    return rex(w, r, reg_is_extended(m.index), reg_is_extended(m.base));
}

void emit_mem_operand(ByteBuf *buf, u8 reg, X86Mem m)
{
    if (m.base == NO_REG)
    {
        bytebuf_append(buf, modrm(0, reg, 5));
        bytebuf_append_i32(buf, m.disp);
        return;
    }

    bool need_sib = m.index != NO_REG || m.base == R_ESP;
    u8 mod;
    if (m.disp == 0 && (need_sib || m.base != R_EBP))
    {
        mod = 0;
    }
    else if (fits_i8(m.disp))
    {
        mod = 1;
    }
    else
    {
        mod = 2;
    }

    /* rm = 4 (rsp) always requires a SIB byte; otherwise rm names the base register. */
    u8 rm = need_sib ? 4 : m.base;
    bytebuf_append(buf, modrm(mod, reg, rm));
    if (need_sib)
    {
        /* No index ⇒ SIB index field 4 (rsp); scale bytes come from encode_sib_scale. */
        u8 idx = m.index == NO_REG ? 4 : (m.index & 7);
        u8 scale = encode_sib_scale(m.scale);
        bytebuf_append(buf, (u8) ((scale << 6) | (idx << 3) | (m.base & 7)));
    }

    if (mod == 1)
    {
        bytebuf_append_i8(buf, (i8) m.disp);
    }
    else if (mod == 2)
    {
        bytebuf_append_i32(buf, m.disp);
    }
}

static void emit_os16(ByteBuf *buf, u8 width)
{
    if (width == 2)
    {
        bytebuf_append(buf, X86_OPERAND_SIZE);
    }
}

void emit_mov_byte(ByteBuf *buf, X86Operand dst, X86Operand src)
{
    if (src.kind == XOP_IMM)
    {
        bytebuf_append(buf, rex(false, false, false, dst.u.reg >= 8));
        bytebuf_append(buf, (u8) (X86_MOV_REG8_IMM8_BASE + (dst.u.reg & 7)));
        bytebuf_append_i8(buf, (i8) src.u.imm);
        return;
    }

    if (dst.kind == XOP_REG && src.kind == XOP_REG)
    {
        bytebuf_append(buf, rex(false, src.u.reg >= 8, false, dst.u.reg >= 8));
        bytebuf_append(buf, X86_MOV_RM8_REG8);
        bytebuf_append(buf, modrm(3, src.u.reg, dst.u.reg));
        return;
    }

    bool to_reg = dst.kind == XOP_REG;
    u8 reg = to_reg ? dst.u.reg : src.u.reg;
    X86Mem mem = to_reg ? src.u.mem : dst.u.mem;
    u8 mov_op = to_reg ? X86_MOV_REG8_RM8 : X86_MOV_RM8_REG8;
    bytebuf_append(buf, rex_mem(false, reg >= 8, mem));
    bytebuf_append(buf, mov_op);
    emit_mem_operand(buf, reg, mem);
}

void emit_mov_scalar(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src)
{
    ASSERT(width == 2 || width == 4 || width == 8);
    emit_os16(buf, width);

    if (src.kind == XOP_IMM)
    {
        bytebuf_append(buf, rex(width == 8, false, false, dst.u.reg >= 8));
        bytebuf_append(buf, (u8) (X86_MOV_REG_IMM_BASE + (dst.u.reg & 7)));
        if (width == 2)
        {
            u16 imm = (u16) src.u.imm;
            bytebuf_append(buf, (u8) (imm & 0xFF));
            bytebuf_append(buf, (u8) (imm >> 8));
        }
        else if (width == 8)
        {
            bytebuf_append_u64(buf, (u64) src.u.imm);
        }
        else
        {
            bytebuf_append_u32(buf, (u32) src.u.imm);
        }
        return;
    }

    if (dst.kind == XOP_REG && src.kind == XOP_REG)
    {
        bytebuf_append(buf, rex(width == 8, src.u.reg >= 8, false, dst.u.reg >= 8));
        bytebuf_append(buf, X86_MOV_RM32_REG32);
        bytebuf_append(buf, modrm(3, src.u.reg, dst.u.reg));
        return;
    }

    bool to_reg = dst.kind == XOP_REG;
    u8 reg = to_reg ? dst.u.reg : src.u.reg;
    X86Mem mem = to_reg ? src.u.mem : dst.u.mem;
    u8 mov_op = to_reg ? X86_MOV_REG32_RM32 : X86_MOV_RM32_REG32;
    bytebuf_append(buf, rex_mem(width == 8, reg >= 8, mem));
    bytebuf_append(buf, mov_op);
    emit_mem_operand(buf, reg, mem);
}

void emit_mov(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src)
{
    if (width == 1)
    {
        emit_mov_byte(buf, dst, src);
    }
    else
    {
        emit_mov_scalar(buf, width, dst, src);
    }
}

const ArithSpec arith_specs[] = {
    [OP_ADD] = {0x03, 0x83, 0x81, 0, false, false}, [OP_SUB] = {0x2B, 0x83, 0x81, 5, false, false},
    [OP_MUL] = {0xAF, 0x6B, 0x69, 0, true, true},   [OP_AND] = {0x23, 0x83, 0x81, 4, false, false},
    [OP_OR] = {0x0B, 0x83, 0x81, 1, false, false},  [OP_XOR] = {0x33, 0x83, 0x81, 6, false, false},
    [OP_FADD] = {0x58, 0, 0, 0, true, false},       [OP_FSUB] = {0x5C, 0, 0, 0, true, false},
    [OP_FMUL] = {0x59, 0, 0, 0, true, false},       [OP_FDIV] = {0x5E, 0, 0, 0, true, false},
};
const ArithSpec cmp_spec = {0x3B, 0x83, 0x81, 7, false, false};

const u8 unary_digit[OP_NOT + 1] = {[OP_NEG] = 3, [OP_NOT] = 2};
const u8 shift_digit[64] = {[OP_SHL] = 4, [OP_LSHR] = 5, [OP_ASHR] = 7};

const u8 icmp_cc[OP_ICMP_SGE + 1] = {
    [OP_ICMP_EQ] = CC_E,  [OP_ICMP_NE] = CC_NE,  [OP_ICMP_ULT] = CC_B, [OP_ICMP_ULE] = CC_BE,
    [OP_ICMP_UGT] = CC_A, [OP_ICMP_UGE] = CC_AE, [OP_ICMP_SLT] = CC_L, [OP_ICMP_SLE] = CC_LE,
    [OP_ICMP_SGT] = CC_G, [OP_ICMP_SGE] = CC_GE,
};

/* REX.W reg,reg form; operand regs must be < 8. */
void emit_reg_reg(ByteBuf *buf, u8 opcode, u8 dst_reg, u8 src_reg)
{
    bytebuf_append(buf, X86_REX_W);
    bytebuf_append(buf, opcode);
    bytebuf_append(buf, modrm(3, dst_reg, src_reg));
}

static void emit_binop_byte(ByteBuf *buf, const ArithSpec *s, u8 dst_reg, X86Operand rhs)
{
    if (rhs.kind == XOP_IMM)
    {
        bytebuf_append(buf, X86_GROUP1_IMM8);
        bytebuf_append(buf, modrm(3, s->digit, dst_reg));
        bytebuf_append_i8(buf, (i8) rhs.u.imm);
        return;
    }
    /* Byte ops use `op r8, r/m8` (mem opcode − 1); a memory RHS is loaded to %cl first. */
    if (rhs.kind == XOP_MEM)
    {
        emit_mov_byte(buf, xop_reg(R_ECX), rhs);
        rhs = xop_reg(R_ECX);
    }
    bytebuf_append(buf, (u8) (s->mem - 1));
    bytebuf_append(buf, modrm(3, dst_reg, rhs.u.reg));
}

static void emit_binop_imm(ByteBuf *buf, u8 width, const ArithSpec *s, u8 dst_reg, i64 v)
{
    emit_os16(buf, width);
    u8 reg_field = s->imm_dst ? dst_reg : s->digit;
    bytebuf_append(buf, rex(width == 8, reg_field >= 8, false, dst_reg >= 8));
    if (fits_i8(v))
    {
        bytebuf_append(buf, s->imm8);
        bytebuf_append(buf, modrm(3, reg_field, dst_reg));
        bytebuf_append_i8(buf, (i8) v);
    }
    else
    {
        bytebuf_append(buf, s->imm32);
        bytebuf_append(buf, modrm(3, reg_field, dst_reg));
        bytebuf_append_i32(buf, (i32) v);
    }
}

void emit_binop_rhs(ByteBuf *buf, u8 width, const ArithSpec *s, u8 dst_reg, X86Operand rhs)
{
    if (width == 1)
    {
        emit_binop_byte(buf, s, dst_reg, rhs);
        return;
    }
    if (rhs.kind == XOP_IMM)
    {
        emit_binop_imm(buf, width, s, dst_reg, rhs.u.imm);
        return;
    }
    if (rhs.kind == XOP_REG)
    {
        emit_os16(buf, width);
        bytebuf_append(buf, rex(width == 8, dst_reg >= 8, false, rhs.u.reg >= 8));
        if (s->mem_0f)
        {
            bytebuf_append(buf, X86_TWO_BYTE_ESC);
        }
        bytebuf_append(buf, s->mem);
        bytebuf_append(buf, modrm(3, dst_reg, rhs.u.reg));
        return;
    }
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, dst_reg >= 8, reg_is_extended(rhs.u.mem.index),
                            reg_is_extended(rhs.u.mem.base)));
    if (s->mem_0f)
    {
        bytebuf_append(buf, X86_TWO_BYTE_ESC);
    }
    bytebuf_append(buf, s->mem);
    emit_mem_operand(buf, dst_reg, rhs.u.mem);
}

void emit_unary(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, GRP3_OPCODE(width));
    bytebuf_append(buf, modrm(3, digit, reg));
}

void emit_shift_cl(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, SHIFT_OPCODE(width));
    bytebuf_append(buf, modrm(3, digit, reg));
}

void emit_cdq(ByteBuf *buf, u8 width, bool is_unsigned)
{
    if (is_unsigned)
    {
        /* `div` reads rdx:rax; the high half must be zeroed (8-bit uses ax:al). */
        if (width == 1)
        {
            bytebuf_append(buf, X86_XOR_RM8_REG8);
            bytebuf_append(buf, modrm(3, 4, 4));
            return;
        }
        bytebuf_append(buf, X86_XOR_REG_RM);
        bytebuf_append(buf, modrm(3, 2, 2)); /* xor edx, edx */
        return;
    }
    if (width == 1)
    {
        bytebuf_append(buf, X86_OPERAND_SIZE);
        bytebuf_append(buf, X86_CBW_CWDE_CDQE);
        return;
    }
    if (width == 8)
    {
        bytebuf_append(buf, X86_REX_W);
    }
    bytebuf_append(buf, X86_CWD_CDQ_CQO);
}

/* /7 digit: idiv (GRP3 keeps the 8-bit F6 form). */
void emit_idiv(ByteBuf *buf, u8 width, u8 reg)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, GRP3_OPCODE(width));
    bytebuf_append(buf, modrm(3, 7, reg));
}

/* /6 digit: div (GRP3 keeps the 8-bit F6 form). */
void emit_div(ByteBuf *buf, u8 width, u8 reg)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, GRP3_OPCODE(width));
    bytebuf_append(buf, modrm(3, 6, reg));
}

void emit_imul_imm(ByteBuf *buf, u8 width, u8 reg, i64 imm)
{
    bytebuf_append(buf, rex(width == 8, reg >= 8, false, reg >= 8));
    if (fits_i8(imm))
    {
        bytebuf_append(buf, X86_IMUL_IMM8);
        bytebuf_append(buf, modrm(3, reg, reg));
        bytebuf_append_i8(buf, (i8) imm);
    }
    else
    {
        bytebuf_append(buf, X86_IMUL_IMM32);
        bytebuf_append(buf, modrm(3, reg, reg));
        bytebuf_append_i32(buf, (i32) imm);
    }
}

/* test %reg: `test al, al` at width 1 so adjacent slot bytes cannot leak in. */
void emit_test_reg(ByteBuf *buf, u8 width, u8 reg)
{
    if (width == 1)
    {
        bytebuf_append(buf, X86_TEST_RM8_REG8);
        bytebuf_append(buf, modrm(3, 0, reg));
        return;
    }
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, X86_TEST_REG_RM);
    bytebuf_append(buf, modrm(3, 0, reg));
}

void emit_xor_eax_eax(ByteBuf *buf)
{
    bytebuf_append(buf, X86_XOR_REG_RM);
    bytebuf_append(buf, modrm(3, 0, 0));
}

/* setcc r8; only the low 3 bits of `reg` are used. */
void emit_setcc_reg(ByteBuf *buf, u8 cc, u8 reg)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, (u8) (X86_SETCC_BASE + cc));
    bytebuf_append(buf, modrm(3, 0, reg));
}

void emit_setcc(ByteBuf *buf, u8 cc)
{
    emit_setcc_reg(buf, cc, R_EAX);
}

void emit_movzbl_al_eax(ByteBuf *buf)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_MOVZX_REG8);
    bytebuf_append(buf, modrm(3, 0, 0));
}

void emit_ud2(ByteBuf *buf)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_UD2);
}

/* Opcode + placeholder rel32; the record goes to `patches` for the right resolution pass. */
static void emit_rel_patch(ByteBuf *buf, u8 opcode, const char *target, Vec *patches, Arena *arena)
{
    PatchSite *site = arena_alloc(arena, sizeof(PatchSite), sizeof(void *));
    site->target = target;
    bytebuf_append(buf, opcode);
    site->offset = bytebuf_len(buf);
    vec_push(patches, site);
    bytebuf_append_i32(buf, 0);
}

void emit_jcc(ByteBuf *buf, u8 cc, const char *target, Vec *patches, Arena *arena)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    emit_rel_patch(buf, (u8) (X86_JCC_BASE + cc), target, patches, arena);
}

void emit_jmp(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    emit_rel_patch(buf, X86_JMP_REL32, target, patches, arena);
}

void emit_call(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    emit_rel_patch(buf, X86_CALL_REL32, target, patches, arena);
}

/* FF /4: jmp r/m64 — indirect jump to the absolute address in a register. */
void emit_jmp_reg(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, rex(true, false, false, reg >= 8));
    bytebuf_append(buf, X86_IND_JMP);
    bytebuf_append(buf, modrm(3, 4, reg));
}

/* FF /2: call r/m64 — indirect call through a function pointer. */
void emit_call_reg(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, rex(true, false, false, reg >= 8));
    bytebuf_append(buf, X86_IND_JMP);
    bytebuf_append(buf, modrm(3, 2, reg));
}

/* Internal-label branch: emit a placeholder rel32 and return the field offset to poke later. */
size_t emit_jcc_pending(ByteBuf *buf, u8 cc)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, (u8) (X86_JCC_BASE + cc));
    size_t off = bytebuf_len(buf);
    bytebuf_append_i32(buf, 0);
    return off;
}

size_t emit_jmp_pending(ByteBuf *buf)
{
    bytebuf_append(buf, X86_JMP_REL32);
    size_t off = bytebuf_len(buf);
    bytebuf_append_i32(buf, 0);
    return off;
}

void patch_rel32(ByteBuf *buf, size_t field_off, size_t target)
{
    i32 rel = (i32) ((i64) target - (i64) (field_off + 4));
    bytebuf_poke_u32(buf, field_off, (u32) rel);
}

void emit_sse_load(ByteBuf *buf, u8 mf, u8 xmm, X86Mem mem)
{
    bytebuf_append(buf, mf);
    bytebuf_append(buf, rex_mem(false, xmm >= 8, mem));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOV);
    emit_mem_operand(buf, xmm, mem);
}

void emit_sse_store(ByteBuf *buf, u8 mf, X86Mem mem, u8 xmm)
{
    bytebuf_append(buf, mf);
    bytebuf_append(buf, rex_mem(false, xmm >= 8, mem));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOV_RM);
    emit_mem_operand(buf, xmm, mem);
}

/* movups 16-byte copy through xmm0 (scalar movs only admit 2|4|8). */
void emit_mov16(ByteBuf *buf, X86Mem src, X86Mem dst)
{
    bytebuf_append(buf, rex_mem(false, false, src));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOV);
    emit_mem_operand(buf, R_XMM0, src);
    bytebuf_append(buf, rex_mem(false, false, dst));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOV_RM);
    emit_mem_operand(buf, R_XMM0, dst);
}

void emit_mov16_store(ByteBuf *buf, X86Mem dst)
{
    bytebuf_append(buf, rex_mem(false, false, dst));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOV_RM);
    emit_mem_operand(buf, R_XMM0, dst);
}

/* REX.R/REX.B for an SSE reg, r/m pair; xmm0-7 need none (and stay byte-stable). */
static void emit_sse_rex(ByteBuf *buf, u8 reg, u8 rm)
{
    if (reg >= 8 || rm >= 8)
    {
        bytebuf_append(buf, rex(false, reg >= 8, false, rm >= 8));
    }
}

/* cvtss2sd/cvtsd2ss; the prefix selects the source precision. */
void emit_sse_cvt(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm)
{
    bytebuf_append(buf, mf);
    emit_sse_rex(buf, dst_xmm, src_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_CVTS2S);
    bytebuf_append(buf, modrm(3, dst_xmm, src_xmm));
}

/* cvtsi2sd/cvtsi2ss; always REX.W, so the source is a 64-bit GP reg. */
void emit_cvtsi2fp(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_reg)
{
    bytebuf_append(buf, mf);
    bytebuf_append(buf, rex(true, dst_xmm >= 8, false, src_reg >= 8));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_CVTSI2);
    bytebuf_append(buf, modrm(3, dst_xmm, src_reg));
}

void emit_cvtts2i(ByteBuf *buf, u8 mf, u8 dst_reg, u8 src_xmm, bool to_64)
{
    bytebuf_append(buf, mf);
    if (to_64 || dst_reg >= 8 || src_xmm >= 8)
    {
        bytebuf_append(buf, rex(to_64, dst_reg >= 8, false, src_xmm >= 8));
    }
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_CVTT);
    bytebuf_append(buf, modrm(3, dst_reg, src_xmm));
}

/* ucomiss/ucomisd: width 4 omits the 66 prefix; unordered (NaN) sets ZF, CF and PF. */
void emit_sse_ucomis(ByteBuf *buf, u8 width, u8 lhs_xmm, u8 rhs_xmm)
{
    if (width != 4)
    {
        bytebuf_append(buf, X86_SSE_66);
    }
    emit_sse_rex(buf, lhs_xmm, rhs_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_UCOMIS);
    bytebuf_append(buf, modrm(3, lhs_xmm, rhs_xmm));
}

void emit_sse_add(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm)
{
    bytebuf_append(buf, mf);
    emit_sse_rex(buf, dst_xmm, src_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_ADD);
    bytebuf_append(buf, modrm(3, dst_xmm, src_xmm));
}

void emit_sse_sub(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm)
{
    bytebuf_append(buf, mf);
    emit_sse_rex(buf, dst_xmm, src_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_SUB);
    bytebuf_append(buf, modrm(3, dst_xmm, src_xmm));
}

/* movd (is64=false) / movq (is64=true): xmm ← GP r/m. */
void emit_movd_to_xmm(ByteBuf *buf, u8 dst_xmm, u8 src_reg, bool is64)
{
    bytebuf_append(buf, X86_SSE_66);
    if (is64 || dst_xmm >= 8 || src_reg >= 8)
    {
        bytebuf_append(buf, rex(is64, dst_xmm >= 8, false, src_reg >= 8));
    }
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_MOVD);
    bytebuf_append(buf, modrm(3, dst_xmm, src_reg));
}

void emit_sse_op_mem(ByteBuf *buf, u8 mf, u8 op, u8 dst_xmm, X86Mem mem)
{
    bytebuf_append(buf, mf);
    bytebuf_append(buf, rex_mem(false, dst_xmm >= 8, mem));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, op);
    emit_mem_operand(buf, dst_xmm, mem);
}

void emit_sse_op_reg(ByteBuf *buf, u8 mf, u8 op, u8 dst_xmm, u8 src_xmm)
{
    bytebuf_append(buf, mf);
    emit_sse_rex(buf, dst_xmm, src_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, op);
    bytebuf_append(buf, modrm(3, dst_xmm, src_xmm));
}

/* xor p[sd] xmm, xmm: flip the sign bit. FNEG uses xorpd (66) for doubles, xorps (no prefix) for
 * floats. */
void emit_sse_xor(ByteBuf *buf, u8 mand, u8 dst_xmm, u8 src_xmm)
{
    if (mand)
    {
        bytebuf_append(buf, mand);
    }
    emit_sse_rex(buf, dst_xmm, src_xmm);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_SSE_XOR);
    bytebuf_append(buf, modrm(3, dst_xmm, src_xmm));
}

static void emit_rm_operand(ByteBuf *buf, u8 dst_reg, X86Operand src)
{
    if (src.kind == XOP_REG)
    {
        bytebuf_append(buf, modrm(3, dst_reg, src.u.reg));
    }
    else
    {
        emit_mem_operand(buf, dst_reg, src.u.mem);
    }
}

/* REX for a reg, r/m form: the R bit comes from `dst_reg`, the B bit from an extended base. */
static u8 rex_rm(bool w, u8 dst_reg, X86Operand src)
{
    return rex(w, dst_reg >= 8, false, src.kind == XOP_MEM && reg_is_extended(src.u.mem.base));
}

void emit_movzx(ByteBuf *buf, u8 src_width, u8 dst_width, u8 dst_reg, X86Operand src)
{
    if (dst_width == 1)
    {
        /* No widening: a byte-to-byte move. */
        emit_mov(buf, 1, xop_reg(dst_reg), src);
        return;
    }
    ASSERT(dst_width == 2 || dst_width == 4 || dst_width == 8);
    emit_os16(buf, dst_width);
    bytebuf_append(buf, rex_rm(dst_width == 8, dst_reg, src));
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    /* Source width 1 → reg8 form, 2 → reg16 form. */
    u8 ext_op = src_width == 1 ? X86_MOVZX_REG8 : X86_MOVZX_REG16;
    bytebuf_append(buf, ext_op);
    emit_rm_operand(buf, dst_reg, src);
}

void emit_movsx(ByteBuf *buf, u8 src_width, u8 dst_width, u8 dst_reg, X86Operand src)
{
    ASSERT(dst_width == 2 || dst_width == 4 || dst_width == 8);
    emit_os16(buf, dst_width);
    if (src_width == 4)
    {
        ASSERT(dst_width == 8);
        bytebuf_append(buf, rex_rm(true, dst_reg, src));
        bytebuf_append(buf, X86_MOVSXD_REG32);
    }
    else
    {
        bytebuf_append(buf, rex_rm(dst_width == 8, dst_reg, src));
        bytebuf_append(buf, X86_TWO_BYTE_ESC);
        /* Source width 1 → reg8 form, 2 → reg16 form. */
        u8 ext_op = src_width == 1 ? X86_MOVSX_REG8 : X86_MOVSX_REG16;
        bytebuf_append(buf, ext_op);
    }
    emit_rm_operand(buf, dst_reg, src);
}

/* `mov reg, imm32sx` (C7 /0): GNU ld relocates it with R_X86_64_32S. */
static void emit_addr_mov_imm32(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, rex(true, false, false, reg >= 8));
    bytebuf_append(buf, X86_MOV_RM_IMM32SX);
    bytebuf_append(buf, modrm(3, 0, reg));
}

void emit_global_addr_to(ByteBuf *buf, u8 reg, u32 global_idx, Vec *patches, Arena *arena)
{
    emit_addr_mov_imm32(buf, reg);
    GlobalPatch *gp = arena_alloc(arena, sizeof(GlobalPatch), sizeof(void *));
    gp->offset = bytebuf_len(buf);
    gp->global_index = global_idx;
    vec_push(patches, gp);
    bytebuf_append_u32(buf, 0);
}

void emit_global_addr(ByteBuf *buf, u32 global_idx, Vec *patches, Arena *arena)
{
    emit_global_addr_to(buf, R_EAX, global_idx, patches, arena);
}

void emit_func_addr_to(ByteBuf *buf, u8 reg, const char *name, Vec *patches, Arena *arena)
{
    emit_addr_mov_imm32(buf, reg);
    FuncAddrPatch *fp = arena_alloc(arena, sizeof(FuncAddrPatch), sizeof(void *));
    fp->name = name;
    fp->offset = bytebuf_len(buf);
    vec_push(patches, fp);
    bytebuf_append_u32(buf, 0);
}

void emit_lea(ByteBuf *buf, u8 dst_reg, X86Mem src)
{
    bytebuf_append(buf, rex_mem(true, dst_reg >= 8, src));
    bytebuf_append(buf, X86_LEA);
    emit_mem_operand(buf, dst_reg, src);
}

/* push r64 (50+rd); the default operand size is already 64-bit, REX.B alone extends. */
void emit_push_reg(ByteBuf *buf, u8 reg)
{
    if (reg >= 8)
    {
        bytebuf_append(buf, X86_REX_B);
    }
    bytebuf_append(buf, (u8) (X86_PUSH_R_BASE + (reg & 7)));
}
