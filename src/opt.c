#include "opt.h"
#include "optpasses/opt_internal.h"

#include "util/assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void opt_error(const char *fmt, ...)
{
    printf("[opt] error: ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* -O0 runs no passes; the optimizer only canonicalizes when asked to. */
static const OptPassId level1_passes[] = {
    OPT_PASS_CANON,    OPT_PASS_FOLD_CONST, OPT_PASS_IDENTITY,  OPT_PASS_CAST,      OPT_PASS_CPROP,
    OPT_PASS_PHI_SIMP, OPT_PASS_DCE,        OPT_PASS_CFG_CLEAN, OPT_PASS_PREHEADER, OPT_PASS_INLINE,
    OPT_PASS_GVN,      OPT_PASS_LICM,       OPT_PASS_MEM_FWD,
};
static const OptPassId level2_passes[] = {
    OPT_PASS_CANON,    OPT_PASS_FOLD_CONST, OPT_PASS_IDENTITY,  OPT_PASS_CAST,      OPT_PASS_CPROP,
    OPT_PASS_PHI_SIMP, OPT_PASS_DCE,        OPT_PASS_CFG_CLEAN, OPT_PASS_PREHEADER, OPT_PASS_INLINE,
    OPT_PASS_GVN,      OPT_PASS_LICM,       OPT_PASS_MEM_FWD,
};
static const OptPassId level3_passes[] = {
    OPT_PASS_CANON,     OPT_PASS_FOLD_CONST, OPT_PASS_IDENTITY, OPT_PASS_CAST,
    OPT_PASS_CPROP,     OPT_PASS_PHI_SIMP,   OPT_PASS_DCE,      OPT_PASS_CFG_CLEAN,
    OPT_PASS_PREHEADER, OPT_PASS_INLINE,     OPT_PASS_GVN,      OPT_PASS_LICM,
    OPT_PASS_MEM_FWD,   OPT_PASS_REASSOC,    OPT_PASS_STRENGTH, OPT_PASS_DSE,
};

#define PASS_COUNT(list) (sizeof(list) / sizeof((list)[0]))

static const OptConfig level0_cfg = {.passlist = {NULL, 0}, .max_iterations = 100};
static const OptConfig level1_cfg = {.passlist = {level1_passes, PASS_COUNT(level1_passes)},
                                     .max_iterations = 100};
static const OptConfig level2_cfg = {.passlist = {level2_passes, PASS_COUNT(level2_passes)},
                                     .max_iterations = 100};
static const OptConfig level3_cfg = {.passlist = {level3_passes, PASS_COUNT(level3_passes)},
                                     .max_iterations = 100};

const OptConfig *opt_config_for(OptLevel level)
{
    switch (level)
    {
        case OPT_LEVEL_0:
            return &level0_cfg;
        case OPT_LEVEL_1:
            return &level1_cfg;
        case OPT_LEVEL_2:
            return &level2_cfg;
        case OPT_LEVEL_3:
            return &level3_cfg;
        default:
            return &level0_cfg;
    }
}

/* The pass registry; each pass file fills its row's fn (NULL = reserved). */
static const OptPass opt_passes[] = {
    {OPT_PASS_FOLD_CONST, "fold_const", opt_pass_fold_const},
    {OPT_PASS_IDENTITY, "identity", opt_pass_identity},
    {OPT_PASS_CAST, "cast", opt_pass_cast},
    {OPT_PASS_CPROP, "cprop", opt_pass_cprop},
    {OPT_PASS_PHI_SIMP, "phi_simp", opt_pass_phi_simp},
    {OPT_PASS_DCE, "dce", opt_pass_dce},
    {OPT_PASS_CFG_CLEAN, "cfg_clean", opt_pass_cfg_clean},
    {OPT_PASS_PREHEADER, "preheader", opt_pass_preheader},
    {OPT_PASS_INLINE, "inline", opt_pass_inline},
    {OPT_PASS_DFE, "dfe", opt_pass_dfe},
    {OPT_PASS_GVN, "gvn", opt_pass_gvn},
    {OPT_PASS_LICM, "licm", opt_pass_licm},
    {OPT_PASS_MEM_FWD, "mem_fwd", opt_pass_mem_fwd},
    {OPT_PASS_REASSOC, "reassoc", opt_pass_reassoc},
    {OPT_PASS_STRENGTH, "strength", opt_pass_strength},
    {OPT_PASS_DSE, "dse", opt_pass_dse},
    {OPT_PASS_CANON, "canon", opt_pass_canon},
    {0},
};

static const OptPass *pass_lookup(OptPassId id)
{
    for (const OptPass *p = opt_passes; p->name; p++)
    {
        if (p->id == id)
        {
            return p;
        }
    }
    return NULL;
}

static bool pass_skipped(const char *name)
{
    const char *skip = getenv("FICC_SKIP_PASSES");
    if (!skip || !*skip)
    {
        return false;
    }
    size_t n = strlen(name);
    for (const char *p = skip; *p;)
    {
        while (*p == ' ' || *p == ',')
        {
            p++;
        }
        const char *s = p;
        while (*p && *p != ' ' && *p != ',')
        {
            p++;
        }
        size_t len = (size_t) (p - s);
        if (len == n && strncmp(s, name, n) == 0)
        {
            return true;
        }
    }
    return false;
}

static void trace_counts(IrModule *mod, size_t *instrs, size_t *blocks)
{
    size_t ni = 0, nb = 0;
    size_t nf = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nf; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        nb += vec_size(f->blocks);
        size_t nblk = vec_size(f->blocks);
        for (size_t b = 0; b < nblk; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            ni += vec_size(bb->instrs);
        }
    }
    *instrs = ni;
    *blocks = nb;
}

/* Run the config's passes to fixpoint, bounded by its iteration budget. */
static void run_pipeline(OptimizerContext *ctx)
{
    const OptConfig *cfg = ctx->opts;
    u32 max_iters = cfg->max_iterations;
    const char *env_iters = getenv("FICC_OPT_MAX_ITER");
    if (env_iters && *env_iters)
    {
        max_iters = (u32) strtoul(env_iters, NULL, 10);
    }
    u32 iterations = 0;
    for (;;)
    {
        ctx->changed = false;
        for (u32 i = 0; i < cfg->passlist.count; i++)
        {
            OptPassId id = cfg->passlist.passes[i];
            const OptPass *pass = pass_lookup(id);
            if (!pass)
            {
                opt_error("unknown pass id %d", (int) id);
                exit(1);
            }
            if (!pass->fn || pass_skipped(pass->name))
            {
                continue;
            }
            bool trace = getenv("FICC_OPT_TRACE") != NULL;
            size_t bi = 0, bb0 = 0, ai = 0, ab = 0;
            if (trace)
            {
                trace_counts(ctx->mod, &bi, &bb0);
            }
            bool pass_changed = pass->fn(ctx);
            if (pass_changed)
            {
                ctx->changed = true;
                ctx->cfg_epoch++;
            }
            if (trace)
            {
                trace_counts(ctx->mod, &ai, &ab);
                size_t abytes = arena_bytes(ctx->arena);
                fprintf(stderr, "opt: iter %u pass %s %s ins %zu->%zu blk %zu->%zu arena %zuMB\n",
                        iterations, pass->name, pass_changed ? "CHG" : "   ", bi, ai, bb0, ab,
                        abytes / (1024 * 1024));
            }
#ifdef OPT_VERIFY
            if (!opt_verify(ctx->mod))
            {
                opt_error("pass '%s' left the IR malformed", pass->name);
                exit(1);
            }
#endif
        }
        if (!ctx->changed)
        {
            break;
        }
        if (iterations >= max_iters)
        {
            opt_error("pass pipeline failed to converge");
            exit(1);
        }
        iterations++;
    }
    /* Dead function elimination runs once the rest of the pipeline stops
       cloning: a function may be unreferenced at any single iteration and
       still be referenced by a clone a later inline pass produces. */
    const OptPass *dfe = pass_lookup(OPT_PASS_DFE);
    if (dfe && dfe->fn && !pass_skipped(dfe->name))
    {
        while (dfe->fn(ctx))
        {
            ctx->cfg_epoch++;
        }
    }
}

static void ensure_caches(OptimizerContext *ctx, IrFunction *f)
{
    if (ctx->cfg && ctx->cache_f == f && ctx->cache_epoch == ctx->cfg_epoch)
    {
        return;
    }
    ctx->cfg = opt_cfg_build(f, ctx->arena);
    ctx->doms = opt_doms_build(ctx->cfg, ctx->arena);
    ctx->loops = opt_loops_find(f, ctx->cfg, ctx->doms, ctx->arena);
    ctx->cache_f = f;
    ctx->cache_epoch = ctx->cfg_epoch;
}

CfgInfo *opt_get_cfg(OptimizerContext *ctx, IrFunction *f)
{
    ensure_caches(ctx, f);
    return ctx->cfg;
}

Dominators *opt_get_doms(OptimizerContext *ctx, IrFunction *f)
{
    ensure_caches(ctx, f);
    return ctx->doms;
}

LoopInfo *opt_get_loops(OptimizerContext *ctx, IrFunction *f)
{
    ensure_caches(ctx, f);
    return ctx->loops;
}

Vec *opt_rpo_order(OptimizerContext *ctx, IrFunction *f)
{
    CfgInfo *cfg = opt_get_cfg(ctx, f);
    Vec *order = vec_new(ctx->arena);
    for (u32 k = 0; k < cfg->nreach; k++)
    {
        vec_push(order, cfg->rpo[k]);
    }
    return order;
}

u32 opt_instr_index(IrBlock *bb, IrInstr *in)
{
    size_t n = vec_size(bb->instrs);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(bb->instrs, i) == in)
        {
            return (u32) i;
        }
    }
    return UINT32_MAX;
}

void opt_erase_instr(IrBlock *bb, IrInstr *in)
{
    u32 idx = opt_instr_index(bb, in);
    ASSERT(idx != UINT32_MAX && "opt_erase_instr: instruction is not in its block");
    size_t n = vec_size(bb->instrs);
    for (size_t j = idx; j + 1 < n; j++)
    {
        vec_set(bb->instrs, j, vec_get(bb->instrs, j + 1));
    }
    vec_pop(bb->instrs);
}

void opt_insert_instr(IrBlock *bb, u32 idx, IrInstr *in)
{
    vec_insert(bb->instrs, idx, in);
}

void opt_replace_operand(IrInstr *in, u8 which, IrOperand val)
{
    ASSERT(which < in->nops && "opt_replace_operand: index out of range");
    in->ops[which] = val;
}

void opt_copy_line(IrInstr *in, const IrInstr *model)
{
    in->line = model->line;
}

static void retarget_labels(IrInstr *last, const char *old_label, const char *new_label)
{
    switch (last->opcode)
    {
        case OP_BR:
            if (strcmp(last->extra.br.target_label, old_label) == 0)
            {
                last->extra.br.target_label = new_label;
            }
            break;
        case OP_BRCOND:
            if (strcmp(last->extra.brcond.true_label, old_label) == 0)
            {
                last->extra.brcond.true_label = new_label;
            }
            if (strcmp(last->extra.brcond.false_label, old_label) == 0)
            {
                last->extra.brcond.false_label = new_label;
            }
            break;
        case OP_SWITCH:
            for (u32 c = 0; c < last->extra.sw.ncases; c++)
            {
                if (strcmp(last->extra.sw.cases[c].label, old_label) == 0)
                {
                    last->extra.sw.cases[c].label = new_label;
                }
            }
            if (last->extra.sw.default_label &&
                strcmp(last->extra.sw.default_label, old_label) == 0)
            {
                last->extra.sw.default_label = new_label;
            }
            break;
        default:
            break;
    }
}

void opt_retarget_terminator(IrBlock *from, const char *old_label, const char *new_label)
{
    if (vec_size(from->instrs) == 0)
    {
        return;
    }
    retarget_labels((IrInstr *) vec_last(from->instrs), old_label, new_label);
}

static void preds_remove(Vec *preds, IrBlock *gone)
{
    size_t write = 0;
    size_t n = vec_size(preds);
    for (size_t read = 0; read < n; read++)
    {
        if (vec_get(preds, read) != gone)
        {
            vec_set(preds, write, vec_get(preds, read));
            write++;
        }
    }
    while (vec_size(preds) > write)
    {
        vec_pop(preds);
    }
}

static void preds_push_unique(Vec *preds, IrBlock *bb)
{
    size_t n = vec_size(preds);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(preds, i) == bb)
        {
            return;
        }
    }
    vec_push(preds, bb);
}

static IrBlock *new_block(IrFunction *f, const char *prefix)
{
    size_t idx = vec_size(f->blocks);
    int n = snprintf(NULL, 0, "%s_%zu", prefix, idx);
    char *buf = arena_alloc(f->arena, (size_t) n + 1, sizeof(char));
    snprintf(buf, (size_t) n + 1, "%s_%zu", prefix, idx);
    return ir_func_add_block(f, buf);
}

static u32 alloc_merge_vreg(IrModule *mod, IrInstr *phi)
{
    return ir_alloc_vreg(mod, mod->widths[phi->result], mod->signedness[phi->result],
                         mod->floatness[phi->result]);
}

static bool pred_list_has_label(Vec *preds, const char *label)
{
    size_t n = vec_size(preds);
    for (size_t i = 0; i < n; i++)
    {
        if (strcmp(((IrBlock *) vec_get(preds, i))->label, label) == 0)
        {
            return true;
        }
    }
    return false;
}

static IrOperand phi_value_for_pred(IrInstr *phi, IrBlock *pred)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (strcmp(phi->extra.phi.entries[e].label, pred->label) == 0)
        {
            return phi->extra.phi.entries[e].val;
        }
    }
    ASSERT(false && "insert_empty_block: pred has no matching phi entry");
    return ir_operand_imm(0);
}

/* Collapse the spliced preds' entries in `phi` into one entry for the new block. */
static void phi_retag_entries(IrInstr *phi, Vec *preds, IrBlock *bb, IrOperand merged)
{
    u32 nkept = 0;
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (!pred_list_has_label(preds, phi->extra.phi.entries[e].label))
        {
            nkept++;
        }
    }
    IrPhiEntry *entries = arena_alloc(bb->arena, (nkept + 1) * sizeof(IrPhiEntry), sizeof(void *));
    u32 k = 0;
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        if (pred_list_has_label(preds, phi->extra.phi.entries[e].label))
        {
            continue;
        }
        entries[k++] = phi->extra.phi.entries[e];
    }
    entries[k].val = merged;
    entries[k].label = bb->label;
    phi->extra.phi.entries = entries;
    phi->extra.phi.nentries = nkept + 1;
    phi->extra.phi.nfilled = nkept + 1;
}

IrBlock *opt_insert_empty_block(IrModule *mod, IrFunction *f, Vec *preds, IrBlock *succ,
                                const char *prefix)
{
    ASSERT(succ != (IrBlock *) vec_get(f->blocks, 0) &&
           "insert_empty_block: cannot split an edge into the entry block");
    size_t npreds = vec_size(preds);
    ASSERT(npreds > 0 && "insert_empty_block: no predecessors to splice");

    IrBlock *bb = new_block(f, prefix);

    size_t ninstr = vec_size(succ->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(succ->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        IrInstr *mp = ir_emit_phi_at_start(bb, alloc_merge_vreg(mod, in), (u32) npreds);
        for (size_t p = 0; p < npreds; p++)
        {
            IrBlock *pred = (IrBlock *) vec_get(preds, p);
            ir_phi_add_entry(mp, phi_value_for_pred(in, pred), pred);
        }
        phi_retag_entries(in, preds, bb, ir_operand_vreg(mp->result));
    }
    ir_emit_br(bb, succ->label);

    for (size_t p = 0; p < npreds; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(preds, p);
        opt_retarget_terminator(pred, succ->label, bb->label);
        preds_remove(succ->preds, pred);
        preds_push_unique(bb->preds, pred);
    }
    preds_push_unique(succ->preds, bb);
    return bb;
}

IrBlock *opt_insert_preheader(IrModule *mod, IrFunction *f, IrBlock *pred, IrBlock *succ,
                              const char *prefix)
{
    Vec *preds = vec_new(f->arena);
    vec_push(preds, pred);
    return opt_insert_empty_block(mod, f, preds, succ, prefix);
}

/* Drop `from`'s phi entries so a block's phis stay exactly its pred set. */
static void phi_drop_pred(IrBlock *bb, const char *label)
{
    size_t ninstr = vec_size(bb->instrs);
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
        if (in->opcode != OP_PHI)
        {
            break;
        }
        u32 nentries = in->extra.phi.nentries;
        u32 nkept = 0;
        for (u32 e = 0; e < nentries; e++)
        {
            if (strcmp(in->extra.phi.entries[e].label, label) != 0)
            {
                nkept++;
            }
        }
        if (nkept == nentries)
        {
            continue;
        }
        IrPhiEntry *entries =
            arena_alloc(bb->arena, (nkept + 1) * sizeof(IrPhiEntry), sizeof(void *));
        u32 k = 0;
        for (u32 e = 0; e < nentries; e++)
        {
            if (strcmp(in->extra.phi.entries[e].label, label) == 0)
            {
                continue;
            }
            entries[k++] = in->extra.phi.entries[e];
        }
        in->extra.phi.entries = entries;
        in->extra.phi.nentries = nkept;
        in->extra.phi.nfilled = nkept;
    }
}

void opt_drop_edge(IrBlock *from, IrBlock *to)
{
    preds_remove(to->preds, from);
    phi_drop_pred(to, from->label);
}

static void count_use_operand(IrModule *mod, u32 *uses, IrOperand op)
{
    if (op.is_imm || op.is_global || op.is_func)
    {
        return;
    }
    if (op.u.vreg < mod->width_count)
    {
        uses[op.u.vreg]++;
    }
}

void opt_ensure_value_arrays(OptimizerContext *ctx)
{
    u32 n = ctx->mod->width_count;
    if (n <= ctx->value_cap)
    {
        return;
    }
    u32 cap = n < 64 ? 64 : n;
    ctx->def_vreg = arena_alloc(ctx->arena, cap * sizeof(IrInstr *), sizeof(void *));
    ctx->use_count = arena_alloc(ctx->arena, cap * sizeof(u32), sizeof(u32));
    ctx->def_block = arena_alloc(ctx->arena, cap * sizeof(IrBlock *), sizeof(void *));
    ctx->def_instr = arena_alloc(ctx->arena, cap * sizeof(IrInstr *), sizeof(void *));
    ctx->value_cap = cap;
}

void opt_make_value_analysis(OptimizerContext *ctx, IrFunction *f)
{
    IrModule *mod = ctx->mod;
    u32 nvregs = mod->width_count;
    opt_ensure_value_arrays(ctx);
    IrInstr **defs = ctx->def_vreg;
    u32 *uses = ctx->use_count;
    for (u32 v = 0; v < nvregs; v++)
    {
        defs[v] = NULL;
        uses[v] = 0;
    }

    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->result != NO_VREG)
            {
                defs[in->result] = in;
            }
            for (u8 o = 0; o < in->nops; o++)
            {
                count_use_operand(mod, uses, in->ops[o]);
            }
            if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    count_use_operand(mod, uses, in->extra.phi.entries[e].val);
                }
            }
            else if (in->opcode == OP_CALL)
            {
                for (u32 arg = 0; arg < in->extra.call.nargs; arg++)
                {
                    count_use_operand(mod, uses, in->extra.call.args[arg]);
                }
                if (in->extra.call.is_indirect)
                {
                    count_use_operand(mod, uses, in->extra.call.callee);
                }
            }
        }
    }
    ctx->def_vreg = defs;
    ctx->use_count = uses;
}

void opt_repl_begin(OptimizerContext *ctx)
{
    u32 n = ctx->mod->width_count;
    if (n > ctx->repl_cap)
    {
        u32 cap = n < 64 ? 64 : n;
        ctx->repl_val = arena_alloc(ctx->arena, cap * sizeof(IrOperand), _Alignof(IrOperand));
        ctx->repl_gen = arena_alloc(ctx->arena, cap * sizeof(u32), sizeof(u32));
        for (u32 i = 0; i < cap; i++)
        {
            ctx->repl_gen[i] = 0;
        }
        ctx->repl_cap = cap;
        ctx->repl_serial = 0;
    }
    if (++ctx->repl_serial == 0)
    {
        for (u32 i = 0; i < ctx->repl_cap; i++)
        {
            ctx->repl_gen[i] = 0;
        }
        ctx->repl_serial = 1;
    }
}

void opt_repl_set(OptimizerContext *ctx, u32 vreg, IrOperand val)
{
    ASSERT(vreg != NO_VREG && vreg < ctx->mod->width_count);
    if (vreg >= ctx->repl_cap)
    {
        return;
    }
    ctx->repl_val[vreg] = val;
    ctx->repl_gen[vreg] = ctx->repl_serial;
}

/* Follow a pending replacement to its final operand (with a cycle guard). */
static IrOperand repl_resolve(OptimizerContext *ctx, IrOperand op)
{
    u32 nvregs = ctx->mod->width_count;
    u32 guard = 0;
    while (!op.is_imm && !op.is_global && !op.is_func && op.u.vreg < nvregs &&
           op.u.vreg < ctx->repl_cap && ctx->repl_gen[op.u.vreg] == ctx->repl_serial)
    {
        op = ctx->repl_val[op.u.vreg];
        if (++guard > nvregs)
        {
            break;
        }
    }
    return op;
}

static void repl_rewrite_slot(OptimizerContext *ctx, IrOperand *slot)
{
    if (slot->is_imm || slot->is_global || slot->is_func)
    {
        return;
    }
    u32 v = slot->u.vreg;
    if (v >= ctx->repl_cap || ctx->repl_gen[v] != ctx->repl_serial)
    {
        return;
    }
    IrOperand resolved = repl_resolve(ctx, *slot);
    ctx->repl_val[v] = resolved; /* path-compress repeated chains */
    *slot = resolved;
}

bool opt_repl_apply(OptimizerContext *ctx, IrFunction *f)
{
    u32 serial = ctx->repl_serial;
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            for (u8 o = 0; o < in->nops; o++)
            {
                repl_rewrite_slot(ctx, &in->ops[o]);
            }
            if (in->opcode == OP_PHI)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    repl_rewrite_slot(ctx, &in->extra.phi.entries[e].val);
                }
            }
            else if (in->opcode == OP_CALL)
            {
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    repl_rewrite_slot(ctx, &in->extra.call.args[a]);
                }
                if (in->extra.call.is_indirect)
                {
                    repl_rewrite_slot(ctx, &in->extra.call.callee);
                }
            }
        }
    }

    bool erased = false;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t n = vec_size(bb->instrs);
        size_t write = 0;
        for (size_t read = 0; read < n; read++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, read);
            if (in->result != NO_VREG && in->result < ctx->repl_cap &&
                ctx->repl_gen[in->result] == serial)
            {
                erased = true;
                continue;
            }
            vec_set(bb->instrs, write, in);
            write++;
        }
        while (vec_size(bb->instrs) > write)
        {
            vec_pop(bb->instrs);
        }
    }
    return erased;
}

void optimize(IrModule *mod, OptLevel level, Arena *arena)
{
    if (mod == NULL || level == OPT_LEVEL_0)
    {
        return; /* -O0 runs no passes; nothing to build or run */
    }

    OptimizerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod = mod;
    ctx.arena = arena;
    ctx.opts = opt_config_for(level);

    run_pipeline(&ctx);
}
