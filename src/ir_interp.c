#include "ir_interp.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Interpreter diagnostics                                             */
/* ------------------------------------------------------------------ */

static void interp_error(const char *fmt, ...)
{
    fprintf(stderr, "[interp] error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Interpreter context                                                 */
/* ------------------------------------------------------------------ */

typedef struct InterpGlobal InterpGlobal;
struct InterpGlobal
{
    u8 *data;
    u64 size;
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
    IrBlock *current_bb; /* block currently being executed */
    IrBlock *next_bb;    /* next block to execute (set by branch ops) */
    IrBlock *next_pred;  /* the block that jumps to next_bb */
    bool jumped;
    bool returned;
    bool error; /* set on a runtime trap (e.g. null dereference) */

    /* Alloca and global state */
    u8 *alloca_base;
    u64 alloca_top;
    u64 alloca_limit;
    InterpGlobal *globals;
    u32 nglobals;
};

/* Frame: call-stack entry with register file */
typedef struct
{
    IrFunction *func;
    i64 *regs;
} Frame;

/* ------------------------------------------------------------------ */
/* Frame management                                                    */
/* ------------------------------------------------------------------ */

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

static Frame *frame_new(Arena *arena, IrFunction *func, u32 nregs)
{
    Frame *f = arena_alloc(arena, sizeof(Frame), sizeof(void *));
    f->func = func;
    f->regs = arena_alloc(arena, nregs * sizeof(i64), sizeof(i64));
    memset(f->regs, 0, nregs * sizeof(i64));
    return f;
}

/* ------------------------------------------------------------------ */
/* Width-aware masking                                                  */
/* ------------------------------------------------------------------ */

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

static void mask_vreg(InterpCtx *ctx, i64 *regs, u32 vreg)
{
    ASSERT(vreg < ctx->nregs);
    u8 w = ctx->mod->widths[vreg];
    bool is_signed = ir_vreg_signed(ctx->mod, vreg);
    regs[vreg] = is_signed ? sext_result(regs[vreg], w) : trunc_result(regs[vreg], w);
}

/* ------------------------------------------------------------------ */
/* Eval dispatch                                                       */
/* ------------------------------------------------------------------ */

typedef i64 (*EvalFn)(IrInstr *in, InterpCtx *ctx, i64 *regs);

/* Forward declarations for all eval functions */
static i64 eval_binary(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_divrem(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_unary(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_shift(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_icmp(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_call(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_br(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_brcond(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_ret(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_phi(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_trunc(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_zext(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_sext(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_unreachable(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_load(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_store(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_gep(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_alloca(IrInstr *in, InterpCtx *ctx, i64 *regs);
static i64 eval_memcpy(IrInstr *in, InterpCtx *ctx, i64 *regs);

/* Forward declaration for recursion */
static i64 run_block(InterpCtx *ctx, i64 *regs, IrBlock *start_bb, IrBlock *start_pred);

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
    X(OP_ZEXT, eval_zext)                                                                          \
    X(OP_SEXT, eval_sext)                                                                          \
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
    X(OP_RET, eval_ret)                                                                            \
    X(OP_PHI, eval_phi)                                                                            \
    X(OP_UNREACHABLE, eval_unreachable)                                                            \
    X(OP_LOAD, eval_load)                                                                          \
    X(OP_STORE, eval_store)                                                                        \
    X(OP_GEP, eval_gep)                                                                            \
    X(OP_ALLOCA, eval_alloca)                                                                      \
    X(OP_MEMCPY, eval_memcpy)

/* Dispatch table indexed by opcode; unlisted opcodes are NULL and diagnosed
   in the block loop rather than silently misinterpreting. */
static const EvalFn eval_fns[] = {
#define EVAL_INIT(op, fn) [op] = fn,
    EVAL_ENTRIES(EVAL_INIT)
#undef EVAL_INIT
};

/* ------------------------------------------------------------------ */
/* Eval functions                                                      */
/* ------------------------------------------------------------------ */

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
    mask_vreg(ctx, regs, in->result);
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
    if (in->opcode == OP_SDIV)
    {
        regs[in->result] = lhs / rhs;
    }
    else if (in->opcode == OP_SREM)
    {
        regs[in->result] = lhs % rhs;
    }
    else if (in->opcode == OP_UDIV)
    {
        regs[in->result] = (i64) ((u64) lhs / (u64) rhs);
    }
    else
    {
        regs[in->result] = (i64) ((u64) lhs % (u64) rhs);
    }
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_unary(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    if (in->opcode == OP_NEG)
    {
        regs[in->result] = -src;
    }
    else
    {
        regs[in->result] = ~src;
    }
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_shift(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 lhs = operand_val(ctx, in->ops[0], regs);
    i64 rhs = operand_val(ctx, in->ops[1], regs);
    u8 w = ctx->mod->widths[in->result];
    u32 limit = (w == 8) ? 64 : (u32) w * 8;
    if (rhs < 0 || rhs >= (i64) limit)
    {
        interp_error("shift by %lld is undefined", (long long) rhs);
        ASSERT(false);
        return 1;
    }
    if (in->opcode == OP_SHL)
    {
        regs[in->result] = lhs << rhs;
    }
    else if (in->opcode == OP_ASHR)
    {
        regs[in->result] = lhs >> rhs;
    }
    else
    {
        regs[in->result] = (i64) ((u64) lhs >> rhs);
    }
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_icmp(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) ctx;
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
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_call(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    IrFunction *callee = strmap_get(ctx->func_map, in->extra.call.name);
    if (!callee)
    {
        interp_error("undefined function '%s'", in->extra.call.name);
        return 1;
    }

    StrMap *saved_block_map = ctx->block_map;

    Frame *callee_fr = frame_new(ctx->frame_arena, callee, ctx->nregs);
    vec_push(ctx->stack, callee_fr);
    for (u32 a = 0; a < in->extra.call.nargs; a++)
    {
        IrParam *p = (IrParam *) vec_get(callee->params, a);
        callee_fr->regs[p->vreg] = operand_val(ctx, in->extra.call.args[a], regs);
        u8 pw = ctx->mod->widths[p->vreg];
        callee_fr->regs[p->vreg] = type_is_signed(p->type)
                                       ? sext_result(callee_fr->regs[p->vreg], pw)
                                       : trunc_result(callee_fr->regs[p->vreg], pw);
    }

    /* Build callee's block map */
    ctx->block_map = strmap_new(ctx->frame_arena);
    size_t nblocks = vec_size(callee->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(callee->blocks, bi);
        strmap_set(ctx->block_map, blk->label, blk);
    }

    IrBlock *entry = (IrBlock *) vec_get(callee->blocks, 0);
    i64 ret = run_block(ctx, callee_fr->regs, entry, NULL);
    vec_pop(ctx->stack);

    /* Callee's run_block sets returned/jumped; reset for caller's loop. */
    ctx->returned = false;
    ctx->jumped = false;
    ctx->block_map = saved_block_map;
    if (in->result != NO_VREG)
    {
        regs[in->result] = ret;
        mask_vreg(ctx, regs, in->result);
    }
    return 0;
}

static i64 eval_br(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    (void) regs;
    IrBlock *target = strmap_get(ctx->block_map, in->extra.br.target_label);
    ASSERT(target != NULL && "branch target names a block the IR builder created");
    ctx->next_bb = target;
    ctx->jumped = true;
    return 0;
}

static i64 eval_brcond(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 cond = operand_val(ctx, in->ops[0], regs);
    const char *target_label = cond ? in->extra.brcond.true_label : in->extra.brcond.false_label;
    IrBlock *target = strmap_get(ctx->block_map, target_label);
    ASSERT(target != NULL && "branch target names a block the IR builder created");
    ctx->next_bb = target;
    ctx->jumped = true;
    return 0;
}

static i64 eval_ret(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 result = 0;
    if (in->nops > 0)
    {
        result = operand_val(ctx, in->ops[0], regs);
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
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_zext(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    if (in->ops[0].is_imm)
    {
        regs[in->result] = src;
        return 0;
    }
    u8 src_w = ctx->mod->widths[in->ops[0].u.vreg];
    regs[in->result] = trunc_result(src, src_w);
    return 0;
}

static i64 eval_sext(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 src = operand_val(ctx, in->ops[0], regs);
    if (in->ops[0].is_imm)
    {
        regs[in->result] = src;
        return 0;
    }
    u8 src_w = ctx->mod->widths[in->ops[0].u.vreg];
    regs[in->result] = sext_result(src, src_w);
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

static i64 eval_load(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    u8 *addr = resolve_ptr(ctx, in->ops[0], regs);
    if (!addr)
    {
        return 1;
    }
    u8 w = ctx->mod->widths[in->result];
    i64 val = 0;
    memcpy(&val, addr, w > 8 ? 8 : w);
    regs[in->result] = val;
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_store(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 val = operand_val(ctx, in->ops[0], regs);
    u8 *addr = resolve_ptr(ctx, in->ops[1], regs);
    if (!addr)
    {
        return 1;
    }
    u32 w = (u32) in->ops[2].u.imm;
    memcpy(addr, &val, w > 8 ? 8 : w);
    return 0;
}

static i64 eval_gep(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 base_val;
    if (in->ops[0].is_global)
    {
        base_val = (i64) (uintptr_t) resolve_ptr(ctx, in->ops[0], regs);
    }
    else
    {
        base_val = operand_val(ctx, in->ops[0], regs);
    }
    i64 index = operand_val(ctx, in->ops[1], regs);
    i64 stride = in->ops[2].u.imm;
    regs[in->result] = base_val + index * stride;
    mask_vreg(ctx, regs, in->result);
    return 0;
}

static i64 eval_alloca(IrInstr *in, InterpCtx *ctx, i64 *regs)
{
    i64 size = operand_val(ctx, in->ops[0], regs);
    u64 aligned = ((u64) size + 7) & ~7ULL;
    if (ctx->alloca_top + aligned > ctx->alloca_limit)
    {
        interp_error("stack overflow");
        return 1;
    }
    regs[in->result] = (i64) (uintptr_t) (ctx->alloca_base + ctx->alloca_top);
    ctx->alloca_top += aligned;
    mask_vreg(ctx, regs, in->result);
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

/* ------------------------------------------------------------------ */
/* PHI evaluation                                                      */
/* ------------------------------------------------------------------ */

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
                regs[in->result] = operand_val(ctx, in->extra.phi.entries[e].val, regs);
                mask_vreg(ctx, regs, in->result);
                found = true;
                break;
            }
        }
        ASSERT(found && "phi entry names a real predecessor block");
    }
}

/* ------------------------------------------------------------------ */
/* Block execution                                                     */
/* ------------------------------------------------------------------ */

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

        ctx->current_bb = bb;
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

/* ------------------------------------------------------------------ */
/* Function execution                                                  */
/* ------------------------------------------------------------------ */

static i64 run_func(IrFunction *func, InterpCtx *ctx)
{
    Frame *fr = frame_new(ctx->frame_arena, func, ctx->nregs);
    vec_push(ctx->stack, fr);

    ctx->block_map = strmap_new(ctx->frame_arena);
    size_t nblocks = vec_size(func->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *blk = (IrBlock *) vec_get(func->blocks, bi);
        strmap_set(ctx->block_map, blk->label, blk);
    }

    IrBlock *entry = (IrBlock *) vec_get(func->blocks, 0);
    i64 result = run_block(ctx, fr->regs, entry, NULL);

    vec_pop(ctx->stack);
    return result;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

i64 ir_interp_run(IrModule *m)
{
    IrFunction *main_fn = NULL;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, i);
        if (strcmp(f->name, "main") == 0)
        {
            main_fn = f;
            break;
        }
    }
    if (!main_fn)
    {
        interp_error("no main function found");
        return 1;
    }

    Arena *frame_arena = arena_new();
    Vec *stack = vec_new(frame_arena);
    u32 nregs = m->next_vreg;
    StrMap *func_map = strmap_new(frame_arena);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, i);
        strmap_set(func_map, f->name, f);
    }

    InterpGlobal *globals = NULL;
    u32 nglobals = (u32) vec_size(m->globals);
    if (nglobals > 0)
    {
        globals = arena_alloc(frame_arena, nglobals * sizeof(InterpGlobal), _Alignof(InterpGlobal));
        for (u32 i = 0; i < nglobals; i++)
        {
            IrGlobal *ig = (IrGlobal *) vec_get(m->globals, i);
            /* BSS globals carry no init data, so size must come from the type,
               not init_len (which is 0). */
            u32 size = (u32) type_sizeof(ig->type);
            globals[i].size = size;
            globals[i].data = arena_alloc(frame_arena, size, ig->align);
            if (ig->init_data)
            {
                memcpy(globals[i].data, ig->init_data, ig->init_len);
            }
            else
            {
                memset(globals[i].data, 0, size);
            }
        }
        /* Second pass: pointer globals initialized to strings hold the target's
           address (the ELF writer patches this via .rela.data). */
        for (u32 i = 0; i < nglobals; i++)
        {
            IrGlobal *ig = (IrGlobal *) vec_get(m->globals, i);
            if (ig->init_reloc_target >= 0)
            {
                u64 addr = (u64) (uintptr_t) globals[ig->init_reloc_target].data;
                memcpy(globals[i].data, &addr, 8);
                globals[i].size = 8;
            }
        }
    }

#define ALLOCA_SIZE (1ULL << 20)
    u8 *alloca_buf = arena_alloc(frame_arena, ALLOCA_SIZE, 8);

    InterpCtx ctx = {
        .mod = m,
        .stack = stack,
        .frame_arena = frame_arena,
        .nregs = nregs,
        .func_map = func_map,
        .globals = globals,
        .nglobals = nglobals,
        .alloca_base = alloca_buf,
        .alloca_top = 0,
        .alloca_limit = ALLOCA_SIZE,
    };

    i64 result = run_func(main_fn, &ctx);

    arena_free(frame_arena);
    return result;
}
