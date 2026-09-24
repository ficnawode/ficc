#ifndef FICC_X86_EMIT_H
#define FICC_X86_EMIT_H

#include "util/arena.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "util/vec.h"

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

/* DWARF register numbers (SysV psABI §3.6.2) for the `.debug_info` parameter
   locations and `.eh_frame` callee-saved rules: GPRs are remapped from X86Reg
   order, XMM lanes are 17 + lane. */
u8 x86_dwarf_gpr_number(u8 reg);
u8 x86_dwarf_xmm_number(u8 lane);

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

typedef enum
{
    W_BYTE = 1,
    W_WORD = 2,
    W_DWORD = 4,
    W_QWORD = 8,
    W_LD = 16,
} Width;

/* Opcode/prefix bytes; *_BASE names take a register nibble in the low 3 bits. */
typedef enum
{
    X86_TWO_BYTE_ESC = 0x0F,
    X86_UD2 = 0x0B,
    X86_OPERAND_SIZE = 0x66,
    X86_REX_B = 0x41,
    X86_REX_W = 0x48,
    X86_REP = 0xF3,

    X86_SSE_F2 = 0xF2,     /* cvtsd2ss/cvttsd2si/movsd/addsd mandatory prefix */
    X86_SSE_66 = 0x66,     /* movd/movq to/from xmm */
    X86_SSE_MOV = 0x10,    /* movss (F3) / movsd (F2), ->reg=load, ->mem=store */
    X86_SSE_MOV_RM = 0x11, /* movss/movsd r/m <- xmm: store form (MOV + 1) */
    X86_SSE_CVTSI2 = 0x2A, /* cvtsi2sd (F2, REX.W for 64) / cvtsi2ss (F3) */
    X86_SSE_CVTT = 0x2C,   /* cvttsd2si (F2, REX.W for 64) / cvttss2si (F3) */
    X86_SSE_CVTS2S = 0x5A, /* cvtsd2ss (F2) / cvtss2sd (F3) */
    X86_SSE_ADD = 0x58,    /* addsd (F2) / addss (F3) */
    X86_SSE_MUL = 0x59,    /* mulsd (F2) / mulss (F3) */
    X86_SSE_SUB = 0x5C,    /* subsd (F2) / subss (F3) */
    X86_SSE_DIV = 0x5E,    /* divsd (F2) / divss (F3) */
    X86_SSE_XOR = 0x57,    /* xorpd (66) / xorps (no prefix) */
    X86_SSE_UCOMIS = 0x2E, /* ucomisd (66) / ucomiss (no prefix) */
    X86_SSE_MOVD = 0x6E,   /* movd/movq xmm, r/m (66 prefix, REX.W for 64) */
    X86_BIT_BASE = 0xBA,   /* 0F BA /digit ib: bt/bts/btr on r/m64 */

    X86_PUSH_R_BASE = 0x50,
    X86_POP_R_BASE = 0x58,
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

    X86_INC_DEC_RM8 = 0xFE,
    X86_INC_DEC_RM32 = 0xFF,

    X86_SHIFT_RM8_CL = 0xD2,
    X86_SHIFT_RM32_CL = 0xD3,
    X86_SHIFT_RM8_IMM = 0xC0,
    X86_SHIFT_RM32_IMM = 0xC1,

    X86_CBW_CWDE_CDQE = 0x98,
    X86_CWD_CDQ_CQO = 0x99,

    X86_XOR_REG_RM = 0x31,
    X86_XOR_RM8_REG8 = 0x30,
    X86_TEST_REG_RM = 0x85,
    X86_TEST_RM8_REG8 = 0x84,

    X86_MOVZX_REG8 = 0xB6,
    X86_MOVZX_REG16 = 0xB7,
    X86_MOVSX_REG8 = 0xBE,
    X86_MOVSX_REG16 = 0xBF,
    X86_MOVSXD_REG32 = 0x63,

    X86_JCC_BASE = 0x80,
    X86_SETCC_BASE = 0x90,
} X86Opcode;

#define NO_REG 0xFF

/* SSE scratch: only xmm0/xmm1 are ever used as values. */
#define R_XMM0 0
#define R_XMM1 1

/* Mandatory prefixes: float lanes use F3, double lanes F2. */
#define MF_FLOAT 0xF3
#define MF_DOUBLE 0xF2

/* The mandatory prefix for a floating-point lane width (4 → float, else double). */
#define MF_OF(w) ((w) == 4 ? MF_FLOAT : MF_DOUBLE)

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

static inline bool reg_is_extended(u8 reg)
{
    return reg != NO_REG && reg >= 8;
}

static inline X86Operand xop_imm(i64 val)
{
    X86Operand o;
    o.kind = XOP_IMM;
    o.u.imm = val;
    return o;
}

static inline X86Operand xop_reg(u8 reg)
{
    X86Operand o;
    o.kind = XOP_REG;
    o.u.reg = reg;
    return o;
}

static inline X86Operand xop_mem(X86Mem m)
{
    X86Operand o;
    o.kind = XOP_MEM;
    o.u.mem = m;
    return o;
}

static inline X86Mem x86_mem_rbp(i32 disp)
{
    X86Mem m;
    m.base = R_EBP;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static inline X86Mem x86_mem_rsp(i32 disp)
{
    X86Mem m;
    m.base = R_ESP;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static inline X86Mem x86_mem_rax(i32 disp)
{
    X86Mem m;
    m.base = R_EAX;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static inline X86Mem x86_mem_rcx(i32 disp)
{
    X86Mem m;
    m.base = R_ECX;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

static inline X86Mem x86_mem_r11(i32 disp)
{
    X86Mem m;
    m.base = R_R11;
    m.index = NO_REG;
    m.scale = 1;
    m.disp = disp;
    return m;
}

/* x86 encoding recipe for a binary arithmetic/logic operation. */
typedef struct
{
    u8 mem;       /* reg op= r/m opcode */
    u8 imm8;      /* reg op= imm8 opcode (0x83, or 0x6B for imul) */
    u8 imm32;     /* reg op= imm32 opcode (0x81, or 0x69 for imul) */
    u8 digit;     /* /digit for all forms */
    bool mem_0f;  /* true if the mem opcode needs a 0x0F prefix */
    bool imm_dst; /* imm form encodes the destination in the reg field (imul) */
} ArithSpec;

extern const ArithSpec arith_specs[];
extern const ArithSpec cmp_spec;
extern const u8 unary_digit[];
extern const u8 shift_digit[];
extern const u8 icmp_cc[];

/* Patch record for a global-data reference; elf.c reads these to emit R_X86_64_32 relocations. */
typedef struct
{
    size_t offset;    /* byte offset of the immediate within the function's bytebuf */
    u32 global_index; /* index into IrModule globals */
} GlobalPatch;

/* Taking a function's address (`&f`/designator): `mov $f, imm32sx` with an R_X86_64_32S relocation.
 */
typedef struct
{
    const char *name;
    size_t offset; /* byte offset of the immediate within the function's bytebuf */
} FuncAddrPatch;

/* A rel32 field patched later; `target` is a function name (calls) or block label (jcc/jmp). */
typedef struct
{
    size_t offset;
    const char *target;
} PatchSite;

/* Named `jcc`/`jmp`/`call` sites record their rel32 field plus target in a patch vector for
 * a later resolution pass; the `_pending` forms instead return the field offset so
 * intra-function branches can be poked directly. */

u8 modrm(u8 mod, u8 reg, u8 rm);
u8 rex(bool w, bool r, bool x, bool b);
void emit_mem_operand(ByteBuf *buf, u8 reg, X86Mem m);

/* width 1 uses the byte mov; width 2|4|8 the scalar form. */
void emit_mov(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src);
void emit_mov_byte(ByteBuf *buf, X86Operand dst, X86Operand src);
void emit_mov_scalar(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src);

/* REX.W reg,reg form; operand regs must be < 8. */
void emit_reg_reg(ByteBuf *buf, u8 opcode, u8 dst_reg, u8 src_reg);
void emit_binop_rhs(ByteBuf *buf, u8 width, const ArithSpec *s, u8 dst_reg, X86Operand rhs);

void emit_unary(ByteBuf *buf, u8 width, u8 reg, u8 digit);
void emit_inc_dec(ByteBuf *buf, u8 width, u8 reg, bool dec);
void emit_shift_cl(ByteBuf *buf, u8 width, u8 reg, u8 digit);
void emit_shift_imm(ByteBuf *buf, u8 width, u8 reg, u8 digit, u8 imm);
void emit_cdq(ByteBuf *buf, u8 width, bool is_unsigned);
void emit_idiv(ByteBuf *buf, u8 width, u8 reg);
void emit_div(ByteBuf *buf, u8 width, u8 reg);
void emit_imul_imm(ByteBuf *buf, u8 width, u8 reg, i64 imm);

void emit_test_reg(ByteBuf *buf, u8 width, u8 reg);
void emit_xor_eax_eax(ByteBuf *buf);
void emit_xor_zero(ByteBuf *buf, u8 width, u8 reg);
void emit_setcc_reg(ByteBuf *buf, u8 cc, u8 reg);
void emit_setcc(ByteBuf *buf, u8 cc);
void emit_movzbl_al_eax(ByteBuf *buf);
void emit_ud2(ByteBuf *buf);

void emit_jcc(ByteBuf *buf, u8 cc, const char *target, Vec *patches, Arena *arena);
void emit_jmp(ByteBuf *buf, const char *target, Vec *patches, Arena *arena);
void emit_call(ByteBuf *buf, const char *target, Vec *patches, Arena *arena);
void emit_call_reg(ByteBuf *buf, u8 reg);
void emit_jmp_reg(ByteBuf *buf, u8 reg);
size_t emit_jcc_pending(ByteBuf *buf, u8 cc);
size_t emit_jmp_pending(ByteBuf *buf);
void patch_rel32(ByteBuf *buf, size_t field_off, size_t target);

void emit_sse_load(ByteBuf *buf, u8 mf, u8 xmm, X86Mem mem);
void emit_sse_store(ByteBuf *buf, u8 mf, X86Mem mem, u8 xmm);
void emit_mov16(ByteBuf *buf, X86Mem src, X86Mem dst);
void emit_mov16_store(ByteBuf *buf, X86Mem dst);
void emit_sse_cvt(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm);
void emit_cvtsi2fp(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_reg);
void emit_cvtts2i(ByteBuf *buf, u8 mf, u8 dst_reg, u8 src_xmm, bool to_64);
void emit_sse_ucomis(ByteBuf *buf, u8 width, u8 lhs_xmm, u8 rhs_xmm);
void emit_sse_add(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm);
void emit_sse_sub(ByteBuf *buf, u8 mf, u8 dst_xmm, u8 src_xmm);
void emit_movd_to_xmm(ByteBuf *buf, u8 dst_xmm, u8 src_reg, bool is64);
void emit_sse_op_mem(ByteBuf *buf, u8 mf, u8 op, u8 dst_xmm, X86Mem mem);
void emit_sse_op_reg(ByteBuf *buf, u8 mf, u8 op, u8 dst_xmm, u8 src_xmm);
void emit_sse_xor(ByteBuf *buf, u8 mand, u8 dst_xmm, u8 src_xmm);

void emit_movzx(ByteBuf *buf, u8 src_width, u8 dst_width, u8 dst_reg, X86Operand src);
void emit_movsx(ByteBuf *buf, u8 src_width, u8 dst_width, u8 dst_reg, X86Operand src);
void emit_lea(ByteBuf *buf, u8 dst_reg, X86Mem src);
void emit_push_reg(ByteBuf *buf, u8 reg);
void emit_pop_reg(ByteBuf *buf, u8 reg);
void emit_global_addr_to(ByteBuf *buf, u8 reg, u32 global_idx, Vec *patches, Arena *arena);
void emit_global_addr(ByteBuf *buf, u32 global_idx, Vec *patches, Arena *arena);
void emit_func_addr_to(ByteBuf *buf, u8 reg, const char *name, Vec *patches, Arena *arena);

#define GRP3_OPCODE(w) ((w) == 1 ? X86_GROUP3_RM8 : X86_GROUP3_RM32)
#define SHIFT_OPCODE(w) ((w) == 1 ? X86_SHIFT_RM8_CL : X86_SHIFT_RM32_CL)

#endif