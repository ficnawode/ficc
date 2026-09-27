#include "x86_frame.h"
#include "util/assert.h"
#include "x86_emit.h"
#include "x86_sysv.h"

#define VA_GP_STRIDE 8
#define VA_XMM_STRIDE 16
#define STACK_PARAM_BASE 16
#define STACK_ALIGN 16

static u32 align_up(u32 n, u32 a)
{
    return (n + a - 1) / a * a;
}

static X86Mem mem_plus(X86Mem m, u32 add)
{
    m.disp += (i32) add;
    return m;
}

X86Mem x86_frame_mem(const LinearFrame *frame, i32 disp)
{
    if (frame->omit_fp)
    {
        return x86_mem_rsp(disp + frame->disp_bias);
    }
    return x86_mem_rbp(disp);
}

static SysvArgPlan *plan_params(IrFunction *f, Arena *arena)
{
    size_t n = vec_size(f->params);
    size_t nalloc = MAX(n, 1);
    Type **types = arena_alloc(arena, nalloc * sizeof(Type *), sizeof(Type *));
    SysvArgPlan *plans = arena_alloc(arena, nalloc * sizeof(SysvArgPlan), _Alignof(SysvArgPlan));
    for (size_t i = 0; i < n; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        if (p->agg_type)
        {
            types[i] = p->agg_type;
        }
        else
        {
            types[i] = p->type;
        }
    }
    u32 fp_used = 0;
    (void) sysv_plan_args(types, (u32) n, plans, &fp_used);
    return plans;
}

static u32 frame_param_stage_bytes(const SysvArgPlan *plan, bool debug)
{
    if (plan->is_record)
    {
        return sysv_param_stage_bytes(plan);
    }
    return debug ? STACK_ALIGN : 0;
}

static u32 stage_total_bytes(const SysvArgPlan *plans, size_t n, bool debug)
{
    u32 total = 0;
    for (size_t i = 0; i < n; i++)
    {
        total += align_up(frame_param_stage_bytes(&plans[i], debug), STACK_ALIGN);
    }
    return total;
}

/* Every OP_ALLOCA is a fixed frame slot: lowering recomputes a lea instead of spilling. */
static u32 plan_allocas(IrFunction *f, u32 base)
{
    u32 cum = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_ALLOCA)
            {
                continue;
            }
            u32 size = (u32) in->ops[0].u.imm;
            u32 align = size >= STACK_ALIGN ? STACK_ALIGN : 8;
            cum = align_up(cum, align);
            in->frame_off = base + cum + size;
            cum += align_up(size, align);
        }
    }
    return cum;
}

static void collect_saved(RegAllocation *alloc, const TargetDesc *target, LinearFrame *out)
{
    out->nsaved = 0;
    for (u8 i = 0; i < target->gpr.ncallee_saved; i++)
    {
        if (alloc->saved_mask & (1u << i))
        {
            out->saved_regs[out->nsaved++] = target->gpr.callee_saved[i];
        }
    }
    out->saved_bytes = align_up((u32) out->nsaved * 8, STACK_ALIGN);
}

static bool function_makes_calls(IrFunction *f)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == OP_CALL)
            {
                return true;
            }
        }
    }
    return false;
}

/* A call with stack-passed args moves %rsp, so an %rsp-based frame would not line up. */
static bool function_uses_stack_args(IrFunction *f)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_CALL || in->extra.call.nargs == 0)
            {
                continue;
            }
            u32 nargs = in->extra.call.nargs;
            u32 nalloc = MAX(nargs, 1);
            SysvArgPlan *plans =
                arena_alloc(f->arena, nalloc * sizeof(SysvArgPlan), _Alignof(SysvArgPlan));
            Type **types = arena_alloc(f->arena, nalloc * sizeof(Type *), sizeof(Type *));
            for (u32 i = 0; i < nargs; i++)
            {
                types[i] = in->extra.call.arg_types[i];
            }
            u32 fp_used = 0;
            if (sysv_plan_args(types, nargs, plans, &fp_used) > 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* x87 conversions stage through %rsp, so any long double rules out the frame pointer. */
static bool function_uses_x87(IrModule *mod, IrFunction *f)
{
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        if (mod->widths[((IrParam *) vec_get(f->params, i))->vreg] == 16)
        {
            return true;
        }
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->result != NO_VREG && mod->widths[in->result] == 16)
            {
                return true;
            }
            for (u32 o = 0; o < in->nops; o++)
            {
                if (ir_operand_is_vreg(in->ops[o]) && mod->widths[in->ops[o].u.vreg] == 16)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool x86_frame_can_omit_fp(IrModule *mod, IrFunction *f, bool debug)
{
    if (debug || f->is_variadic || function_uses_stack_args(f) || function_uses_x87(mod, f))
    {
        return false;
    }
    const SysvArgPlan *plans = plan_params(f, f->arena);
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        if (plans[i].on_stack || plans[i].is_record)
        {
            return false;
        }
    }
    return true;
}

void x86_frame_plan(RegAllocation *alloc, IrFunction *f, const TargetDesc *target, bool debug,
                    bool omit_fp, LinearFrame *out)
{
    size_t nparams = vec_size(f->params);
    const SysvArgPlan *plans = plan_params(f, f->arena);

    out->omit_fp = omit_fp;
    collect_saved(alloc, target, out);
    for (u32 v = 0; v < alloc->nvregs; v++)
    {
        if (alloc->has_slot[v])
        {
            alloc->slot_map[v] += out->saved_bytes;
        }
    }

    u32 locals_end = out->saved_bytes + alloc->frame_size;
    u32 stage_bytes = nparams > 0 ? stage_total_bytes(plans, nparams, debug) : 0;
    if (stage_bytes > 0)
    {
        /* First stage slot starts a 16-byte slot below the locals, clearing the saved %rbp. */
        out->stage_base = align_up(locals_end, STACK_ALIGN) + STACK_ALIGN;
        locals_end = out->stage_base + stage_bytes;
    }
    else
    {
        out->stage_base = 0;
    }
    if (f->is_variadic)
    {
        u32 va_bytes = (u32) target->ngp * VA_GP_STRIDE + (u32) target->nfp * VA_XMM_STRIDE;
        out->save_area_off = align_up(locals_end, STACK_ALIGN) + va_bytes;
        locals_end = out->save_area_off;
    }
    locals_end += plan_allocas(f, locals_end);
    u32 frame_size = align_up(locals_end, STACK_ALIGN);

    /* Pad to 16-align call sites when the saved-register pushes leave %rsp misaligned. */
    if (out->nsaved & 1)
    {
        frame_size += 8;
    }
    out->frame_size = frame_size;

    if (!omit_fp)
    {
        return;
    }
    if (locals_end > out->saved_bytes)
    {
        /* Slot disps stay %rbp-relative; %rsp sits 8 bytes below the missing push. */
        out->disp_bias = (i32) ((u32) out->nsaved * 8 + frame_size);
        out->frame_size = frame_size + 8;
    }
    else
    {
        out->disp_bias = 0;
        out->frame_size = (function_makes_calls(f) && (out->nsaved % 2 == 0)) ? 8 : 0;
    }
}

static X86Mem stage_mem(const LinearFrame *frame, u32 off)
{
    return x86_frame_mem(frame, -(i32) (frame->stage_base + off));
}

static u32 param_stage_next(const SysvArgPlan *plan, bool debug, u32 *cursor)
{
    u32 at = *cursor;
    *cursor += align_up(frame_param_stage_bytes(plan, debug), STACK_ALIGN);
    return at;
}

static void spill_variadic_regs(ByteBuf *buf, IrFunction *f, const TargetDesc *target,
                                const LinearFrame *frame)
{
    if (!f->is_variadic)
    {
        return;
    }
    u32 va_gp_bytes = (u32) target->ngp * VA_GP_STRIDE;
    for (u8 i = 0; i < target->ngp; i++)
    {
        emit_mov(
            buf, W_QWORD,
            xop_mem(x86_frame_mem(frame, -(i32) frame->save_area_off + (i32) i * VA_GP_STRIDE)),
            xop_reg(target->gp_args[i]));
    }
    for (u8 i = 0; i < target->nfp; i++)
    {
        emit_sse_store(buf, MF_DOUBLE,
                       x86_frame_mem(frame, -(i32) frame->save_area_off + (i32) va_gp_bytes +
                                                (i32) i * VA_XMM_STRIDE),
                       i);
    }
}

static void stage_record(ByteBuf *buf, const SysvArgPlan *plan, const TargetDesc *target,
                         X86Mem dst)
{
    for (u8 c = 0; c < plan->nchunks; c++)
    {
        const SysvChunk *chunk = &plan->chunks[c];
        X86Mem where = mem_plus(dst, chunk->chunk_off);
        if (chunk->kind == SYSV_GP)
        {
            emit_mov(buf, W_QWORD, xop_mem(where), xop_reg(target->gp_args[chunk->reg]));
        }
        else
        {
            emit_sse_store(buf, MF_DOUBLE, where, chunk->reg);
        }
    }
}

static void stage_scalar(ByteBuf *buf, const SysvArgPlan *plan, u8 width, const TargetDesc *target,
                         X86Mem dst)
{
    bool is_fp = type_is_fp(plan->type);
    if (is_fp && type_sizeof(plan->type) == 16)
    {
        emit_mov16(buf, x86_mem_rbp(STACK_PARAM_BASE + (i32) plan->stack_off), dst);
    }
    else if (plan->on_stack)
    {
        X86Mem src = x86_mem_rbp(STACK_PARAM_BASE + (i32) plan->stack_off);
        if (is_fp)
        {
            emit_sse_load(buf, MF_OF(width), R_XMM0, src);
            emit_sse_store(buf, MF_OF(width), dst, R_XMM0);
        }
        else
        {
            emit_mov(buf, width, xop_reg(R_EAX), xop_mem(src));
            emit_mov(buf, width, xop_mem(dst), xop_reg(R_EAX));
        }
    }
    else if (is_fp)
    {
        emit_sse_store(buf, MF_OF(width), dst, plan->chunks[0].reg);
    }
    else
    {
        emit_mov(buf, width, xop_mem(dst), xop_reg(target->gp_args[plan->chunks[0].reg]));
    }
}

static void stage_incoming(ByteBuf *buf, IrFunction *f, IrModule *mod, const TargetDesc *target,
                           const LinearFrame *frame, const SysvArgPlan *plans, bool debug)
{
    u32 off = 0;
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        const SysvArgPlan *plan = &plans[i];
        X86Mem dst = stage_mem(frame, param_stage_next(plan, debug, &off));
        if (plan->is_record)
        {
            if (plan->register_passed)
            {
                stage_record(buf, plan, target, dst);
            }
        }
        else if (debug)
        {
            stage_scalar(buf, plan, mod->widths[p->vreg], target, dst);
        }
    }
}

static void load_record_home(ByteBuf *buf, const SysvArgPlan *plan, RegLoc home,
                             const LinearFrame *frame, u32 off)
{
    i32 disp;
    if (plan->on_stack)
    {
        disp = STACK_PARAM_BASE + (i32) plan->stack_off;
    }
    else
    {
        disp = -(i32) (frame->stage_base + off);
    }
    emit_lea(buf, R_R11, plan->on_stack ? x86_mem_rbp(disp) : x86_frame_mem(frame, disp));
    if (home.kind == LOC_REG)
    {
        if (home.reg != R_R11)
        {
            emit_mov(buf, W_QWORD, xop_reg(home.reg), xop_reg(R_R11));
        }
    }
    else
    {
        emit_mov(buf, W_QWORD, xop_mem(x86_frame_mem(frame, home.disp)), xop_reg(R_R11));
    }
}

static void load_scalar_home(ByteBuf *buf, const LinearFrame *frame, RegLoc home, u8 width,
                             bool is_fp, X86Operand src)
{
    if (is_fp && width == W_LD)
    {
        ASSERT(home.kind == LOC_MEM && src.kind == XOP_MEM && "x87 values are memory-only");
        emit_mov16(buf, src.u.mem, x86_frame_mem(frame, home.disp));
    }
    else if (is_fp && home.kind == LOC_REG)
    {
        if (src.kind == XOP_REG)
        {
            if (src.u.reg != home.reg)
            {
                emit_sse_op_reg(buf, MF_OF(width), X86_SSE_MOV, home.reg, src.u.reg);
            }
        }
        else
        {
            emit_sse_load(buf, MF_OF(width), home.reg, src.u.mem);
        }
    }
    else if (is_fp)
    {
        if (src.kind == XOP_REG)
        {
            emit_sse_store(buf, MF_OF(width), x86_frame_mem(frame, home.disp), src.u.reg);
        }
        else
        {
            emit_sse_load(buf, MF_OF(width), R_XMM0, src.u.mem);
            emit_sse_store(buf, MF_OF(width), x86_frame_mem(frame, home.disp), R_XMM0);
        }
    }
    else if (home.kind == LOC_REG)
    {
        if (src.kind == XOP_REG)
        {
            if (src.u.reg != home.reg)
            {
                emit_mov(buf, width, xop_reg(home.reg), xop_reg(src.u.reg));
            }
        }
        else
        {
            emit_mov(buf, width, xop_reg(home.reg), xop_mem(src.u.mem));
        }
    }
    else if (src.kind == XOP_REG)
    {
        emit_mov(buf, width, xop_mem(x86_frame_mem(frame, home.disp)), xop_reg(src.u.reg));
    }
    else
    {
        emit_mov(buf, width, xop_reg(R_EAX), xop_mem(src.u.mem));
        emit_mov(buf, width, xop_mem(x86_frame_mem(frame, home.disp)), xop_reg(R_EAX));
    }
}

static X86Operand scalar_param_src(IrParam *p, const SysvArgPlan *plan, IrModule *mod,
                                   const TargetDesc *target)
{
    if (plan->on_stack)
    {
        return xop_mem(x86_mem_rbp(STACK_PARAM_BASE + (i32) plan->stack_off));
    }
    if (mod->floatness[p->vreg])
    {
        return xop_reg(plan->chunks[0].reg);
    }
    return xop_reg(target->gp_args[plan->chunks[0].reg]);
}

static void load_param_homes(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             const LinearFrame *frame, const SysvArgPlan *plans, bool debug)
{
    const TargetDesc *target = x86_64_target();
    u32 off = 0;
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        const SysvArgPlan *plan = &plans[i];
        RegLoc home = loc_at(alloc, ir_operand_vreg(p->vreg), 0);
        u32 at = param_stage_next(plan, debug, &off);
        if (plan->is_record)
        {
            load_record_home(buf, plan, home, frame, at);
        }
        else
        {
            load_scalar_home(buf, frame, home, mod->widths[p->vreg], mod->floatness[p->vreg],
                             scalar_param_src(p, plan, mod, target));
        }
    }
}

void x86_frame_emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             LinearFrame *frame, bool debug)
{
    const TargetDesc *target = x86_64_target();
    size_t before = bytebuf_len(buf);
    if (!frame->omit_fp)
    {
        bytebuf_append(buf, X86_PUSH_RBP);
        frame->off_push = (u32) (bytebuf_len(buf) - before);
        emit_mov(buf, W_QWORD, xop_reg(R_EBP), xop_reg(R_ESP));
        frame->off_mov = (u32) (bytebuf_len(buf) - before);
    }
    else
    {
        frame->off_push = 0;
        frame->off_mov = 0;
    }
    for (u8 i = 0; i < frame->nsaved; i++)
    {
        emit_push_reg(buf, frame->saved_regs[i]);
    }
    if (frame->frame_size > 0)
    {
        emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(frame->frame_size));
    }
    frame->off_sub = (u32) (bytebuf_len(buf) - before);
    spill_variadic_regs(buf, f, target, frame);
    if (vec_size(f->params) > 0)
    {
        const SysvArgPlan *plans = plan_params(f, f->arena);
        stage_incoming(buf, f, mod, target, frame, plans, debug);
        load_param_homes(buf, f, mod, alloc, frame, plans, debug);
    }
}

void x86_frame_restore_callee(ByteBuf *buf, const LinearFrame *frame)
{
    if (frame->omit_fp)
    {
        if (frame->frame_size > 0)
        {
            emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_ADD], R_ESP, xop_imm(frame->frame_size));
        }
        for (u8 i = frame->nsaved; i > 0; i--)
        {
            emit_pop_reg(buf, frame->saved_regs[i - 1]);
        }
        return;
    }
    for (u8 i = 0; i < frame->nsaved; i++)
    {
        emit_mov(buf, W_QWORD, xop_reg(frame->saved_regs[i]),
                 xop_mem(x86_mem_rbp(-(i32) ((u32) i + 1) * 8)));
    }
}

void x86_frame_param_stages(IrFunction *f, const LinearFrame *frame, bool debug, u32 *out)
{
    const SysvArgPlan *plans = plan_params(f, f->arena);
    u32 off = 0;
    size_t n = vec_size(f->params);
    for (size_t i = 0; i < n; i++)
    {
        const SysvArgPlan *plan = &plans[i];
        u32 at = param_stage_next(plan, debug, &off);
        if (frame_param_stage_bytes(plan, debug) > 0)
        {
            out[i] = frame->stage_base + at;
        }
        else
        {
            out[i] = 0;
        }
    }
}
