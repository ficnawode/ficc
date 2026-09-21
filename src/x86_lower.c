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
#include <stdint.h>
#include <stdio.h>

typedef struct
{
    IrOperand src;
    u32 dst_vreg;
} LowerPhiCopy;

/* Jump table appended to the function's .text, indexed by `val - min`. */
typedef struct
{
    size_t disp_field_off;
    u32 nentries;
    const char **targets;
} LowerSwitchTable;

typedef struct
{
    IrFunction *func;
    IrModule *mod;
    Arena *arena;
    ByteBuf *buf;
    const TargetDesc *target;
    const RegAllocation *alloc;
    const LinearFrame *frame;
    Vec *patches;
    Vec *block_patches;
    Vec *global_patches;
    Vec *func_patches;
    Vec **phi_copies;
    Vec *switch_tables;
    size_t *block_offsets;
    StrMap *label_to_index;
} X86LowerCtx;

#define STACK_ALIGN 16

static u32 align_up(u32 n, u32 a)
{
    return (n + a - 1) / a * a;
}

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
    RegLoc l = loc_of(ctx->alloc, op);
    u8 w = vreg_width(ctx, op.u.vreg);
    if (l.kind == LOC_REG)
    {
        if (l.reg != reg)
        {
            emit_mov(b, w, xop_reg(reg), xop_reg(l.reg));
        }
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
    RegLoc l = loc_of(ctx->alloc, op);
    if (l.kind == LOC_REG)
    {
        return xop_reg(l.reg);
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
    return loc_of(ctx->alloc, ir_operand_vreg(in->result));
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

static void lower_binary(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    const ArithSpec *s = &arith_specs[in->opcode];
    RegLoc rl = result_loc(ctx, in);
    u8 dst = rl.kind == LOC_REG ? rl.reg : R_EAX;
    force_to_reg(ctx, in->ops[0], dst);
    X86Operand rhs = resolve_rhs(ctx, in->ops[1], w, R_ECX);
    emit_binop_rhs(ctx->buf, w, s, dst, rhs);
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
    force_to_reg(ctx, in->ops[1], R_ECX);
    emit_shift_cl(ctx->buf, w, dst, shift_digit[in->opcode]);
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
    force_to_reg(ctx, in->ops[1], R_ECX);
    if (is_unsigned)
    {
        emit_div(ctx->buf, w, R_ECX);
    }
    else
    {
        emit_idiv(ctx->buf, w, R_ECX);
    }
    u8 src = (in->opcode == OP_SREM || in->opcode == OP_UREM) ? R_EDX : R_EAX;
    store_reg_result(ctx, in, w, src);
}

static void lower_icmp(IrInstr *in, X86LowerCtx *ctx)
{
    u8 rw = vreg_width(ctx, in->result);
    IrOperand lhs = in->ops[0];
    IrOperand rhs = in->ops[1];
    u8 w0 = operand_width(ctx, lhs);
    u8 w1 = operand_width(ctx, rhs);
    u8 w = MAX(w0, w1);

    force_to_reg(ctx, lhs, R_EAX);
    if (!lhs.is_imm && w0 < w && w0 < 4)
    {
        emit_movzx(ctx->buf, w0, w, R_EAX, xop_reg(R_EAX));
    }
    force_to_reg(ctx, rhs, R_ECX);
    if (!rhs.is_imm && w1 < w && w1 < 4)
    {
        emit_movzx(ctx->buf, w1, w, R_ECX, xop_reg(R_ECX));
    }
    emit_binop_rhs(ctx->buf, w, &cmp_spec, R_EAX, xop_reg(R_ECX));
    emit_setcc(ctx->buf, icmp_cc[in->opcode]);
    emit_movzbl_al_eax(ctx->buf);
    store_reg_result(ctx, in, rw, R_EAX);
}

static void lower_trunc(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    force_to_reg(ctx, in->ops[0], R_EAX);
    store_reg_result(ctx, in, w, R_EAX);
}

static void lower_zext(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    force_to_reg(ctx, in->ops[0], R_EAX);
    if (!in->ops[0].is_imm)
    {
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        if (sw < 4)
        {
            emit_movzx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
        }
    }
    store_reg_result(ctx, in, dw, R_EAX);
}

static void lower_sext(IrInstr *in, X86LowerCtx *ctx)
{
    u8 dw = vreg_width(ctx, in->result);
    force_to_reg(ctx, in->ops[0], R_EAX);
    if (!in->ops[0].is_imm)
    {
        u8 sw = vreg_width(ctx, in->ops[0].u.vreg);
        emit_movsx(ctx->buf, sw, dw, R_EAX, xop_reg(R_EAX));
    }
    store_reg_result(ctx, in, dw, R_EAX);
}

static void lower_load(IrInstr *in, X86LowerCtx *ctx)
{
    u8 w = vreg_width(ctx, in->result);
    ASSERT(w != W_LD && "long double loads lower in the x87 module");
    X86Mem addr = pointer_in_rax(ctx, in->ops[0]);
    RegLoc rl = result_loc(ctx, in);
    if (rl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, w, xop_reg(rl.reg), xop_mem(addr));
        return;
    }
    emit_mov(ctx->buf, w, xop_reg(R_ECX), xop_mem(addr));
    emit_mov(ctx->buf, w, xop_mem(rbp_mem(rl.disp)), xop_reg(R_ECX));
}

static void lower_store(IrInstr *in, X86LowerCtx *ctx)
{
    u32 w = (u32) in->ops[2].u.imm;
    ASSERT(w != W_LD && "long double stores lower in the x87 module");
    force_to_reg(ctx, in->ops[0], R_ECX);
    X86Mem addr = pointer_in_rax(ctx, in->ops[1]);
    emit_mov(ctx->buf, (u8) w, xop_mem(addr), xop_reg(R_ECX));
}

static void lower_gep(IrInstr *in, X86LowerCtx *ctx)
{
    i64 stride = in->ops[2].u.imm;
    if (in->ops[1].is_imm && in->ops[1].u.imm == 1)
    {
        X86Mem base = pointer_in_rax(ctx, in->ops[0]);
        X86Mem m = {.base = base.base, .index = NO_REG, .scale = 1, .disp = (i32) stride};
        emit_lea(ctx->buf, R_EDX, m);
        store_reg_result(ctx, in, W_QWORD, R_EDX);
        return;
    }
    X86Mem base = pointer_in_rax(ctx, in->ops[0]);
    force_to_reg(ctx, in->ops[1], R_ECX);
    if (stride == 1 || stride == 2 || stride == 4 || stride == 8)
    {
        X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = (u8) stride, .disp = 0};
        emit_lea(ctx->buf, R_EDX, scaled);
    }
    else
    {
        emit_imul_imm(ctx->buf, W_QWORD, R_ECX, stride);
        X86Mem scaled = {.base = base.base, .index = R_ECX, .scale = 1, .disp = 0};
        emit_lea(ctx->buf, R_EDX, scaled);
    }
    store_reg_result(ctx, in, W_QWORD, R_EDX);
}

static void lower_alloca(IrInstr *in, X86LowerCtx *ctx)
{
    u32 aligned = align_up((u32) in->ops[0].u.imm, STACK_ALIGN);
    emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(aligned));
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_reg(R_ESP));
    store_reg_result(ctx, in, W_QWORD, R_ECX);
}

static void lower_memcpy(IrInstr *in, X86LowerCtx *ctx)
{
    (void) pointer_in_rax(ctx, in->ops[0]);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_EDI), xop_reg(R_EAX));
    (void) pointer_in_rax(ctx, in->ops[1]);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ESI), xop_reg(R_EAX));
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(in->ops[2].u.imm));
    bytebuf_append(ctx->buf, X86_REP);
    bytebuf_append(ctx->buf, X86_MOVSB);
}

static void lower_ret(IrInstr *in, X86LowerCtx *ctx)
{
    if (in->nops > 0)
    {
        ASSERT(!type_is_fp(ctx->func->ret_type) && "floating returns lower in the x87 module");
        force_to_reg(ctx, in->ops[0], R_EAX);
    }
    else
    {
        emit_mov(ctx->buf, W_DWORD, xop_reg(R_EAX), xop_imm(0));
    }
    x86_frame_restore_callee(ctx->buf, ctx->frame);
    bytebuf_append(ctx->buf, X86_LEAVE);
    bytebuf_append(ctx->buf, X86_RET);
}

static void lower_unreachable(IrInstr *in, X86LowerCtx *ctx)
{
    (void) in;
    emit_ud2(ctx->buf);
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

static void lower_brcond(IrInstr *in, X86LowerCtx *ctx)
{
    u8 cw = operand_width(ctx, in->ops[0]);
    force_to_reg(ctx, in->ops[0], R_EAX);
    emit_test_reg(ctx->buf, cw, R_EAX);
    emit_jcc(ctx->buf, CC_E, in->extra.brcond.false_label, ctx->block_patches, ctx->arena);
    emit_jmp(ctx->buf, in->extra.brcond.true_label, ctx->block_patches, ctx->arena);
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

/* Bounds-check %rax to [min,max], subtract min, jump through the full-range table. */
static void emit_switch_table(X86LowerCtx *ctx, IrSwitchCase *cases, u32 n, i64 min, i64 max,
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
            emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm(cases[i].val));
            emit_reg_reg(ctx->buf, cmp_spec.mem, R_EAX, R_ECX);
        }
        emit_jcc(ctx->buf, CC_E, cases[i].label, ctx->block_patches, ctx->arena);
    }
    emit_jmp(ctx->buf, default_label, ctx->block_patches, ctx->arena);
}

static void lower_switch(IrInstr *in, X86LowerCtx *ctx)
{
    u32 n = in->extra.sw.ncases;
    IrSwitchCase *cases = in->extra.sw.cases;
    const char *default_label = in->extra.sw.default_label;

    bool is_signed;
    (void) load_int_operand(ctx, in->ops[0], &is_signed);

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
    X(OP_UNREACHABLE, lower_unreachable)

/* Dispatch table indexed by opcode; unlisted opcodes hit the unsupported path. */
static const LowerFn lower_fns[OP_FCMP_GE + 1] = {
#define LOWER_INIT(op, fn) [op] = fn,
    LOWER_ENTRIES(LOWER_INIT)
#undef LOWER_INIT
};

static void lower_instr(IrInstr *in, X86LowerCtx *ctx)
{
    LowerFn fn = lower_fns[in->opcode];
    if (!fn)
    {
        lower_unsupported(in, ctx);
        return;
    }
    fn(in, ctx);
}

static void store_vreg_from_reg(X86LowerCtx *ctx, u32 vreg, u8 width, u8 reg)
{
    RegLoc l = loc_of(ctx->alloc, ir_operand_vreg(vreg));
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

/* A PHI edge's copy runs at its predecessor's end, so it reads the incoming
   value at its final use and defines the phi result at the same position. */
static void emit_phi_copy(X86LowerCtx *ctx, IrOperand src, u32 dst_vreg)
{
    u8 dw = vreg_width(ctx, dst_vreg);
    ASSERT(dw != W_LD && "long double phi copies lower in the x87 module");
    RegLoc dl = loc_of(ctx->alloc, ir_operand_vreg(dst_vreg));
    if (src.is_global)
    {
        emit_global_addr_to(ctx->buf, R_EAX, src.u.global_index, ctx->global_patches, ctx->arena);
        store_vreg_from_reg(ctx, dst_vreg, dw, R_EAX);
        return;
    }
    if (src.is_func)
    {
        emit_func_addr_to(ctx->buf, R_EAX, src.u.func_name, ctx->func_patches, ctx->arena);
        store_vreg_from_reg(ctx, dst_vreg, dw, R_EAX);
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
    RegLoc sl = loc_of(ctx->alloc, src);
    if (dl.kind == LOC_REG)
    {
        if (sl.kind == LOC_REG)
        {
            emit_mov(ctx->buf, dw, xop_reg(dl.reg), xop_reg(sl.reg));
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
                vec_push(ctx->phi_copies[pj], pc);
            }
        }
    }
}

static void emit_block_linear(IrBlock *blk, size_t bi, X86LowerCtx *ctx)
{
    ctx->block_offsets[bi] = bytebuf_len(ctx->buf);
    size_t ninstr = vec_size(blk->instrs);
    size_t ii = 0;
    for (; ii < ninstr; ii++)
    {
        IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
        if (is_terminator(in->opcode))
        {
            break;
        }
        if (in->opcode != OP_PHI)
        {
            lower_instr(in, ctx);
        }
    }
    ASSERT(ii < ninstr && "every block ends in a terminator");

    size_t npc = vec_size(ctx->phi_copies[bi]);
    for (size_t pi = 0; pi < npc; pi++)
    {
        LowerPhiCopy *pc = (LowerPhiCopy *) vec_get(ctx->phi_copies[bi], pi);
        emit_phi_copy(ctx, pc->src, pc->dst_vreg);
    }

    lower_instr((IrInstr *) vec_get(blk->instrs, ii), ctx);
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
    bytebuf_align(ctx->buf, W_QWORD);
    for (size_t t = 0; t < nst; t++)
    {
        LowerSwitchTable *rec = (LowerSwitchTable *) vec_get(ctx->switch_tables, t);
        size_t table_off = bytebuf_len(ctx->buf);
        for (u32 i = 0; i < rec->nentries; i++)
        {
            size_t ti = block_index_of_label(ctx, rec->targets[i]);
            bytebuf_append_u64(ctx->buf, (u64) ((i64) ctx->block_offsets[ti] - (i64) table_off));
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

static void lower_func(IrFunction *f, CodegenFunc *cf, IrModule *mod, Arena *arena, bool debug)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);
    Vec *global_patches = vec_new(arena);
    Vec *func_patches = vec_new(arena);

    const TargetDesc *target = x86_64_target();
    LiveIntervals set = liveinterval_compute(f, mod, arena);
    RegAllocation *alloc = regalloc_linear(f, &set, target, arena);
    LinearFrame frame = {0};
    x86_frame_plan(alloc, f, target, &frame);

    size_t nblocks = vec_size(f->blocks);
    Vec **phi_copies = arena_alloc(arena, (nblocks ? nblocks : 1) * sizeof(Vec *), sizeof(void *));
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        phi_copies[bi] = vec_new(arena);
    }

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
    };

    x86_frame_emit_prologue(buf, f, mod, alloc, &frame);

    add_phi_copies(&ctx);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        emit_block_linear((IrBlock *) vec_get(f->blocks, bi), bi, &ctx);
    }
    emit_switch_tables(&ctx);
    resolve_block_patches(&ctx);

    cf->name = f->name;
    cf->bytes = buf;
    cf->offset = 0;
    cf->patches = patches;
    cf->global_patches = global_patches;
    cf->func_patches = func_patches;
    cf->lines = debug ? vec_new(arena) : NULL;
    cf->frame.off_push = frame.off_push;
    cf->frame.off_mov = frame.off_mov;
    cf->frame.off_sub = frame.off_sub;
    cf->is_static = f->is_static;
    cf->func = f;
    cf->slot_off = alloc->slot_map;
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
