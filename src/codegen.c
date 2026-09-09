#include "codegen.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include "util/types.h"
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* A rel32 field patched later; `target` is a function name (calls) or block label (jcc/jmp). */
typedef struct
{
    size_t offset;
    const char *target;
} PatchSite;

typedef struct
{
    IrOperand src;
    u32 dst_vreg;
} PhiCopy;

/* Jump table appended to the function's .text, indexed by `val − min`; gaps route to default. */
typedef struct
{
    size_t disp_field_off; /* byte offset of the lea rip+disp32 field */
    u32 nentries;          /* range + 1 */
    const char **targets;  /* nentries block labels, index = value − min */
} SwitchTableRec;

/* Per-function frame: vreg i at slot (i+1)*8 below %rbp; variadic funcs add a 176-byte SysV save
 * area. */
typedef struct
{
    u32 frame_size;    /* total frame size below %rbp, rounded up to 16 (ABI) */
    u32 save_area_off; /* register save area offset below %rbp, 0 if non-variadic */
} FrameInfo;

typedef struct CodegenCtx CodegenCtx;
struct CodegenCtx
{
    ByteBuf *buf;
    IrFunction *func;
    IrModule *mod; /* for the per-vreg width table */
    Arena *arena;
    Vec **phi_copies;       /* per-block Vec<PhiCopy*>, indexed by block index */
    StrMap *label_to_block; /* block label -> IrBlock* */
    U64Map *block_to_index; /* IrBlock* -> block index */
    size_t *block_offsets;  /* per-block offset within the function bytes */
    Vec *patches;           /* Vec<PatchSite*> — function call rel32 fields */
    Vec *block_patches;     /* Vec<PatchSite*> — intra-function jump rel32 fields */
    Vec *global_patches;    /* Vec<GlobalPatch*> */
    Vec *func_patches;      /* Vec<FuncAddrPatch*> — function-address loads */
    Vec *switch_tables;     /* Vec<SwitchTableRec*> */
    u32 save_area_off;      /* register save area offset below %rbp (variadic fns) */
};

static void codegen_error(CodegenCtx *ctx, const char *fmt, ...)
{
    fprintf(stderr, "[codegen] error: ");
    if (ctx && ctx->func)
    {
        fprintf(stderr, "in function '%s': ", ctx->func->name);
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* x86-64 register and operand model                                   */
/* ------------------------------------------------------------------ */

typedef enum
{
    R_EAX,
    R_ECX,
    R_EDX,
    R_EBX,
    R_ESP,
    R_EBP,
    R_ESI,
    R_EDI,
    R_R8,
    R_R9,
    R_R10,
    R_R11,
    R_R12,
    R_R13,
    R_R14,
    R_R15
} X86Reg;

/* x86 condition-code encodings, shared by setcc and jcc. */
typedef enum
{
    CC_O = 0,
    CC_NO,
    CC_B,
    CC_AE,
    CC_E,
    CC_NE,
    CC_BE,
    CC_A,
    CC_S,
    CC_NS,
    CC_P,
    CC_NP,
    CC_L,
    CC_GE,
    CC_LE,
    CC_G
} CondCode;

/* Opcode and prefix bytes used by the encoder; values taking a register nibble in the low 3 bits
 * are named *_BASE. */
typedef enum
{
    X86_TWO_BYTE_ESC = 0x0F,
    X86_UD2 = 0x0B,
    X86_OPERAND_SIZE = 0x66,
    X86_REX_W = 0x48,
    X86_REP = 0xF3,

    X86_PUSH_RBP = 0x55,
    X86_LEAVE = 0xC9,
    X86_RET = 0xC3,
    X86_MOVSB = 0xA4,

    X86_IMUL_IMM8 = 0x6B,
    X86_IMUL_IMM32 = 0x69,

    X86_CALL_REL32 = 0xE8,
    X86_JMP_REL32 = 0xE9,
    X86_IND_JMP = 0xFF, /* /4: jmp r/m64 (register operand) */

    X86_MOV_RM8_REG8 = 0x88,
    X86_MOV_RM32_REG32 = 0x89,
    X86_MOV_REG8_RM8 = 0x8A,
    X86_MOV_REG32_RM32 = 0x8B,
    X86_LEA = 0x8D,

    X86_MOV_REG8_IMM8_BASE = 0xB0,
    X86_MOV_REG_IMM_BASE = 0xB8,
    X86_MOV_RM_IMM32SX = 0xC7, /* mov r/m64, imm32 sign-extended (address constants) */

    X86_GROUP1_IMM8 = 0x80,
    X86_GROUP1_IMM32 = 0x81,
    X86_GROUP1_IMM8SX = 0x83,

    X86_GROUP3_RM8 = 0xF6,
    X86_GROUP3_RM32 = 0xF7,

    X86_SHIFT_RM8_CL = 0xD2,
    X86_SHIFT_RM32_CL = 0xD3,

    X86_CBW_CWDE_CDQE = 0x98,
    X86_CWD_CDQ_CQO = 0x99,

    X86_XOR_REG_RM = 0x31,
    X86_XOR_RM8_REG8 = 0x30,
    X86_TEST_REG_RM = 0x85,

    X86_MOVZX_REG8 = 0xB6,
    X86_MOVZX_REG16 = 0xB7,
    X86_MOVSX_REG8 = 0xBE,
    X86_MOVSX_REG16 = 0xBF,
    X86_MOVSXD_REG32 = 0x63,

    X86_JCC_BASE = 0x80,
    X86_SETCC_BASE = 0x90,
} X86Opcode;

#define NO_REG 0xFF

static bool reg_is_extended(u8 reg)
{
    return reg != NO_REG && reg >= 8;
}

typedef struct
{
    u8 base;
    u8 index;
    u8 scale;
    i32 disp;
} X86Mem;

typedef enum
{
    XOP_IMM,
    XOP_REG,
    XOP_MEM
} X86OpKind;

typedef struct
{
    X86OpKind kind;
    union
    {
        i64 imm;
        u8 reg;
        X86Mem mem;
    } u;
} X86Operand;

static X86Operand xop_imm(i64 val)
{
    X86Operand o;
    o.kind = XOP_IMM;
    o.u.imm = val;
    return o;
}

static X86Operand xop_reg(u8 reg)
{
    X86Operand o;
    o.kind = XOP_REG;
    o.u.reg = reg;
    return o;
}

static X86Operand xop_mem(X86Mem m)
{
    X86Operand o;
    o.kind = XOP_MEM;
    o.u.mem = m;
    return o;
}

static X86Mem x86_mem_rbp(i32 disp)
{
    X86Mem m;
    m.base = R_EBP;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static X86Mem x86_mem_rsp(i32 disp)
{
    X86Mem m;
    m.base = R_ESP;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static X86Mem x86_mem_rax(i32 disp)
{
    X86Mem m;
    m.base = R_EAX;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static X86Mem x86_mem_rcx(i32 disp)
{
    X86Mem m;
    m.base = R_ECX;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

/* Immediates stay immediate; vregs live in their frame slot. */
static X86Operand xop_vreg(u32 vreg)
{
    return xop_mem(x86_mem_rbp(-(i32) ((vreg + 1) * 8)));
}

static X86Operand xop_from_operand(IrOperand op)
{
    if (op.is_global)
    {
        return xop_imm(0);
    }
    if (op.is_imm)
    {
        return xop_imm(op.u.imm);
    }
    return xop_vreg(op.u.vreg);
}

static void emit_global_addr_to(ByteBuf *buf, u8 reg, u32 global_idx, Vec *patches, Arena *arena);
static void emit_func_addr_to(ByteBuf *buf, u8 reg, const char *name, Vec *patches, Arena *arena);
static u8 vreg_width(CodegenCtx *ctx, u32 vreg);
static X86Operand lowered_operand(CodegenCtx *ctx, IrOperand op, u8 reg)
{
    if (op.is_global)
    {
        emit_global_addr_to(ctx->buf, reg, op.u.global_index, ctx->global_patches, ctx->arena);
        return xop_reg(reg);
    }
    if (op.is_func)
    {
        emit_func_addr_to(ctx->buf, reg, op.u.func_name, ctx->func_patches, ctx->arena);
        return xop_reg(reg);
    }
    return xop_from_operand(op);
}

/* Lowered value width: addresses are 8 bytes; a materialized immediate counts as 4 (mov-imm32sx).
 */
static u8 operand_width(CodegenCtx *ctx, IrOperand op)
{
    if (op.is_global || op.is_func)
    {
        return 8;
    }
    return op.is_imm ? 4 : vreg_width(ctx, op.u.vreg);
}

static u8 modrm(u8 mod, u8 reg, u8 rm)
{
    return (mod << 6) | ((reg & 7) << 3) | (rm & 7);
}

static u8 rex(bool w, bool r, bool x, bool b)
{
    return 0x40 | (w ? 0x08 : 0) | (r ? 0x04 : 0) | (x ? 0x02 : 0) | (b ? 0x01 : 0);
}

static bool fits_i8(i32 v)
{
    return v >= -128 && v <= 127;
}

static bool fits_i32(i64 v)
{
    return v >= (i64) INT32_MIN && v <= (i64) INT32_MAX;
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

static void emit_mem_operand(ByteBuf *buf, u8 reg, X86Mem m)
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

    bytebuf_append(buf, modrm(mod, reg, need_sib ? 4 : m.base));
    if (need_sib)
    {
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

static void emit_mov_byte(ByteBuf *buf, X86Operand dst, X86Operand src)
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
    bytebuf_append(buf,
                   rex(false, reg >= 8, reg_is_extended(mem.index), reg_is_extended(mem.base)));
    bytebuf_append(buf, to_reg ? X86_MOV_REG8_RM8 : X86_MOV_RM8_REG8);
    emit_mem_operand(buf, reg, mem);
}

static void emit_mov_scalar(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src)
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
    bytebuf_append(
        buf, rex(width == 8, reg >= 8, reg_is_extended(mem.index), reg_is_extended(mem.base)));
    bytebuf_append(buf, to_reg ? X86_MOV_REG32_RM32 : X86_MOV_RM32_REG32);
    emit_mem_operand(buf, reg, mem);
}

static void emit_mov(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src)
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

/* x86 encoding recipe for a binary arithmetic/logic operation. */
typedef struct
{
    u8 mem;      /* reg op= r/m opcode */
    u8 imm8;     /* reg op= imm8 opcode (0x83, or 0x6B for imul) */
    u8 imm32;    /* reg op= imm32 opcode (0x81, or 0x69 for imul) */
    u8 digit;    /* /digit for all forms */
    bool mem_0f; /* true if the mem opcode needs a 0x0F prefix */
} ArithSpec;

static const ArithSpec arith_specs[] = {
    [OP_ADD] = {0x03, 0x83, 0x81, 0, false}, [OP_SUB] = {0x2B, 0x83, 0x81, 5, false},
    [OP_MUL] = {0xAF, 0x6B, 0x69, 0, true},  [OP_AND] = {0x23, 0x83, 0x81, 4, false},
    [OP_OR] = {0x0B, 0x83, 0x81, 1, false},  [OP_XOR] = {0x33, 0x83, 0x81, 6, false},
};

/* cmp: same shape as the arithmetic ops, /7. Not an IR opcode itself. */
static const ArithSpec cmp_spec = {0x3B, 0x83, 0x81, 7, false};

static const u8 unary_digit[OP_NOT + 1] = {[OP_NEG] = 3, [OP_NOT] = 2};
static const u8 shift_digit[64] = {[OP_SHL] = 4, [OP_LSHR] = 5, [OP_ASHR] = 7};

/* setcc/jcc condition code per icmp predicate, indexed by opcode. */
static const u8 icmp_cc[OP_ICMP_SGE + 1] = {
    [OP_ICMP_EQ] = CC_E,  [OP_ICMP_NE] = CC_NE,  [OP_ICMP_ULT] = CC_B, [OP_ICMP_ULE] = CC_BE,
    [OP_ICMP_UGT] = CC_A, [OP_ICMP_UGE] = CC_AE, [OP_ICMP_SLT] = CC_L, [OP_ICMP_SLE] = CC_LE,
    [OP_ICMP_SGT] = CC_G, [OP_ICMP_SGE] = CC_GE,
};

/* reg64,reg64 op form: destination is modrm.reg, source modrm.rm; REX.W only, caller regs < 8. */
static void emit_reg_reg(ByteBuf *buf, u8 opcode, u8 dst_reg, u8 src_reg)
{
    bytebuf_append(buf, X86_REX_W);
    bytebuf_append(buf, opcode);
    bytebuf_append(buf, modrm(3, dst_reg, src_reg));
}

/* %reg op= rhs */
static void emit_binop_rhs(ByteBuf *buf, u8 width, const ArithSpec *s, u8 dst_reg, X86Operand rhs)
{
    if (width == 1)
    {
        if (rhs.kind == XOP_IMM)
        {
            bytebuf_append(buf, X86_GROUP1_IMM8);
            bytebuf_append(buf, modrm(3, s->digit, dst_reg));
            bytebuf_append_i8(buf, (i8) rhs.u.imm);
            return;
        }
        /* Byte ops use `op r8, r/m8` (mem opcode − 1); AL/CL only, no REX, a memory RHS is loaded
         * to %cl first. */
        if (rhs.kind == XOP_MEM)
        {
            emit_mov_byte(buf, xop_reg(R_ECX), rhs);
            rhs = xop_reg(R_ECX);
        }
        bytebuf_append(buf, (u8) (s->mem - 1));
        bytebuf_append(buf, modrm(3, dst_reg, rhs.u.reg));
        return;
    }
    if (rhs.kind == XOP_IMM)
    {
        i64 v = rhs.u.imm;
        emit_os16(buf, width);
        bytebuf_append(buf, rex(width == 8, false, false, dst_reg >= 8));
        if (fits_i8(v))
        {
            bytebuf_append(buf, s->imm8);
            bytebuf_append(buf, modrm(3, s->digit, dst_reg));
            bytebuf_append_i8(buf, (i8) v);
        }
        else
        {
            bytebuf_append(buf, s->imm32);
            bytebuf_append(buf, modrm(3, s->digit, dst_reg));
            bytebuf_append_i32(buf, (i32) v);
        }
        return;
    }
    if (rhs.kind == XOP_REG)
    {
        /* `%reg op= reg` form: RHS is a global/function address lowered into a register (cf.
         * lowered_operand). */
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

/* F7 /digit; for 16-bit 0x66 prefix added. 8-bit uses F6. */
static void emit_unary(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, width == 1 ? X86_GROUP3_RM8 : X86_GROUP3_RM32);
    bytebuf_append(buf, modrm(3, digit, reg));
}

/* D3 /digit, count in %cl. 8-bit uses D2. */
static void emit_shift_cl(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    emit_os16(buf, width);
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, width == 1 ? X86_SHIFT_RM8_CL : X86_SHIFT_RM32_CL);
    bytebuf_append(buf, modrm(3, digit, reg));
}

static void emit_cdq(ByteBuf *buf, u8 width, bool is_unsigned)
{
    if (width == 1)
    {
        if (is_unsigned)
        {
            bytebuf_append(buf, X86_XOR_RM8_REG8);
            bytebuf_append(buf, modrm(3, 4, 4));
        }
        else
        {
            bytebuf_append(buf, X86_OPERAND_SIZE);
            bytebuf_append(buf, X86_CBW_CWDE_CDQE);
        }
        return;
    }
    if (width == 8)
    {
        bytebuf_append(buf, X86_REX_W);
    }
    bytebuf_append(buf, X86_CWD_CDQ_CQO);
}

/* F7 /7: idiv %reg. 8-bit uses F6 /7. */
static void emit_idiv(ByteBuf *buf, u8 width, u8 reg)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, width == 1 ? X86_GROUP3_RM8 : X86_GROUP3_RM32);
    bytebuf_append(buf, modrm(3, 7, reg));
}

/* imul r, r/m, imm (6B ib / 69 id) for the GEP fallback path. */
static void emit_imul_imm(ByteBuf *buf, u8 width, u8 reg, i64 imm)
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

static void emit_test_eax_eax(ByteBuf *buf)
{
    bytebuf_append(buf, X86_TEST_REG_RM);
    bytebuf_append(buf, modrm(3, 0, 0));
}

static void emit_xor_eax_eax(ByteBuf *buf)
{
    bytebuf_append(buf, X86_XOR_REG_RM);
    bytebuf_append(buf, modrm(3, 0, 0));
}

/* 0F 90+cc: setcc %al */
static void emit_setcc(ByteBuf *buf, u8 cc)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, (u8) (X86_SETCC_BASE + cc));
    bytebuf_append(buf, modrm(3, 0, 0));
}

static void emit_movzbl_al_eax(ByteBuf *buf)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_MOVZX_REG8);
    bytebuf_append(buf, modrm(3, 0, 0));
}

static void emit_ud2(ByteBuf *buf)
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

static void emit_jcc(ByteBuf *buf, u8 cc, const char *target, Vec *patches, Arena *arena)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    emit_rel_patch(buf, (u8) (X86_JCC_BASE + cc), target, patches, arena);
}

static void emit_jmp(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    emit_rel_patch(buf, X86_JMP_REL32, target, patches, arena);
}

static void emit_call(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    emit_rel_patch(buf, X86_CALL_REL32, target, patches, arena);
}

/* Internal-label branch: emit a placeholder rel32 and return the field offset to poke later. */
static size_t emit_jcc_pending(ByteBuf *buf, u8 cc)
{
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, (u8) (X86_JCC_BASE + cc));
    size_t off = bytebuf_len(buf);
    bytebuf_append_i32(buf, 0);
    return off;
}

static size_t emit_jmp_pending(ByteBuf *buf)
{
    bytebuf_append(buf, X86_JMP_REL32);
    size_t off = bytebuf_len(buf);
    bytebuf_append_i32(buf, 0);
    return off;
}

static void patch_rel32(ByteBuf *buf, size_t field_off, size_t target)
{
    i32 rel = (i32) ((i64) target - (i64) (field_off + 4));
    bytebuf_poke_u32(buf, field_off, (u32) rel);
}

/* FF /4: jmp r/m64 — indirect jump to the absolute address in a register. */
static void emit_jmp_reg(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, X86_REX_W);
    bytebuf_append(buf, X86_IND_JMP);
    bytebuf_append(buf, modrm(3, 4, reg));
}

/* `call *%reg` (FF /2) — indirect call through a function pointer. */
static void emit_call_reg(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, rex(true, false, false, reg >= 8));
    bytebuf_append(buf, X86_IND_JMP);
    bytebuf_append(buf, modrm(3, 2, reg));
}

/* ------------------------------------------------------------------ */
/* IR lowering                                                         */
/* ------------------------------------------------------------------ */

/* Scratch: lower_* may clobber R_EAX/R_ECX/R_EDX, so the allocator must not schedule live ranges
 * across one lowering. */
typedef void (*LowerFn)(IrInstr *in, CodegenCtx *ctx);

static void emit_movzx(ByteBuf *buf, u8 src_w, u8 dst_w, u8 dst_reg, X86Operand src);
static void emit_movsx(ByteBuf *buf, u8 src_w, u8 dst_w, u8 dst_reg, X86Operand src);
static void emit_lea(ByteBuf *buf, u8 dst_reg, X86Mem src);

static void lower_binary(IrInstr *in, CodegenCtx *ctx);
static void lower_unary(IrInstr *in, CodegenCtx *ctx);
static void lower_shift(IrInstr *in, CodegenCtx *ctx);
static void lower_div(IrInstr *in, CodegenCtx *ctx);
static void lower_icmp(IrInstr *in, CodegenCtx *ctx);
static void lower_call(IrInstr *in, CodegenCtx *ctx);
static void lower_ret(IrInstr *in, CodegenCtx *ctx);
static void lower_br(IrInstr *in, CodegenCtx *ctx);
static void lower_brcond(IrInstr *in, CodegenCtx *ctx);
static void lower_switch(IrInstr *in, CodegenCtx *ctx);
static void lower_phi(IrInstr *in, CodegenCtx *ctx);
static void lower_unreachable(IrInstr *in, CodegenCtx *ctx);
static void lower_trunc(IrInstr *in, CodegenCtx *ctx);
static void lower_zext(IrInstr *in, CodegenCtx *ctx);
static void lower_sext(IrInstr *in, CodegenCtx *ctx);
static void lower_load(IrInstr *in, CodegenCtx *ctx);
static void lower_store(IrInstr *in, CodegenCtx *ctx);
static void lower_gep(IrInstr *in, CodegenCtx *ctx);
static void lower_alloca(IrInstr *in, CodegenCtx *ctx);
static void lower_memcpy(IrInstr *in, CodegenCtx *ctx);
static void lower_va_start(IrInstr *in, CodegenCtx *ctx);
static void lower_va_arg(IrInstr *in, CodegenCtx *ctx);
static void lower_va_end(IrInstr *in, CodegenCtx *ctx);
static X86Mem load_ptr(CodegenCtx *ctx, IrOperand ptr);

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
    X(OP_BR, lower_br)                                                                             \
    X(OP_BRCOND, lower_brcond)                                                                     \
    X(OP_SWITCH, lower_switch)                                                                     \
    X(OP_CALL, lower_call)                                                                         \
    X(OP_PHI, lower_phi)                                                                           \
    X(OP_UNREACHABLE, lower_unreachable)                                                           \
    X(OP_LOAD, lower_load)                                                                         \
    X(OP_STORE, lower_store)                                                                       \
    X(OP_GEP, lower_gep)                                                                           \
    X(OP_ALLOCA, lower_alloca)                                                                     \
    X(OP_MEMCPY, lower_memcpy)                                                                     \
    X(OP_VA_START, lower_va_start)                                                                 \
    X(OP_VA_ARG, lower_va_arg)                                                                     \
    X(OP_VA_END, lower_va_end)

/* Dispatch table indexed by opcode; unlisted opcodes hit the error path in lower_instr. */
static const LowerFn lower_fns[] = {
#define LOWER_INIT(op, fn) [op] = fn,
    LOWER_ENTRIES(LOWER_INIT)
#undef LOWER_INIT
};

static u8 vreg_width(CodegenCtx *ctx, u32 vreg)
{
    return ctx->mod->widths[vreg];
}

static void lower_binary(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    const ArithSpec *s = &arith_specs[in->opcode];
    X86Operand rhs = lowered_operand(ctx, in->ops[1], R_ECX);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    if (in->opcode == OP_AND && w == 4 && rhs.kind == XOP_IMM && rhs.u.imm == 0xFF)
    {
        emit_movzbl_al_eax(ctx->buf); /* andl $0xFF, %eax → movzbl %al, %eax */
    }
    else if (in->opcode == OP_XOR && rhs.kind == XOP_IMM && rhs.u.imm == 0)
    {
        emit_xor_eax_eax(ctx->buf);
    }
    else
    {
        emit_binop_rhs(ctx->buf, w, s, R_EAX, rhs);
    }
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_unary(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_unary(ctx->buf, w, R_EAX, unary_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_shift(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_mov(ctx->buf, w, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    emit_shift_cl(ctx->buf, w, R_EAX, shift_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_div(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    bool is_unsigned = in->opcode == OP_UDIV || in->opcode == OP_UREM;
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_cdq(ctx->buf, w, is_unsigned);
    emit_mov(ctx->buf, w, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    emit_idiv(ctx->buf, w, R_ECX);
    if (in->opcode == OP_SREM || in->opcode == OP_UREM)
    {
        emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_reg(R_EDX));
    }
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_icmp(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_binop_rhs(ctx->buf, w, &cmp_spec, R_EAX, lowered_operand(ctx, in->ops[1], R_ECX));
    emit_setcc(ctx->buf, icmp_cc[in->opcode]);
    emit_movzbl_al_eax(ctx->buf);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

/* System V AMD64 argument registers. */
static const u8 abi_arg_regs[6] = {R_EDI, R_ESI, R_EDX, R_ECX, R_R8, R_R9};

static void lower_call(IrInstr *in, CodegenCtx *ctx)
{
    u32 nargs = in->extra.call.nargs;
    u32 n_stack = MAX(nargs, 6) - 6;
    u32 pad = (n_stack % 2) * 8;
    u32 total_stack = n_stack * 8 + pad;

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, 8, &arith_specs[OP_SUB], R_ESP, xop_imm(total_stack));
    }

    for (u32 i = 6; i < nargs; i++)
    {
        IrOperand arg = in->extra.call.args[i];
        u8 w = operand_width(ctx, arg);
        emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, arg, R_EAX));
        emit_mov(ctx->buf, 8, xop_mem(x86_mem_rsp((i32) (i - 6) * 8)), xop_reg(R_EAX));
    }

    u32 n_reg_args = MIN(nargs, 6);
    for (i32 i = (i32) n_reg_args - 1; i >= 0; i--)
    {
        IrOperand arg = in->extra.call.args[i];
        u8 w = operand_width(ctx, arg);
        emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, arg, R_EAX));
        emit_mov(ctx->buf, w, xop_reg(abi_arg_regs[i]), xop_reg(R_EAX));
    }

    /* SysV: variadic callees get the vector count in %al; no floats yet, so emit 0 here, after the
     * arg loads clobber %eax. */
    if (in->extra.call.is_variadic)
    {
        emit_xor_eax_eax(ctx->buf);
    }

    if (in->extra.call.is_indirect)
    {
        /* Indirect call: load the callee pointer into %r11 (caller-saved, not an arg register) and
         * `call *%r11`. */
        emit_mov(ctx->buf, 8, xop_reg(R_R11), lowered_operand(ctx, in->extra.call.callee, R_R11));
        emit_call_reg(ctx->buf, R_R11);
    }
    else
    {
        emit_call(ctx->buf, in->extra.call.name, ctx->patches, ctx->arena);
    }

    if (in->result != NO_VREG)
    {
        u8 rw = vreg_width(ctx, in->result);
        emit_mov(ctx->buf, rw, xop_vreg(in->result), xop_reg(R_EAX));
    }

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, 8, &arith_specs[OP_ADD], R_ESP, xop_imm(total_stack));
    }
}

/* va_start: record the four va_list fields; the GP regs were spilled by emit_prologue, so this only
 * stores addresses and constants. */
static void lower_va_start(IrInstr *in, CodegenCtx *ctx)
{
    (void) load_ptr(ctx, in->ops[0]);                                 /* ap -> %rax */
    emit_mov(ctx->buf, 4, xop_reg(R_EDX), xop_imm(in->ops[2].u.imm)); /* gp_offset */
    emit_mov(ctx->buf, 4, xop_mem(x86_mem_rax(0)), xop_reg(R_EDX));
    emit_mov(ctx->buf, 4, xop_reg(R_EDX), xop_imm(48)); /* fp_offset: no xmm use */
    emit_mov(ctx->buf, 4, xop_mem(x86_mem_rax(4)), xop_reg(R_EDX));

    u32 skip = (u32) in->ops[1].u.imm;
    emit_lea(ctx->buf, R_EDX, x86_mem_rbp(16 + (i32) skip)); /* overflow_arg_area */
    emit_mov(ctx->buf, 8, xop_mem(x86_mem_rax(8)), xop_reg(R_EDX));

    emit_lea(ctx->buf, R_EDX, x86_mem_rbp(-(i32) ctx->save_area_off)); /* reg_save_area */
    emit_mov(ctx->buf, 8, xop_mem(x86_mem_rax(16)), xop_reg(R_EDX));
}

/* va_arg: GP-then-overflow walk; all slots are 8 bytes and gp_offset is a multiple of 8 below 48,
 * so a signed compare is exact. */
static void lower_va_arg(IrInstr *in, CodegenCtx *ctx)
{
    ByteBuf *b = ctx->buf;
    X86Mem ap = load_ptr(ctx, in->ops[0]); /* ap -> %rax */

    emit_mov(b, 4, xop_reg(R_EDX), xop_mem(ap)); /* gp = *(u32*)(ap+0) */
    emit_binop_rhs(b, 4, &cmp_spec, R_EDX, xop_imm(48));

    size_t jge_field = emit_jcc_pending(b, CC_GE); /* gp >= 48 -> overflow arm */

    /* GP arm: src = regs + gp; ap.gp_offset = gp + 8 */
    emit_mov(b, 8, xop_reg(R_ECX), xop_mem(x86_mem_rax(16)));
    emit_reg_reg(b, arith_specs[OP_ADD].mem, R_ECX, R_EDX); /* rcx = regs + gp */
    emit_binop_rhs(b, 4, &arith_specs[OP_ADD], R_EDX, xop_imm(8));
    emit_mov(b, 4, xop_mem(ap), xop_reg(R_EDX));
    size_t ovf_jmp_field = emit_jmp_pending(b);

    /* Overflow arm: src reads the saved old ovf; ap.overflow += 8 */
    size_t ovf_arm = bytebuf_len(b);
    emit_mov(b, 8, xop_reg(R_ECX), xop_mem(x86_mem_rax(8)));
    emit_mov(b, 8, xop_reg(R_R8), xop_reg(R_ECX));
    emit_binop_rhs(b, 8, &arith_specs[OP_ADD], R_ECX, xop_imm(8));
    emit_mov(b, 8, xop_mem(x86_mem_rax(8)), xop_reg(R_ECX));
    emit_mov(b, 8, xop_reg(R_ECX), xop_reg(R_R8));

    /* done: raw = *(u64*)src, stored to the width-8 result vreg */
    size_t done = bytebuf_len(b);
    patch_rel32(b, jge_field, ovf_arm);
    patch_rel32(b, ovf_jmp_field, done);

    emit_mov(b, 8, xop_reg(R_EDX), xop_mem(x86_mem_rcx(0)));
    emit_mov(b, 8, xop_vreg(in->result), xop_reg(R_EDX));
}

/* va_end: no-op (SysV has no va_end action), kept in the IR for symmetry and future va_copy. */
static void lower_va_end(IrInstr *in, CodegenCtx *ctx)
{
    (void) in;
    (void) ctx;
}

static void lower_ret(IrInstr *in, CodegenCtx *ctx)
{
    if (in->nops > 0)
    {
        IrOperand val = in->ops[0];
        u8 w = operand_width(ctx, val);
        emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, val, R_ECX));
    }
    else
    {
        emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_imm(0));
    }
    bytebuf_append(ctx->buf, X86_LEAVE);
    bytebuf_append(ctx->buf, X86_RET);
}

static void lower_br(IrInstr *in, CodegenCtx *ctx)
{
    emit_jmp(ctx->buf, in->extra.br.target_label, ctx->block_patches, ctx->arena);
}

static void lower_brcond(IrInstr *in, CodegenCtx *ctx)
{
    emit_mov(ctx->buf, 4, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_test_eax_eax(ctx->buf);
    emit_jcc(ctx->buf, CC_E, in->extra.brcond.false_label, ctx->block_patches, ctx->arena);
    emit_jmp(ctx->buf, in->extra.brcond.true_label, ctx->block_patches, ctx->arena);
}

/* Jump table only when the case range stays small (range+1 ≤ JT_MAX_RANGE entries); sparse switches
 * use the compare-chain. */
#define JT_MAX_RANGE 256

/* Load the control value into %rax at its exact 64-bit semantic value (sign/zero-extend narrow
 * sources). */
static void emit_switch_control(CodegenCtx *ctx, IrOperand src)
{
    u8 w = operand_width(ctx, src);
    if (src.is_imm)
    {
        emit_mov(ctx->buf, 8, xop_reg(R_EAX), xop_imm(src.u.imm));
        return;
    }
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, src, R_EAX));
    if (w < 8 && ir_vreg_signed(ctx->mod, src.u.vreg))
    {
        emit_movsx(ctx->buf, w, 8, R_EAX, xop_reg(R_EAX));
    }
    else if (w < 4)
    {
        emit_movzx(ctx->buf, w, 8, R_EAX, xop_reg(R_EAX));
    }
    /* w==4 unsigned: mov %eax already zero-extends into %rax */
}

/* Bounds-check %rax to [min,max], subtract min, then jump through the full-range table (gaps →
 * default). */
static void emit_switch_table(CodegenCtx *ctx, IrSwitchCase *cases, u32 n, i64 min, i64 max,
                              const char *default_label)
{
    /* The wrap-around subtraction is exact because the bounds checks give index < 2^63; negative
     * ranges need signed ordering. */
    u8 below_cc = min < 0 ? CC_L : CC_B;
    u8 above_cc = min < 0 ? CC_G : CC_A;
    u64 range = (u64) max - (u64) min;

    emit_mov(ctx->buf, 8, xop_reg(R_ECX), xop_imm(min));
    emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_ECX); /* cmp %rax, %rcx */
    emit_jcc(ctx->buf, below_cc, default_label, ctx->block_patches, ctx->arena);
    emit_mov(ctx->buf, 8, xop_reg(R_EDX), xop_imm(max));
    emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_EDX); /* cmp %rax, %rdx */
    emit_jcc(ctx->buf, above_cc, default_label, ctx->block_patches, ctx->arena);
    emit_reg_reg(ctx->buf, arith_specs[OP_SUB].mem, R_EAX, R_ECX); /* sub %rax, %rcx */

    /* lea %rdx,[rip+disp32] patched once the table offset is known; RIP-relative entries need no
     * relocations. */
    emit_lea(ctx->buf, R_EDX, (X86Mem) {.base = NO_REG, .index = NO_REG, .scale = 1, .disp = 0});
    size_t disp_field_off = bytebuf_len(ctx->buf) - 4;

    emit_mov(ctx->buf, 8, xop_reg(R_EAX),
             xop_mem((X86Mem) {.base = R_EDX, .index = R_EAX, .scale = 8, .disp = 0}));
    emit_reg_reg(ctx->buf, arith_specs[OP_ADD].mem, R_EAX, R_EDX); /* add %rax, %rdx */
    emit_jmp_reg(ctx->buf, R_EAX);

    size_t nentries = (size_t) range + 1;
    SwitchTableRec *rec = arena_alloc(ctx->arena, sizeof(SwitchTableRec), sizeof(void *));
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

static void lower_switch(IrInstr *in, CodegenCtx *ctx)
{
    u32 n = in->extra.sw.ncases;
    IrSwitchCase *cases = in->extra.sw.cases;
    const char *default_label = in->extra.sw.default_label;

    emit_switch_control(ctx, in->ops[0]);

    if (n >= 2)
    {
        i64 min = cases[0].val, max = cases[0].val;
        for (u32 i = 1; i < n; i++)
        {
            min = MIN(cases[i].val, min);
            max = MAX(cases[i].val, max);
        }
        /* u64 wrap subtraction gives the true range for |range| < 2^63. */
        u64 range = (u64) max - (u64) min;
        if (range <= JT_MAX_RANGE)
        {
            emit_switch_table(ctx, cases, n, min, max, default_label);
            return;
        }
    }

    /* Compare-chain: test %rax against each case in turn, else default. */
    for (u32 i = 0; i < n; i++)
    {
        if (fits_i32(cases[i].val))
        {
            emit_binop_rhs(ctx->buf, 8, &cmp_spec, R_EAX, xop_imm(cases[i].val));
        }
        else
        {
            emit_mov(ctx->buf, 8, xop_reg(R_ECX), xop_imm(cases[i].val));
            emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_ECX); /* cmp %rax, %rcx */
        }
        emit_jcc(ctx->buf, CC_E, cases[i].label, ctx->block_patches, ctx->arena);
    }
    emit_jmp(ctx->buf, default_label, ctx->block_patches, ctx->arena);
}

static void lower_phi(IrInstr *in, CodegenCtx *ctx)
{
    (void) in;
    (void) ctx; /* lowered into copies in predecessor blocks */
}

static void lower_unreachable(IrInstr *in, CodegenCtx *ctx)
{
    (void) in;
    emit_ud2(ctx->buf);
}

static void lower_trunc(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    X86Operand src = lowered_operand(ctx, in->ops[0], R_EAX);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), src);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void emit_movzx(ByteBuf *buf, u8 src_w, u8 dst_w, u8 dst_reg, X86Operand src)
{
    ASSERT(dst_w == 4 || dst_w == 8);
    if (src_w == 1)
    {
        bytebuf_append(buf, rex(dst_w == 8, dst_reg >= 8, false,
                                src.kind == XOP_MEM && reg_is_extended(src.u.mem.base)));
        bytebuf_append(buf, X86_TWO_BYTE_ESC);
        bytebuf_append(buf, X86_MOVZX_REG8);
    }
    else
    {
        bytebuf_append(buf, rex(dst_w == 8, dst_reg >= 8, false,
                                src.kind == XOP_MEM && reg_is_extended(src.u.mem.base)));
        bytebuf_append(buf, X86_TWO_BYTE_ESC);
        bytebuf_append(buf, X86_MOVZX_REG16);
    }
    if (src.kind == XOP_REG)
    {
        bytebuf_append(buf, modrm(3, dst_reg, src.u.reg));
    }
    else
    {
        emit_mem_operand(buf, dst_reg, src.u.mem);
    }
}

static void emit_movsx(ByteBuf *buf, u8 src_w, u8 dst_w, u8 dst_reg, X86Operand src)
{
    ASSERT(dst_w == 4 || dst_w == 8);
    if (src_w == 4)
    {
        ASSERT(dst_w == 8);
        bytebuf_append(buf, rex(true, dst_reg >= 8, false,
                                src.kind == XOP_MEM && reg_is_extended(src.u.mem.base)));
        bytebuf_append(buf, X86_MOVSXD_REG32);
    }
    else
    {
        bytebuf_append(buf, rex(dst_w == 8, dst_reg >= 8, false,
                                src.kind == XOP_MEM && reg_is_extended(src.u.mem.base)));
        bytebuf_append(buf, X86_TWO_BYTE_ESC);
        bytebuf_append(buf, src_w == 1 ? X86_MOVSX_REG8 : X86_MOVSX_REG16);
    }
    if (src.kind == XOP_REG)
    {
        bytebuf_append(buf, modrm(3, dst_reg, src.u.reg));
    }
    else
    {
        emit_mem_operand(buf, dst_reg, src.u.mem);
    }
}

static void lower_zext(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    X86Operand src = lowered_operand(ctx, in->ops[0], R_EAX);
    if (in->ops[0].is_imm)
    {
        emit_mov(ctx->buf, dw, xop_reg(R_EAX), src);
    }
    else
    {
        u8 sw = ctx->mod->widths[in->ops[0].u.vreg];
        emit_mov(ctx->buf, sw, xop_reg(R_EAX), src);
        /* A 4-byte source needs no MOVZX: writing EAX zero-extends to RAX, and 0F B7 only
         * zero-extends 16 bits. */
        if (sw < 4)
        {
            emit_movzx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
        }
    }
    emit_mov(ctx->buf, dw, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_sext(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    X86Operand src = lowered_operand(ctx, in->ops[0], R_EAX);
    if (in->ops[0].is_imm)
    {
        emit_mov(ctx->buf, dw, xop_reg(R_EAX), src);
    }
    else
    {
        u8 sw = ctx->mod->widths[in->ops[0].u.vreg];
        emit_mov(ctx->buf, sw, xop_reg(R_EAX), src);
        emit_movsx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
    }
    emit_mov(ctx->buf, dw, xop_vreg(in->result), xop_reg(R_EAX));
}

/* `mov reg, imm32sx` (C7 /0): the zero-extension-free encoding GNU ld relocates with R_X86_64_32S.
 */
static void emit_addr_mov_imm32(ByteBuf *buf, u8 reg)
{
    bytebuf_append(buf, rex(true, false, false, reg >= 8));
    bytebuf_append(buf, X86_MOV_RM_IMM32SX);
    bytebuf_append(buf, modrm(3, 0, reg));
}

static void emit_global_addr_to(ByteBuf *buf, u8 reg, u32 global_idx, Vec *patches, Arena *arena)
{
    emit_addr_mov_imm32(buf, reg);
    GlobalPatch *gp = arena_alloc(arena, sizeof(GlobalPatch), sizeof(void *));
    gp->offset = bytebuf_len(buf);
    gp->global_index = global_idx;
    vec_push(patches, gp);
    bytebuf_append_u32(buf, 0);
}

static void emit_global_addr(ByteBuf *buf, u32 global_idx, Vec *patches, Arena *arena)
{
    emit_global_addr_to(buf, R_EAX, global_idx, patches, arena);
}

static void emit_func_addr_to(ByteBuf *buf, u8 reg, const char *name, Vec *patches, Arena *arena)
{
    emit_addr_mov_imm32(buf, reg);
    FuncAddrPatch *fp = arena_alloc(arena, sizeof(FuncAddrPatch), sizeof(void *));
    fp->name = name;
    fp->offset = bytebuf_len(buf);
    vec_push(patches, fp);
    bytebuf_append_u32(buf, 0);
}

static void emit_lea(ByteBuf *buf, u8 dst_reg, X86Mem src)
{
    u8 rex_b = rex(true, dst_reg >= 8, reg_is_extended(src.index), reg_is_extended(src.base));
    bytebuf_append(buf, rex_b);
    bytebuf_append(buf, X86_LEA);
    emit_mem_operand(buf, dst_reg, src);
}

static X86Mem load_ptr(CodegenCtx *ctx, IrOperand ptr)
{
    if (ptr.is_global)
    {
        emit_global_addr(ctx->buf, ptr.u.global_index, ctx->global_patches, ctx->arena);
    }
    else if (ptr.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, ptr.u.func_name, ctx->func_patches, ctx->arena);
    }
    else
    {
        emit_mov(ctx->buf, 8, xop_reg(R_EAX), lowered_operand(ctx, ptr, R_EAX));
    }
    return (X86Mem) {.base = R_EAX, .index = NO_REG, .scale = 1, .disp = 0};
}

static void lower_load(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    X86Mem indirect = load_ptr(ctx, in->ops[0]);
    emit_mov(ctx->buf, w, xop_reg(R_EDX), xop_mem(indirect));
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EDX));
}

static void lower_store(IrInstr *in, CodegenCtx *ctx)
{
    u32 w = (u32) in->ops[2].u.imm;
    /* Materialize the value first (ECX) so a global-pointer destination can't clobber it. */
    X86Operand val = lowered_operand(ctx, in->ops[0], R_ECX);
    if (val.kind == XOP_MEM || val.kind == XOP_IMM)
    {
        emit_mov(ctx->buf, w, xop_reg(R_EDX), val);
        val = xop_reg(R_EDX);
    }
    X86Mem indirect = load_ptr(ctx, in->ops[1]);
    emit_mov(ctx->buf, w, xop_mem(indirect), val);
}

static void lower_gep(IrInstr *in, CodegenCtx *ctx)
{
    i64 stride = in->ops[2].u.imm;

    /* Member-access fast path: index=1 folds base + stride into a plain displacement. */
    if (in->ops[1].is_imm && in->ops[1].u.imm == 1)
    {
        X86Mem base = load_ptr(ctx, in->ops[0]);
        X86Mem m = {.base = base.base, .index = NO_REG, .scale = 1, .disp = (i32) stride};
        emit_lea(ctx->buf, R_EDX, m);
        emit_mov(ctx->buf, 8, xop_vreg(in->result), xop_reg(R_EDX));
        return;
    }

    if (stride == 1 || stride == 2 || stride == 4 || stride == 8)
    {
        X86Mem base = load_ptr(ctx, in->ops[0]);
        emit_mov(ctx->buf, 8, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
        X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = (u8) stride, .disp = 0};
        emit_lea(ctx->buf, R_EDX, scaled);
        emit_mov(ctx->buf, 8, xop_vreg(in->result), xop_reg(R_EDX));
        return;
    }

    /* General stride: index *= stride, then lea (base, index, 1). */
    X86Mem base = load_ptr(ctx, in->ops[0]);
    emit_mov(ctx->buf, 8, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    emit_imul_imm(ctx->buf, 8, R_ECX, stride);
    X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = 1, .disp = 0};
    emit_lea(ctx->buf, R_EDX, scaled);
    emit_mov(ctx->buf, 8, xop_vreg(in->result), xop_reg(R_EDX));
}

static void lower_alloca(IrInstr *in, CodegenCtx *ctx)
{
    i64 aligned = (in->ops[0].u.imm + 15) & ~15;
    emit_binop_rhs(ctx->buf, 8, &arith_specs[OP_SUB], R_ESP, xop_imm(aligned));
    emit_mov(ctx->buf, 8, xop_reg(R_EDX), xop_reg(R_ESP));
    emit_mov(ctx->buf, 8, xop_vreg(in->result), xop_reg(R_EDX));
}

static void lower_memcpy(IrInstr *in, CodegenCtx *ctx)
{
    (void) load_ptr(ctx, in->ops[0]); /* dst address -> %rax */
    emit_mov(ctx->buf, 8, xop_reg(R_EDI), xop_reg(R_EAX));
    (void) load_ptr(ctx, in->ops[1]); /* src address -> %rax */
    emit_mov(ctx->buf, 8, xop_reg(R_ESI), xop_reg(R_EAX));
    emit_mov(ctx->buf, 8, xop_reg(R_ECX), xop_imm(in->ops[2].u.imm));
    bytebuf_append(ctx->buf, X86_REP);
    bytebuf_append(ctx->buf, X86_MOVSB);
}

/* True for the ten icmp predicates; designated initializers keep this order-independent from
 * IR_OPCODES. */
static const bool is_icmp_op_table[OP_ICMP_SGE + 1] = {
    [OP_ICMP_EQ] = true,  [OP_ICMP_NE] = true,  [OP_ICMP_ULT] = true, [OP_ICMP_ULE] = true,
    [OP_ICMP_UGT] = true, [OP_ICMP_UGE] = true, [OP_ICMP_SLT] = true, [OP_ICMP_SLE] = true,
    [OP_ICMP_SGT] = true, [OP_ICMP_SGE] = true,
};

/* arithmetic/compare have no imm64 form: an imm RHS must fit a sign-extended imm32 */
static bool is_imm_rhs_op(IrOpcode op)
{
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_AND || op == OP_OR ||
           op == OP_XOR || (op <= OP_ICMP_SGE && is_icmp_op_table[op]);
}

static bool instr_has_bad_imm(IrInstr *in, CodegenCtx *ctx)
{
    return is_imm_rhs_op(in->opcode) && vreg_width(ctx, in->result) == 8 && in->ops[1].is_imm &&
           !fits_i32(in->ops[1].u.imm);
}

static void lower_instr(IrInstr *in, CodegenCtx *ctx)
{
    if (instr_has_bad_imm(in, ctx))
    {
        codegen_error(ctx,
                      "%s: 64-bit immediate RHS outside signed i32 range "
                      "(no imm64 form for arithmetic/compare)",
                      ir_opcode_name(in->opcode));
        emit_ud2(ctx->buf);
        return;
    }
    LowerFn fn = lower_fns[in->opcode];
    if (!fn)
    {
        codegen_error(ctx, "unsupported opcode %s", ir_opcode_name(in->opcode));
        emit_ud2(ctx->buf);
        return;
    }
    fn(in, ctx);
}

/* ------------------------------------------------------------------ */
/* IrFunction emission                                                   */
/* ------------------------------------------------------------------ */

static void scan_vreg(u32 *max, u32 vreg)
{
    if (vreg != NO_VREG && vreg > *max)
    {
        *max = vreg;
    }
}

static void scan_operand(u32 *max, IrOperand op)
{
    if (op.is_imm || op.is_global || op.is_func)
    {
        return;
    }
    scan_vreg(max, op.u.vreg);
}

static void scan_instr_vregs(u32 *max, IrInstr *in)
{
    scan_vreg(max, in->result);
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        scan_operand(max, in->ops[oi]);
    }
    if (in->opcode == OP_CALL)
    {
        if (in->extra.call.is_indirect)
        {
            scan_operand(max, in->extra.call.callee);
        }
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            scan_operand(max, in->extra.call.args[a]);
        }
    }
}

static FrameInfo frame_plan(IrFunction *f)
{
    u32 max_vreg = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            scan_instr_vregs(&max_vreg, (IrInstr *) vec_get(blk->instrs, ii));
        }
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        scan_vreg(&max_vreg, ((IrParam *) vec_get(f->params, i))->vreg);
    }

    FrameInfo fr = {0};
    u32 total = (max_vreg + 1) * 8;
    if (f->is_variadic)
    {
        /* SysV register save area below the vreg slots (48 GP + 128-byte xmm reservation); the
         * prologue spills into it. */
        fr.save_area_off = total + 176;
        total = fr.save_area_off;
    }
    fr.frame_size = (total + 15) & ~15u;
    return fr;
}

/* Move incoming args into their param vreg slots; stack args (7+) sit at 16+(i-6)*8(%rbp), routed
 * through %eax. */
static void emit_param_shuffle(ByteBuf *buf, IrFunction *f, IrModule *mod)
{
    size_t nparams = vec_size(f->params);
    size_t n_reg = MIN(nparams, 6);
    for (size_t i = 0; i < n_reg; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        u8 w = mod->widths[p->vreg];
        emit_mov(buf, w, xop_vreg(p->vreg), xop_reg(abi_arg_regs[i]));
    }
    for (size_t i = n_reg; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        u8 w = mod->widths[p->vreg];
        emit_mov(buf, w, xop_reg(R_EAX), xop_mem(x86_mem_rbp(16 + (i32) ((i - 6) * 8))));
        emit_mov(buf, w, xop_vreg(p->vreg), xop_reg(R_EAX));
    }
}

static void emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, FrameInfo *fr)
{
    bytebuf_append(buf, X86_PUSH_RBP);
    emit_mov(buf, 8, xop_reg(R_EBP), xop_reg(R_ESP));

    /* The frame always holds at least one 8-byte vreg slot, so it's ≥ 16. */
    emit_binop_rhs(buf, 8, &arith_specs[OP_SUB], R_ESP, xop_imm(fr->frame_size));

    /* Spill the six GP arg registers here, before emit_param_shuffle's %eax scratch for stack args
     * (mov reg→[mem] never clobbers the source). */
    if (f->is_variadic)
    {
        for (size_t i = 0; i < 6; i++)
        {
            emit_mov(buf, 8, xop_mem(x86_mem_rbp(-(i32) fr->save_area_off + (i32) i * 8)),
                     xop_reg(abi_arg_regs[i]));
        }
    }

    emit_param_shuffle(buf, f, mod);
}

static bool is_terminator(IrOpcode op)
{
    return op == OP_RET || op == OP_UNREACHABLE || op == OP_BR || op == OP_BRCOND ||
           op == OP_SWITCH;
}

/* Each PHI entry becomes a copy in the named predecessor block. */
static void add_phi_copies(CodegenCtx *ctx, IrInstr *phi)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        IrPhiEntry *entry = &phi->extra.phi.entries[e];
        IrBlock *pred = strmap_get(ctx->label_to_block, entry->label);
        ASSERT(pred != NULL && "phi entry names a real predecessor in this function");
        size_t pj = (size_t) u64map_get(ctx->block_to_index, (u64) (uintptr_t) pred);
        PhiCopy *pc = arena_alloc(ctx->arena, sizeof(PhiCopy), sizeof(void *));
        pc->src = entry->val;
        pc->dst_vreg = phi->result;
        vec_push(ctx->phi_copies[pj], pc);
    }
}

static void collect_phi_copies(IrFunction *f, CodegenCtx *ctx)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI)
            {
                continue;
            }
            add_phi_copies(ctx, in);
        }
    }
}

/* Lower a PHI source into vreg `dst`; global/func addresses materialize as 8-byte addresses, else
 * at the value's own width. */
static void emit_phi_copy(CodegenCtx *ctx, IrOperand src, u32 dst_vreg)
{
    if (src.is_global)
    {
        emit_global_addr(ctx->buf, src.u.global_index, ctx->global_patches, ctx->arena);
        emit_mov(ctx->buf, 8, xop_vreg(dst_vreg), xop_reg(R_EAX));
        return;
    }
    if (src.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, src.u.func_name, ctx->func_patches, ctx->arena);
        emit_mov(ctx->buf, 8, xop_vreg(dst_vreg), xop_reg(R_EAX));
        return;
    }
    u8 pw = operand_width(ctx, src);
    emit_mov(ctx->buf, pw, xop_reg(R_EAX), lowered_operand(ctx, src, R_ECX));
    emit_mov(ctx->buf, pw, xop_vreg(dst_vreg), xop_reg(R_EAX));
}

static void emit_block(IrBlock *blk, size_t bi, CodegenCtx *ctx)
{
    ctx->block_offsets[bi] = bytebuf_len(ctx->buf);
    size_t ninstr = vec_size(blk->instrs);

    /* Non-terminator instructions first. */
    size_t ii = 0;
    for (; ii < ninstr; ii++)
    {
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
        if (is_terminator(in->opcode))
        {
            break;
        }
        lower_instr(in, ctx);
    }

    /* PHI copies must run in the predecessor, just before its terminator. */
    if (ii < ninstr)
    {
        size_t npc = vec_size(ctx->phi_copies[bi]);
        for (size_t pi = 0; pi < npc; pi++)
        {
            PhiCopy *pc = (PhiCopy *) vec_get(ctx->phi_copies[bi], pi);
            emit_phi_copy(ctx, pc->src, pc->dst_vreg);
        }
        lower_instr((IrInstr *) vec_get(blk->instrs, ii), ctx);
    }
}

static void resolve_block_patches(CodegenCtx *ctx)
{
    size_t npatches = vec_size(ctx->block_patches);
    for (size_t pi = 0; pi < npatches; pi++)
    {
        PatchSite *site = (PatchSite *) vec_get(ctx->block_patches, pi);
        IrBlock *target = strmap_get(ctx->label_to_block, site->target);
        ASSERT(target != NULL && "branch target names a block the IR builder created");
        size_t ti = (size_t) u64map_get(ctx->block_to_index, (u64) (uintptr_t) target);
        patch_rel32(ctx->buf, site->offset, ctx->block_offsets[ti]);
    }
}

static void emit_func_mc(IrFunction *f, CodegenFunc *cf, IrModule *mod, Arena *arena)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);
    Vec *global_patches = vec_new(arena);
    Vec *func_patches = vec_new(arena);

    FrameInfo fr = frame_plan(f);
    emit_prologue(buf, f, mod, &fr);

    size_t nblocks = vec_size(f->blocks);
    Vec **phi_copies = arena_alloc(arena, nblocks * sizeof(Vec *), sizeof(void *));
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        phi_copies[bi] = vec_new(arena);
    }

    CodegenCtx ctx = {
        .buf = buf,
        .func = f,
        .mod = mod,
        .arena = arena,
        .phi_copies = phi_copies,
        .label_to_block = strmap_new(arena),
        .block_to_index = u64map_new(arena),
        .block_offsets = arena_alloc(arena, nblocks * sizeof(size_t), sizeof(size_t)),
        .patches = patches,
        .block_patches = block_patches,
        .global_patches = global_patches,
        .func_patches = func_patches,
        .switch_tables = vec_new(arena),
        .save_area_off = fr.save_area_off,
    };

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, bi);
        strmap_set(ctx.label_to_block, blk->label, blk);
        u64map_set(ctx.block_to_index, (u64) (uintptr_t) blk, (void *) bi);
    }

    collect_phi_copies(f, &ctx);

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        emit_block((IrBlock *) vec_get(f->blocks, bi), bi, &ctx);
    }

    /* Append jump tables after the function body; entries store target−table_base so runtime
     * resolves table_base + entry, with no relocations. */
    size_t nst = vec_size(ctx.switch_tables);
    if (nst > 0)
    {
        bytebuf_align(buf, 8);
    }
    for (size_t t = 0; t < nst; t++)
    {
        SwitchTableRec *rec = (SwitchTableRec *) vec_get(ctx.switch_tables, t);
        size_t table_off = bytebuf_len(buf);
        for (u32 i = 0; i < rec->nentries; i++)
        {
            IrBlock *target = strmap_get(ctx.label_to_block, rec->targets[i]);
            ASSERT(target != NULL && "switch case targets a real block");
            size_t ti = (size_t) u64map_get(ctx.block_to_index, (u64) (uintptr_t) target);
            bytebuf_append_u64(buf, (u64) ((i64) ctx.block_offsets[ti] - (i64) table_off));
        }
        patch_rel32(buf, rec->disp_field_off, table_off);
    }

    resolve_block_patches(&ctx);

    cf->name = f->name;
    cf->bytes = buf;
    cf->offset = 0;
    cf->patches = patches;
    cf->global_patches = global_patches;
    cf->func_patches = func_patches;
    cf->is_static = f->is_static;
}

static CodegenFunc *find_codegen_func(CodegenModule *cm, const char *name)
{
    size_t n = vec_size(cm->funcs);
    for (size_t i = 0; i < n; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        if (strcmp(cf->name, name) == 0)
        {
            return cf;
        }
    }
    return NULL;
}

CodegenModule *codegen_ir_to_machine(IrModule *ir, Arena *arena)
{
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->funcs = vec_new(arena);
    cm->globals = ir->globals;
    cm->extern_calls = vec_new(arena);

    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        emit_func_mc(f, cf, ir, arena);
        vec_push(cm->funcs, cf);
    }

    /* Compute function offsets in .text */
    size_t function_offset = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        cf->offset = function_offset;
        function_offset += bytebuf_len(cf->bytes);
    }

    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            PatchSite *site = (PatchSite *) vec_get(cf->patches, pi);
            CodegenFunc *target = find_codegen_func(cm, site->target);
            if (!target)
            {
                /* Call to a declaration-only extern: leave rel32 as 0 and let elf.c emit an
                 * SHN_UNDEF symbol + R_X86_64_PLT32 relocation. */
                ExternCall *ec = arena_alloc(arena, sizeof(ExternCall), sizeof(void *));
                ec->name = site->target;
                ec->text_offset = cf->offset + site->offset;
                vec_push(cm->extern_calls, ec);
                continue;
            }
            i32 rel = (i32) (target->offset - (cf->offset + site->offset + 4));
            bytebuf_poke_u32(cf->bytes, site->offset, (u32) rel);
        }
    }

    return cm;
}
