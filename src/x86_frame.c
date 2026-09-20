#include "x86_frame.h"
#include "util/assert.h"
#include "x86_emit.h"

#define VA_GP_STRIDE 8
#define VA_XMM_STRIDE 16
#define STACK_PARAM_BASE 16
#define STACK_ALIGN 16

static u32 align_up(u32 n, u32 a)
{
    return (n + a - 1) / a * a;
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

void x86_frame_plan(RegAllocation *alloc, IrFunction *f, const TargetDesc *target, LinearFrame *out)
{
    collect_saved(alloc, target, out);
    for (u32 v = 0; v < alloc->nvregs; v++)
    {
        if (alloc->phys_map[v] < 0)
        {
            alloc->slot_map[v] += out->saved_bytes;
        }
    }

    u32 locals_end = out->saved_bytes + alloc->frame_size;
    size_t nparams = vec_size(f->params);
    if (nparams > 0)
    {
        out->stage_base = align_up(locals_end, STACK_ALIGN);
        locals_end = out->stage_base + (u32) nparams * STACK_ALIGN;
    }
    if (f->is_variadic)
    {
        u32 va_bytes = (u32) target->ngp * VA_GP_STRIDE + (u32) target->nfp * VA_XMM_STRIDE;
        out->save_area_off = align_up(locals_end, STACK_ALIGN) + va_bytes;
        locals_end = out->save_area_off;
    }
    out->frame_size = align_up(locals_end, STACK_ALIGN);
}

static X86Mem stage_slot(const LinearFrame *frame, size_t i)
{
    return x86_mem_rbp(-(i32) (frame->stage_base + (u32) i * STACK_ALIGN));
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
        emit_mov(buf, W_QWORD,
                 xop_mem(x86_mem_rbp(-(i32) frame->save_area_off + (i32) i * VA_GP_STRIDE)),
                 xop_reg(target->gp_args[i]));
    }
    for (u8 i = 0; i < target->nfp; i++)
    {
        emit_sse_store(
            buf, MF_DOUBLE,
            x86_mem_rbp(-(i32) frame->save_area_off + (i32) va_gp_bytes + (i32) i * VA_XMM_STRIDE),
            i);
    }
}

static void stage_incoming(ByteBuf *buf, IrFunction *f, IrModule *mod, const TargetDesc *target,
                           const LinearFrame *frame)
{
    u32 gp_used = 0, fp_used = 0, ovf = 0;
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        u8 w = mod->widths[p->vreg];
        bool is_fp = mod->floatness[p->vreg];
        X86Mem dst = stage_slot(frame, i);
        if (is_fp && w == W_LD)
        {
            ovf = align_up(ovf, STACK_ALIGN);
            emit_mov16(buf, x86_mem_rbp(STACK_PARAM_BASE + (i32) ovf), dst);
            ovf += W_LD;
        }
        else if (is_fp && fp_used < target->nfp)
        {
            emit_sse_store(buf, MF_OF(w), dst, (u8) fp_used);
            fp_used++;
        }
        else if (!is_fp && gp_used < target->ngp)
        {
            emit_mov(buf, w, xop_mem(dst), xop_reg(target->gp_args[gp_used]));
            gp_used++;
        }
        else
        {
            X86Mem src = x86_mem_rbp(STACK_PARAM_BASE + (i32) ovf);
            emit_mov(buf, w, xop_reg(R_EAX), xop_mem(src));
            emit_mov(buf, w, xop_mem(dst), xop_reg(R_EAX));
            ovf += W_QWORD;
        }
    }
}

static void load_param_homes(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             const LinearFrame *frame)
{
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        u8 w = mod->widths[p->vreg];
        bool is_fp = mod->floatness[p->vreg];
        X86Mem stage = stage_slot(frame, i);
        RegLoc home = loc_of(alloc, ir_operand_vreg(p->vreg));
        if (is_fp && w == W_LD)
        {
            ASSERT(home.kind == LOC_MEM && "x87 values are memory-only");
            emit_mov16(buf, stage, x86_mem_rbp(home.disp));
        }
        else if (is_fp && home.kind == LOC_REG)
        {
            emit_sse_load(buf, MF_OF(w), home.reg, stage);
        }
        else if (is_fp)
        {
            emit_sse_load(buf, MF_OF(w), R_XMM0, stage);
            emit_sse_store(buf, MF_OF(w), x86_mem_rbp(home.disp), R_XMM0);
        }
        else if (home.kind == LOC_REG)
        {
            emit_mov(buf, w, xop_reg(home.reg), xop_mem(stage));
        }
        else
        {
            emit_mov(buf, w, xop_reg(R_EAX), xop_mem(stage));
            emit_mov(buf, w, xop_mem(x86_mem_rbp(home.disp)), xop_reg(R_EAX));
        }
    }
}

void x86_frame_emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             LinearFrame *frame)
{
    const TargetDesc *target = x86_64_target();
    size_t before = bytebuf_len(buf);
    bytebuf_append(buf, X86_PUSH_RBP);
    frame->off_push = (u32) (bytebuf_len(buf) - before);
    emit_mov(buf, W_QWORD, xop_reg(R_EBP), xop_reg(R_ESP));
    frame->off_mov = (u32) (bytebuf_len(buf) - before);
    for (u8 i = 0; i < frame->nsaved; i++)
    {
        emit_push_reg(buf, frame->saved_regs[i]);
    }
    emit_binop_rhs(buf, W_QWORD, &arith_specs[OP_SUB], R_ESP, xop_imm(frame->frame_size));
    frame->off_sub = (u32) (bytebuf_len(buf) - before);
    spill_variadic_regs(buf, f, target, frame);
    if (vec_size(f->params) > 0)
    {
        stage_incoming(buf, f, mod, target, frame);
        load_param_homes(buf, f, mod, alloc, frame);
    }
}

void x86_frame_restore_callee(ByteBuf *buf, const LinearFrame *frame)
{
    for (u8 i = 0; i < frame->nsaved; i++)
    {
        emit_mov(buf, W_QWORD, xop_reg(frame->saved_regs[i]),
                 xop_mem(x86_mem_rbp(-(i32) ((u32) i + 1) * 8)));
    }
}
