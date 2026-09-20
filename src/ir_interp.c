#include "ir_interp.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* --- Interpreter diagnostics --- */

static void interp_error(const char *fmt, ...)
{
    fprintf(stderr, "[interp] error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* --- Interpreter context --- */

typedef struct InterpGlobal InterpGlobal;
struct InterpGlobal
{
    u8 *data;
};

typedef struct InterpCtx InterpCtx;
struct InterpCtx
{
    IrModule *mod;
    Vec *stack; /* Vec<Frame*> */
    Arena *frame_arena;
    u32 nregs;
    StrMap *func_map; /* name -> IrFunction* */

    /* Per-function block lookup (rebuilt on every run_func / eval_call) */
    StrMap *block_map; /* label -> IrBlock* */

    /* Per-block execution state */
    IrBlock *next_bb;   /* next block to execute (set by branch ops) */
    IrBlock *next_pred; /* the block that jumps to next_bb */
    bool jumped;
    bool returned;
    bool error; /* set on a runtime trap (e.g. null dereference) */

    /* Alloca and global state */
    u8 *alloca_base;
    u64 alloca_top;
    u64 alloca_limit;
    InterpGlobal *globals;
    u32 nglobals;

    /* 16-byte return channel between eval_ret and eval_call. */
    u8 ret_cell[16];

    /* Stable synthetic addresses for function designators / `&f`. */
    u64 *func_addrs;
    u32 nfuncs;
};

/* --- SysV x86-64 varargs ABI --- (6 GP slots + 8 xmm slots at a 16-byte stride) */
#define VA_GP_ARGS 6
#define VA_GP_BYTES (VA_GP_ARGS * 8) /* 48 */
#define VA_XMM_ARGS 8
#define VA_XMM_STRIDE 16
#define VA_SAVE_BYTES (VA_GP_BYTES + VA_XMM_ARGS * VA_XMM_STRIDE) /* 176 */

/* Call-stack entry with register file; variadic callees get va_list regions. */
typedef struct
{
    i64 *regs;
    u8 *va_save;     /* register save area (VA_SAVE_BYTES), or NULL */
    u8 *va_overflow; /* class-overflowed args, 8 bytes each */
} Frame;

/* `regs[v]` is the 64-bit value; width-16 also uses regs[nregs + v]. */
static void read_cell(InterpCtx *ctx, i64 *regs, u32 vreg, void *dst)
{
    memcpy(dst, &regs[vreg], 8);
    memcpy((u8 *) dst + 8, &regs[ctx->nregs + vreg], 8);
}

static void write_cell(InterpCtx *ctx, i64 *regs, u32 vreg, const void *src)
{
    memcpy(&regs[vreg], src, 8);
    memcpy(&regs[ctx->nregs + vreg], (const u8 *) src + 8, 8);
}

static void copy_cell(InterpCtx *ctx, i64 *dst_regs, u32 dst_vreg, i64 *src_regs, u32 src_vreg)
{
    memcpy(&dst_regs[dst_vreg], &src_regs[src_vreg], 8);
    memcpy(&dst_regs[ctx->nregs + dst_vreg], &src_regs[ctx->nregs + src_vreg], 8);
}

/* --- Function pseudo-addresses --- */

#define FUNC_ADDR_BASE 0x400000000ULL

static size_t func_index_by_name(IrModule *mod, const char *name)
{
    size_t n = vec_size(mod->funcs);
    for (size_t i = 0; i < n; i++)
    {
        IrFunction *fn = (IrFunction *) vec_get(mod->funcs, i);
        if (strcmp(fn->name, name) == 0)
        {
            return i;
        }
    }
    return n;
}

static i64 func_addr(size_t index)
{
    return (i64) (FUNC_ADDR_BASE + (u64) index * 8);
}

static i64 func_addr_by_name(IrModule *mod, const char *name)
{
    size_t fi = func_index_by_name(mod, name);
    return fi < vec_size(mod->funcs) ? func_addr(fi) : 0;
}

/* --- Frame management --- */

static i64 operand_val(InterpCtx *ctx, IrOperand o, i64 *regs)
{
    if (o.is_imm)
    {
        return o.u.imm;
    }
    if (o.is_global)
    {
        ASSERT(o.u.global_index < ctx->nglobals);
        return (i64) (uintptr_t) ctx->globals[o.u.global_index].data;
    }
    if (o.is_func)
    {
        size_t i = func_index_by_name(ctx->mod, o.u.func_name);
        if (i < ctx->nfuncs)
        {
            return (i64) ctx->func_addrs[i];
        }
        interp_error("undefined function '%s'", o.u.func_name);
        ctx->error = true;
        return 0;
    }
    ASSERT(o.u.vreg < ctx->nregs);
    return regs[o.u.vreg];
}

static u8 *resolve_ptr(InterpCtx *ctx, IrOperand op, i64 *regs)
{
    if (op.is_global)
    {
        ASSERT(op.u.global_index < ctx->nglobals);
        return ctx->globals[op.u.global_index].data;
    }
    i64 ptr_val = operand_val(ctx, op, regs);
    if (ptr_val == 0)
    {
        interp_error("null pointer dereference");
        ctx->error = true;
        return NULL;
    }
    return (u8 *) (uintptr_t) ptr_val;
}

static Frame *frame_new(Arena *arena, u32 nregs)
{
    Frame *f = arena_alloc(arena, sizeof(Frame), sizeof(void *));
    /* The second nregs hold the upper half of width-16 values. */
    f->regs = arena_alloc(arena, nregs * 2 * sizeof(i64), sizeof(i64));
    memset(f->regs, 0, nregs * 2 * sizeof(i64));
    f->va_save = NULL;
    f->va_overflow = NULL;
    return f;
}

/* Bump from the interpreter's alloca region (shared with eval_alloca). */
static u8 *interp_alloc(InterpCtx *ctx, u64 size)
{
    u64 aligned = (size + 7) & ~7ULL;
    if (ctx->alloca_top + aligned > ctx->alloca_limit)
    {
        interp_error("stack overflow");
        return NULL;
    }
    u8 *p = ctx->alloca_base + ctx->alloca_top;
    ctx->alloca_top += aligned;
    return p;
}

/* --- Width-aware masking --- */

static i64 trunc_result(i64 val, u8 width_bytes)
{
    switch (width_bytes)
    {
        case 1:
            return val & 0xFF;
        case 2:
            return val & 0xFFFF;
        case 4:
            return val & 0xFFFFFFFF;
        default:
            return val;
    }
}

static i64 sext_result(i64 val, u8 width_bytes)
{
    switch (width_bytes)
    {
        case 1:
            return (i64) (i8) val;
        case 2:
            return (i64) (i16) val;
        case 4:
            return (i64) (i32) val;
        default:
            return val;
    }
}

static void apply_vreg_width(InterpCtx *ctx, i64 *regs, u32 vreg)
{
    ASSERT(vreg < ctx->nregs);
    u8 w = ctx->mod->widths[vreg];
    bool is_signed = ir_vreg_signed(ctx->mod, vreg);
    regs[vreg] = is_signed ? sext_result(regs[vreg], w) : trunc_result(regs[vreg], w);
}

/* --- Eval dispatch --- */

typedef i64 (*EvalFn)(IrInstr *in, InterpCtx *ctx, i64 *regs);

/* Forward declaration for the mutual recursion with eval_call. */
static i64 run_block(InterpCtx *ctx, i64 *regs, IrBlock *start_bb, IrBlock *start_pred);

/* --- Eval functions --- */

static i64 eval_binary(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 lhs = operand_val(ctx, in->ops[0], regs);
    i64 rhs = operand_val(ctx, in->ops[1], regs);
    switch (in->opcode)
    {
        case OP_ADD:
            regs[in->result] = lhs + rhs;
            break;
        case OP_SUB:
            regs[in->result] = lhs - rhs;
            break;
        case OP_MUL:
            regs[in->result] = lhs * rhs;
            break;
        case OP_AND:
            regs[in->result] = lhs & rhs;
            break;
        case OP_OR:
            regs[in->result] = lhs | rhs;
            break;
        case OP_XOR:
            regs[in->result] = lhs ^ rhs;
            break;
        default:
            break;
    }
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_divrem(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 rhs = operand_val(ctx, in->ops[1], regs);
    if (rhs == 0)
    {
        interp_error("division by zero");
        ASSERT(false);
        return 1;
    }
    i64 lhs = operand_val(ctx, in->ops[0], regs);
    switch (in->opcode)
    {
        case OP_SDIV:
            regs[in->result] = lhs / rhs;
            break;
        case OP_SREM:
            regs[in->result] = lhs % rhs;
            break;
        case OP_UDIV:
            regs[in->result] = (i64) ((u64) lhs / (u64) rhs);
            break;
        default:
            regs[in->result] = (i64) ((u64) lhs % (u64) rhs);
            break;
    }
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_unary(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    switch (in->opcode)
    {
        case OP_NEG:
            regs[in->result] = -src;
            break;
        case OP_NOT:
            regs[in->result] = ~src;
            break;
        default:
            ASSERT(false && "eval_unary is only bound to OP_NEG/OP_NOT");
            return 1;
    }
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_shift(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 lhs = operand_val(ctx, in->ops[0], regs);
    i64 rhs = operand_val(ctx, in->ops[1], regs);
    u8 w = ctx->mod->widths[in->result];
    u32 max_shift = (u32) w * 8;
    if (rhs < 0 || rhs >= (i64) max_shift)
    {
        interp_error("shift by %lld is undefined", (long long) rhs);
        ASSERT(false);
        return 1;
    }
    switch (in->opcode)
    {
        case OP_SHL:
            regs[in->result] = lhs << rhs;
            break;
        case OP_ASHR:
            regs[in->result] = lhs >> rhs;
            break;
        default:
            regs[in->result] = (i64) ((u64) lhs >> rhs);
            break;
    }
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_icmp(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 lhs = operand_val(ctx, in->ops[0], regs);
    i64 rhs = operand_val(ctx, in->ops[1], regs);
    bool cond = false;
    switch (in->opcode)
    {
        case OP_ICMP_EQ:
            cond = lhs == rhs;
            break;
        case OP_ICMP_NE:
            cond = lhs != rhs;
            break;
        case OP_ICMP_ULT:
            cond = (u64) lhs < (u64) rhs;
            break;
        case OP_ICMP_ULE:
            cond = (u64) lhs <= (u64) rhs;
            break;
        case OP_ICMP_UGT:
            cond = (u64) lhs > (u64) rhs;
            break;
        case OP_ICMP_UGE:
            cond = (u64) lhs >= (u64) rhs;
            break;
        case OP_ICMP_SLT:
            cond = lhs < rhs;
            break;
        case OP_ICMP_SLE:
            cond = lhs <= rhs;
            break;
        case OP_ICMP_SGT:
            cond = lhs > rhs;
            break;
        case OP_ICMP_SGE:
            cond = lhs >= rhs;
            break;
        default:
            break;
    }
    regs[in->result] = cond ? 1 : 0;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

/* Resolve a function pseudo-address back to its IrFunction; NULL if unknown. */
static IrFunction *find_func_by_addr(InterpCtx *ctx, i64 addr)
{
    for (u32 i = 0; i < ctx->nfuncs; i++)
    {
        if ((i64) ctx->func_addrs[i] == addr)
        {
            return (IrFunction *) vec_get(ctx->mod->funcs, i);
        }
    }
    return NULL;
}

static void build_block_map(InterpCtx *ctx, IrFunction *func)
{
    ctx->block_map = strmap_new(ctx->frame_arena);
    size_t nblocks = vec_size(func->blocks);
    for (size_t i = 0; i < nblocks; i++)
    {
        IrBlock *block = (IrBlock *) vec_get(func->blocks, i);
        strmap_set(ctx->block_map, block->label, block);
    }
}

/* Direct/indirect callee resolution; NULL (with a diagnostic) if unknown. */
static IrFunction *resolve_callee(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    if (in->extra.call.is_indirect)
    {
        i64 addr = operand_val(ctx, in->extra.call.callee, regs);
        IrFunction *callee = find_func_by_addr(ctx, addr);
        if (!callee)
        {
            interp_error("indirect call through an invalid function pointer");
        }
        return callee;
    }
    IrFunction *callee = strmap_get(ctx->func_map, in->extra.call.name);
    if (!callee)
    {
        interp_error("undefined function '%s'", in->extra.call.name);
    }
    return callee;
}

/* Bind named arguments into the callee frame (extras left to varargs). */
static void bind_args(IrInstr *in, InterpCtx *ctx, i64 *regs, Frame *callee_frame,
                      IrFunction *callee)
{
    size_t nparams = vec_size(callee->params);
    for (u32 a = 0; a < in->extra.call.nargs && a < nparams; a++)
    {
        IrParam *p = (IrParam *) vec_get(callee->params, a);
        u8 width = ctx->mod->widths[p->vreg];
        if (width == 16)
        {
            IrOperand arg = in->extra.call.args[a];
            ASSERT(!arg.is_imm && !arg.is_global && !arg.is_func); /* width-16 ⇒ vreg */
            copy_cell(ctx, callee_frame->regs, p->vreg, regs, arg.u.vreg);
        }
        else
        {
            i64 v = operand_val(ctx, in->extra.call.args[a], regs);
            callee_frame->regs[p->vreg] =
                type_is_signed_int(p->type) ? sext_result(v, width) : trunc_result(v, width);
        }
    }
}

/* A call arg is FP when it's an FP-classed vreg; immediates/addresses are GP. */
static bool call_arg_is_fp(InterpCtx *ctx, IrOperand arg)
{
    if (arg.is_imm || arg.is_global || arg.is_func)
    {
        return false;
    }
    return ir_vreg_float(ctx->mod, arg.u.vreg);
}

/* A call arg is the X87 class (width 16): never a GP/SSE register. */
static bool call_arg_is_ld(InterpCtx *ctx, IrOperand arg)
{
    if (arg.is_imm || arg.is_global || arg.is_func)
    {
        return false;
    }
    return ctx->mod->widths[arg.u.vreg] == 16;
}

/* Spill GP/SSE args and stack the rest; width-16 args use 16-aligned slots. */
static bool materialize_varargs(IrInstr *in, InterpCtx *ctx, i64 *regs, Frame *callee_frame)
{
    u32 nargs = in->extra.call.nargs;
    u32 stack_bytes = 0;
    u32 gp_reg = 0, fp_reg = 0;
    for (u32 a = 0; a < nargs; a++)
    {
        IrOperand arg = in->extra.call.args[a];
        if (call_arg_is_ld(ctx, arg))
        {
            stack_bytes = (stack_bytes + 15) & ~15u;
            stack_bytes += 16;
        }
        else if (call_arg_is_fp(ctx, arg))
        {
            if (fp_reg < VA_XMM_ARGS)
            {
                fp_reg++;
            }
            else
            {
                stack_bytes += 8;
            }
        }
        else
        {
            if (gp_reg < VA_GP_ARGS)
            {
                gp_reg++;
            }
            else
            {
                stack_bytes += 8;
            }
        }
    }

    u8 *save_area = interp_alloc(ctx, VA_SAVE_BYTES);
    /* +16 slack keeps the overflow base 16-aligned (and never NULL). */
    u8 *raw_ovf = interp_alloc(ctx, stack_bytes + 16);
    if (!save_area || !raw_ovf)
    {
        return false;
    }
    u8 *overflow_area = (u8 *) (((uintptr_t) raw_ovf + 15) & ~(uintptr_t) 15);
    memset(save_area, 0, VA_SAVE_BYTES);

    gp_reg = 0;
    fp_reg = 0;
    u32 ovf_off = 0;
    for (u32 a = 0; a < nargs; a++)
    {
        IrOperand arg = in->extra.call.args[a];
        if (call_arg_is_ld(ctx, arg))
        {
            ovf_off = (ovf_off + 15) & ~15u;
            ASSERT(!arg.is_imm && !arg.is_global && !arg.is_func);
            memcpy(overflow_area + ovf_off, &regs[arg.u.vreg], 8);
            memcpy(overflow_area + ovf_off + 8, &regs[ctx->nregs + arg.u.vreg], 8);
            ovf_off += 16;
        }
        else if (call_arg_is_fp(ctx, arg))
        {
            i64 v = operand_val(ctx, arg, regs);
            if (fp_reg < VA_XMM_ARGS)
            {
                /* The promoted double value sits in the low 8 bytes of the 16-byte slot. */
                memcpy(save_area + VA_GP_BYTES + fp_reg * VA_XMM_STRIDE, &v, 8);
                fp_reg++;
            }
            else
            {
                memcpy(overflow_area + ovf_off, &v, 8);
                ovf_off += 8;
            }
        }
        else
        {
            i64 v = operand_val(ctx, arg, regs);
            if (gp_reg < VA_GP_ARGS)
            {
                memcpy(save_area + gp_reg * 8, &v, 8);
                gp_reg++;
            }
            else
            {
                memcpy(overflow_area + ovf_off, &v, 8);
                ovf_off += 8;
            }
        }
    }
    callee_frame->va_overflow = overflow_area;
    callee_frame->va_save = save_area;
    return true;
}

static i64 eval_call(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    IrFunction *callee = resolve_callee(in, ctx, regs);
    if (!callee)
    {
        return 1;
    }

    StrMap *saved_block_map = ctx->block_map;
    Frame *callee_frame = frame_new(ctx->frame_arena, ctx->nregs);
    vec_push(ctx->stack, callee_frame);

    bind_args(in, ctx, regs, callee_frame, callee);
    if (callee->is_variadic && !materialize_varargs(in, ctx, regs, callee_frame))
    {
        return 1;
    }

    build_block_map(ctx, callee);
    IrBlock *entry = (IrBlock *) vec_get(callee->blocks, 0);

    /* The callee's run_block reuses and overwrites the caller's block-walk
       scratch (next_bb/next_pred/jumped/returned). Save it so the caller
       resumes its own walk after the call returns. */
    IrBlock *saved_next_bb = ctx->next_bb;
    IrBlock *saved_next_pred = ctx->next_pred;
    bool saved_jumped = ctx->jumped;
    bool saved_returned = ctx->returned;

    i64 ret = run_block(ctx, callee_frame->regs, entry, NULL);

    ctx->next_bb = saved_next_bb;
    ctx->next_pred = saved_next_pred;
    ctx->jumped = saved_jumped;
    ctx->returned = saved_returned;
    ctx->block_map = saved_block_map;
    vec_pop(ctx->stack);
    if (in->result != NO_VREG)
    {
        if (ctx->mod->widths[in->result] == 16)
        {
            /* The callee's eval_ret staged the full cell in ctx->ret_cell. */
            write_cell(ctx, regs, in->result, ctx->ret_cell);
        }
        else
        {
            regs[in->result] = ret;
            apply_vreg_width(ctx, regs, in->result);
        }
    }
    return 0;
}

/* The va_list object va_start writes / va_arg walks (SysV field layout). */
typedef struct
{
    u32 gp_offset;
    u32 fp_offset;
    u64 overflow;
    u64 reg_save;
} VaFields;

/* va_start(ap, last): write the four va_list fields. */
static i64 eval_va_start(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    Frame *frame = (Frame *) vec_get(ctx->stack, vec_size(ctx->stack) - 1);
    u8 *ap = resolve_ptr(ctx, in->ops[0], regs);
    if (!ap)
    {
        return 1;
    }
    VaFields va = {
        .gp_offset = (u32) in->extra.va_start.gp_offset,
        .fp_offset = (u32) in->extra.va_start.fp_offset,
        .overflow = frame->va_overflow ? (u64) (uintptr_t) (frame->va_overflow +
                                                            (u32) in->extra.va_start.stack_skip)
                                       : 0,
        .reg_save = (u64) (uintptr_t) frame->va_save,
    };
    memcpy(ap, &va, sizeof(va));
    return 0;
}

/* The result width/class picks the va_arg walk (ld: overflow-only). */
static i64 eval_va_arg(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 *ap = resolve_ptr(ctx, in->ops[0], regs);
    if (!ap)
    {
        return 1;
    }
    VaFields va;
    memcpy(&va, ap, sizeof(va));
    if (ctx->mod->widths[in->result] == 16)
    {
        /* X87 args ride the overflow area only: align up to 16, read 16, bump 16. */
        u64 src = (va.overflow + 15) & ~15ULL;
        va.overflow = src + 16;
        memcpy(ap, &va, sizeof(va));
        write_cell(ctx, regs, in->result, (u8 *) (uintptr_t) src);
        return 0;
    }
    bool is_fp = ir_vreg_float(ctx->mod, in->result);
    u32 *off = is_fp ? &va.fp_offset : &va.gp_offset;
    u32 limit = is_fp ? VA_SAVE_BYTES : VA_GP_BYTES;
    u32 stride = is_fp ? VA_XMM_STRIDE : 8;
    u64 src;
    if (*off < limit)
    {
        src = va.reg_save + *off;
        *off += stride;
    }
    else
    {
        src = va.overflow;
        va.overflow += 8;
    }
    i64 val;
    memcpy(&val, (u8 *) (uintptr_t) src, 8);
    memcpy(ap, &va, sizeof(va));
    regs[in->result] = val;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

/* __builtin_va_end(ap): no-op (SysV has no va_end action). */
static i64 eval_va_end(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) in;
    (void) ctx;
    (void) regs;
    return 0;
}

/* Assert the jump target exists and record it for the block loop. */
static void jump_to(InterpCtx *ctx, const char *target_label)
{
    IrBlock *target = strmap_get(ctx->block_map, target_label);
    ASSERT(target != NULL && "branch target names a block the IR builder created");
    ctx->next_bb = target;
    ctx->jumped = true;
}

static i64 eval_br(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) regs;
    jump_to(ctx, in->extra.br.target_label);
    return 0;
}

static i64 eval_brcond(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 cond = operand_val(ctx, in->ops[0], regs);
    const char *target_label = cond ? in->extra.brcond.true_label : in->extra.brcond.false_label;
    jump_to(ctx, target_label);
    return 0;
}

static i64 eval_switch(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 val = operand_val(ctx, in->ops[0], regs);
    const char *target_label = in->extra.sw.default_label;
    for (u32 c = 0; c < in->extra.sw.ncases; c++)
    {
        if (val == in->extra.sw.cases[c].val)
        {
            target_label = in->extra.sw.cases[c].label;
            break;
        }
    }
    jump_to(ctx, target_label);
    return 0;
}

static i64 eval_ret(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 result = 0;
    if (in->nops > 0)
    {
        IrOperand val = in->ops[0];
        if (!val.is_imm && !val.is_global && !val.is_func && ctx->mod->widths[val.u.vreg] == 16)
        {
            read_cell(ctx, regs, val.u.vreg, ctx->ret_cell);
        }
        else
        {
            result = operand_val(ctx, val, regs);
        }
    }
    ctx->returned = true;
    return result;
}

static i64 eval_phi(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) in;
    (void) ctx;
    (void) regs;
    ASSERT(false && "PHI must be evaluated before block execution via eval_phis");
    return 1;
}

static i64 eval_trunc(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    regs[in->result] = src;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

/* Zero- or sign-extend to the destination width (literals are already i64). */
static i64 eval_extend(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    if (in->ops[0].is_imm)
    {
        regs[in->result] = src;
        return 0;
    }
    u8 src_w = ctx->mod->widths[in->ops[0].u.vreg];
    switch (in->opcode)
    {
        case OP_SEXT:
            regs[in->result] = sext_result(src, src_w);
            break;
        case OP_ZEXT:
            regs[in->result] = trunc_result(src, src_w);
            break;
        default:
            ASSERT(false && "eval_extend is only bound to OP_SEXT/OP_ZEXT");
            return 1;
    }
    return 0;
}

static i64 eval_unreachable(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) in;
    (void) ctx;
    (void) regs;
    interp_error("reached unreachable");
    return 1;
}

/* Conversion boundaries at long-double precision (2^31/2^63/2^64 exact). */
#define U64_SIGN_BIT (1ULL << 63)
#define LDBL_TWO_31 2147483648.0L
#define LDBL_TWO_63 9223372036854775808.0L
#define LDBL_TWO_64 18446744073709551616.0L

/* Write an FP pattern into a vreg: the low `width` bytes, or the pair for 16. */
static void store_fp_bits(InterpCtx *ctx, i64 *regs, u32 vreg, const void *bits, u8 width)
{
    if (width == 16)
    {
        write_cell(ctx, regs, vreg, bits);
        return;
    }
    regs[vreg] = 0;
    memcpy(&regs[vreg], bits, width);
}

static u8 operand_fp_width(InterpCtx *ctx, IrOperand o)
{
    return o.is_imm ? 8 : ctx->mod->widths[o.u.vreg];
}

/* Read an FP operand into the host long-double channel at its own width. */
static long double read_fp_value(InterpCtx *ctx, i64 *regs, IrOperand o, u8 w)
{
    if (w == 16)
    {
        ASSERT(!o.is_imm && !o.is_global && !o.is_func); /* width-16 ⇒ vreg */
        u8 cell[16];
        read_cell(ctx, regs, o.u.vreg, cell);
        long double v;
        memcpy(&v, cell, sizeof(v));
        return v;
    }
    i64 bits = operand_val(ctx, o, regs);
    if (w == 4)
    {
        float f;
        memcpy(&f, &bits, 4);
        return (long double) f;
    }
    double d;
    memcpy(&d, &bits, 8);
    return (long double) d;
}

/* Round a host long double back to the destination precision (4/8 re-round). */
static void store_fp_value(InterpCtx *ctx, i64 *regs, u32 vreg, long double v, u8 dw)
{
    if (dw == 16)
    {
        write_cell(ctx, regs, vreg, &v);
        return;
    }
    if (dw == 4)
    {
        float f = (float) v;
        store_fp_bits(ctx, regs, vreg, &f, 4);
        return;
    }
    double d = (double) v;
    store_fp_bits(ctx, regs, vreg, &d, 8);
}

/* cvttsd2si semantics: truncate toward zero; NaN/out-of-i64-range → INT64_MIN. */
static i64 trunc_to_i64_long(long double d)
{
    if (d != d || d >= LDBL_TWO_63 || d < -LDBL_TWO_63)
    {
        return INT64_MIN;
    }
    return (i64) d;
}

static i64 eval_itof(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    bool is_signed = in->ops[0].is_imm || ir_vreg_signed(ctx->mod, in->ops[0].u.vreg);
    u8 dw = ctx->mod->widths[in->result];
    long double v;
    if (is_signed || (u64) src < U64_SIGN_BIT)
    {
        v = (long double) (i64) src;
    }
    else
    {
        /* u64 ≥ 2^63: clear the top bit, convert, add back 2^63. */
        u64 y = (u64) src & ~U64_SIGN_BIT;
        v = (long double) (u64) y + LDBL_TWO_63;
    }
    store_fp_value(ctx, regs, in->result, v, dw);
    return 0;
}

static i64 eval_ftoi(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    long double d = read_fp_value(ctx, regs, in->ops[0], operand_fp_width(ctx, in->ops[0]));
    u8 dw = ctx->mod->widths[in->result];
    bool is_signed = ir_vreg_signed(ctx->mod, in->result);
    i64 t;
    if (dw == 8 && is_signed)
    {
        /* Signed 64: i64 window; NaN/out-of-range brand INT64_MIN. */
        t = trunc_to_i64_long(d);
    }
    else if (dw == 8)
    {
        /* Unsigned 64: cvttsd2si, then for d ≥ 2^63 subtract 2^63 and set bit 63. */
        if (d != d || d < -LDBL_TWO_63 || d >= LDBL_TWO_64)
        {
            t = INT64_MIN;
        }
        else if (d < LDBL_TWO_63)
        {
            t = trunc_to_i64_long(d);
        }
        else
        {
            t = (i64) ((u64) trunc_to_i64_long(d - LDBL_TWO_63) | U64_SIGN_BIT);
        }
    }
    else if (is_signed)
    {
        /* Narrow signed: i32 window (x86's 0x80000000 out-of-range brand). */
        if (d != d || d >= LDBL_TWO_31 || d < -LDBL_TWO_31)
        {
            t = INT32_MIN;
        }
        else
        {
            t = (i64) (i32) d;
        }
    }
    else
    {
        /* Narrow unsigned: i64 window, masked by the width pass. */
        t = trunc_to_i64_long(d);
    }
    regs[in->result] = t;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_fconv(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    long double v = read_fp_value(ctx, regs, in->ops[0], operand_fp_width(ctx, in->ops[0]));
    u8 dw = ctx->mod->widths[in->result];
    store_fp_value(ctx, regs, in->result, v, dw);
    return 0;
}

/* An immediate carries the FP pattern for the destination width (a bare imm
   rides no width-table entry); classed vregs name their own width. */
static u8 fp_operand_width(InterpCtx *ctx, IrOperand o, u8 dw)
{
    return o.is_imm ? dw : ctx->mod->widths[o.u.vreg];
}

/* FP arithmetic on the host long-double channel, re-rounded per destination width. */
static i64 eval_fbin(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 dw = ctx->mod->widths[in->result];
    IrOperand l = in->ops[0];
    IrOperand r = in->ops[1];
    u8 lw = fp_operand_width(ctx, l, dw);
    u8 rw = fp_operand_width(ctx, r, dw);
    long double a = read_fp_value(ctx, regs, l, lw);
    long double b = read_fp_value(ctx, regs, r, rw);
    long double rr;
    switch (in->opcode)
    {
        case OP_FADD:
            rr = a + b;
            break;
        case OP_FSUB:
            rr = a - b;
            break;
        case OP_FMUL:
            rr = a * b;
            break;
        case OP_FDIV:
            rr = a / b;
            break;
        default:
            ASSERT(false && "eval_fbin dispatches only to the FP arithmetic opcodes");
            return 1;
    }
    store_fp_value(ctx, regs, in->result, rr, dw);
    return 0;
}

static i64 eval_fneg(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 dw = ctx->mod->widths[in->result];
    u8 sw = fp_operand_width(ctx, in->ops[0], dw);
    long double v = read_fp_value(ctx, regs, in->ops[0], sw);
    store_fp_value(ctx, regs, in->result, -v, dw);
    return 0;
}

/* C11 NaN semantics on the host long-double channel (float→ld widening is exact). */
static i64 eval_fcmp(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    long double a = read_fp_value(ctx, regs, in->ops[0], operand_fp_width(ctx, in->ops[0]));
    long double b = read_fp_value(ctx, regs, in->ops[1], operand_fp_width(ctx, in->ops[1]));
    bool r;
    switch (in->opcode)
    {
        case OP_FCMP_EQ:
            r = a == b;
            break;
        case OP_FCMP_NE:
            r = a != b;
            break;
        case OP_FCMP_LT:
            r = a < b;
            break;
        case OP_FCMP_GT:
            r = a > b;
            break;
        case OP_FCMP_LE:
            r = a <= b;
            break;
        case OP_FCMP_GE:
            r = a >= b;
            break;
        default:
            ASSERT(false && "eval_fcmp dispatches only to the FP compare opcodes");
            return 1;
    }
    regs[in->result] = r ? 1 : 0;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

/* Copy at most one register-sized word into/out of a typed slot. */
static void copy_word(u8 *dst, const void *src, u32 bytes)
{
    memcpy(dst, src, bytes > 8 ? 8 : bytes);
}

static i64 eval_load(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 *addr = resolve_ptr(ctx, in->ops[0], regs);
    if (!addr)
    {
        return 1;
    }
    u8 w = ctx->mod->widths[in->result];
    if (w == 16)
    {
        write_cell(ctx, regs, in->result, addr); /* 16-byte move into the pair */
        return 0;
    }
    i64 val = 0;
    copy_word((u8 *) &val, addr, w);
    regs[in->result] = val;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_store(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 *addr = resolve_ptr(ctx, in->ops[1], regs);
    if (!addr)
    {
        return 1;
    }
    u32 w = (u32) in->ops[2].u.imm;
    if (w == 16)
    {
        /* width-16 values are always vregs: no immediate has a 16-byte form */
        ASSERT(!in->ops[0].is_imm && !in->ops[0].is_global && !in->ops[0].is_func);
        u8 cell[16];
        read_cell(ctx, regs, in->ops[0].u.vreg, cell);
        memcpy(addr, cell, 16);
        return 0;
    }
    i64 val = operand_val(ctx, in->ops[0], regs);
    copy_word(addr, &val, w);
    return 0;
}

static i64 eval_gep(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 base = operand_val(ctx, in->ops[0], regs);
    i64 index = operand_val(ctx, in->ops[1], regs);
    i64 stride = in->ops[2].u.imm;
    regs[in->result] = base + index * stride;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_alloca(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 size = operand_val(ctx, in->ops[0], regs);
    u8 *p = interp_alloc(ctx, (u64) size);
    if (!p)
    {
        return 1;
    }
    regs[in->result] = (i64) (uintptr_t) p;
    apply_vreg_width(ctx, regs, in->result);
    return 0;
}

static i64 eval_memcpy(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 *dst = resolve_ptr(ctx, in->ops[0], regs);
    if (!dst)
    {
        return 1;
    }
    u8 *src = resolve_ptr(ctx, in->ops[1], regs);
    if (!src)
    {
        return 1;
    }
    u64 size = (u64) in->ops[2].u.imm;
    memcpy(dst, src, size);
    return 0;
}

/* --- PHI evaluation --- */

static void eval_phis(InterpCtx *ctx, i64 *regs, IrBlock *bb, IrBlock *pred)
{
    size_t ninstr = vec_size(bb->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        bool found = false;
        for (u32 e = 0; e < in->extra.phi.nentries; e++)
        {
            if (strcmp(in->extra.phi.entries[e].label, pred->label) == 0)
            {
                if (ctx->mod->widths[in->result] == 16)
                {
                    IrOperand v = in->extra.phi.entries[e].val;
                    ASSERT(!v.is_imm && !v.is_global && !v.is_func); /* width-16 ⇒ vreg */
                    copy_cell(ctx, regs, in->result, regs, v.u.vreg);
                }
                else
                {
                    regs[in->result] = operand_val(ctx, in->extra.phi.entries[e].val, regs);
                    apply_vreg_width(ctx, regs, in->result);
                }
                found = true;
                break;
            }
        }
        ASSERT(found && "phi entry names a real predecessor block");
    }
}

/* --- Dispatch table --- */

#define EVAL_ENTRIES(X)                                                                            \
    X(OP_ADD, eval_binary)                                                                         \
    X(OP_SUB, eval_binary)                                                                         \
    X(OP_MUL, eval_binary)                                                                         \
    X(OP_AND, eval_binary)                                                                         \
    X(OP_OR, eval_binary)                                                                          \
    X(OP_XOR, eval_binary)                                                                         \
    X(OP_SDIV, eval_divrem)                                                                        \
    X(OP_SREM, eval_divrem)                                                                        \
    X(OP_UDIV, eval_divrem)                                                                        \
    X(OP_UREM, eval_divrem)                                                                        \
    X(OP_NEG, eval_unary)                                                                          \
    X(OP_NOT, eval_unary)                                                                          \
    X(OP_SHL, eval_shift)                                                                          \
    X(OP_ASHR, eval_shift)                                                                         \
    X(OP_LSHR, eval_shift)                                                                         \
    X(OP_TRUNC, eval_trunc)                                                                        \
    X(OP_ZEXT, eval_extend)                                                                        \
    X(OP_SEXT, eval_extend)                                                                        \
    X(OP_ICMP_EQ, eval_icmp)                                                                       \
    X(OP_ICMP_NE, eval_icmp)                                                                       \
    X(OP_ICMP_ULT, eval_icmp)                                                                      \
    X(OP_ICMP_ULE, eval_icmp)                                                                      \
    X(OP_ICMP_UGT, eval_icmp)                                                                      \
    X(OP_ICMP_UGE, eval_icmp)                                                                      \
    X(OP_ICMP_SLT, eval_icmp)                                                                      \
    X(OP_ICMP_SLE, eval_icmp)                                                                      \
    X(OP_ICMP_SGT, eval_icmp)                                                                      \
    X(OP_ICMP_SGE, eval_icmp)                                                                      \
    X(OP_CALL, eval_call)                                                                          \
    X(OP_BR, eval_br)                                                                              \
    X(OP_BRCOND, eval_brcond)                                                                      \
    X(OP_SWITCH, eval_switch)                                                                      \
    X(OP_RET, eval_ret)                                                                            \
    X(OP_PHI, eval_phi)                                                                            \
    X(OP_UNREACHABLE, eval_unreachable)                                                            \
    X(OP_VA_START, eval_va_start)                                                                  \
    X(OP_VA_ARG, eval_va_arg)                                                                      \
    X(OP_VA_END, eval_va_end)                                                                      \
    X(OP_LOAD, eval_load)                                                                          \
    X(OP_STORE, eval_store)                                                                        \
    X(OP_GEP, eval_gep)                                                                            \
    X(OP_ALLOCA, eval_alloca)                                                                      \
    X(OP_MEMCPY, eval_memcpy)                                                                      \
    X(OP_ITOF, eval_itof)                                                                          \
    X(OP_FTOI, eval_ftoi)                                                                          \
    X(OP_FCONV, eval_fconv)                                                                        \
    X(OP_FADD, eval_fbin)                                                                          \
    X(OP_FSUB, eval_fbin)                                                                          \
    X(OP_FMUL, eval_fbin)                                                                          \
    X(OP_FDIV, eval_fbin)                                                                          \
    X(OP_FNEG, eval_fneg)                                                                          \
    X(OP_FCMP_EQ, eval_fcmp)                                                                       \
    X(OP_FCMP_NE, eval_fcmp)                                                                       \
    X(OP_FCMP_LT, eval_fcmp)                                                                       \
    X(OP_FCMP_GT, eval_fcmp)                                                                       \
    X(OP_FCMP_LE, eval_fcmp)                                                                       \
    X(OP_FCMP_GE, eval_fcmp)

/* Opcode dispatch table; unlisted opcodes are NULL and diagnosed in run_block. */
static const EvalFn eval_fns[] = {
#define EVAL_INIT(op, fn) [op] = fn,
    EVAL_ENTRIES(EVAL_INIT)
#undef EVAL_INIT
};

/* --- Block execution --- */

static i64 run_block(InterpCtx *ctx, i64 *regs, IrBlock *start_bb, IrBlock *start_pred)
{
    IrBlock *bb = start_bb;
    IrBlock *pred = start_pred;

    while (bb)
    {
        if (pred)
        {
            eval_phis(ctx, regs, bb, pred);
        }

        ctx->next_bb = NULL;
        ctx->next_pred = bb;
        ctx->jumped = false;
        ctx->returned = false;

        size_t ninstr = vec_size(bb->instrs);
        for (size_t i = 0; i < ninstr; i++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
            if (in->opcode == OP_PHI)
            {
                continue;
            }
            EvalFn fn = eval_fns[in->opcode];
            if (!fn)
            {
                interp_error("unsupported opcode %s", ir_opcode_name(in->opcode));
                ASSERT(false);
                return 1;
            }
            i64 result = fn(in, ctx, regs);
            if (ctx->error)
            {
                return 1;
            }
            if (ctx->returned)
            {
                return result;
            }
            if (ctx->jumped)
            {
                break;
            }
        }

        if (!ctx->next_bb)
        {
            return 0;
        }

        pred = ctx->next_pred;
        bb = ctx->next_bb;
    }

    return 0;
}

/* --- Function execution --- */

static i64 run_func(IrFunction *func, InterpCtx *ctx)
{
    Frame *frame = frame_new(ctx->frame_arena, ctx->nregs);
    vec_push(ctx->stack, frame);

    build_block_map(ctx, func);
    IrBlock *entry = (IrBlock *) vec_get(func->blocks, 0);
    i64 result = run_block(ctx, frame->regs, entry, NULL);

    vec_pop(ctx->stack);
    return result;
}

/* --- Public entry point --- */

#define ALLOCA_SIZE (1ULL << 20)

static IrFunction *find_main(IrModule *m)
{
    size_t i = func_index_by_name(m, "main");
    return i < vec_size(m->funcs) ? (IrFunction *) vec_get(m->funcs, i) : NULL;
}

static InterpGlobal *init_globals(Arena *arena, IrModule *m, u32 *out_count)
{
    u32 n = (u32) vec_size(m->globals);
    *out_count = n;
    if (n == 0)
    {
        return NULL;
    }
    InterpGlobal *globals = arena_alloc(arena, n * sizeof(InterpGlobal), _Alignof(InterpGlobal));
    for (u32 i = 0; i < n; i++)
    {
        IrGlobal *ig = (IrGlobal *) vec_get(m->globals, i);
        /* BSS globals carry no init data, so size must come from the type. */
        u32 size = (u32) type_sizeof(ig->type);
        globals[i].data = arena_alloc(arena, size, ig->align);
        if (ig->init_data)
        {
            memcpy(globals[i].data, ig->init_data, ig->init_len);
        }
        else
        {
            memset(globals[i].data, 0, size);
        }
    }
    return globals;
}

/* Patch address-constant pointer subobjects with the target's address. */
static void apply_global_relocs(IrModule *m, InterpGlobal *globals)
{
    size_t nglobals = vec_size(m->globals);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *global = (IrGlobal *) vec_get(m->globals, i);
        if (!global->relocs)
        {
            continue;
        }
        size_t nrelocs = vec_size(global->relocs);
        for (size_t r = 0; r < nrelocs; r++)
        {
            GlobalReloc *reloc = (GlobalReloc *) vec_get(global->relocs, r);
            if (reloc->is_func)
            {
                i64 addr = func_addr_by_name(m, reloc->func_name);
                memcpy(globals[i].data + reloc->offset, &addr, 8);
            }
            else
            {
                u64 addr = (u64) (uintptr_t) globals[reloc->target].data;
                memcpy(globals[i].data + reloc->offset, &addr, 8);
            }
        }
    }
}

static u64 *init_func_addrs(Arena *arena, size_t nfuncs)
{
    u64 *addrs = arena_alloc(arena, (nfuncs ? nfuncs : 1) * sizeof(u64), sizeof(u64));
    for (size_t i = 0; i < nfuncs; i++)
    {
        addrs[i] = (u64) func_addr(i);
    }
    return addrs;
}

i64 ir_interp_run(IrModule *m)
{
    IrFunction *main_fn = find_main(m);
    if (!main_fn)
    {
        interp_error("no main function found");
        return 1;
    }

    size_t nfuncs = vec_size(m->funcs);
    Arena *frame_arena = arena_new();
    Vec *stack = vec_new(frame_arena);
    StrMap *func_map = strmap_new(frame_arena);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, i);
        strmap_set(func_map, f->name, f);
    }

    u32 nglobals;
    InterpGlobal *globals = init_globals(frame_arena, m, &nglobals);
    apply_global_relocs(m, globals);

    u8 *alloca_buf = arena_alloc(frame_arena, ALLOCA_SIZE, 8);
    u64 *func_addrs = init_func_addrs(frame_arena, nfuncs);

    InterpCtx ctx = {
        .mod = m,
        .stack = stack,
        .frame_arena = frame_arena,
        .nregs = m->next_vreg,
        .func_map = func_map,
        .globals = globals,
        .nglobals = nglobals,
        .func_addrs = func_addrs,
        .nfuncs = (u32) nfuncs,
        .alloca_base = alloca_buf,
        .alloca_top = 0,
        .alloca_limit = ALLOCA_SIZE,
    };

    i64 result = run_func(main_fn, &ctx);

    arena_free(frame_arena);
    return result;
}
