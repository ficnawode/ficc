#include "codegen.h"
#include "liveinterval.h"
#include "regalloc.h"
#include "target.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "x86_emit.h"
#include "x86_lower.h"
#include "x87.h"
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static u64 align_up(u64 n, u64 a)
{
    return (n + a - 1) / a * a;
}

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

/* Per-function frame: slot_off[vreg] is the vreg's offset below %rbp (see frame_plan). */
typedef struct
{
    u32 frame_size;    /* total frame size below %rbp, rounded up to 16 (ABI) */
    u32 save_area_off; /* register save area offset below %rbp, 0 if non-variadic */
    u32 *slot_off;     /* per-vreg frame offsets below %rbp */
    u32 off_push;      /* bytes emitted for `push rbp` */
    u32 off_mov;       /* bytes emitted for `mov rbp, rsp` */
    u32 off_sub;       /* bytes emitted for `sub rsp, N` */
} FrameInfo;

/* Block label -> IrBlock* plus IrBlock* -> index, built once per function. */
typedef struct
{
    size_t nblocks;
    StrMap *label_to_block; /* block label -> IrBlock* */
    U64Map *block_to_index; /* IrBlock* -> index into f->blocks */
} BlockIndex;

static BlockIndex *index_blocks(IrFunction *f, Arena *arena)
{
    BlockIndex *bi = arena_alloc(arena, sizeof(BlockIndex), sizeof(void *));
    bi->nblocks = vec_size(f->blocks);
    bi->label_to_block = strmap_new(arena);
    bi->block_to_index = u64map_new(arena);
    for (size_t i = 0; i < bi->nblocks; i++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, i);
        strmap_set(bi->label_to_block, blk->label, blk);
        u64map_set(bi->block_to_index, (u64) (uintptr_t) blk, (void *) (uintptr_t) i);
    }
    return bi;
}

static size_t block_index_of(const BlockIndex *bi, IrBlock *blk)
{
    return (size_t) (uintptr_t) u64map_get(bi->block_to_index, (u64) (uintptr_t) blk);
}

static IrBlock *block_by_label(const BlockIndex *bi, const char *label)
{
    return (IrBlock *) strmap_get(bi->label_to_block, label);
}

typedef struct CodegenCtx CodegenCtx;
struct CodegenCtx
{
    ByteBuf *buf;
    IrFunction *func;
    IrModule *mod; /* for the per-vreg width table */
    Arena *arena;
    Vec **phi_copies;      /* per-block Vec<PhiCopy*>, indexed by block index */
    BlockIndex *blocks;    /* label/pointer lookup for this function's blocks */
    size_t *block_offsets; /* per-block offset within the function bytes */
    Vec *patches;          /* Vec<PatchSite*> — function call rel32 fields */
    Vec *block_patches;    /* Vec<PatchSite*> — intra-function jump rel32 fields */
    Vec *global_patches;   /* Vec<GlobalPatch*> */
    Vec *func_patches;     /* Vec<FuncAddrPatch*> — function-address loads */
    Vec *switch_tables;    /* Vec<SwitchTableRec*> */
    u32 save_area_off;     /* register save area offset below %rbp (variadic fns) */
    u32 *slot_off;         /* per-vreg frame offsets (see FrameInfo) */
    Vec *lines;            /* Vec<LineEntry*>, NULL without -g */
    bool debug;            /* record line boundaries for DWARF */
    int fpu_depth;         /* x87 stack depth; lowered sequences leave no residue */
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

#define BIT_63 63

#define F32_SIGN_BIT 0x80000000
#define F64_SIGN_BIT 0x8000000000000000ULL

#define ALIGN_UP(v, align) (((v) + ((align) - 1)) & ~((align) - 1))

/* SysV va_list register save area: ngp × 8-byte GP slots, then nfp × 16-byte
   xmm slots; the slot counts come from the target (B). */
#define VA_GP_STRIDE 8
#define VA_NGP x86_64_target()->ngp
#define VA_NXMM x86_64_target()->nfp
#define VA_GP_BYTES (VA_NGP * VA_GP_STRIDE)
#define VA_SAVE_BYTES (VA_GP_BYTES + VA_NXMM * VA_XMM_STRIDE)
#define VA_XMM_STRIDE 16

#define VA_FIELD_GP_OFFSET 0
#define VA_FIELD_FP_OFFSET 4
#define VA_FIELD_OVF 8
#define VA_FIELD_REGS 16

/* Caller arguments sit 16 bytes above %rbp (return address + saved %rbp). */
#define STACK_PARAM_BASE 16

/* %rsp and outgoing call area stay 16-byte aligned (SysV ABI). */
#define STACK_ALIGN 16

/* IEEE bit patterns of 2^63, added back by the u64 int→FP sequence. */
#define F32_BITS_2POW63 0x5F000000
#define F64_BITS_2POW63 0x43E0000000000000

/* 80-bit pattern of 2^63: significand 0x8000000000000000 @ exponent 0x403E. */
#define LD_EXPONENT_2POW63 0x403E
#define LD_SIGNIFICAND_2POW63 0x8000000000000000ULL

/* 0F BA /digit ib: bt=4, bts=5, btr=6. */
#define X86_XOP_BT 4
#define X86_XOP_BTS 5
#define X86_XOP_BTR 6

/* Frame offset for a vreg below %rbp; frame_plan always provides the slot table. */
static i32 vreg_frame_disp(u32 vreg, const u32 *slot_off)
{
    ASSERT(slot_off != NULL && "frame planner always provides a slot table");
    return -(i32) slot_off[vreg];
}

static X86Operand xop_vreg_slot(u32 vreg, const u32 *slot_off)
{
    return xop_mem(x86_mem_rbp(vreg_frame_disp(vreg, slot_off)));
}

static X86Operand xop_vreg(CodegenCtx *ctx, u32 vreg)
{
    return xop_mem(x86_mem_rbp(vreg_frame_disp(vreg, ctx->slot_off)));
}

/* Frame memory slot for a vreg; the common `.u.mem` read of xop_vreg. */
static X86Mem vreg_slot_mem(CodegenCtx *ctx, u32 vreg)
{
    return xop_vreg(ctx, vreg).u.mem;
}

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
    if (op.is_imm)
    {
        return xop_imm(op.u.imm);
    }
    return xop_vreg(ctx, op.u.vreg);
}

/* Integer-immediate load width: 4 bytes if it fits a sign-extended imm32, else full 64-bit. */
static bool fits_i32(i64 v)
{
    return v >= (i64) INT32_MIN && v <= (i64) INT32_MAX;
}

static u8 imm_load_width(i64 imm)
{
    return fits_i32(imm) ? 4 : 8;
}

/* Lowered value width: addresses/fn ptrs are 8B, immediates via imm_load_width, vregs their own. */
static u8 operand_width(CodegenCtx *ctx, IrOperand op)
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

/* A leaked st(1) silently poisons later FP results; every lowering leaves depth 0. */
static void x87_push(CodegenCtx *ctx)
{
    ctx->fpu_depth++;
}

static void x87_pop(CodegenCtx *ctx)
{
    ctx->fpu_depth--;
    ASSERT(ctx->fpu_depth >= 0 && "x87 stack underflow");
}

static void x87_balance(CodegenCtx *ctx)
{
    ASSERT(ctx->fpu_depth == 0 && "x87 stack not balanced after a width-16 lowering");
}

/* Restore the depth counter at a branch label (fall-through vs taken may differ). */
static void x87_set_depth(CodegenCtx *ctx, int depth)
{
    ctx->fpu_depth = depth;
}

static void emit_fldt(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fldt(ctx->buf, m);
    x87_push(ctx);
}
static void emit_fstpt(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fstpt(ctx->buf, m);
    x87_pop(ctx);
}
/* A callee left this value on %st0; the depth counter never saw the push. */
static void emit_fstpt_return(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fstpt(ctx->buf, m);
}
static void emit_flds(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_flds(ctx->buf, m);
    x87_push(ctx);
}
static void emit_fstps(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fstps(ctx->buf, m);
    x87_pop(ctx);
}
static void emit_fldl(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fldl(ctx->buf, m);
    x87_push(ctx);
}
static void emit_fstpl(CodegenCtx *ctx, X86Mem m)
{
    x87_emit_fstpl(ctx->buf, m);
    x87_pop(ctx);
}
static void emit_fild(CodegenCtx *ctx, u8 size, X86Mem m)
{
    x87_emit_fild(ctx->buf, size, m);
    x87_push(ctx);
}
static void emit_fisttp(CodegenCtx *ctx, u8 size, X86Mem m)
{
    x87_emit_fisttp(ctx->buf, size, m);
    x87_pop(ctx);
}
static void emit_fldz(CodegenCtx *ctx)
{
    x87_emit_fldz(ctx->buf);
    x87_push(ctx);
}
static void emit_fchs(CodegenCtx *ctx)
{
    x87_emit_fchs(ctx->buf);
}
/* fsubp name matches the x87 semantics used: st(1) ← st(1) − st(0), pop. */
static void emit_faddp(CodegenCtx *ctx)
{
    x87_emit_faddp(ctx->buf);
    x87_pop(ctx);
}
static void emit_fsubp(CodegenCtx *ctx)
{
    x87_emit_fsubp(ctx->buf);
    x87_pop(ctx);
}
/* fsubrp st(1), st(0): st(1) ← st(0) − st(1), pop — the reverse of fsubp. */
static void emit_fsubrp(CodegenCtx *ctx)
{
    x87_emit_fsubrp(ctx->buf);
    x87_pop(ctx);
}
static void emit_fmulp(CodegenCtx *ctx)
{
    x87_emit_fmulp(ctx->buf);
    x87_pop(ctx);
}
static void emit_fdivp(CodegenCtx *ctx)
{
    x87_emit_fdivp(ctx->buf);
    x87_pop(ctx);
}
/* fucomip st(0), st(1): unordered compare, sets ZF/CF/PF like ucomis*, pops st(0). */
static void emit_fucomip(CodegenCtx *ctx)
{
    x87_emit_fucomip(ctx->buf);
    x87_pop(ctx);
}
static void emit_fstp_st0(CodegenCtx *ctx)
{
    x87_emit_fstp_st0(ctx->buf);
    x87_pop(ctx);
}

/* Build the 80-bit 2^63 in a temporary %rsp scratch region, fldt it, release. */
static void emit_load_2pow63_ld(CodegenCtx *ctx)
{
    ByteBuf *b = ctx->buf;
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(W_LD));
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_imm((i64) LD_SIGNIFICAND_2POW63));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rsp(0)), xop_reg(R_EAX));
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_imm(0));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rsp(8)), xop_reg(R_EAX));
    emit_mov(b, W_WORD, xop_reg(R_EAX), xop_imm(LD_EXPONENT_2POW63));
    emit_mov(b, W_WORD, xop_mem(x86_mem_rsp(8)), xop_reg(R_EAX));
    emit_fldt(ctx, x86_mem_rsp(0));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_ADD], R_ESP, xop_imm(W_LD));
}

static void emit_bit_imm(ByteBuf *buf, u8 digit, u8 dst_reg, u8 imm)
{
    bytebuf_append(buf, X86_REX_W);
    bytebuf_append(buf, X86_TWO_BYTE_ESC);
    bytebuf_append(buf, X86_BIT_BASE);
    bytebuf_append(buf, modrm(3, digit, dst_reg));
    bytebuf_append_i8(buf, (i8) imm);
}

/* Scratch: lower_* may clobber EAX/ECX/EDX, so live ranges must not span a lowering. */
typedef void (*LowerFn)(IrInstr *in, CodegenCtx *ctx);

static void emit_fp_source_to_xmm(CodegenCtx *ctx, IrOperand op, u8 w, u8 xmm);
static void emit_fp_source_to_xmm0(CodegenCtx *ctx, IrOperand op, u8 w);
static X86Mem x87_slot(CodegenCtx *ctx, IrOperand op);
static u8 load_int_operand(CodegenCtx *ctx, IrOperand op, bool *is_signed);

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
static void lower_itof(IrInstr *in, CodegenCtx *ctx);
static void lower_ftoi(IrInstr *in, CodegenCtx *ctx);
static void lower_fconv(IrInstr *in, CodegenCtx *ctx);
static void lower_fbin(IrInstr *in, CodegenCtx *ctx);
static void lower_fneg(IrInstr *in, CodegenCtx *ctx);
static void lower_fcmp(IrInstr *in, CodegenCtx *ctx);
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
    X(OP_VA_END, lower_va_end)                                                                     \
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

/* RHS: a width-8 immediate beyond imm32 is moved through `scratch` (movabs) instead of erroring. */
static X86Operand lowered_operand_rhs(CodegenCtx *ctx, IrOperand op, u8 width, u8 scratch)
{
    if (op.is_imm && width == 8 && !fits_i32(op.u.imm))
    {
        emit_mov(ctx->buf, 8, xop_reg(scratch), lowered_operand(ctx, op, scratch));
        return xop_reg(scratch);
    }
    return lowered_operand(ctx, op, scratch);
}

static void lower_binary(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    const ArithSpec *s = &arith_specs[in->opcode];
    X86Operand rhs = lowered_operand_rhs(ctx, in->ops[1], w, R_ECX);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    if (in->opcode == OP_AND && w == 4 && rhs.kind == XOP_IMM && rhs.u.imm == 0xFF)
    {
        emit_movzbl_al_eax(ctx->buf);
    }
    else if (in->opcode == OP_XOR && rhs.kind == XOP_IMM && rhs.u.imm == 0)
    {
        emit_xor_eax_eax(ctx->buf);
    }
    else
    {
        emit_binop_rhs(ctx->buf, w, s, R_EAX, rhs);
    }
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

static void lower_unary(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_unary(ctx->buf, w, R_EAX, unary_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

static void lower_shift(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_mov(ctx->buf, w, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    emit_shift_cl(ctx->buf, w, R_EAX, shift_digit[in->opcode]);
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

static void lower_div(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    bool is_unsigned = in->opcode == OP_UDIV || in->opcode == OP_UREM;
    emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_cdq(ctx->buf, w, is_unsigned);
    emit_mov(ctx->buf, w, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    if (is_unsigned)
    {
        emit_div(ctx->buf, w, R_ECX);
    }
    else
    {
        emit_idiv(ctx->buf, w, R_ECX);
    }
    if (in->opcode == OP_SREM || in->opcode == OP_UREM)
    {
        emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_reg(R_EDX));
    }
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

static void lower_icmp(IrInstr *in, CodegenCtx *ctx)
{
    /* Compare at the widest operand width to keep narrow-vs-imm32 comparisons precise. */
    u8 rw = vreg_width(ctx, in->result);
    IrOperand lhs = in->ops[0];
    IrOperand rhs = in->ops[1];
    u8 w0 = operand_width(ctx, lhs);
    u8 w1 = operand_width(ctx, rhs);
    u8 w = MAX(w0, w1);

    if (lhs.is_imm)
    {
        emit_mov(ctx->buf, w, xop_reg(R_EAX), xop_imm(lhs.u.imm));
    }
    else
    {
        emit_mov(ctx->buf, w0, xop_reg(R_EAX), lowered_operand(ctx, lhs, R_EAX));
        if (w0 < w && w0 < 4)
        {
            emit_movzx(ctx->buf, w0, w, R_EAX, xop_reg(R_EAX));
        }
    }
    if (rhs.is_imm)
    {
        emit_mov(ctx->buf, w, xop_reg(R_ECX), xop_imm(rhs.u.imm));
    }
    else
    {
        emit_mov(ctx->buf, w1, xop_reg(R_ECX), lowered_operand(ctx, rhs, R_ECX));
        if (w1 < w && w1 < 4)
        {
            emit_movzx(ctx->buf, w1, w, R_ECX, xop_reg(R_ECX));
        }
    }

    emit_binop_rhs(ctx->buf, w, &cmp_spec, R_EAX, xop_reg(R_ECX));
    emit_setcc(ctx->buf, icmp_cc[in->opcode]);
    emit_movzbl_al_eax(ctx->buf);
    emit_mov(ctx->buf, rw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

/* The GP argument register sequence is the target's (x86_64_target().gp_args). */

/* Where a call argument travels: GP/SSE register, or a caller-stack spill slot. */
typedef enum
{
    ARG_GP,
    ARG_SSE,
    ARG_X87,   /* width-16: always pushed on the stack, 16-aligned */
    ARG_STACK, /* overflow: pushed on the stack */
} ArgClass;

/* One call argument, classified once up front so the stack and register passes agree. */
typedef struct
{
    IrOperand op;
    ArgClass cls;
    u8 width;       /* operands load/move at this width */
    bool is_fp;     /* a floating-point vreg (never true for immediates) */
    u32 reg_or_off; /* GP/SSE register index, or byte offset below %rsp for stack args */
} CallArg;

static u32 classify_call_args(CodegenCtx *ctx, IrInstr *in, CallArg *args, u32 *fp_count)
{
    u32 nargs = in->extra.call.nargs;
    u32 gp_count = 0, stack_off = 0;
    *fp_count = 0;
    for (u32 i = 0; i < nargs; i++)
    {
        IrOperand arg = in->extra.call.args[i];
        CallArg *a = &args[i];
        a->op = arg;
        a->width = operand_width(ctx, arg);
        a->is_fp = ir_operand_is_vreg(arg) && ir_vreg_float(ctx->mod, arg.u.vreg);
        if (a->is_fp && a->width == W_LD)
        {
            stack_off = ALIGN_UP(stack_off, STACK_ALIGN);
            a->cls = ARG_X87;
            a->reg_or_off = stack_off;
            stack_off += W_LD;
        }
        else if (a->is_fp && *fp_count < VA_NXMM)
        {
            a->cls = ARG_SSE;
            a->reg_or_off = *fp_count;
            (*fp_count)++;
        }
        else if (!a->is_fp && gp_count < VA_NGP)
        {
            a->cls = ARG_GP;
            a->reg_or_off = gp_count;
            gp_count++;
        }
        else
        {
            a->cls = ARG_STACK;
            a->reg_or_off = stack_off;
            stack_off += W_QWORD;
        }
    }
    return stack_off;
}

static void emit_call_stack_args(CodegenCtx *ctx, CallArg *args, u32 nargs)
{
    for (u32 i = 0; i < nargs; i++)
    {
        CallArg *a = &args[i];
        if (a->cls != ARG_STACK && a->cls != ARG_X87)
        {
            continue;
        }
        X86Mem dst = x86_mem_rsp((i32) a->reg_or_off);
        if (a->cls == ARG_X87)
        {
            emit_fldt(ctx, x87_slot(ctx, a->op));
            emit_fstpt(ctx, dst);
        }
        else if (a->is_fp)
        {
            emit_fp_source_to_xmm(ctx, a->op, a->width, R_XMM0);
            emit_sse_store(ctx->buf, MF_OF(a->width), dst, R_XMM0);
        }
        else
        {
            emit_mov(ctx->buf, a->width, xop_reg(R_EAX), lowered_operand(ctx, a->op, R_EAX));
            emit_mov(ctx->buf, W_QWORD, xop_mem(dst), xop_reg(R_EAX));
        }
    }
}

static void emit_call_reg_args(CodegenCtx *ctx, CallArg *args, u32 nargs)
{
    for (u32 i = 0; i < nargs; i++)
    {
        CallArg *a = &args[i];
        if (a->cls == ARG_SSE)
        {
            emit_fp_source_to_xmm(ctx, a->op, a->width, (u8) a->reg_or_off);
        }
        else if (a->cls == ARG_GP)
        {
            emit_mov(ctx->buf, a->width, xop_reg(R_EAX), lowered_operand(ctx, a->op, R_EAX));
            emit_mov(ctx->buf, a->width, xop_reg(x86_64_target()->gp_args[a->reg_or_off]),
                     xop_reg(R_EAX));
        }
    }
}

static void emit_call_target(CodegenCtx *ctx, IrInstr *in)
{
    if (in->extra.call.is_indirect)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_R11),
                 lowered_operand(ctx, in->extra.call.callee, R_R11));
        emit_call_reg(ctx->buf, R_R11);
    }
    else
    {
        emit_call(ctx->buf, in->extra.call.name, ctx->patches, ctx->arena);
    }
}

static void store_call_result(CodegenCtx *ctx, IrInstr *in)
{
    if (in->result == NO_VREG)
    {
        return;
    }
    u8 rw = vreg_width(ctx, in->result);
    if (ir_vreg_float(ctx->mod, in->result))
    {
        if (rw == W_LD)
        {
            emit_fstpt_return(ctx, vreg_slot_mem(ctx, in->result));
        }
        else
        {
            emit_sse_store(ctx->buf, MF_OF(rw), vreg_slot_mem(ctx, in->result), R_XMM0);
        }
    }
    else
    {
        emit_mov(ctx->buf, rw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
    }
}

static void lower_call(IrInstr *in, CodegenCtx *ctx)
{
    u32 nargs = in->extra.call.nargs;
    CallArg *args = arena_alloc(ctx->arena, nargs * sizeof(CallArg), sizeof(void *));
    u32 fp_count;
    u32 total_stack = ALIGN_UP(classify_call_args(ctx, in, args, &fp_count), STACK_ALIGN);

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(total_stack));
    }
    emit_call_stack_args(ctx, args, nargs);
    emit_call_reg_args(ctx, args, nargs);

    if (in->extra.call.is_variadic)
    {
        emit_mov_byte(ctx->buf, xop_reg(R_EAX), xop_imm((i64) fp_count));
    }

    emit_call_target(ctx, in);
    store_call_result(ctx, in);

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_ADD], R_ESP, xop_imm(total_stack));
    }
}

static void lower_va_start(IrInstr *in, CodegenCtx *ctx)
{
    (void) load_ptr(ctx, in->ops[0]);
    IrVaStartPayload *vd = &in->extra.va_start;
    emit_mov(ctx->buf, W_DWORD, xop_reg(R_EDX), xop_imm(vd->gp_offset));
    emit_mov(ctx->buf, W_DWORD, xop_mem(x86_mem_rax(VA_FIELD_GP_OFFSET)), xop_reg(R_EDX));
    emit_mov(ctx->buf, W_DWORD, xop_reg(R_EDX), xop_imm(vd->fp_offset));
    emit_mov(ctx->buf, W_DWORD, xop_mem(x86_mem_rax(VA_FIELD_FP_OFFSET)), xop_reg(R_EDX));
    emit_lea(ctx->buf, R_EDX, x86_mem_rbp(STACK_PARAM_BASE + (i32) vd->stack_skip));
    emit_mov(ctx->buf, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_EDX));
    emit_lea(ctx->buf, R_EDX, x86_mem_rbp(-(i32) ctx->save_area_off));
    emit_mov(ctx->buf, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_REGS)), xop_reg(R_EDX));
}

/* ld args ride the overflow area only: align 16, read 16, bump 16. */
static void emit_va_arg_ld(CodegenCtx *ctx, IrInstr *in)
{
    ByteBuf *b = ctx->buf;
    (void) load_ptr(ctx, in->ops[0]);
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_mem(x86_mem_rax(VA_FIELD_OVF)));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_ADD], R_ECX, xop_imm(STACK_ALIGN - 1));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_AND], R_ECX, xop_imm(-STACK_ALIGN));
    emit_lea(b, R_EDX, x86_mem_rcx(STACK_ALIGN));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_EDX));
    emit_mov16(b, x86_mem_rcx(0), vreg_slot_mem(ctx, in->result));
}

static void lower_va_arg(IrInstr *in, CodegenCtx *ctx)
{
    if (vreg_width(ctx, in->result) == W_LD)
    {
        emit_va_arg_ld(ctx, in);
        return;
    }
    ByteBuf *b = ctx->buf;
    (void) load_ptr(ctx, in->ops[0]);

    X86Mem off_mem;
    i64 limit;
    i64 stride;
    if (ir_vreg_float(ctx->mod, in->result))
    {
        off_mem = x86_mem_rax(VA_FIELD_FP_OFFSET);
        limit = VA_SAVE_BYTES;
        stride = VA_XMM_STRIDE;
    }
    else
    {
        off_mem = x86_mem_rax(VA_FIELD_GP_OFFSET);
        limit = VA_GP_BYTES;
        stride = W_QWORD;
    }

    emit_mov(b, W_DWORD, xop_reg(R_EDX), xop_mem(off_mem));
    emit_binop_rhs(b, W_DWORD, &cmp_spec, R_EDX, xop_imm(limit));
    size_t jge_field = emit_jcc_pending(b, CC_GE);

    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_mem(x86_mem_rax(VA_FIELD_REGS)));
    emit_reg_reg(b, arith_specs[OP_ADD].mem, R_ECX, R_EDX);
    emit_binop_rhs(b, W_DWORD, &arith_specs[OP_ADD], R_EDX, xop_imm(stride));
    emit_mov(b, W_DWORD, xop_mem(off_mem), xop_reg(R_EDX));
    size_t ovf_jmp_field = emit_jmp_pending(b);

    size_t ovf_arm = bytebuf_len(b);
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_mem(x86_mem_rax(VA_FIELD_OVF)));
    emit_mov(b, W_QWORD, xop_reg(R_R8), xop_reg(R_ECX));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_ADD], R_ECX, xop_imm(W_QWORD));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_ECX));
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_reg(R_R8));

    size_t done = bytebuf_len(b);
    patch_rel32(b, jge_field, ovf_arm);
    patch_rel32(b, ovf_jmp_field, done);

    emit_mov(b, W_QWORD, xop_reg(R_EDX), xop_mem(x86_mem_rcx(0)));
    emit_mov(b, W_QWORD, xop_vreg(ctx, in->result), xop_reg(R_EDX));
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
        if (type_is_fp(ctx->func->ret_type))
        {
            if (w == 16)
            {
                /* %st0 return: leave the value on the x87 stack, reset the counter. */
                emit_fldt(ctx, x87_slot(ctx, val));
                x87_set_depth(ctx, 0);
                bytebuf_append(ctx->buf, X86_LEAVE);
                bytebuf_append(ctx->buf, X86_RET);
                return;
            }
            /* FP returns leave the value in %xmm0 (SysV). */
            emit_fp_source_to_xmm0(ctx, val, w);
        }
        else
        {
            emit_mov(ctx->buf, w, xop_reg(R_EAX), lowered_operand(ctx, val, R_ECX));
        }
    }
    else
    {
        emit_mov(ctx->buf, W_DWORD, xop_reg(R_EAX), xop_imm(0));
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
    /* Test at the condition's width so bytes past a byte-sized slot can't influence the branch. */
    u8 cw = operand_width(ctx, in->ops[0]);
    emit_mov(ctx->buf, cw, xop_reg(R_EAX), lowered_operand(ctx, in->ops[0], R_EAX));
    emit_test_reg(ctx->buf, cw, R_EAX);
    emit_jcc(ctx->buf, CC_E, in->extra.brcond.false_label, ctx->block_patches, ctx->arena);
    emit_jmp(ctx->buf, in->extra.brcond.true_label, ctx->block_patches, ctx->arena);
}

/* Tables only when the case range is small (≤ JT_MAX_RANGE); sparse switches use a chain. */
#define JT_MAX_RANGE 256

/* Load the control value into %rax at its exact 64-bit semantic value. */
static void emit_switch_control(CodegenCtx *ctx, IrOperand src)
{
    bool is_signed;
    (void) load_int_operand(ctx, src, &is_signed);
}

/* Bounds-check %rax to [min,max], subtract min, jump through the full-range table. */
static void emit_switch_table(CodegenCtx *ctx, IrSwitchCase *cases, u32 n, i64 min, i64 max,
                              const char *default_label)
{
    u8 below_cc = min < 0 ? CC_L : CC_B;
    u8 above_cc = min < 0 ? CC_G : CC_A;
    u64 range = (u64) max - (u64) min;

    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(min));
    emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_ECX);
    emit_jcc(ctx->buf, below_cc, default_label, ctx->block_patches, ctx->arena);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_EDX), xop_imm(max));
    emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_EDX);
    emit_jcc(ctx->buf, above_cc, default_label, ctx->block_patches, ctx->arena);
    emit_reg_reg(ctx->buf, arith_specs[OP_SUB].mem, R_EAX, R_ECX);

    emit_lea(ctx->buf, R_EDX, (X86Mem) {.base = NO_REG, .index = NO_REG, .scale = 1, .disp = 0});
    size_t disp_field_off = bytebuf_len(ctx->buf) - 4;

    emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX),
             xop_mem((X86Mem) {.base = R_EDX, .index = R_EAX, .scale = 8, .disp = 0}));
    emit_reg_reg(ctx->buf, arith_specs[OP_ADD].mem, R_EAX, R_EDX);
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

static void emit_switch_chain(CodegenCtx *ctx, IrSwitchCase *cases, u32 n,
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
            emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(cases[i].val));
            emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_ECX);
        }
        emit_jcc(ctx->buf, CC_E, cases[i].label, ctx->block_patches, ctx->arena);
    }
    emit_jmp(ctx->buf, default_label, ctx->block_patches, ctx->arena);
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
        u64 range = (u64) max - (u64) min;
        if (range <= JT_MAX_RANGE)
        {
            emit_switch_table(ctx, cases, n, min, max, default_label);
            return;
        }
    }
    emit_switch_chain(ctx, cases, n, default_label);
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
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EAX));
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
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        emit_mov(ctx->buf, sw, xop_reg(R_EAX), src);
        /* A 4-byte source needs no MOVZX: writing EAX zero-extends to RAX. */
        if (sw < 4)
        {
            emit_movzx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
        }
    }
    emit_mov(ctx->buf, dw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
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
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        emit_mov(ctx->buf, sw, xop_reg(R_EAX), src);
        emit_movsx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
    }
    emit_mov(ctx->buf, dw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
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
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), lowered_operand(ctx, ptr, R_EAX));
    }
    return (X86Mem) {.base = R_EAX, .index = NO_REG, .scale = 1, .disp = 0};
}

static void lower_load(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    X86Mem indirect = load_ptr(ctx, in->ops[0]);
    if (w == 16)
    {
        emit_mov16(ctx->buf, indirect, vreg_slot_mem(ctx, in->result));
        return;
    }
    emit_mov(ctx->buf, w, xop_reg(R_EDX), xop_mem(indirect));
    emit_mov(ctx->buf, w, xop_vreg(ctx, in->result), xop_reg(R_EDX));
}

static void lower_store(IrInstr *in, CodegenCtx *ctx)
{
    u32 w = (u32) in->ops[2].u.imm;
    if (w == 16)
    {
        /* Move the value via xmm0 before the destination address is computed. */
        X86Operand val = lowered_operand(ctx, in->ops[0], R_ECX);
        X86Mem indirect = load_ptr(ctx, in->ops[1]);
        if (val.kind == XOP_IMM)
        {
            /* width-16 local zero-init: xorps zeros xmm0, movups stores it. */
            ASSERT(val.u.imm == 0 && "nonzero immediate in a width-16 store");
            emit_sse_xor(ctx->buf, 0, R_XMM0, R_XMM0);
            emit_mov16_store(ctx->buf, indirect);
            return;
        }
        emit_mov16(ctx->buf, val.u.mem, indirect);
        return;
    }
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

/* A conversion source is a slot vreg; a bare immediate is treated as double. */
static u8 fconv_src_width(CodegenCtx *ctx, IrOperand op)
{
    return op.is_imm ? 8 : vreg_width(ctx, op.u.vreg);
}

/* GP load width for an FP immediate: 4 bytes for a float lane, else imm_load_width. */
static u8 fp_imm_load_width(u8 w, i64 imm)
{
    return w == 4 ? 4 : imm_load_width(imm);
}

/* Move an FP operand's bits into `xmm`; a bare immediate rides GP mov + movd/movq. */
static void emit_fp_source_to_xmm(CodegenCtx *ctx, IrOperand op, u8 w, u8 xmm)
{
    ByteBuf *b = ctx->buf;
    if (w == 16)
    {
        /* 80-bit values never travel through XMM lanes (x87 is the carrier). */
        codegen_error(ctx, "long double operands are not lowered yet");
        emit_ud2(b);
        return;
    }
    u8 mf = MF_OF(w);
    X86Operand src = lowered_operand(ctx, op, R_EAX);
    if (src.kind == XOP_MEM)
    {
        emit_sse_load(b, mf, xmm, src.u.mem);
        return;
    }
    if (src.kind == XOP_IMM)
    {
        emit_mov(b, fp_imm_load_width(w, src.u.imm), xop_reg(R_EAX), src);
        emit_movd_to_xmm(b, xmm, R_EAX, w != 4);
        return;
    }
    codegen_error(ctx, "unexpected operand class for a floating-point conversion");
}

static void emit_fp_source_to_xmm0(CodegenCtx *ctx, IrOperand op, u8 w)
{
    emit_fp_source_to_xmm(ctx, op, w, R_XMM0);
}

static void emit_load_2pow63(ByteBuf *buf, bool is_f32)
{
    if (is_f32)
    {
        emit_mov(buf, W_DWORD, xop_reg(R_ECX), xop_imm(F32_BITS_2POW63));
        emit_movd_to_xmm(buf, R_XMM1, R_ECX, false);
    }
    else
    {
        emit_mov(buf, W_QWORD, xop_reg(R_ECX), xop_imm(F64_BITS_2POW63));
        emit_movd_to_xmm(buf, R_XMM1, R_ECX, true);
    }
}

/* A width-16 operand is always a vreg slot (immediates carry no 16-byte form). */
static X86Mem x87_slot(CodegenCtx *ctx, IrOperand op)
{
    ASSERT(ir_operand_is_vreg(op));
    return vreg_slot_mem(ctx, op.u.vreg);
}

/* Load an integer operand into %rax, sign/zero-extended to its 64-bit value; reports signedness. */
static u8 load_int_operand(CodegenCtx *ctx, IrOperand op, bool *is_signed)
{
    if (op.is_imm)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), xop_imm(op.u.imm));
        *is_signed = true;
        return W_QWORD;
    }
    if (op.is_global || op.is_func)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_EAX), lowered_operand(ctx, op, R_EAX));
        *is_signed = true;
        return W_QWORD;
    }
    u8 sw = vreg_width(ctx, op.u.vreg);
    *is_signed = ir_vreg_signed(ctx->mod, op.u.vreg);
    emit_mov(ctx->buf, sw, xop_reg(R_EAX), lowered_operand(ctx, op, R_EAX));
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

/* u64 ≥ 2^63 to long double: clear bit 63, convert, add the exact 2^63 back. */
static void emit_itof_u64_x87(CodegenCtx *ctx, X86Mem scratch)
{
    ByteBuf *b = ctx->buf;
    emit_test_reg(b, W_QWORD, R_EAX);
    size_t big_field = emit_jcc_pending(b, CC_S);
    emit_mov(b, W_QWORD, xop_mem(scratch), xop_reg(R_EAX));
    emit_fild(ctx, W_QWORD, scratch);
    emit_fstpt(ctx, scratch);
    size_t small_done = emit_jmp_pending(b);

    size_t big_off = bytebuf_len(b);
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_reg(R_EAX));
    emit_bit_imm(b, X86_XOP_BTR, R_ECX, BIT_63);
    emit_mov(b, W_QWORD, xop_mem(scratch), xop_reg(R_ECX));
    emit_fild(ctx, W_QWORD, scratch);
    emit_load_2pow63_ld(ctx);
    emit_faddp(ctx);
    emit_fstpt(ctx, scratch);

    patch_rel32(b, big_field, big_off);
    patch_rel32(b, small_done, bytebuf_len(b));
}

static void lower_itof_x87(IrInstr *in, CodegenCtx *ctx)
{
    bool is_signed;
    u8 sw = load_int_operand(ctx, in->ops[0], &is_signed);
    X86Mem scratch = vreg_slot_mem(ctx, in->result);
    if (sw == W_QWORD && !is_signed)
    {
        emit_itof_u64_x87(ctx, scratch);
    }
    else
    {
        emit_mov(ctx->buf, W_QWORD, xop_mem(scratch), xop_reg(R_EAX));
        emit_fild(ctx, W_QWORD, scratch);
        emit_fstpt(ctx, scratch);
    }
    x87_balance(ctx);
}

/* u64 ≥ 2^63 to float/double: clear the top bit, convert, add 2^63 back. */
static void emit_itof_u64_xmm(CodegenCtx *ctx, u8 mf, bool is_f32)
{
    ByteBuf *b = ctx->buf;
    emit_test_reg(b, W_QWORD, R_EAX);
    size_t js_field = emit_jcc_pending(b, CC_S);
    emit_cvtsi2fp(b, mf, R_XMM0, R_EAX);
    size_t jmp_field = emit_jmp_pending(b);
    size_t big_off = bytebuf_len(b);
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_reg(R_EAX));
    emit_bit_imm(b, X86_XOP_BTR, R_ECX, BIT_63);
    emit_cvtsi2fp(b, mf, R_XMM0, R_ECX);
    emit_load_2pow63(b, is_f32);
    emit_sse_add(b, mf, R_XMM0, R_XMM1);
    patch_rel32(b, js_field, big_off);
    patch_rel32(b, jmp_field, bytebuf_len(b));
}

static void lower_itof_xmm(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
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
    emit_sse_store(ctx->buf, mf, vreg_slot_mem(ctx, in->result), R_XMM0);
}

static void lower_itof(IrInstr *in, CodegenCtx *ctx)
{
    if (vreg_width(ctx, in->result) == W_LD)
    {
        lower_itof_x87(in, ctx);
    }
    else
    {
        lower_itof_xmm(in, ctx);
    }
}

/* u64 result from long double: values ≥ 2^63 subtract 2^63, re-truncate, set bit 63. */
static void emit_ftoi_u64_x87(CodegenCtx *ctx, X86Mem src, X86Mem scratch)
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

    x87_set_depth(ctx, 1); /* fast path re-enters with the leftover 2^63 on the stack */
    size_t fast_off = bytebuf_len(b);
    emit_fldt(ctx, src);
    emit_fisttp(ctx, W_QWORD, scratch);
    emit_mov(b, W_QWORD, xop_reg(R_EAX), xop_mem(scratch));
    emit_fstp_st0(ctx);

    patch_rel32(b, fast_field, fast_off);
    patch_rel32(b, slow_done, bytebuf_len(b));
}

static void lower_ftoi_x87(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    bool is_signed = ir_vreg_signed(ctx->mod, in->result);
    X86Mem src = x87_slot(ctx, in->ops[0]);
    X86Mem scratch = vreg_slot_mem(ctx, in->result);
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
    emit_mov(ctx->buf, dw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
    x87_balance(ctx);
}

/* u64 from float/double: convert, then for ≥ 2^63 subtract 2^63, re-convert, set bit 63. */
static void emit_ftoi_u64_xmm(CodegenCtx *ctx, u8 mf, u8 sw)
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

static void lower_ftoi_xmm(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    bool is_signed = ir_vreg_signed(ctx->mod, in->result);
    u8 sw = fconv_src_width(ctx, in->ops[0]);
    u8 mf = MF_OF(sw);
    emit_fp_source_to_xmm0(ctx, in->ops[0], sw);
    if (dw == W_QWORD && !is_signed)
    {
        emit_ftoi_u64_xmm(ctx, mf, sw);
    }
    else
    {
        emit_cvtts2i(ctx->buf, mf, R_EAX, R_XMM0, dw == W_QWORD || !is_signed);
    }
    emit_mov(ctx->buf, dw, xop_vreg(ctx, in->result), xop_reg(R_EAX));
}

static void lower_ftoi(IrInstr *in, CodegenCtx *ctx)
{
    if (fconv_src_width(ctx, in->ops[0]) == W_LD)
    {
        lower_ftoi_x87(in, ctx);
    }
    else
    {
        lower_ftoi_xmm(in, ctx);
    }
}

static void lower_fconv(IrInstr *in, CodegenCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    u8 sw = fconv_src_width(ctx, in->ops[0]);

    if (dw == W_LD || sw == W_LD)
    {
        if (sw == W_LD)
        {
            emit_fldt(ctx, x87_slot(ctx, in->ops[0]));
            if (dw == W_DWORD)
            {
                emit_fstps(ctx, vreg_slot_mem(ctx, in->result));
            }
            else
            {
                emit_fstpl(ctx, vreg_slot_mem(ctx, in->result));
            }
        }
        else if (sw == W_DWORD)
        {
            emit_flds(ctx, x87_slot(ctx, in->ops[0]));
            emit_fstpt(ctx, vreg_slot_mem(ctx, in->result));
        }
        else
        {
            emit_fldl(ctx, x87_slot(ctx, in->ops[0]));
            emit_fstpt(ctx, vreg_slot_mem(ctx, in->result));
        }
        x87_balance(ctx);
        return;
    }

    u8 mf = MF_OF(sw); /* cvt*2s prefix = source precision */

    emit_fp_source_to_xmm0(ctx, in->ops[0], sw);
    emit_sse_cvt(ctx->buf, mf, R_XMM0, R_XMM0);
    emit_sse_store(ctx->buf, MF_OF(dw), vreg_slot_mem(ctx, in->result), R_XMM0);
}

/* long-double arithmetic: fldt both, <fop>p st(1), fstpt. */
static void emit_x87_fbin(IrInstr *in, CodegenCtx *ctx)
{
    emit_fldt(ctx, x87_slot(ctx, in->ops[0]));
    emit_fldt(ctx, x87_slot(ctx, in->ops[1]));
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
            codegen_error(ctx, "unsupported long-double arithmetic opcode %s",
                          ir_opcode_name(in->opcode));
            emit_ud2(ctx->buf);
            return;
    }
    emit_fstpt(ctx, vreg_slot_mem(ctx, in->result));
    x87_balance(ctx);
}

/* FP arithmetic: xmm0 = lhs <op> rhs, stored back to the result slot. */
static void lower_fbin(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    if (w == W_LD)
    {
        emit_x87_fbin(in, ctx);
        return;
    }
    u8 mf = MF_OF(w);
    const ArithSpec *s = &arith_specs[in->opcode];
    ByteBuf *b = ctx->buf;

    emit_fp_source_to_xmm0(ctx, in->ops[0], w);

    X86Operand rhs = lowered_operand(ctx, in->ops[1], R_EAX);
    if (rhs.kind == XOP_MEM)
    {
        emit_sse_op_mem(b, mf, s->mem, R_XMM0, rhs.u.mem);
    }
    else
    {
        if (rhs.kind == XOP_IMM)
        {
            emit_mov(b, fp_imm_load_width(w, rhs.u.imm), xop_reg(R_EAX), rhs);
        }
        emit_movd_to_xmm(b, R_XMM1, R_EAX, w != W_DWORD);
        emit_sse_op_reg(b, mf, s->mem, R_XMM0, R_XMM1);
    }
    emit_sse_store(b, mf, vreg_slot_mem(ctx, in->result), R_XMM0);
}

/* FNEG: flip the sign bit with xorp[sd], keeping the rest of the value. */
static void emit_x87_fneg(CodegenCtx *ctx, IrInstr *in)
{
    emit_fldt(ctx, x87_slot(ctx, in->ops[0]));
    emit_fchs(ctx);
    emit_fstpt(ctx, vreg_slot_mem(ctx, in->result));
    x87_balance(ctx);
}

static void lower_fneg(IrInstr *in, CodegenCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    if (w == W_LD)
    {
        emit_x87_fneg(ctx, in);
        return;
    }
    ByteBuf *b = ctx->buf;

    emit_fp_source_to_xmm0(ctx, in->ops[0], w);
    if (w == W_DWORD)
    {
        emit_mov(b, W_DWORD, xop_reg(R_ECX), xop_imm(F32_SIGN_BIT));
        emit_movd_to_xmm(b, R_XMM1, R_ECX, false);
        emit_sse_xor(b, 0, R_XMM0, R_XMM1);
    }
    else
    {
        emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_imm((i64) F64_SIGN_BIT));
        emit_movd_to_xmm(b, R_XMM1, R_ECX, true);
        emit_sse_xor(b, X86_SSE_66, R_XMM0, R_XMM1);
    }
    emit_sse_store(b, MF_OF(w), vreg_slot_mem(ctx, in->result), R_XMM0);
}

/* FP compare setcc matrix; join=0 predicates need no PF (unordered) fixup. */
typedef struct
{
    u8 cc;   /* primary condition code */
    u8 join; /* OP_AND/OP_OR to combine the unordered (PF) flag; 0 = none */
} FcmpSpec;

static const FcmpSpec fcmp_specs[OP_FCMP_GE + 1] = {
    [OP_FCMP_EQ] = {CC_E, OP_AND}, [OP_FCMP_NE] = {CC_NE, OP_OR},  [OP_FCMP_LT] = {CC_B, OP_AND},
    [OP_FCMP_GT] = {CC_A, 0},      [OP_FCMP_LE] = {CC_BE, OP_AND}, [OP_FCMP_GE] = {CC_AE, 0},
};

static void emit_fcmp_result(CodegenCtx *ctx, IrOpcode opcode, u8 rw, u32 result_vreg)
{
    const FcmpSpec *spec = &fcmp_specs[opcode];
    ByteBuf *b = ctx->buf;
    emit_setcc_reg(b, spec->cc, R_EAX);
    if (spec->join)
    {
        u8 pf_cc = spec->join == OP_AND ? CC_NP : CC_P;
        emit_setcc_reg(b, pf_cc, R_EDX);
        emit_binop_rhs(b, W_BYTE, &arith_specs[spec->join], R_EAX, xop_reg(R_EDX));
    }
    emit_movzbl_al_eax(b);
    emit_mov(b, rw, xop_vreg(ctx, result_vreg), xop_reg(R_EAX));
}

static void emit_x87_fcmp(CodegenCtx *ctx, IrInstr *in)
{
    ByteBuf *b = ctx->buf;
    IrOperand rhs = in->ops[1];
    if (rhs.is_imm)
    {
        if (rhs.u.imm != 0)
        {
            codegen_error(ctx, "nonzero immediate in a long-double compare");
            emit_ud2(b);
            return;
        }
        emit_fldz(ctx);
    }
    else
    {
        emit_fldt(ctx, x87_slot(ctx, rhs));
    }
    emit_fldt(ctx, x87_slot(ctx, in->ops[0]));
    emit_fucomip(ctx);
    emit_fstp_st0(ctx);
    emit_fcmp_result(ctx, in->opcode, vreg_width(ctx, in->result), in->result);
    x87_balance(ctx);
}

static bool emit_fcmp_rhs_xmm1(CodegenCtx *ctx, IrOperand rhs, u8 sw)
{
    ByteBuf *b = ctx->buf;
    u8 mf = MF_OF(sw);
    X86Operand src = lowered_operand(ctx, rhs, R_EAX);
    if (src.kind == XOP_MEM)
    {
        emit_sse_load(b, mf, R_XMM1, src.u.mem);
    }
    else if (src.kind == XOP_IMM)
    {
        emit_mov(b, fp_imm_load_width(sw, src.u.imm), xop_reg(R_EAX), src);
        emit_movd_to_xmm(b, R_XMM1, R_EAX, sw != W_DWORD);
    }
    else
    {
        codegen_error(ctx, "unexpected operand class for a floating-point compare");
        return false;
    }
    return true;
}

static void lower_fcmp(IrInstr *in, CodegenCtx *ctx)
{
    u8 rw = vreg_width(ctx, in->result);
    u8 sw = fconv_src_width(ctx, in->ops[0]);
    if (sw == W_LD)
    {
        emit_x87_fcmp(ctx, in);
        return;
    }
    ByteBuf *b = ctx->buf;
    emit_fp_source_to_xmm0(ctx, in->ops[0], sw);
    if (!emit_fcmp_rhs_xmm1(ctx, in->ops[1], sw))
    {
        return;
    }
    emit_sse_ucomis(b, sw, R_XMM0, R_XMM1);
    emit_fcmp_result(ctx, in->opcode, rw, in->result);
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
        emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, in->result), xop_reg(R_EDX));
        return;
    }

    if (stride == 1 || stride == 2 || stride == 4 || stride == 8)
    {
        X86Mem base = load_ptr(ctx, in->ops[0]);
        emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
        X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = (u8) stride, .disp = 0};
        emit_lea(ctx->buf, R_EDX, scaled);
        emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, in->result), xop_reg(R_EDX));
        return;
    }

    /* General stride: index *= stride, then lea (base, index, 1). */
    X86Mem base = load_ptr(ctx, in->ops[0]);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), lowered_operand(ctx, in->ops[1], R_ECX));
    emit_imul_imm(ctx->buf, W_QWORD, R_ECX, stride);
    X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = 1, .disp = 0};
    emit_lea(ctx->buf, R_EDX, scaled);
    emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, in->result), xop_reg(R_EDX));
}

static void lower_alloca(IrInstr *in, CodegenCtx *ctx)
{
    i64 aligned = ALIGN_UP(in->ops[0].u.imm, STACK_ALIGN);
    emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(aligned));
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_EDX), xop_reg(R_ESP));
    emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, in->result), xop_reg(R_EDX));
}

static void lower_memcpy(IrInstr *in, CodegenCtx *ctx)
{
    (void) load_ptr(ctx, in->ops[0]);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_EDI), xop_reg(R_EAX));
    (void) load_ptr(ctx, in->ops[1]);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ESI), xop_reg(R_EAX));
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(in->ops[2].u.imm));
    bytebuf_append(ctx->buf, X86_REP);
    bytebuf_append(ctx->buf, X86_MOVSB);
}

static void lower_instr(IrInstr *in, CodegenCtx *ctx)
{
    LowerFn fn = lower_fns[in->opcode];
    if (!fn)
    {
        codegen_error(ctx, "unsupported opcode %s", ir_opcode_name(in->opcode));
        emit_ud2(ctx->buf);
        return;
    }
    fn(in, ctx);
}

static FrameInfo frame_plan(IrFunction *f, IrModule *mod, Arena *arena)
{
    LiveIntervals ivs = liveinterval_compute(f, mod, arena);
    RegAllocation *alloc = regalloc_all_spilled(f, &ivs, arena);
    u32 total = alloc->frame_size;
    FrameInfo fr = {0};
    fr.slot_off = alloc->slot_map;
    if (f->is_variadic)
    {
        /* The va_list register save area sits below the vreg slots; the prologue spills into it. */
        fr.save_area_off = total + VA_SAVE_BYTES;
        total = fr.save_area_off;
    }
    fr.frame_size = ALIGN_UP(total, STACK_ALIGN);
    return fr;
}

static void emit_param_shuffle(ByteBuf *buf, IrFunction *f, IrModule *mod, const u32 *slot_off)
{
    size_t nparams = vec_size(f->params);
    u32 gp_used = 0, fp_used = 0, ovf = 0;
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        u8 w = mod->widths[p->vreg];
        bool is_fp = mod->floatness[p->vreg];
        X86Operand dst = xop_vreg_slot(p->vreg, slot_off);
        if (is_fp)
        {
            if (w == 16)
            {
                /* X87 params arrive on the caller's stack at a 16-aligned slot. */
                ovf = ALIGN_UP(ovf, W_LD);
                emit_mov16(buf, x86_mem_rbp(STACK_PARAM_BASE + (i32) ovf), dst.u.mem);
                ovf += 16;
            }
            else if (fp_used < VA_NXMM)
            {
                /* xmm → slot; the value lives in the low 32/64 bits. */
                emit_sse_store(buf, MF_OF(w), dst.u.mem, fp_used);
                fp_used++;
            }
            else
            {
                emit_sse_load(buf, MF_OF(w), R_XMM0, x86_mem_rbp(STACK_PARAM_BASE + (i32) ovf));
                emit_sse_store(buf, MF_OF(w), dst.u.mem, R_XMM0);
                ovf += 8;
            }
        }
        else
        {
            if (gp_used < VA_NGP)
            {
                emit_mov(buf, w, dst, xop_reg(x86_64_target()->gp_args[gp_used]));
                gp_used++;
            }
            else
            {
                emit_mov(buf, w, xop_reg(R_EAX),
                         xop_mem(x86_mem_rbp(STACK_PARAM_BASE + (i32) ovf)));
                emit_mov(buf, w, dst, xop_reg(R_EAX));
                ovf += 8;
            }
        }
    }
}

static void emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, FrameInfo *fr)
{
    size_t before_push = bytebuf_len(buf);
    bytebuf_append(buf, X86_PUSH_RBP);
    fr->off_push = (u32) (bytebuf_len(buf) - before_push);
    emit_mov(buf, W_QWORD, xop_reg(R_EBP), xop_reg(R_ESP));
    fr->off_mov = (u32) (bytebuf_len(buf) - before_push);
    emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(fr->frame_size));
    fr->off_sub = (u32) (bytebuf_len(buf) - before_push);

    /* Spill register args for va_arg: GP first, then the xmm slots. */
    if (f->is_variadic)
    {
        for (size_t i = 0; i < VA_NGP; i++)
        {
            emit_mov(buf, W_QWORD,
                     xop_mem(x86_mem_rbp(-(i32) fr->save_area_off + (i32) i * VA_GP_STRIDE)),
                     xop_reg(x86_64_target()->gp_args[i]));
        }
        for (size_t i = 0; i < VA_NXMM; i++)
        {
            emit_sse_store(
                buf, MF_DOUBLE,
                x86_mem_rbp(-(i32) fr->save_area_off + VA_GP_BYTES + (i32) i * VA_XMM_STRIDE), i);
        }
    }

    emit_param_shuffle(buf, f, mod, fr->slot_off);
}

static bool is_terminator(IrOpcode op)
{
    return op == OP_RET || op == OP_UNREACHABLE || op == OP_BR || op == OP_BRCOND ||
           op == OP_SWITCH;
}

static void add_phi_copies(CodegenCtx *ctx, IrInstr *phi)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        IrPhiEntry *entry = &phi->extra.phi.entries[e];
        IrBlock *pred = block_by_label(ctx->blocks, entry->label);
        ASSERT(pred != NULL && "phi entry names a real predecessor in this function");
        size_t pj = block_index_of(ctx->blocks, pred);
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

/* Copy a PHI source into `dst_vreg` at its width (no stale high bytes). */
static void emit_phi_copy(CodegenCtx *ctx, IrOperand src, u32 dst_vreg)
{
    if (src.is_global)
    {
        emit_global_addr(ctx->buf, src.u.global_index, ctx->global_patches, ctx->arena);
        emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, dst_vreg), xop_reg(R_EAX));
        return;
    }
    if (src.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, src.u.func_name, ctx->func_patches, ctx->arena);
        emit_mov(ctx->buf, W_QWORD, xop_vreg(ctx, dst_vreg), xop_reg(R_EAX));
        return;
    }
    u8 dw = vreg_width(ctx, dst_vreg);
    if (dw == W_LD)
    {
        X86Operand sv = xop_vreg(ctx, src.u.vreg);
        emit_mov16(ctx->buf, sv.u.mem, vreg_slot_mem(ctx, dst_vreg));
        return;
    }
    emit_mov(ctx->buf, dw, xop_reg(R_EAX), lowered_operand(ctx, src, R_ECX));
    emit_mov(ctx->buf, dw, xop_vreg(ctx, dst_vreg), xop_reg(R_EAX));
}

/* Record where `in` starts lowering; skip line-0 (pre-statement) rows. */
static void record_line_entry(CodegenCtx *ctx, IrInstr *in)
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

static void emit_block(IrBlock *blk, size_t bi, CodegenCtx *ctx)
{
    ctx->block_offsets[bi] = bytebuf_len(ctx->buf);
    size_t ninstr = vec_size(blk->instrs);

    /* Non-terminator instructions first; the terminator and successor phi copies come last. */
    size_t ii = 0;
    for (; ii < ninstr; ii++)
    {
        if (is_terminator(((IrInstr *) vec_get(blk->instrs, ii))->opcode))
        {
            break;
        }
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
        record_line_entry(ctx, in);
        lower_instr(in, ctx);
    }
    ASSERT(ii < ninstr && "every block ends in a terminator");

    /* Phi copies for the successors run just before the terminator. */
    size_t npc = vec_size(ctx->phi_copies[bi]);
    for (size_t pi = 0; pi < npc; pi++)
    {
        PhiCopy *pc = (PhiCopy *) vec_get(ctx->phi_copies[bi], pi);
        emit_phi_copy(ctx, pc->src, pc->dst_vreg);
    }
    IrInstr *term = (IrInstr *) vec_get(blk->instrs, ii);
    record_line_entry(ctx, term);
    lower_instr(term, ctx);
    ASSERT(ii + 1 == ninstr && "the terminator is the last instruction in a block");
}

static void resolve_block_patches(CodegenCtx *ctx)
{
    size_t npatches = vec_size(ctx->block_patches);
    for (size_t pi = 0; pi < npatches; pi++)
    {
        PatchSite *site = (PatchSite *) vec_get(ctx->block_patches, pi);
        IrBlock *target = block_by_label(ctx->blocks, site->target);
        ASSERT(target != NULL && "branch target names a block the IR builder created");
        size_t ti = block_index_of(ctx->blocks, target);
        patch_rel32(ctx->buf, site->offset, ctx->block_offsets[ti]);
    }
}

/* Append jump tables after the function body; entries store target−table_base offset deltas. */
static void emit_switch_tables(CodegenCtx *ctx, ByteBuf *buf)
{
    size_t nst = vec_size(ctx->switch_tables);
    if (nst == 0)
    {
        return;
    }
    bytebuf_align(buf, W_QWORD);
    for (size_t t = 0; t < nst; t++)
    {
        SwitchTableRec *rec = (SwitchTableRec *) vec_get(ctx->switch_tables, t);
        size_t table_off = bytebuf_len(buf);
        for (u32 i = 0; i < rec->nentries; i++)
        {
            IrBlock *target = block_by_label(ctx->blocks, rec->targets[i]);
            ASSERT(target != NULL && "switch case targets a real block");
            size_t ti = block_index_of(ctx->blocks, target);
            bytebuf_append_u64(buf, (u64) ((i64) ctx->block_offsets[ti] - (i64) table_off));
        }
        patch_rel32(buf, rec->disp_field_off, table_off);
    }
}

static void emit_func_mc(IrFunction *f, CodegenFunc *cf, IrModule *mod, Arena *arena, bool debug)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);
    Vec *global_patches = vec_new(arena);
    Vec *func_patches = vec_new(arena);
    Vec *lines = debug ? vec_new(arena) : NULL;

    BlockIndex *blocks = index_blocks(f, arena);
    FrameInfo fr = frame_plan(f, mod, arena);
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
        .blocks = blocks,
        .block_offsets = arena_alloc(arena, nblocks * sizeof(size_t), sizeof(size_t)),
        .patches = patches,
        .block_patches = block_patches,
        .global_patches = global_patches,
        .func_patches = func_patches,
        .switch_tables = vec_new(arena),
        .save_area_off = fr.save_area_off,
        .slot_off = fr.slot_off,
        .lines = lines,
        .debug = debug,
        .fpu_depth = 0,
    };

    collect_phi_copies(f, &ctx);

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        emit_block((IrBlock *) vec_get(f->blocks, bi), bi, &ctx);
    }

    emit_switch_tables(&ctx, buf);
    resolve_block_patches(&ctx);

    cf->name = f->name;
    cf->bytes = buf;
    cf->offset = 0;
    cf->patches = patches;
    cf->global_patches = global_patches;
    cf->func_patches = func_patches;
    cf->lines = lines;
    cf->frame.off_push = fr.off_push;
    cf->frame.off_mov = fr.off_mov;
    cf->frame.off_sub = fr.off_sub;
    cf->is_static = f->is_static;
    cf->func = f;
    cf->slot_off = fr.slot_off;
}

static size_t emit_all_funcs(CodegenModule *cm, IrModule *ir, Arena *arena, bool debug)
{
    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        emit_func_mc(f, cf, ir, arena, debug);
        vec_push(cm->funcs, cf);
    }
    return nfuncs;
}

static void assign_func_offsets(CodegenModule *cm, size_t nfuncs)
{
    size_t function_offset = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        cf->offset = function_offset;
        function_offset += bytebuf_len(cf->bytes);
    }
}

/* Resolve direct calls by name; undefined targets become SHN_UNDEF + R_X86_64_PLT32 in elf.c. */
static void resolve_direct_calls(CodegenModule *cm, size_t nfuncs, Arena *arena)
{
    StrMap *func_by_name = strmap_new(arena);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        strmap_set(func_by_name, cf->name, cf);
    }
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            PatchSite *site = (PatchSite *) vec_get(cf->patches, pi);
            CodegenFunc *target = (CodegenFunc *) strmap_get(func_by_name, site->target);
            if (!target)
            {
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
}

CodegenModule *codegen_ir_to_machine(IrModule *ir, const CodegenConfig *cfg, Arena *arena)
{
    bool debug = cfg && cfg->debug;
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->funcs = vec_new(arena);
    cm->globals = ir->globals;
    cm->extern_calls = vec_new(arena);

    size_t nfuncs;
    if (cfg && cfg->backend == CG_LINEAR)
    {
        nfuncs = x86_lower_module(cm, ir, debug, arena);
    }
    else
    {
        nfuncs = emit_all_funcs(cm, ir, arena, debug);
    }
    assign_func_offsets(cm, nfuncs);
    resolve_direct_calls(cm, nfuncs, arena);
    return cm;
}

static u64 append_global(ByteBuf *buf, IrGlobal *g)
{
    u64 off = align_up(bytebuf_len(buf), g->align);
    while ((u64) bytebuf_len(buf) < off)
    {
        bytebuf_append(buf, 0);
    }
    if (g->init_data)
    {
        bytebuf_append_bytes(buf, g->init_data, g->init_len);
    }
    return off;
}

u64 *codegen_global_offsets(CodegenModule *cm, ByteBuf *rodata, ByteBuf *data, Arena *arena)
{
    size_t nglobals = cm->globals ? vec_size(cm->globals) : 0;
    u64 *global_off = arena_alloc(arena, (nglobals ? nglobals : 1) * sizeof(u64), sizeof(u64));
    u64 bss_size = 0;
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        switch (g->section)
        {
            case IR_SECTION_RODATA:
                global_off[i] = append_global(rodata, g);
                break;
            case IR_SECTION_DATA:
                global_off[i] = append_global(data, g);
                break;
            case IR_SECTION_BSS:
                global_off[i] = align_up(bss_size, g->align);
                bss_size = global_off[i] + type_sizeof(g->type);
                break;
        }
    }
    return global_off;
}
