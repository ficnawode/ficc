#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"

#include <stdio.h>
#include <string.h>

/* Self-contained partial unrolling for dynamic-trip counted loops.
 *
 * Runs late in the -O3 pipeline (after loop canonicalization has run, and after
 * the full unroller has already taken the constant-trip cases) so that no later
 * pass re-canonicalizes or rewrites the CFG this pass builds.
 *
 * A loop in canonical form
 *
 *     P -> H: (phis) cmp ; brcond cmp, B, X
 *     B .. L: body ; br H
 *
 * is rewritten into a main loop that runs the body K times per iteration plus
 * the original loop as a remainder:
 *
 *     P -> MH: (mh-phis) guard = i < bound-(K-1)*step ; brcond guard, N0, H
 *     N0 .. Nlast: K fresh copies of the body ; br MH
 *     H(remainder): (phis, preds MH+L) cmp ; brcond cmp, B, X
 *     B .. L: body ; br H
 *
 * The main header MH carries every original header phi, so the remainder
 * header's carried entry becomes an MH phi read and no value has to be moved
 * across the guard edge after the fact.
 *
 * Both the main header and the remainder header are tagged with the "pun_"
 * label prefix so a re-run of the pass never touches its own output. */

#define PARTIAL_UNROLL_FACTOR 4
#define PARTIAL_UNROLL_MAX_BODY 48
#define PUN_PREFIX "pun_"

typedef struct
{
    IrInstr *phi;
    IrOperand init;  /* value on the preheader edge */
    IrOperand latch; /* value on the latch edge (defined in the body) */
} HeaderPhi;

typedef struct
{
    IrBlock *header;
    IrBlock *preheader;
    IrBlock *exit;
    Vec *chain; /* loop blocks from body head to latch, in order */
    Vec *phis;  /* Vec<HeaderPhi*> */
    u32 body_instrs;
    int ind_idx;     /* index into phis of the induction phi, or -1 */
    i64 step;        /* positive constant induction step */
    IrOperand bound; /* induction comparison bound (imm or vreg) */
} PunCandidate;

static IrInstr *terminator(IrBlock *bb)
{
    if (vec_size(bb->instrs) == 0)
    {
        return NULL;
    }
    return (IrInstr *) vec_last(bb->instrs);
}

static bool vec_has(Vec *v, void *item)
{
    size_t n = vec_size(v);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(v, i) == item)
        {
            return true;
        }
    }
    return false;
}

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static Vec *outside_preds(IrFunction *f, Loop *loop)
{
    (void) f;
    Vec *outside = vec_new(f->arena);
    size_t npred = vec_size(loop->header->preds);
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(loop->header->preds, p);
        if (!vec_has(loop->latches, pred))
        {
            vec_push(outside, pred);
        }
    }
    return outside;
}

/* Walk body_head -> ... -> latch through unconditional branches. */
static bool collect_chain(IrFunction *f, IrBlock *body_head, IrBlock *latch, Vec *out)
{
    IrBlock *bb = body_head;
    u32 guard = 0;
    while (bb && guard++ <= vec_size(f->blocks))
    {
        vec_push(out, bb);
        if (bb == latch)
        {
            return true;
        }
        IrInstr *last = terminator(bb);
        if (!last || last->opcode != OP_BR)
        {
            return false;
        }
        bb = opt_block_by_label(f, last->extra.br.target_label);
    }
    return false;
}

/* The constant positive step of the induction variable, read from the chain
   definition feeding the latch edge of its phi. */
static bool induction_step(IrBlock *latch, IrOperand latch_val, i64 *step)
{
    if (!ir_operand_is_vreg(latch_val))
    {
        return false;
    }
    size_t ninstr = vec_size(latch->instrs);
    for (size_t j = 0; j < ninstr; j++)
    {
        IrInstr *in = (IrInstr *) vec_get(latch->instrs, j);
        if (in->result == latch_val.u.vreg && (in->opcode == OP_ADD || in->opcode == OP_SUB))
        {
            i64 c = 0;
            if (in->ops[1].is_imm)
            {
                c = in->ops[1].u.imm;
            }
            else if (in->opcode == OP_ADD && in->ops[0].is_imm)
            {
                c = in->ops[0].u.imm;
            }
            else
            {
                return false;
            }
            *step = (in->opcode == OP_SUB) ? -c : c;
            return *step > 0;
        }
    }
    return false;
}

/* True when `bound` is a vreg defined by any instruction in any loop of this
   function. A bound drawn from a loop (e.g. an enclosing loop's induction, or a
   load in this loop's own header) is not invariant in the way the guard assumes,
   so such loops are skipped. */
static bool bound_defined_in_any_loop(LoopInfo *all, IrOperand bound)
{
    if (!ir_operand_is_vreg(bound))
    {
        return false;
    }
    size_t nloops = vec_size(all->loops);
    for (size_t i = 0; i < nloops; i++)
    {
        Loop *l = (Loop *) vec_get(all->loops, i);
        size_t nblocks = vec_size(l->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(l->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                if (((IrInstr *) vec_get(bb->instrs, j))->result == bound.u.vreg)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool find_candidate(OptimizerContext *ctx, IrFunction *f, Loop *loop, LoopInfo *all,
                           PunCandidate *c)
{
    if (vec_size(loop->latches) != 1)
    {
        return false;
    }
    IrBlock *header = loop->header;
    if (has_prefix(header->label, PUN_PREFIX))
    {
        return false;
    }
    Vec *outside = outside_preds(f, loop);
    if (vec_size(outside) != 1)
    {
        return false;
    }
    IrBlock *pre = (IrBlock *) vec_get(outside, 0);
    IrBlock *latch = (IrBlock *) vec_get(loop->latches, 0);

    IrInstr *hterm = terminator(header);
    if (!hterm || hterm->opcode != OP_BRCOND)
    {
        return false;
    }
    IrBlock *t = opt_block_by_label(f, hterm->extra.brcond.true_label);
    IrBlock *e = opt_block_by_label(f, hterm->extra.brcond.false_label);
    if (!t || !e || !vec_has(loop->blocks, t) || vec_has(loop->blocks, e))
    {
        return false;
    }
    /* Single exit edge, and it must not carry phi values into the exit. */
    if (vec_size(e->preds) != 1 || vec_get(e->preds, 0) != header)
    {
        return false;
    }
    if (vec_size(e->instrs) > 0 && ((IrInstr *) vec_get(e->instrs, 0))->opcode == OP_PHI)
    {
        return false;
    }

    Vec *chain = vec_new(ctx->scratch);
    if (!collect_chain(f, t, latch, chain) || vec_size(chain) == 0)
    {
        return false;
    }
    u32 body_instrs = 0;
    for (size_t i = 0; i < vec_size(chain); i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(chain, i);
        IrInstr *last = terminator(bb);
        if (!last || last->opcode != OP_BR)
        {
            return false;
        }
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            if (((IrInstr *) vec_get(bb->instrs, j))->opcode == OP_PHI)
            {
                return false;
            }
            body_instrs++;
        }
        if (bb != latch)
        {
            IrBlock *s = opt_block_by_label(f, last->extra.br.target_label);
            if (!s || !vec_has(loop->blocks, s))
            {
                return false;
            }
        }
    }

    Vec *phis = vec_new(ctx->scratch);
    IrInstr *cmp = NULL;
    size_t nh = vec_size(header->instrs);
    for (size_t i = 0; i < nh; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(header->instrs, i);
        if (in->opcode == OP_PHI)
        {
            IrOperand init = ir_operand_imm(0), latchv = ir_operand_imm(0);
            bool have_pre = false, have_latch = false;
            for (u32 k = 0; k < in->extra.phi.nentries; k++)
            {
                const char *lab = in->extra.phi.entries[k].label;
                if (strcmp(lab, pre->label) == 0)
                {
                    init = in->extra.phi.entries[k].val;
                    have_pre = true;
                }
                else if (strcmp(lab, latch->label) == 0)
                {
                    latchv = in->extra.phi.entries[k].val;
                    have_latch = true;
                }
                else
                {
                    return false;
                }
            }
            if (!have_pre || !have_latch)
            {
                return false;
            }
            HeaderPhi *hp = arena_alloc(ctx->scratch, sizeof(HeaderPhi), _Alignof(HeaderPhi));
            hp->phi = in;
            hp->init = init;
            hp->latch = latchv;
            vec_push(phis, hp);
            continue;
        }
        if ((in->opcode == OP_ICMP_SLT || in->opcode == OP_ICMP_ULT) && !cmp)
        {
            cmp = in;
        }
    }
    /* Partial unrolling only handles signed `i < bound` with a constant step. */
    if (!cmp || cmp->opcode != OP_ICMP_SLT || vec_size(phis) == 0)
    {
        return false;
    }
    if (!ir_operand_is_vreg(cmp->ops[0]))
    {
        return false;
    }
    if (hterm->nops < 1 || !ir_operand_is_vreg(hterm->ops[0]) ||
        hterm->ops[0].u.vreg != cmp->result)
    {
        return false;
    }

    int ind_idx = -1;
    for (size_t p = 0; p < vec_size(phis); p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(phis, p);
        if (hp->phi->result == cmp->ops[0].u.vreg)
        {
            ind_idx = (int) p;
            break;
        }
    }
    if (ind_idx < 0)
    {
        return false;
    }
    HeaderPhi *ind = (HeaderPhi *) vec_get(phis, (size_t) ind_idx);
    i64 step = 0;
    if (!induction_step(latch, ind->latch, &step))
    {
        return false;
    }
    /* The guard must read a loop-invariant bound: a bound defined anywhere in
       the loop (including a non-phi instruction in the header, e.g. a load)
       would not be available in the main header on the first iteration. */
    if (cmp->ops[1].is_imm)
    {
        /* loop-invariant by construction */
    }
    else if (ir_operand_is_vreg(cmp->ops[1]) || cmp->ops[1].is_global || cmp->ops[1].is_func)
    {
        if (bound_defined_in_any_loop(all, cmp->ops[1]))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    c->header = header;
    c->preheader = pre;
    c->exit = e;
    c->chain = chain;
    c->phis = phis;
    c->body_instrs = body_instrs;
    c->ind_idx = ind_idx;
    c->step = step;
    c->bound = cmp->ops[1];
    return true;
}

static const char *clone_label(IrFunction *f, const char *base, u32 iter, u32 idx)
{
    int n = snprintf(NULL, 0, "%sclone_%s_%u_%u", PUN_PREFIX, base, iter, idx);
    char *buf = arena_alloc(f->arena, (size_t) n + 1, sizeof(char));
    snprintf(buf, (size_t) n + 1, "%sclone_%s_%u_%u", PUN_PREFIX, base, iter, idx);
    return buf;
}

/* Map `op` through an iteration's rename tables. A header phi result reads the
   carried value; a body definition reads this iteration's fresh vreg. */
static IrOperand remap_operand(u32 *vmap, IrOperand *phi_val, bool *is_phi, u32 nvregs,
                               IrOperand op)
{
    if (op.is_imm || op.is_global || op.is_func || op.u.vreg >= nvregs)
    {
        return op;
    }
    if (is_phi[op.u.vreg])
    {
        return phi_val[op.u.vreg];
    }
    if (vmap[op.u.vreg] != NO_VREG)
    {
        return ir_operand_vreg(vmap[op.u.vreg]);
    }
    return op;
}

static void add_pred_unique(IrBlock *bb, IrBlock *pred)
{
    if (!vec_has(bb->preds, pred))
    {
        vec_push(bb->preds, pred);
    }
}

/* Rewrite every textual reference to old_label in the function. */
static void retag_label(IrFunction *f, const char *old_label, const char *new_label)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            switch (in->opcode)
            {
                case OP_BR:
                    if (strcmp(in->extra.br.target_label, old_label) == 0)
                    {
                        in->extra.br.target_label = new_label;
                    }
                    break;
                case OP_BRCOND:
                    if (strcmp(in->extra.brcond.true_label, old_label) == 0)
                    {
                        in->extra.brcond.true_label = new_label;
                    }
                    if (strcmp(in->extra.brcond.false_label, old_label) == 0)
                    {
                        in->extra.brcond.false_label = new_label;
                    }
                    break;
                case OP_SWITCH:
                    for (u32 c = 0; c < in->extra.sw.ncases; c++)
                    {
                        if (strcmp(in->extra.sw.cases[c].label, old_label) == 0)
                        {
                            in->extra.sw.cases[c].label = new_label;
                        }
                    }
                    if (in->extra.sw.default_label &&
                        strcmp(in->extra.sw.default_label, old_label) == 0)
                    {
                        in->extra.sw.default_label = new_label;
                    }
                    break;
                case OP_PHI:
                    for (u32 e = 0; e < in->extra.phi.nentries; e++)
                    {
                        if (strcmp(in->extra.phi.entries[e].label, old_label) == 0)
                        {
                            in->extra.phi.entries[e].label = new_label;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
}

/* Resolve a bound operand that names a header phi to that phi's main-header
   value, so the guard reads MH's state rather than the original loop's. */
static IrOperand resolve_bound(OptimizerContext *ctx, PunCandidate *c, u32 *mh_phi, IrOperand bound)
{
    if (!ir_operand_is_vreg(bound))
    {
        return bound;
    }
    size_t nphis = vec_size(c->phis);
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        if (hp->phi->result == bound.u.vreg)
        {
            return ir_operand_vreg(mh_phi[p]);
        }
    }
    (void) ctx;
    return bound;
}

static bool partial_unroll(OptimizerContext *ctx, IrFunction *f, PunCandidate *c)
{
    IrModule *mod = ctx->mod;
    const u32 K = PARTIAL_UNROLL_FACTOR;
    u32 nvregs = mod->width_count;
    size_t nchain = vec_size(c->chain);
    size_t nphis = vec_size(c->phis);

    IrBlock *pre = c->preheader;
    IrBlock *header = c->header;
    HeaderPhi *ind = (HeaderPhi *) vec_get(c->phis, (size_t) c->ind_idx);
    u32 ind_result = ind->phi->result;
    u8 ind_width = mod->widths[ind_result];

    /* Main loop header. */
    /* Unique labels: a function can contain several unrolled loops. */
    char *mh_name =
        arena_alloc(f->arena, strlen(PUN_PREFIX "main_") + strlen(header->label) + 1, sizeof(char));
    sprintf(mh_name, "%smain_%s", PUN_PREFIX, header->label);
    IrBlock *mh = ir_func_add_block(f, mh_name);
    mh->is_loop_header = true;

    /* A main-header phi for every original header phi. */
    u32 *mh_phi = arena_alloc(ctx->scratch, nphis * sizeof(u32), sizeof(u32));
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        mh_phi[p] =
            ir_alloc_vreg(mod, mod->widths[hp->phi->result], mod->signedness[hp->phi->result],
                          mod->floatness[hp->phi->result]);
    }

    /* Guard: at least K iterations remain, i.e. `bound - i >= (K-1)*step + 1`
       with `i < bound`. The naive `i < bound - (K-1)*step` folds the constant
       into the bound, which signed-overflows when the bound is near INT_MIN
       (then the guard stays true and the loop never falls through). Compute the
       unsigned modular difference instead: when `i < bound` it is exact even
       when the true count exceeds INT_MAX, and the `i < bound` test discards the
       `i >= bound` cases where the wrapped difference is meaningless. */
    IrOperand bound_op = resolve_bound(ctx, c, mh_phi, c->bound);
    u32 udiff = ir_alloc_vreg(mod, ind_width, false, false);
    ir_emit_binop(mh, OP_SUB, udiff, bound_op, ir_operand_vreg(mh_phi[c->ind_idx]));
    u32 in_range = ir_alloc_vreg(mod, 8, false, false);
    ir_emit_binop(mh, OP_ICMP_SLT, in_range, ir_operand_vreg(mh_phi[c->ind_idx]), bound_op);
    u32 enough = ir_alloc_vreg(mod, 8, false, false);
    ir_emit_binop(mh, OP_ICMP_UGE, enough, ir_operand_vreg(udiff),
                  ir_operand_imm((i64) (K - 1) * c->step + 1));
    u32 guard = ir_alloc_vreg(mod, 8, false, false);
    ir_emit_binop(mh, OP_AND, guard, ir_operand_vreg(in_range), ir_operand_vreg(enough));

    /* Clone K copies of the chain. */
    IrOperand *carry = arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));
    bool *is_phi = arena_alloc(ctx->scratch, nvregs * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        is_phi[v] = false;
    }
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        /* Copy 0 reads the main-header phi (which holds the running state on
           every MH iteration), not the preheader init constant. */
        carry[p] = ir_operand_vreg(mh_phi[p]);
        is_phi[hp->phi->result] = true;
    }
    u32 *vmap = arena_alloc(ctx->scratch, nvregs * sizeof(u32), sizeof(u32));
    IrOperand *phi_val = arena_alloc(ctx->scratch, nvregs * sizeof(IrOperand), _Alignof(IrOperand));

    IrBlock *first = NULL;
    IrBlock *last_clone = NULL;
    IrBlock *prev_latch = NULL;
    IrOperand *final = arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));
    IrOperand *next_carry =
        arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));

    for (u32 iter = 0; iter < K; iter++)
    {
        for (u32 v = 0; v < nvregs; v++)
        {
            vmap[v] = NO_VREG;
            phi_val[v] = ir_operand_imm(0);
        }
        for (size_t b = 0; b < nchain; b++)
        {
            IrBlock *src = (IrBlock *) vec_get(c->chain, b);
            size_t ninstr = vec_size(src->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(src->instrs, j);
                if (in->result != NO_VREG)
                {
                    vmap[in->result] =
                        ir_alloc_vreg(mod, mod->widths[in->result], mod->signedness[in->result],
                                      mod->floatness[in->result]);
                }
            }
        }
        for (size_t p = 0; p < nphis; p++)
        {
            HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
            phi_val[hp->phi->result] = carry[p];
        }

        IrBlock **clone = arena_alloc(ctx->scratch, nchain * sizeof(IrBlock *), sizeof(void *));
        for (size_t b = 0; b < nchain; b++)
        {
            IrBlock *src = (IrBlock *) vec_get(c->chain, b);
            IrBlock *nb = ir_func_add_block(f, clone_label(f, src->label, iter, (u32) b));
            clone[b] = nb;
            size_t ninstr = vec_size(src->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(src->instrs, j);
                if (in->opcode == OP_BR)
                {
                    continue;
                }
                IrInstr *ni = arena_alloc(nb->arena, sizeof(IrInstr), sizeof(void *));
                *ni = *in;
                if (in->result != NO_VREG)
                {
                    ni->result = vmap[in->result];
                }
                for (u8 o = 0; o < in->nops; o++)
                {
                    ni->ops[o] = remap_operand(vmap, phi_val, is_phi, nvregs, in->ops[o]);
                }
                /* Values in payload arrays are operands too. The shallow `*ni = *in`
                   aliases the original array, so every clone must get its own copy
                   before remapping or it would rewrite the original call's args. */
                if (in->opcode == OP_CALL && in->extra.call.nargs > 0)
                {
                    IrOperand *args = arena_alloc(
                        nb->arena, in->extra.call.nargs * sizeof(IrOperand), _Alignof(IrOperand));
                    for (u32 a = 0; a < in->extra.call.nargs; a++)
                    {
                        args[a] =
                            remap_operand(vmap, phi_val, is_phi, nvregs, in->extra.call.args[a]);
                    }
                    ni->extra.call.args = args;
                    if (in->extra.call.is_indirect)
                    {
                        ni->extra.call.callee =
                            remap_operand(vmap, phi_val, is_phi, nvregs, in->extra.call.callee);
                    }
                }
                vec_push(nb->instrs, ni);
            }
        }
        for (size_t b = 0; b < nchain; b++)
        {
            IrBlock *src = (IrBlock *) vec_get(c->chain, b);
            IrBlock *nb = clone[b];
            const char *target;
            if (b + 1 < nchain)
            {
                target = clone[b + 1]->label;
            }
            else if (iter + 1 < K)
            {
                /* Not the last copy: the chain continues in the next copy. */
                target = clone_label(f, ((IrBlock *) vec_get(c->chain, 0))->label, iter + 1, 0);
            }
            else
            {
                target = mh->label;
            }
            IrInstr *br = ir_emit_br(nb, target);
            opt_copy_line(br, terminator(src));
        }
        add_pred_unique(clone[0], iter == 0 ? mh : prev_latch);
        for (size_t b = 1; b < nchain; b++)
        {
            add_pred_unique(clone[b], clone[b - 1]);
        }
        if (!first)
        {
            first = clone[0];
        }
        last_clone = clone[nchain - 1];
        prev_latch = clone[nchain - 1];

        /* Read every phi's old carry before updating any: a latch may name another phi. */
        for (size_t p = 0; p < nphis; p++)
        {
            HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
            IrOperand lv = hp->latch;
            if (ir_operand_is_vreg(lv) && lv.u.vreg < nvregs && is_phi[lv.u.vreg])
            {
                next_carry[p] = ir_operand_imm(0);
                for (size_t q = 0; q < nphis; q++)
                {
                    HeaderPhi *hq = (HeaderPhi *) vec_get(c->phis, q);
                    if (hq->phi->result == lv.u.vreg)
                    {
                        next_carry[p] = carry[q];
                        break;
                    }
                }
            }
            else if (ir_operand_is_vreg(lv) && lv.u.vreg < nvregs && vmap[lv.u.vreg] != NO_VREG)
            {
                next_carry[p] = ir_operand_vreg(vmap[lv.u.vreg]);
            }
            else
            {
                next_carry[p] = lv;
            }
        }
        for (size_t p = 0; p < nphis; p++)
        {
            carry[p] = next_carry[p];
        }
    }

    for (size_t p = 0; p < nphis; p++)
    {
        final[p] = carry[p];
    }

    /* MH guard and its back edge. The chain blocks branch to MH; record those. */
    ir_emit_brcond(mh, ir_operand_vreg(guard), first->label, header->label);

    /* MH phis: from P the original inits, from the main latch the post-K state.
       is_phi now marks the original header results; clear and re-mark for MH. */
    for (u32 v = 0; v < nvregs; v++)
    {
        is_phi[v] = false;
    }
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        IrInstr *mp = ir_emit_phi_at_start(mh, mh_phi[p], 2);
        ir_phi_add_entry(mp, hp->init, pre);
        ir_phi_add_entry(mp, final[p], last_clone);
    }

    /* Rewire the original header as the remainder:
       - its P entry becomes an MH entry reading the MH phi;
       - H's predecessor P is replaced by MH. */
    {
        size_t nh = vec_size(header->instrs);
        for (size_t i = 0; i < nh; i++)
        {
            IrInstr *in = (IrInstr *) vec_get(header->instrs, i);
            if (in->opcode != OP_PHI)
            {
                break;
            }
            for (size_t p = 0; p < nphis; p++)
            {
                HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
                if (hp->phi != in)
                {
                    continue;
                }
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    if (strcmp(in->extra.phi.entries[e].label, pre->label) == 0)
                    {
                        in->extra.phi.entries[e].val = ir_operand_vreg(mh_phi[p]);
                        in->extra.phi.entries[e].label = mh->label;
                    }
                }
                break;
            }
        }

        /* P now targets MH. */
        opt_retarget_terminator(pre, header->label, mh->label);
        add_pred_unique(mh, pre);
        add_pred_unique(mh, last_clone);

        /* Drop P from H's preds; add MH. */
        size_t npred = vec_size(header->preds);
        size_t write = 0;
        for (size_t i = 0; i < npred; i++)
        {
            IrBlock *pred = (IrBlock *) vec_get(header->preds, i);
            if (pred != pre)
            {
                vec_set(header->preds, write, pred);
                write++;
            }
        }
        while (vec_size(header->preds) > write)
        {
            vec_pop(header->preds);
        }
        add_pred_unique(header, mh);

        /* Header phis must list exactly H's preds: {MH, latch}. The MH entry was
           just rewritten; the latch entry is untouched. */

        /* Tag the remainder header. */
        char *rem = arena_alloc(f->arena, strlen(PUN_PREFIX "rem_") + strlen(header->label) + 1,
                                sizeof(char));
        sprintf(rem, "%srem_%s", PUN_PREFIX, header->label);
        retag_label(f, header->label, rem);
        header->label = rem;
    }

    return true;
}

bool opt_pass_partial_unroll(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        for (;;)
        {
            LoopInfo *loops = opt_get_loops(ctx, f);
            bool did = false;
            size_t nloops = vec_size(loops->loops);
            for (size_t li = 0; li < nloops; li++)
            {
                Loop *loop = (Loop *) vec_get(loops->loops, li);
                PunCandidate c;
                memset(&c, 0, sizeof(c));
                if (!find_candidate(ctx, f, loop, loops, &c))
                {
                    continue;
                }
                if (c.body_instrs > PARTIAL_UNROLL_MAX_BODY)
                {
                    continue;
                }
                if (partial_unroll(ctx, f, &c))
                {
                    did = true;
                    changed = true;
                    break;
                }
            }
            if (!did)
            {
                break;
            }
            /* The IR changed; cached analyses are stale. Force a rebuild. */
            ctx->cfg_epoch++;
            ctx->cache_f = NULL;
            ctx->cfg = NULL;
            ctx->doms = NULL;
            ctx->loops = NULL;
        }
    }
    return changed;
}
