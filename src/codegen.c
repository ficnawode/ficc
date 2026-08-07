#include "codegen.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Machine-code context                                                */
/* ------------------------------------------------------------------ */

/* Patch records: a 4-byte PC-relative displacement at `offset` (within the
   function's bytes) that must be resolved to the label `target` later.
   Block labels are resolved during emit_func_mc, function names after
   layout. */
typedef struct
{
    size_t offset;
    const char *target;
} Patch;

/* One copy emitted in a predecessor block for each PHI entry. */
typedef struct
{
    Operand src;
    u32 dst_vreg;
} PhiCopy;

/* Per-function frame layout. Vreg i lives at slot (i+1)*8 below %rbp. */
typedef struct
{
    u32 n_vregs;
    u32 frame_size; /* rounded up to 16 for ABI alignment */
} FrameInfo;

typedef struct CodegenCtx CodegenCtx;
struct CodegenCtx
{
    ByteBuf *buf;
    Function *func;
    Module *mod;     /* for the per-vreg width table */
    Arena *arena;
    Vec **phi_copies;        /* per-block Vec<PhiCopy*>, indexed by block index */
    StrMap *label_to_block;  /* block label -> Block* */
    U64Map *block_to_index;  /* Block* -> block index */
    size_t *block_offsets;   /* per-block offset within the function bytes */
    Vec *patches;            /* Vec<Patch*>, function calls */
    Vec *block_patches;      /* Vec<Patch*>, intra-function jumps */
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
    R_EAX, R_ECX, R_EDX, R_EBX,
    R_ESP, R_EBP, R_ESI, R_EDI,
    R_R8, R_R9, R_R10, R_R11,
    R_R12, R_R13, R_R14, R_R15
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

#define NO_REG 0xFF

typedef struct
{
    u8 base;   /* X86Reg or NO_REG (RIP-relative) */
    u8 index;  /* X86Reg or NO_REG */
    u8 scale;  /* 1, 2, 4 or 8 */
    i32 disp;
} X86Mem;

typedef enum
{
    XOP_IMM, /* sign-extended 32-bit immediate in the instruction stream */
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

/* Where an IR operand lives on the x86 side: immediates stay immediate,
   vregs live in their frame slot. */
static X86Operand xop_vreg(u32 vreg)
{
    return xop_mem(x86_mem_rbp(-(i32) ((vreg + 1) * 8)));
}

static X86Operand xop_from_operand(Operand op)
{
    if (op.is_imm)
    {
        return xop_imm(op.u.imm);
    }
    return xop_vreg(op.u.vreg);
}

/* ------------------------------------------------------------------ */
/* x86-64 encoding                                                     */
/* ------------------------------------------------------------------ */

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

/* Emit modrm + SIB + displacement for a memory operand, `reg` in the modrm
   reg field. REX prefix, if any, is emitted by the caller. */
static void emit_mem_operand(ByteBuf *buf, u8 reg, X86Mem m)
{
    if (m.base == NO_REG)
    {
        /* RIP-relative: mod=00, rm=101, disp32 */
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
        /* SIB: index=100 means "no index register" */
        u8 idx = m.index == NO_REG ? 4 : (m.index & 7);
        u8 scale = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
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

/* Generic mov between any two operands. `width` selects REX.W (8 = 64-bit);
   4 = 32-bit. */
static void emit_mov(ByteBuf *buf, u8 width, X86Operand dst, X86Operand src)
{
    if (src.kind == XOP_IMM)
    {
        /* mov $imm32, %reg */
        bytebuf_append(buf, rex(width == 8, false, false, dst.u.reg >= 8));
        bytebuf_append(buf, (u8) (0xB8 + (dst.u.reg & 7)));
        bytebuf_append_i32(buf, (i32) src.u.imm);
        return;
    }
    if (dst.kind == XOP_REG && src.kind == XOP_REG)
    {
        bytebuf_append(buf, rex(width == 8, src.u.reg >= 8, false, dst.u.reg >= 8));
        bytebuf_append(buf, 0x89);
        bytebuf_append(buf, modrm(3, src.u.reg, dst.u.reg));
        return;
    }
    /* Memory forms: 8B reg <- mem, 89 mem <- reg. */
    bool to_reg = dst.kind == XOP_REG;
    u8 reg = to_reg ? dst.u.reg : src.u.reg;
    X86Mem mem = to_reg ? src.u.mem : dst.u.mem;
    bytebuf_append(buf, rex(width == 8, reg >= 8, mem.index != NO_REG && mem.index >= 8,
                      mem.base != NO_REG && mem.base >= 8));
    bytebuf_append(buf, to_reg ? 0x8B : 0x89);
    emit_mem_operand(buf, reg, mem);
}

/* Binary operation encodings, indexed by IR opcode. `mem` is the load form
   (reg op= mem); `imm8`/`imm32` are the 0x83 /digit and 0x81 /digit (or
   imul's 6B/69) forms. */
typedef struct
{
    u8 mem;      /* load-form opcode (0x03 addl) */
    u8 imm8;     /* 0x83 (or 0x6B imul) */
    u8 imm32;    /* 0x81 (or 0x69 imul) */
    u8 digit;    /* /digit for all forms */
    bool mem_0f; /* mem form has a 0x0F prefix (imul) */
} ArithSpec;

static const ArithSpec arith_specs[] = {
    [OP_ADD] = {0x03, 0x83, 0x81, 0, false},
    [OP_SUB] = {0x2B, 0x83, 0x81, 5, false},
    [OP_MUL] = {0xAF, 0x6B, 0x69, 0, true},
    [OP_AND] = {0x23, 0x83, 0x81, 4, false},
    [OP_OR]  = {0x0B, 0x83, 0x81, 1, false},
    [OP_XOR] = {0x33, 0x83, 0x81, 6, false},
};

/* cmp: same shape as the arithmetic ops, /7. Not an IR opcode itself. */
static const ArithSpec cmp_spec = {0x3B, 0x83, 0x81, 7, false};

static const u8 unary_digit[OP_NOT + 1] = {[OP_NEG] = 3, [OP_NOT] = 2};
static const u8 shift_digit[OP_ASHR + 1] = {[OP_SHL] = 4, [OP_ASHR] = 7};

/* setcc/jcc condition code per icmp predicate, indexed by opcode. */
static const u8 icmp_cc[OP_ICMP_SGE + 1] = {
    [OP_ICMP_EQ]  = CC_E,
    [OP_ICMP_NE]  = CC_NE,
    [OP_ICMP_ULT] = CC_B,
    [OP_ICMP_ULE] = CC_BE,
    [OP_ICMP_UGT] = CC_A,
    [OP_ICMP_UGE] = CC_AE,
    [OP_ICMP_SLT] = CC_L,
    [OP_ICMP_SLE] = CC_LE,
    [OP_ICMP_SGT] = CC_G,
    [OP_ICMP_SGE] = CC_GE,
};

/* %reg op= rhs */
static void emit_binop_rhs(ByteBuf *buf, u8 width, const ArithSpec *s, u8 dst_reg, X86Operand rhs)
{
    if (rhs.kind == XOP_IMM)
    {
        i64 v = rhs.u.imm;
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
    bytebuf_append(buf, rex(width == 8, dst_reg >= 8, rhs.u.mem.index != NO_REG && rhs.u.mem.index >= 8,
                      rhs.u.mem.base != NO_REG && rhs.u.mem.base >= 8));
    if (s->mem_0f)
    {
        bytebuf_append(buf, 0x0F);
    }
    bytebuf_append(buf, s->mem);
    emit_mem_operand(buf, dst_reg, rhs.u.mem);
}

/* F7 /digit */
static void emit_unary(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, 0xF7);
    bytebuf_append(buf, modrm(3, digit, reg));
}

/* D3 /digit, count in %cl */
static void emit_shift_cl(ByteBuf *buf, u8 width, u8 reg, u8 digit)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, 0xD3);
    bytebuf_append(buf, modrm(3, digit, reg));
}

static void emit_cdq(ByteBuf *buf, u8 width)
{
    if (width == 8)
    {
        bytebuf_append(buf, 0x48); /* cqo */
    }
    bytebuf_append(buf, 0x99);
}

/* F7 /7: idiv %reg */
static void emit_idiv(ByteBuf *buf, u8 width, u8 reg)
{
    bytebuf_append(buf, rex(width == 8, false, false, reg >= 8));
    bytebuf_append(buf, 0xF7);
    bytebuf_append(buf, modrm(3, 7, reg));
}

static void emit_test_eax_eax(ByteBuf *buf)
{
    bytebuf_append(buf, 0x85);
    bytebuf_append(buf, modrm(3, 0, 0));
}

static void emit_xor_eax_eax(ByteBuf *buf)
{
    bytebuf_append(buf, 0x31);
    bytebuf_append(buf, modrm(3, 0, 0));
}

/* 0F 90+cc: setcc %al */
static void emit_setcc(ByteBuf *buf, u8 cc)
{
    bytebuf_append(buf, 0x0F);
    bytebuf_append(buf, (u8) (0x90 + cc));
    bytebuf_append(buf, modrm(3, 0, 0));
}

/* 0F B6: movzbl %al, %eax */
static void emit_movzbl_al_eax(ByteBuf *buf)
{
    bytebuf_append(buf, 0x0F);
    bytebuf_append(buf, 0xB6);
    bytebuf_append(buf, modrm(3, 0, 0));
}

static void emit_ud2(ByteBuf *buf)
{
    bytebuf_append(buf, 0x0F);
    bytebuf_append(buf, 0x0B);
}

/* 0F 80+cc rel32: jcc (placeholder, patched later) */
static void emit_jcc(ByteBuf *buf, u8 cc, const char *target, Vec *patches, Arena *arena)
{
    Patch *p = arena_alloc(arena, sizeof(Patch), sizeof(void *));
    p->target = target;
    bytebuf_append(buf, 0x0F);
    bytebuf_append(buf, (u8) (0x80 + cc));
    p->offset = bytebuf_len(buf);
    vec_push(patches, p);
    bytebuf_append_i32(buf, 0);
}

static void emit_jmp_placeholder(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    Patch *p = arena_alloc(arena, sizeof(Patch), sizeof(void *));
    p->target = target;
    bytebuf_append(buf, 0xE9);
    p->offset = bytebuf_len(buf);
    vec_push(patches, p);
    bytebuf_append_i32(buf, 0);
}

static void emit_call_placeholder(ByteBuf *buf, const char *target, Vec *patches, Arena *arena)
{
    Patch *p = arena_alloc(arena, sizeof(Patch), sizeof(void *));
    p->target = target;
    bytebuf_append(buf, 0xE8);
    p->offset = bytebuf_len(buf);
    vec_push(patches, p);
    bytebuf_append_i32(buf, 0);
}

/* ------------------------------------------------------------------ */
/* IR lowering                                                         */
/* ------------------------------------------------------------------ */

typedef void (*LowerFn)(Instr *in, CodegenCtx *ctx);

static void lower_binary(Instr *in, CodegenCtx *ctx);
static void lower_unary(Instr *in, CodegenCtx *ctx);
static void lower_shift(Instr *in, CodegenCtx *ctx);
static void lower_div(Instr *in, CodegenCtx *ctx);
static void lower_icmp(Instr *in, CodegenCtx *ctx);
static void lower_call(Instr *in, CodegenCtx *ctx);
static void lower_ret(Instr *in, CodegenCtx *ctx);
static void lower_br(Instr *in, CodegenCtx *ctx);
static void lower_brcond(Instr *in, CodegenCtx *ctx);
static void lower_phi(Instr *in, CodegenCtx *ctx);
static void lower_unreachable(Instr *in, CodegenCtx *ctx);

#define LOWER_ENTRIES(X)      \
    X(OP_RET, lower_ret)      \
    X(OP_ADD, lower_binary)   \
    X(OP_SUB, lower_binary)   \
    X(OP_MUL, lower_binary)   \
    X(OP_SDIV, lower_div)     \
    X(OP_SREM, lower_div)     \
    X(OP_AND, lower_binary)   \
    X(OP_OR, lower_binary)    \
    X(OP_XOR, lower_binary)   \
    X(OP_SHL, lower_shift)    \
    X(OP_ASHR, lower_shift)   \
    X(OP_NEG, lower_unary)    \
    X(OP_NOT, lower_unary)    \
    X(OP_ICMP_EQ, lower_icmp) \
    X(OP_ICMP_NE, lower_icmp) \
    X(OP_ICMP_ULT, lower_icmp) \
    X(OP_ICMP_ULE, lower_icmp) \
    X(OP_ICMP_UGT, lower_icmp) \
    X(OP_ICMP_UGE, lower_icmp) \
    X(OP_ICMP_SLT, lower_icmp) \
    X(OP_ICMP_SLE, lower_icmp) \
    X(OP_ICMP_SGT, lower_icmp) \
    X(OP_ICMP_SGE, lower_icmp) \
    X(OP_BR, lower_br)        \
    X(OP_BRCOND, lower_brcond) \
    X(OP_CALL, lower_call)    \
    X(OP_PHI, lower_phi)      \
    X(OP_UNREACHABLE, lower_unreachable)

/* Dispatch table indexed by opcode; unlisted opcodes are NULL and diagnosed
   in lower_instr rather than silently miscompiled. */
static const LowerFn lower_fns[] = {
#define LOWER_INIT(op, fn) [op] = fn,
    LOWER_ENTRIES(LOWER_INIT)
#undef LOWER_INIT
};

static u8 vreg_width(CodegenCtx *ctx, u32 vreg)
{
    return ctx->mod->widths[vreg];
}

static void lower_binary(Instr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    const ArithSpec *s = &arith_specs[in->opcode];
    X86Operand rhs = xop_from_operand(in->ops[1]);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    if (in->opcode == OP_AND && rhs.kind == XOP_IMM && rhs.u.imm == 0xFF)
    {
        emit_movzbl_al_eax(ctx->buf); /* andl $0xFF, %eax */
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

static void lower_unary(Instr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    emit_unary(ctx->buf, w, R_EAX, unary_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_shift(Instr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    emit_mov(ctx->buf, w, xop_reg(R_ECX), xop_from_operand(in->ops[1]));
    emit_shift_cl(ctx->buf, w, R_EAX, shift_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_div(Instr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    emit_cdq(ctx->buf, w);
    emit_mov(ctx->buf, w, xop_reg(R_ECX), xop_from_operand(in->ops[1]));
    emit_idiv(ctx->buf, w, R_ECX);
    if (in->opcode == OP_SREM)
    {
        emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_reg(R_EDX));
    }
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

static void lower_icmp(Instr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    emit_binop_rhs(ctx->buf, w, &cmp_spec, R_EAX, xop_from_operand(in->ops[1]));
    emit_setcc(ctx->buf, icmp_cc[in->opcode]);
    emit_movzbl_al_eax(ctx->buf);
    emit_mov(ctx->buf, w, xop_vreg(in->result), xop_reg(R_EAX));
}

/* System V AMD64 argument registers. */
static const u8 abi_arg_regs[6] = {R_EDI, R_ESI, R_EDX, R_ECX, R_R8, R_R9};

static void lower_call(Instr *in, CodegenCtx *ctx)
{
    u32 nargs = in->extra.call.nargs;
    u32 n_stack = (nargs > 6) ? (nargs - 6) : 0;
    u32 pad = (n_stack % 2) * 8;
    u32 total_stack = n_stack * 8 + pad;

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, 8, &arith_specs[OP_SUB], R_ESP, xop_imm(total_stack));
    }

    /* Stack args at 0(%rsp), 8(%rsp), ... */
    for (u32 i = 6; i < nargs; i++)
    {
        emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_from_operand(in->extra.call.args[i]));
        emit_mov(ctx->buf, 4, xop_mem(x86_mem_rsp((i32) (i - 6) * 8)), xop_reg(R_EAX));
    }

    /* Register args in reverse order so no argument is clobbered early. */
    u32 n_reg_args = nargs < 6 ? nargs : 6;
    for (i32 i = (i32) n_reg_args - 1; i >= 0; i--)
    {
        emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_from_operand(in->extra.call.args[i]));
        emit_mov(ctx->buf, 4, xop_reg(abi_arg_regs[i]), xop_reg(R_EAX));
    }

    emit_call_placeholder(ctx->buf, in->extra.call.name, ctx->patches, ctx->arena);

    if (in->result != NO_VREG)
    {
        emit_mov(ctx->buf, 4, xop_vreg(in->result), xop_reg(R_EAX));
    }

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, 8, &arith_specs[OP_ADD], R_ESP, xop_imm(total_stack));
    }
}

static void lower_ret(Instr *in, CodegenCtx *ctx)
{
    if (in->nops > 0)
    {
        emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    }
    else
    {
        emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_imm(0));
    }
    bytebuf_append(ctx->buf, 0xC9); /* leave */
    bytebuf_append(ctx->buf, 0xC3); /* ret */
}

static void lower_br(Instr *in, CodegenCtx *ctx)
{
    emit_jmp_placeholder(ctx->buf, in->extra.br.target_label, ctx->block_patches, ctx->arena);
}

static void lower_brcond(Instr *in, CodegenCtx *ctx)
{
    emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_from_operand(in->ops[0]));
    emit_test_eax_eax(ctx->buf);
    emit_jcc(ctx->buf, CC_E, in->extra.brcond.false_label, ctx->block_patches, ctx->arena);
    emit_jmp_placeholder(ctx->buf, in->extra.brcond.true_label, ctx->block_patches, ctx->arena);
}

static void lower_phi(Instr *in, CodegenCtx *ctx)
{
    (void) in;
    (void) ctx; /* lowered into copies in predecessor blocks */
}

static void lower_unreachable(Instr *in, CodegenCtx *ctx)
{
    (void) in;
    emit_ud2(ctx->buf);
}

/* Every immediate is encoded in a signed 32-bit field. Values that do not
   fit (e.g. a C11 `long` constant, LP64) are user input the current
   32-bit-only lowering cannot represent: diagnose, never assert. */
static bool instr_has_bad_imm(Instr *in)
{
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        if (in->ops[oi].is_imm && !fits_i32(in->ops[oi].u.imm))
        {
            return true;
        }
    }
    if (in->opcode == OP_CALL)
    {
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            if (in->extra.call.args[a].is_imm && !fits_i32(in->extra.call.args[a].u.imm))
            {
                return true;
            }
        }
    }
    return false;
}

static void lower_instr(Instr *in, CodegenCtx *ctx)
{
    if (instr_has_bad_imm(in))
    {
        codegen_error(ctx, "%s: immediate outside i32 range; 64-bit codegen not implemented yet",
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
/* Function emission                                                   */
/* ------------------------------------------------------------------ */

static void scan_vreg(u32 *max, u32 vreg)
{
    if (vreg != NO_VREG && vreg > *max)
    {
        *max = vreg;
    }
}

static void scan_operand(u32 *max, Operand op)
{
    if (!op.is_imm)
    {
        scan_vreg(max, op.u.vreg);
    }
}

static void scan_instr_vregs(u32 *max, Instr *in)
{
    scan_vreg(max, in->result);
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        scan_operand(max, in->ops[oi]);
    }
    if (in->opcode == OP_CALL)
    {
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            scan_operand(max, in->extra.call.args[a]);
        }
    }
}

static FrameInfo frame_plan(Function *f)
{
    u32 max_vreg = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            scan_instr_vregs(&max_vreg, (Instr *) vec_get(blk->instrs, ii));
        }
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        scan_vreg(&max_vreg, ((Param *) vec_get(f->params, i))->vreg);
    }

    FrameInfo fr;
    fr.n_vregs = max_vreg + 1;
    fr.frame_size = (fr.n_vregs * 8 + 15) & ~15u;
    return fr;
}

/* Move incoming args into their param vreg slots. Args 7+ sit at
   16 + (i-6)*8(%rbp) and must be routed through %eax. */
static void emit_param_shuffle(ByteBuf *buf, Function *f)
{
    size_t nparams = vec_size(f->params);
    size_t n_reg = nparams < 6 ? nparams : 6;
    for (size_t i = 0; i < n_reg; i++)
    {
        Param *p = (Param *) vec_get(f->params, i);
        emit_mov(buf, 4, xop_vreg(p->vreg), xop_reg(abi_arg_regs[i]));
    }
    for (size_t i = n_reg; i < nparams; i++)
    {
        Param *p = (Param *) vec_get(f->params, i);
        emit_mov(buf, 4, xop_reg(R_EAX), xop_mem(x86_mem_rbp(16 + (i32) ((i - 6) * 8))));
        emit_mov(buf, 4, xop_vreg(p->vreg), xop_reg(R_EAX));
    }
}

static void emit_prologue(ByteBuf *buf, Function *f)
{
    bytebuf_append(buf, 0x55); /* push %rbp */
    emit_mov(buf, 8, xop_reg(R_EBP), xop_reg(R_ESP)); /* mov %rsp, %rbp */

    /* n_vregs >= 1, so the frame is always at least 16 bytes. */
    emit_binop_rhs(buf, 8, &arith_specs[OP_SUB], R_ESP, xop_imm(frame_plan(f).frame_size));

    emit_param_shuffle(buf, f);
}

static bool is_terminator(IrOpcode op)
{
    return op == OP_RET || op == OP_UNREACHABLE || op == OP_BR || op == OP_BRCOND;
}

/* Each PHI entry becomes a copy in the named predecessor block. */
static void add_phi_copies(CodegenCtx *ctx, Instr *phi)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        PhiEntry *entry = &phi->extra.phi.entries[e];
        Block *pred = strmap_get(ctx->label_to_block, entry->label);
        ASSERT(pred != NULL && "phi entry names a real predecessor in this function");
        size_t pj = (size_t) u64map_get(ctx->block_to_index, (u64) (uintptr_t) pred);
        PhiCopy *pc = arena_alloc(ctx->arena, sizeof(PhiCopy), sizeof(void *));
        pc->src = entry->val;
        pc->dst_vreg = phi->result;
        vec_push(ctx->phi_copies[pj], pc);
    }
}

static void collect_phi_copies(Function *f, CodegenCtx *ctx)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI)
            {
                continue;
            }
            add_phi_copies(ctx, in);
        }
    }
}

static void emit_block(Block *blk, size_t bi, CodegenCtx *ctx)
{
    ctx->block_offsets[bi] = bytebuf_len(ctx->buf);
    size_t ninstr = vec_size(blk->instrs);

    /* Non-terminator instructions first. */
    size_t ii = 0;
    for (; ii < ninstr; ii++)
    {
        Instr *in = (Instr *) vec_get(blk->instrs, ii);
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
            emit_mov(ctx->buf, 4, xop_reg(R_EAX), xop_from_operand(pc->src));
            emit_mov(ctx->buf, 4, xop_vreg(pc->dst_vreg),
                     xop_reg(R_EAX));
        }
        lower_instr((Instr *) vec_get(blk->instrs, ii), ctx);
    }
}

static void resolve_block_patches(CodegenCtx *ctx)
{
    size_t npatches = vec_size(ctx->block_patches);
    for (size_t pi = 0; pi < npatches; pi++)
    {
        Patch *bp = (Patch *) vec_get(ctx->block_patches, pi);
        Block *target = strmap_get(ctx->label_to_block, bp->target);
        ASSERT(target != NULL && "branch target names a block the IR builder created");
        size_t ti = (size_t) u64map_get(ctx->block_to_index, (u64) (uintptr_t) target);
        i32 rel = (i32) ((i64) ctx->block_offsets[ti] - (i64) (bp->offset + 4));
        bytebuf_poke_u32(ctx->buf, bp->offset, (u32) rel);
    }
}

static void emit_func_mc(Function *f, CodegenFunc *cf, Module *mod, Arena *arena)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);

    emit_prologue(buf, f);

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
    };

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        strmap_set(ctx.label_to_block, blk->label, blk);
        u64map_set(ctx.block_to_index, (u64) (uintptr_t) blk, (void *) bi);
    }

    collect_phi_copies(f, &ctx);

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        emit_block((Block *) vec_get(f->blocks, bi), bi, &ctx);
    }

    resolve_block_patches(&ctx);

    cf->name = f->name;
    cf->bytes = buf;
    cf->offset = 0;
    cf->patches = patches;
}

/* ------------------------------------------------------------------ */
/* Module emission                                                     */
/* ------------------------------------------------------------------ */

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

CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena)
{
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->funcs = vec_new(arena);

    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        Function *f = (Function *) vec_get(ir->funcs, i);
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

    /* Apply call patches */
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            Patch *p = (Patch *) vec_get(cf->patches, pi);
            CodegenFunc *target = find_codegen_func(cm, p->target);
            if (!target)
            {
                codegen_error(NULL, "undefined function '%s'", p->target);
                continue;
            }
            i32 rel = (i32) (target->offset - (cf->offset + p->offset + 4));
            bytebuf_poke_u32(cf->bytes, p->offset, (u32) rel);
        }
    }

    return cm;
}
