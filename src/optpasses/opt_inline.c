#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"
#include "util/hashmap.h"

#include <stdio.h>
#include <string.h>

#define INLINE_T2_MAX_INSTRS 2
#define INLINE_T2_LOOP_FACTOR 4 /* a call in a loop amortizes its own overhead */
#define INLINE_T2_MAX_PARAMS 4
#define INLINE_T2_MAX_LOCALS 8
#define INLINE_MAX_CHAIN 16
#define INLINE_CALLER_BUDGET 800
#define INLINE_BUDGET 65536

typedef struct
{
    OptimizerContext *ctx;
    int budget;
} InlinePass;

/* Each caller's clone allowance is persisted so pipeline iterations cannot replay it. */
static u32 caller_absorbed(InlinePass *ip, IrFunction *caller)
{
    void *v = hashmap_get(ip->ctx->inline_caller_used, caller);
    return v ? (u32) (uintptr_t) v : 0;
}

static void charge_caller(InlinePass *ip, IrFunction *caller, u32 n)
{
    hashmap_set(ip->ctx->inline_caller_used, caller,
                (void *) (uintptr_t) (caller_absorbed(ip, caller) + n));
}

static bool is_special_call_name(const char *name)
{
    return strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0 ||
           strcmp(name, "sigsetjmp") == 0 || strcmp(name, "__sigsetjmp") == 0 ||
           strcmp(name, "longjmp") == 0 || strcmp(name, "_longjmp") == 0 ||
           strcmp(name, "siglongjmp") == 0;
}

static IrFunction *resolve_callee(IrModule *mod, const char *name)
{
    size_t n = vec_size(mod->funcs);
    for (size_t i = 0; i < n; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, i);
        if (strcmp(f->name, name) == 0 && vec_size(f->blocks) > 0)
        {
            return f;
        }
    }
    return NULL;
}

static bool block_in_loops(const LoopInfo *loops, IrBlock *bb)
{
    size_t n = vec_size(loops->loops);
    for (size_t i = 0; i < n; i++)
    {
        if (opt_loops_contains((Loop *) vec_get(loops->loops, i), bb))
        {
            return true;
        }
    }
    return false;
}

static bool callee_uses_va_or_special(IrFunction *callee)
{
    size_t nb = vec_size(callee->blocks);
    for (size_t b = 0; b < nb; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(callee->blocks, b);
        size_t nin = vec_size(bb->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            switch (in->opcode)
            {
                case OP_VA_START:
                case OP_VA_ARG:
                case OP_VA_END:
                    return true;
                case OP_CALL:
                    if (!in->extra.call.is_indirect && is_special_call_name(in->extra.call.name))
                    {
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return false;
}

static bool callee_entry_has_phis(IrFunction *callee)
{
    IrBlock *entry = (IrBlock *) vec_get(callee->blocks, 0);
    if (vec_size(entry->instrs) == 0)
    {
        return false;
    }
    return ((IrInstr *) vec_get(entry->instrs, 0))->opcode == OP_PHI;
}

static u32 count_body_instrs(IrFunction *callee)
{
    u32 n = 0;
    size_t nb = vec_size(callee->blocks);
    for (size_t b = 0; b < nb; b++)
    {
        size_t nin = vec_size(((IrBlock *) vec_get(callee->blocks, b))->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            if (((IrInstr *) vec_get(((IrBlock *) vec_get(callee->blocks, b))->instrs, j))
                    ->opcode != OP_PHI)
            {
                n++;
            }
        }
    }
    return n;
}

static u32 count_allocas(IrFunction *callee)
{
    u32 n = 0;
    size_t nb = vec_size(callee->blocks);
    for (size_t b = 0; b < nb; b++)
    {
        size_t nin = vec_size(((IrBlock *) vec_get(callee->blocks, b))->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            if (((IrInstr *) vec_get(((IrBlock *) vec_get(callee->blocks, b))->instrs, j))
                    ->opcode == OP_ALLOCA)
            {
                n++;
            }
        }
    }
    return n;
}

static u32 count_module_calls(IrModule *mod, const char *name)
{
    u32 n = 0;
    size_t nfuncs = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstrs = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstrs; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                if (in->opcode == OP_CALL && !in->extra.call.is_indirect && in->extra.call.name &&
                    strcmp(in->extra.call.name, name) == 0)
                {
                    n++;
                }
            }
        }
    }
    return n;
}

static bool single_use_static(IrModule *mod, IrFunction *callee)
{
    return callee->is_static && count_module_calls(mod, callee->name) == 1;
}

static bool callee_eligible(IrModule *mod, IrFunction *callee, bool in_loop)
{
    if (callee->is_variadic || callee_uses_va_or_special(callee) || callee_entry_has_phis(callee))
    {
        return false;
    }
    if (callee->is_inline || single_use_static(mod, callee))
    {
        return true;
    }
    if (vec_size(callee->params) > INLINE_T2_MAX_PARAMS ||
        count_allocas(callee) > INLINE_T2_MAX_LOCALS)
    {
        return false;
    }
    u32 limit = INLINE_T2_MAX_INSTRS * (in_loop ? INLINE_T2_LOOP_FACTOR : 1);
    return count_body_instrs(callee) <= limit;
}

static bool site_eligible(InlinePass *ip, IrFunction *caller, Vec *lin, IrFunction *callee,
                          bool in_loop)
{
    if (ip->budget <= 0 || caller_absorbed(ip, caller) >= INLINE_CALLER_BUDGET || callee == caller)
    {
        return false;
    }
    if (lin)
    {
        size_t n = vec_size(lin);
        for (size_t i = 0; i < n; i++)
        {
            if (vec_get(lin, i) == callee)
            {
                return false;
            }
        }
        if (n >= INLINE_MAX_CHAIN)
        {
            return false;
        }
    }
    return callee_eligible(ip->ctx->mod, callee, in_loop);
}

typedef struct
{
    u32 nvregs;
    IrOperand *vreg_map;
    StrMap *labels;
} InlineCtx;

static u32 clone_result(InlineCtx *ic, u32 src_result)
{
    if (src_result == NO_VREG)
    {
        return NO_VREG;
    }
    return ic->vreg_map[src_result].u.vreg;
}

static IrOperand remap_operand(InlineCtx *ic, IrOperand op)
{
    if (op.is_imm || op.is_global || op.is_func)
    {
        return op;
    }
    if (op.u.vreg < ic->nvregs)
    {
        IrOperand mapped = ic->vreg_map[op.u.vreg];
        if (mapped.is_imm || mapped.is_global || mapped.is_func || mapped.u.vreg != NO_VREG)
        {
            return mapped;
        }
    }
    return op;
}

static const char *remap_label(InlineCtx *ic, const char *label)
{
    IrBlock *nb = strmap_get(ic->labels, (const char *) label);
    return nb ? nb->label : label;
}

static const char *clone_label(Arena *a, const char *fname, u64 site, u32 idx, const char *tag)
{
    int n = snprintf(NULL, 0, "inl_%s_%llu_%u%s", fname, (unsigned long long) site, idx, tag);
    char *buf = arena_alloc(a, (size_t) n + 1, sizeof(char));
    snprintf(buf, (size_t) n + 1, "inl_%s_%llu_%u%s", fname, (unsigned long long) site, idx, tag);
    return buf;
}

static IrInstr *clone_phi(InlineCtx *ic, IrBlock *nb, const IrInstr *src)
{
    u32 n = src->extra.phi.nentries;
    IrInstr *phi = arena_alloc(nb->arena, sizeof(IrInstr), sizeof(void *));
    phi->opcode = OP_PHI;
    phi->result = clone_result(ic, src->result);
    phi->line = src->line;
    phi->nops = 0;
    phi->extra.phi.nentries = n;
    phi->extra.phi.nfilled = n;
    IrPhiEntry *entries = arena_alloc(nb->arena, n * sizeof(IrPhiEntry), sizeof(void *));
    for (u32 e = 0; e < n; e++)
    {
        entries[e].val = remap_operand(ic, src->extra.phi.entries[e].val);
        entries[e].label = remap_label(ic, src->extra.phi.entries[e].label);
    }
    phi->extra.phi.entries = entries;
    return phi;
}

static IrInstr *ret_as_br(IrBlock *nb, u32 line, const char *target)
{
    IrInstr *ni = arena_alloc(nb->arena, sizeof(IrInstr), sizeof(void *));
    ni->opcode = OP_BR;
    ni->result = NO_VREG;
    ni->line = line;
    ni->nops = 0;
    ni->extra.br.target_label = target;
    return ni;
}

static IrInstr *clone_instr(InlineCtx *ic, IrBlock *nb, const IrInstr *src)
{
    IrInstr *ni = arena_alloc(nb->arena, sizeof(IrInstr), sizeof(void *));
    ni->opcode = src->opcode;
    ni->result = clone_result(ic, src->result);
    ni->line = src->line;
    ni->nops = src->nops;
    for (u8 o = 0; o < src->nops; o++)
    {
        ni->ops[o] = remap_operand(ic, src->ops[o]);
    }

    switch (src->opcode)
    {
        case OP_CALL:
        {
            const IrCallPayload *s = &src->extra.call;
            ni->extra.call.nargs = s->nargs;
            ni->extra.call.name = s->name;
            ni->extra.call.is_variadic = s->is_variadic;
            ni->extra.call.is_indirect = s->is_indirect;
            ni->extra.call.callee = ir_operand_imm(0);
            ni->extra.call.arg_types = s->arg_types;
            ni->extra.call.ret_type = s->ret_type;
            if (s->nargs > 0)
            {
                IrOperand *args =
                    arena_alloc(nb->arena, s->nargs * sizeof(IrOperand), sizeof(IrOperand));
                for (u32 a = 0; a < s->nargs; a++)
                {
                    args[a] = remap_operand(ic, s->args[a]);
                }
                ni->extra.call.args = args;
            }
            else
            {
                ni->extra.call.args = NULL;
            }
            if (s->is_indirect)
            {
                ni->extra.call.callee = remap_operand(ic, s->callee);
            }
            break;
        }
        case OP_BR:
            ni->extra.br.target_label = remap_label(ic, src->extra.br.target_label);
            break;
        case OP_BRCOND:
            ni->extra.brcond.true_label = remap_label(ic, src->extra.brcond.true_label);
            ni->extra.brcond.false_label = remap_label(ic, src->extra.brcond.false_label);
            break;
        case OP_SWITCH:
        {
            u32 n = src->extra.sw.ncases;
            IrSwitchCase *cases = arena_alloc(nb->arena, n * sizeof(IrSwitchCase), sizeof(void *));
            for (u32 c = 0; c < n; c++)
            {
                cases[c].val = src->extra.sw.cases[c].val;
                cases[c].label = remap_label(ic, src->extra.sw.cases[c].label);
            }
            ni->extra.sw.ncases = n;
            ni->extra.sw.cases = cases;
            ni->extra.sw.default_label =
                src->extra.sw.default_label ? remap_label(ic, src->extra.sw.default_label) : NULL;
            break;
        }
        case OP_VA_START:
            ni->extra.va_start = src->extra.va_start;
            break;
        case OP_LOAD:
        case OP_STORE:
            ni->extra.mem.is_volatile = src->extra.mem.is_volatile;
            break;
        default:
            break;
    }
    return ni;
}

static Vec *child_lineage(InlinePass *ip, Vec *parent, IrFunction *callee)
{
    Vec *lin = vec_new(ip->ctx->arena);
    if (parent)
    {
        size_t n = vec_size(parent);
        for (size_t i = 0; i < n; i++)
        {
            vec_push(lin, vec_get(parent, i));
        }
    }
    vec_push(lin, callee);
    return lin;
}

static void rename_phi_pred(IrFunction *f, const char *old_label, const char *new_label)
{
    size_t nb = vec_size(f->blocks);
    for (size_t b = 0; b < nb; b++)
    {
        size_t nin = vec_size(((IrBlock *) vec_get(f->blocks, b))->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(((IrBlock *) vec_get(f->blocks, b))->instrs, j);
            if (in->opcode != OP_PHI)
            {
                break;
            }
            for (u32 e = 0; e < in->extra.phi.nfilled; e++)
            {
                if (strcmp(in->extra.phi.entries[e].label, old_label) == 0)
                {
                    in->extra.phi.entries[e].label = new_label;
                }
            }
        }
    }
}

static void hoist_to_entry(IrBlock *entry, Vec *instrs)
{
    size_t pos = 0;
    for (; pos < vec_size(entry->instrs); pos++)
    {
        if (((IrInstr *) vec_get(entry->instrs, pos))->opcode != OP_PHI)
        {
            break;
        }
    }
    for (size_t i = 0; i < vec_size(instrs); i++)
    {
        vec_insert(entry->instrs, pos++, (IrInstr *) vec_get(instrs, i));
    }
}

static bool perform_inline(InlinePass *ip, IrFunction *caller, IrBlock *bb, u32 ci, IrInstr *call,
                           IrFunction *callee, Vec *parent_lineage)
{
    IrModule *mod = ip->ctx->mod;
    Arena *arena = ip->ctx->arena;
    Arena *scratch = ip->ctx->scratch;
    u64 site = ip->ctx->inline_sites;

    u32 nparams = (u32) vec_size(callee->params);
    u32 nargs = call->extra.call.nargs;
    if (nparams != nargs)
    {
        return false;
    }

    u32 nvregs = mod->width_count;
    IrOperand *vreg_map = arena_alloc(scratch, nvregs * sizeof(IrOperand), sizeof(IrOperand));
    for (u32 v = 0; v < nvregs; v++)
    {
        vreg_map[v] = ir_operand_vreg(NO_VREG);
    }
    for (u32 p = 0; p < nparams; p++)
    {
        IrParam *param = (IrParam *) vec_get(callee->params, p);
        vreg_map[param->vreg] = call->extra.call.args[p];
    }
    size_t ncb = vec_size(callee->blocks);
    for (size_t b = 0; b < ncb; b++)
    {
        size_t nin = vec_size(((IrBlock *) vec_get(callee->blocks, b))->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(((IrBlock *) vec_get(callee->blocks, b))->instrs, j);
            if (in->result == NO_VREG)
            {
                continue;
            }
            u32 fresh = ir_alloc_vreg(mod, mod->widths[in->result], mod->signedness[in->result],
                                      mod->floatness[in->result]);
            vreg_map[in->result] = ir_operand_vreg(fresh);
        }
    }

    u32 nrets = 0;
    for (size_t b = 0; b < ncb; b++)
    {
        if (((IrInstr *) vec_last(((IrBlock *) vec_get(callee->blocks, b))->instrs))->opcode ==
            OP_RET)
        {
            nrets++;
        }
    }

    IrBlock *cont = ir_func_add_block(caller, clone_label(arena, caller->name, site, 0, "r"));
    hashmap_set(ip->ctx->inline_lineage, cont, parent_lineage);
    IrInstr *res_phi = NULL;
    u32 res = call->result;
    if (nrets > 0 && res != NO_VREG)
    {
        res_phi = ir_emit_phi_at_start(cont, res, nrets);
    }
    size_t n_bb = vec_size(bb->instrs);
    for (size_t k = ci + 1; k < n_bb; k++)
    {
        vec_push(cont->instrs, vec_get(bb->instrs, k));
    }
    while (vec_size(bb->instrs) > (size_t) ci)
    {
        vec_pop(bb->instrs);
    }
    rename_phi_pred(caller, bb->label, cont->label);

    Vec *cloned = vec_new(scratch);
    StrMap *labels = strmap_new(scratch);
    Vec *clone_lin = child_lineage(ip, parent_lineage, callee);
    for (size_t cbi = 0; cbi < ncb; cbi++)
    {
        IrBlock *cb = (IrBlock *) vec_get(callee->blocks, cbi);
        IrBlock *nb =
            ir_func_add_block(caller, clone_label(arena, caller->name, site, (u32) cbi, ""));
        nb->is_loop_header = cb->is_loop_header;
        hashmap_set(ip->ctx->inline_lineage, nb, clone_lin);
        vec_push(cloned, nb);
        strmap_set(labels, cb->label, nb);
    }

    InlineCtx ic = {.nvregs = nvregs, .vreg_map = vreg_map, .labels = labels};

    Vec *hoisted = vec_new(scratch);
    u32 ncloned = 0;
    for (size_t cbi = 0; cbi < ncb; cbi++)
    {
        IrBlock *cb = (IrBlock *) vec_get(callee->blocks, cbi);
        IrBlock *nb = (IrBlock *) vec_get(cloned, cbi);
        size_t nin = vec_size(cb->instrs);
        for (size_t j = 0; j < nin; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(cb->instrs, j);
            IrInstr *ni;
            bool is_deferred = false;
            switch (in->opcode)
            {
                case OP_PHI:
                    ni = clone_phi(&ic, nb, in);
                    break;
                case OP_RET:
                    if (res_phi)
                    {
                        ir_phi_add_entry(res_phi, remap_operand(&ic, in->ops[0]), nb);
                    }
                    ni = ret_as_br(nb, in->line, cont->label);
                    break;
                case OP_ALLOCA:
                    ni = clone_instr(&ic, nb, in);
                    vec_push(hoisted, ni);
                    is_deferred = true;
                    break;
                default:
                    ni = clone_instr(&ic, nb, in);
                    break;
            }
            if (!is_deferred)
            {
                vec_push(nb->instrs, ni);
            }
            ip->budget--;
            ncloned++;
        }
    }

    hoist_to_entry((IrBlock *) vec_get(caller->blocks, 0), hoisted);

    charge_caller(ip, caller, ncloned);
    ir_emit_br(bb, ((IrBlock *) vec_get(cloned, 0))->label);
    ip->ctx->inline_sites++;
    return true;
}

static u64 inline_ptr_hash(const void *key)
{
    return (u64) (uintptr_t) key;
}

static bool inline_ptr_eq(const void *a, const void *b)
{
    return a == b;
}

static void scan_blocks(InlinePass *ip, IrFunction *caller, const LoopInfo *loops)
{
    u32 bi = 0;
    while (bi < vec_size(caller->blocks))
    {
        IrBlock *bb = (IrBlock *) vec_get(caller->blocks, bi);
        Vec *lin = (Vec *) hashmap_get(ip->ctx->inline_lineage, bb);
        u32 j = 0;
        while (j < vec_size(bb->instrs))
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->opcode == OP_CALL && !in->extra.call.is_indirect && in->extra.call.name &&
                in->extra.call.name[0])
            {
                IrFunction *callee = resolve_callee(ip->ctx->mod, in->extra.call.name);
                bool in_loop = block_in_loops(loops, bb);
                if (callee && site_eligible(ip, caller, lin, callee, in_loop))
                {
                    if (perform_inline(ip, caller, bb, j, in, callee, lin))
                    {
                        j = 0;
                        continue;
                    }
                }
            }
            j++;
        }
        bi++;
    }
}

static void push_succ_pred(IrBlock *from, Vec *succ_preds)
{
    size_t n = vec_size(succ_preds);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(succ_preds, i) == from)
        {
            return;
        }
    }
    vec_push(succ_preds, from);
}

static void rebuild_preds(IrFunction *f, Arena *arena)
{
    StrMap *labels = opt_label_map_build(f, arena);
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        while (vec_size(bb->preds) > 0)
        {
            vec_pop(bb->preds);
        }
    }
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (vec_size(bb->instrs) == 0)
        {
            continue;
        }
        IrInstr *last = (IrInstr *) vec_last(bb->instrs);
        switch (last->opcode)
        {
            case OP_BR:
            {
                IrBlock *to = (IrBlock *) strmap_get(labels, last->extra.br.target_label);
                if (to)
                {
                    push_succ_pred(bb, to->preds);
                }
                break;
            }
            case OP_BRCOND:
            {
                IrBlock *t = (IrBlock *) strmap_get(labels, last->extra.brcond.true_label);
                if (t)
                {
                    push_succ_pred(bb, t->preds);
                }
                IrBlock *f2 = (IrBlock *) strmap_get(labels, last->extra.brcond.false_label);
                if (f2)
                {
                    push_succ_pred(bb, f2->preds);
                }
                break;
            }
            case OP_SWITCH:
                for (u32 c = 0; c < last->extra.sw.ncases; c++)
                {
                    IrBlock *to = (IrBlock *) strmap_get(labels, last->extra.sw.cases[c].label);
                    if (to)
                    {
                        push_succ_pred(bb, to->preds);
                    }
                }
                if (last->extra.sw.default_label)
                {
                    IrBlock *to = (IrBlock *) strmap_get(labels, last->extra.sw.default_label);
                    if (to)
                    {
                        push_succ_pred(bb, to->preds);
                    }
                }
                break;
            default:
                break;
        }
    }
}

static bool process_caller(InlinePass *ip, IrFunction *caller)
{
    u64 sites_before = ip->ctx->inline_sites;
    if (ip->budget <= 0)
    {
        return false;
    }
    LoopInfo *loops = opt_get_loops(ip->ctx, caller);
    scan_blocks(ip, caller, loops);
    rebuild_preds(caller, ip->ctx->scratch);
    return ip->ctx->inline_sites != sites_before;
}

bool opt_pass_inline(OptimizerContext *ctx)
{
    if (!ctx->inline_lineage)
    {
        ctx->inline_lineage = hashmap_new(ctx->arena, inline_ptr_hash, inline_ptr_eq);
    }
    if (!ctx->inline_caller_used)
    {
        ctx->inline_caller_used = hashmap_new(ctx->arena, inline_ptr_hash, inline_ptr_eq);
    }
    InlinePass ip = {.ctx = ctx, .budget = INLINE_BUDGET};
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, i);
        if (vec_size(f->blocks) == 0)
        {
            continue;
        }
        if (process_caller(&ip, f))
        {
            changed = true;
        }
    }
    return changed;
}
