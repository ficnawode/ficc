#include "x86_sysv.h"
#include "abi.h"
#include "regalloc.h"
#include "util/assert.h"
#include "util/bytebuf.h"
#include "x86_emit.h"
#include "x86_frame.h"
#include "x86_lower.h"
#include "x87.h"

#define STACK_ALIGN 16
#define SYSV_MAX_GP 6
#define SYSV_MAX_XMM 8
#define VA_GP_STRIDE 8
#define VA_XMM_STRIDE 16
#define VA_GP_BYTES (SYSV_MAX_GP * VA_GP_STRIDE)
#define VA_SAVE_BYTES (VA_GP_BYTES + SYSV_MAX_XMM * VA_XMM_STRIDE)
#define VA_FIELD_GP_OFFSET 0
#define VA_FIELD_FP_OFFSET 4
#define VA_FIELD_OVF 8
#define VA_FIELD_REGS 16
#define STACK_PARAM_BASE 16

static u32 align_up(u32 n, u32 a)
{
    return (n + a - 1) / a * a;
}

static u32 stack_align_of(Type *t)
{
    u32 a = (u32) type_alignof(t);
    return MIN(MAX(a, 8), STACK_ALIGN);
}

static void plan_stack_arg(SysvArgPlan *p, u32 *stack, u32 size, u32 align)
{
    *stack = align_up(*stack, align);
    p->on_stack = true;
    p->stack_off = *stack;
    p->stack_size = align_up(size, 8);
    *stack += p->stack_size;
}

/* Assign a record's eightbyte chunks to GP/XMM registers; false when the
   remaining registers cannot hold it and the whole record must go on the stack. */
static bool plan_record_chunks(SysvArgPlan *p, u32 *gp, u32 *fp)
{
    SysVEightByte eb = sysv_eightbyte_split(p->type);
    if (!sysv_eightbyte_register_passed(&eb) || *gp + sysv_eightbyte_gp_count(&eb) > SYSV_MAX_GP ||
        *fp + sysv_eightbyte_xmm_count(&eb) > SYSV_MAX_XMM)
    {
        return false;
    }
    p->register_passed = true;
    for (u8 lane = 0; lane < eb.neightbytes; lane++)
    {
        if (eb.classes[lane] == AC_INTEGER)
        {
            p->chunks[p->nchunks] = (SysvChunk) {SYSV_GP, (u8) (*gp)++, (u32) lane * 8};
        }
        else
        {
            p->chunks[p->nchunks] = (SysvChunk) {SYSV_SSE, (u8) (*fp)++, (u32) lane * 8};
        }
        p->nchunks++;
    }
    return true;
}

u32 sysv_plan_args(Type **types, u32 nargs, SysvArgPlan *plans, u32 *fp_used)
{
    u32 gp = 0;
    u32 fp = 0;
    u32 stack = 0;
    for (u32 i = 0; i < nargs; i++)
    {
        SysvArgPlan *p = &plans[i];
        *p = (SysvArgPlan) {0};
        p->type = types[i];

        if (type_is_record(p->type))
        {
            p->is_record = true;
            if (!plan_record_chunks(p, &gp, &fp))
            {
                plan_stack_arg(p, &stack, (u32) type_sizeof(p->type), stack_align_of(p->type));
            }
        }
        else if (type_is_fp(p->type))
        {
            if (type_sizeof(p->type) == 16)
            {
                p->is_x87_stack = true;
                plan_stack_arg(p, &stack, 16, STACK_ALIGN);
            }
            else if (fp < SYSV_MAX_XMM)
            {
                p->chunks[0] = (SysvChunk) {SYSV_SSE, (u8) fp++, 0};
                p->nchunks = 1;
            }
            else
            {
                plan_stack_arg(p, &stack, 8, 8);
            }
        }
        else if (gp < SYSV_MAX_GP)
        {
            p->chunks[0] = (SysvChunk) {SYSV_GP, (u8) gp++, 0};
            p->nchunks = 1;
        }
        else
        {
            plan_stack_arg(p, &stack, 8, 8);
        }
    }
    *fp_used = fp;
    return stack;
}

u32 sysv_param_stage_bytes(const SysvArgPlan *plan)
{
    if (!plan->is_record)
    {
        return STACK_ALIGN;
    }
    if (plan->on_stack)
    {
        return 0; /* MEMORY-class records point at the incoming stack area */
    }
    return align_up((u32) type_sizeof(plan->type), STACK_ALIGN);
}

static void emit_fp_to_xmm(X86LowerCtx *ctx, IrOperand op, u8 width, u8 xmm)
{
    RegLoc l = x86_lower_operand_loc(ctx, op);
    if (l.kind == LOC_REG)
    {
        if (l.reg != xmm)
        {
            emit_sse_op_reg(ctx->buf, MF_OF(width), X86_SSE_MOV, xmm, l.reg);
        }
    }
    else if (l.kind == LOC_MEM)
    {
        emit_sse_load(ctx->buf, MF_OF(width), xmm, x86_lower_rbp_mem(l.disp));
    }
    else
    {
        emit_ud2(ctx->buf);
    }
}

static void store_fp_home(X86LowerCtx *ctx, IrInstr *in, u8 width, u8 xmm)
{
    RegLoc rl = x86_lower_result_loc(ctx, in);
    if (rl.kind == LOC_REG)
    {
        emit_sse_op_reg(ctx->buf, MF_OF(width), X86_SSE_MOV, rl.reg, xmm);
    }
    else
    {
        emit_sse_store(ctx->buf, MF_OF(width), x86_lower_rbp_mem(rl.disp), xmm);
    }
}

static void copy_record_to_stack(X86LowerCtx *ctx, IrOperand op, u32 size, X86Mem dst)
{
    x86_lower_force_to_reg(ctx, op, R_ESI);
    emit_lea(ctx->buf, R_EDI, dst);
    emit_mov(ctx->buf, W_QWORD, xop_reg(R_ECX), xop_imm((i64) size));
    bytebuf_append(ctx->buf, X86_REP);
    bytebuf_append(ctx->buf, X86_MOVSB);
}

static void emit_stack_arg(X86LowerCtx *ctx, IrOperand op, const SysvArgPlan *p)
{
    X86Mem dst = x86_mem_rsp((i32) p->stack_off);
    if (p->is_record)
    {
        copy_record_to_stack(ctx, op, (u32) type_sizeof(p->type), dst);
        return;
    }
    if (p->is_x87_stack)
    {
        RegLoc l = x86_lower_operand_loc(ctx, op);
        ASSERT(l.kind == LOC_MEM && "x87 values are memory-only");
        emit_mov16(ctx->buf, x86_lower_rbp_mem(l.disp), dst);
        return;
    }
    if (type_is_fp(p->type))
    {
        u8 w = (u8) type_sizeof(p->type);
        emit_fp_to_xmm(ctx, op, w, R_XMM0);
        emit_sse_store(ctx->buf, MF_OF(w), dst, R_XMM0);
        return;
    }
    x86_lower_force_to_reg(ctx, op, R_EAX);
    emit_mov(ctx->buf, W_QWORD, xop_mem(dst), xop_reg(R_EAX));
}

static void emit_reg_arg(X86LowerCtx *ctx, IrOperand op, const SysvArgPlan *p)
{
    if (p->is_record)
    {
        for (u8 c = 0; c < p->nchunks; c++)
        {
            const SysvChunk *chunk = &p->chunks[c];
            x86_lower_force_to_reg(ctx, op, R_R11);
            X86Mem src = x86_mem_r11((i32) chunk->chunk_off);
            if (chunk->kind == SYSV_GP)
            {
                emit_mov(ctx->buf, W_QWORD, xop_reg(ctx->target->gp_args[chunk->reg]),
                         xop_mem(src));
            }
            else
            {
                emit_sse_load(ctx->buf, MF_DOUBLE, chunk->reg, src);
            }
        }
        return;
    }
    if (type_is_fp(p->type))
    {
        emit_fp_to_xmm(ctx, op, (u8) type_sizeof(p->type), p->chunks[0].reg);
        return;
    }
    x86_lower_force_to_reg(ctx, op, ctx->target->gp_args[p->chunks[0].reg]);
}

typedef enum
{
    GP_ARG_REG,  /* source is a vreg already in a register */
    GP_ARG_MEM,  /* source is a spilled vreg */
    GP_ARG_PURE, /* immediate, global, or function address */
} GpArgKind;

typedef struct
{
    IrOperand op;
    u8 dst;
    u8 width;
    GpArgKind kind;
    u8 src_reg;
    i32 disp;
    bool done;
} GpArgMove;

static GpArgMove resolve_gp_arg(X86LowerCtx *ctx, IrOperand op, u8 dst, u8 width)
{
    GpArgMove m = {.op = op, .dst = dst, .width = width, .done = false};
    if (op.is_imm || op.is_global || op.is_func)
    {
        m.kind = GP_ARG_PURE;
        return m;
    }
    RegLoc l = x86_lower_operand_loc(ctx, op);
    if (l.kind == LOC_REG)
    {
        m.kind = GP_ARG_REG;
        m.src_reg = l.reg;
    }
    else if (l.kind == LOC_REMAT)
    {
        m.kind = GP_ARG_PURE; /* recomputed from %rbp; touches no argument lane */
    }
    else
    {
        m.kind = GP_ARG_MEM;
        m.disp = l.disp;
    }
    return m;
}

static void emit_gp_arg_move(X86LowerCtx *ctx, const GpArgMove *m)
{
    if (m->kind == GP_ARG_PURE)
    {
        x86_lower_force_to_reg(ctx, m->op, m->dst);
    }
    else if (m->kind == GP_ARG_MEM)
    {
        emit_mov(ctx->buf, m->width, xop_reg(m->dst), xop_mem(x86_lower_rbp_mem(m->disp)));
    }
    else if (m->src_reg != m->dst)
    {
        emit_mov(ctx->buf, m->width, xop_reg(m->dst), xop_reg(m->src_reg));
    }
}

/* A destination read by another pending move must wait; when every remaining
   move is blocked the cycle is broken through %rax (reserved, never a lane). */
static bool gp_arg_move_ready(const GpArgMove *moves, u32 n, u32 i)
{
    for (u32 j = 0; j < n; j++)
    {
        if (j != i && !moves[j].done && moves[j].kind == GP_ARG_REG &&
            moves[j].src_reg == moves[i].dst)
        {
            return false;
        }
    }
    return true;
}

static void break_gp_arg_cycle(X86LowerCtx *ctx, GpArgMove *moves, u32 n)
{
    u32 c = 0;
    while (moves[c].done || moves[c].kind != GP_ARG_REG)
    {
        c++;
    }
    emit_mov(ctx->buf, moves[c].width, xop_reg(R_EAX), xop_reg(moves[c].src_reg));
    u8 saved = moves[c].src_reg;
    for (u32 i = 0; i < n; i++)
    {
        if (!moves[i].done && moves[i].kind == GP_ARG_REG && moves[i].src_reg == saved)
        {
            moves[i].src_reg = R_EAX;
        }
    }
}

/* Place scalar GP arguments as a parallel copy: an argument may already ride
   its own lane (collect_arg_prefs), and the ones that do not must be moved
   without clobbering a sibling that is still to be read. */
static void emit_gp_args_parallel(X86LowerCtx *ctx, IrInstr *in, const SysvArgPlan *plans,
                                  u32 nargs)
{
    GpArgMove *moves =
        arena_alloc(ctx->arena, (nargs ? nargs : 1) * sizeof(GpArgMove), _Alignof(GpArgMove));
    u32 n = 0;
    for (u32 i = 0; i < nargs; i++)
    {
        if (plans[i].on_stack || plans[i].is_record || type_is_fp(plans[i].type))
        {
            continue;
        }
        IrOperand op = in->extra.call.args[i];
        u8 width = x86_lower_operand_width(ctx, op);
        moves[n++] = resolve_gp_arg(ctx, op, ctx->target->gp_args[plans[i].chunks[0].reg], width);
    }
    u32 remaining = n;
    while (remaining > 0)
    {
        bool progress = false;
        for (u32 i = 0; i < n; i++)
        {
            if (moves[i].done || !gp_arg_move_ready(moves, n, i))
            {
                continue;
            }
            emit_gp_arg_move(ctx, &moves[i]);
            moves[i].done = true;
            remaining--;
            progress = true;
        }
        if (!progress)
        {
            break_gp_arg_cycle(ctx, moves, n);
        }
    }
}

static void store_raw_result(X86LowerCtx *ctx, IrInstr *in, u8 reg, bool is_fp)
{
    RegLoc rl = x86_lower_result_loc(ctx, in);
    if (rl.kind == LOC_REG && is_fp)
    {
        emit_movd_to_xmm(ctx->buf, rl.reg, reg, true);
    }
    else if (rl.kind == LOC_REG)
    {
        emit_mov(ctx->buf, W_QWORD, xop_reg(rl.reg), xop_reg(reg));
    }
    else
    {
        emit_mov(ctx->buf, W_QWORD, xop_mem(x86_lower_rbp_mem(rl.disp)), xop_reg(reg));
    }
}

static void store_call_result(X86LowerCtx *ctx, IrInstr *in)
{
    if (in->result == NO_VREG)
    {
        return;
    }
    u8 w = x86_lower_vreg_width(ctx, in->result);
    if (!ir_vreg_float(ctx->mod, in->result))
    {
        x86_lower_store_reg_result(ctx, in, w, R_EAX);
    }
    else if (w == 16)
    {
        RegLoc rl = x86_lower_result_loc(ctx, in);
        ASSERT(rl.kind == LOC_MEM && "x87 results are memory-only");
        x87_emit_fstpt(ctx->buf, x86_lower_rbp_mem(rl.disp));
    }
    else
    {
        store_fp_home(ctx, in, w, R_XMM0);
    }
}

static void emit_call_target(X86LowerCtx *ctx, IrInstr *in)
{
    if (in->extra.call.is_indirect)
    {
        x86_lower_force_to_reg(ctx, in->extra.call.callee, R_R11);
        emit_call_reg(ctx->buf, R_R11);
    }
    else
    {
        emit_call(ctx->buf, in->extra.call.name, ctx->patches, ctx->arena);
    }
}

void x86_sysv_lower_call(IrInstr *in, X86LowerCtx *ctx)
{
    u32 nargs = in->extra.call.nargs;
    u32 nalloc = MAX(nargs, 1);
    SysvArgPlan *plans =
        arena_alloc(ctx->arena, nalloc * sizeof(SysvArgPlan), _Alignof(SysvArgPlan));
    Type **types = arena_alloc(ctx->arena, nalloc * sizeof(Type *), sizeof(Type *));
    for (u32 i = 0; i < nargs; i++)
    {
        types[i] = in->extra.call.arg_types[i];
    }

    u32 fp_used = 0;
    u32 raw_stack = sysv_plan_args(types, nargs, plans, &fp_used);
    u32 total_stack = align_up(raw_stack, STACK_ALIGN);
    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(total_stack));
    }

    /* Stack arguments first: a record copy clobbers the argument registers. */
    for (u32 i = 0; i < nargs; i++)
    {
        if (plans[i].on_stack)
        {
            emit_stack_arg(ctx, in->extra.call.args[i], &plans[i]);
        }
    }
    /* FP and record arguments never share a register file with the scalar GP
       arguments, so they go in first; the GP moves are then scheduled as a
       parallel copy so a value already in its lane needs no move. */
    for (u32 i = 0; i < nargs; i++)
    {
        if (!plans[i].on_stack && (plans[i].is_record || type_is_fp(plans[i].type)))
        {
            emit_reg_arg(ctx, in->extra.call.args[i], &plans[i]);
        }
    }
    emit_gp_args_parallel(ctx, in, plans, nargs);

    if (in->extra.call.is_variadic)
    {
        emit_mov_byte(ctx->buf, xop_reg(R_EAX), xop_imm((i64) fp_used));
    }
    emit_call_target(ctx, in);
    store_call_result(ctx, in);

    if (total_stack > 0)
    {
        emit_binop_rhs(ctx->buf, W_QWORD, &arith_specs[OP_ADD], R_ESP, xop_imm(total_stack));
    }
}

void x86_sysv_lower_va_start(IrInstr *in, X86LowerCtx *ctx)
{
    IrVaStartPayload *vd = &in->extra.va_start;
    x86_lower_force_to_reg(ctx, in->ops[0], R_EAX);
    emit_mov(ctx->buf, W_DWORD, xop_reg(R_ECX), xop_imm(vd->gp_offset));
    emit_mov(ctx->buf, W_DWORD, xop_mem(x86_mem_rax(VA_FIELD_GP_OFFSET)), xop_reg(R_ECX));
    emit_mov(ctx->buf, W_DWORD, xop_reg(R_ECX), xop_imm(vd->fp_offset));
    emit_mov(ctx->buf, W_DWORD, xop_mem(x86_mem_rax(VA_FIELD_FP_OFFSET)), xop_reg(R_ECX));
    emit_lea(ctx->buf, R_EDX, x86_lower_rbp_mem(STACK_PARAM_BASE + (i32) vd->stack_skip));
    emit_mov(ctx->buf, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_EDX));
    emit_lea(ctx->buf, R_EDX, x86_lower_rbp_mem(-(i32) ctx->frame->save_area_off));
    emit_mov(ctx->buf, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_REGS)), xop_reg(R_EDX));
}

/* long double variadics ride the overflow area only: align 16, read 16, bump 16. */
static void lower_va_arg_ld(IrInstr *in, X86LowerCtx *ctx)
{
    ByteBuf *b = ctx->buf;
    x86_lower_force_to_reg(ctx, in->ops[0], R_EAX);
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_mem(x86_mem_rax(VA_FIELD_OVF)));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_ADD], R_ECX, xop_imm(STACK_ALIGN - 1));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_AND], R_ECX, xop_imm(-STACK_ALIGN));
    emit_lea(b, R_EDX, x86_mem_rcx(STACK_ALIGN));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_EDX));
    RegLoc rl = x86_lower_result_loc(ctx, in);
    emit_mov16(b, x86_mem_rcx(0), x86_lower_rbp_mem(rl.disp));
}

void x86_sysv_lower_va_arg(IrInstr *in, X86LowerCtx *ctx)
{
    if (x86_lower_vreg_width(ctx, in->result) == W_LD)
    {
        lower_va_arg_ld(in, ctx);
        return;
    }

    ByteBuf *b = ctx->buf;
    bool is_fp = ir_vreg_float(ctx->mod, in->result);
    X86Mem off_mem;
    i64 limit;
    i64 stride;
    if (is_fp)
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

    x86_lower_force_to_reg(ctx, in->ops[0], R_EAX);
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
    emit_mov(b, W_QWORD, xop_reg(R_R11), xop_reg(R_ECX));
    emit_binop_rhs(b, W_QWORD, &arith_specs[OP_ADD], R_ECX, xop_imm(W_QWORD));
    emit_mov(b, W_QWORD, xop_mem(x86_mem_rax(VA_FIELD_OVF)), xop_reg(R_ECX));
    emit_mov(b, W_QWORD, xop_reg(R_ECX), xop_reg(R_R11));

    size_t done = bytebuf_len(b);
    patch_rel32(b, jge_field, ovf_arm);
    patch_rel32(b, ovf_jmp_field, done);

    emit_mov(b, W_QWORD, xop_reg(R_EDX), xop_mem(x86_mem_rcx(0)));
    store_raw_result(ctx, in, R_EDX, is_fp);
}
