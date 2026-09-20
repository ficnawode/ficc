#include "x86_lower.h"
#include "liveinterval.h"
#include "regalloc.h"
#include "target.h"
#include "type.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "util/vec.h"
#include "x86_emit.h"
#include "x86_frame.h"
#include <stdint.h>
#include <stdio.h>

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
    Vec *global_patches;
    Vec *func_patches;
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

static void emit_block_linear(IrBlock *blk, X86LowerCtx *ctx)
{
    size_t ninstr = vec_size(blk->instrs);
    for (size_t ii = 0; ii < ninstr; ii++)
    {
        lower_instr((IrInstr *) vec_get(blk->instrs, ii), ctx);
    }
}

static void lower_func(IrFunction *f, CodegenFunc *cf, IrModule *mod, Arena *arena, bool debug)
{
    ByteBuf *buf = arena_alloc(arena, sizeof(ByteBuf), sizeof(void *));
    bytebuf_init(buf, arena);
    Vec *patches = vec_new(arena);
    Vec *global_patches = vec_new(arena);
    Vec *func_patches = vec_new(arena);

    const TargetDesc *target = x86_64_target();
    LiveIntervals set = liveinterval_compute(f, mod, arena);
    RegAllocation *alloc = regalloc_linear(f, &set, target, arena);
    LinearFrame frame = {0};
    x86_frame_plan(alloc, f, target, &frame);

    X86LowerCtx ctx = {
        .func = f,
        .mod = mod,
        .arena = arena,
        .buf = buf,
        .target = target,
        .alloc = alloc,
        .frame = &frame,
        .patches = patches,
        .global_patches = global_patches,
        .func_patches = func_patches,
    };

    x86_frame_emit_prologue(buf, f, mod, alloc, &frame);

    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        emit_block_linear((IrBlock *) vec_get(f->blocks, bi), &ctx);
    }

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
