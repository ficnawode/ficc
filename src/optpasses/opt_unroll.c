#include "opt_internal.h"

#include "ir.h"
#include "util/arena.h"

#include <stdio.h>
#include <string.h>

/* Fully unroll a canonical counted loop with a small compile-time trip count.
   This is an -O3 code-size/speed trade: it removes the loop-carried control
   flow so later passes (and the register allocator) see straight-line code.

   Only the simplest shape is handled; anything else is left alone:
     - exactly one latch and a preheader,
     - the loop body is a straight-line chain from the header to the latch,
     - the header's only in-loop successor is the chain head and its other
       successor (the single exit) lies outside the loop,
     - the exit block has no phis,
     - the header phis are fed by an immediate on the preheader edge and by a
       value defined inside the body on the latch edge,
     - the induction comparison and step are compile-time constants and the trip
       count is between 1 and UNROLL_MAX_ITERS iterations. */

#define UNROLL_MAX_ITERS 8
#define UNROLL_MAX_BODY_INSTRS 64

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
    u32 trip;   /* computed trip count */
} UnrollCandidate;

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

static IrInstr *terminator(IrBlock *bb)
{
    if (vec_size(bb->instrs) == 0)
    {
        return NULL;
    }
    return (IrInstr *) vec_last(bb->instrs);
}

static bool block_in_loop(Loop *loop, IrBlock *bb)
{
    return vec_has(loop->blocks, bb);
}

/* Collect the straight-line chain body_head -> ... -> latch. */
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

/* Decode `iv <COND> bound` plus `iv' = iv + step` into a trip count. Only the
   canonical counted form with constant init/bound/step and a positive step is
   supported. */
static bool trip_count(IrOperand init, IrOperand bound, IrOpcode cmp, i64 step, u32 *out)
{
    if (!init.is_imm || !bound.is_imm || step <= 0)
    {
        return false;
    }
    i64 start = init.u.imm;
    i64 limit = bound.u.imm;
    i64 n;
    if (cmp == OP_ICMP_SLT || cmp == OP_ICMP_ULT)
    {
        if (limit <= start)
        {
            return false;
        }
        n = (limit - start + step - 1) / step;
    }
    else
    {
        return false; /* <= handled by canonicalization folding to <; skip others */
    }
    if (n <= 0 || n > UNROLL_MAX_ITERS)
    {
        return false;
    }
    *out = (u32) n;
    return true;
}

/* The constant step of the induction variable, read from the chain definition
   feeding the latch edge of its phi. */
static bool induction_step(OptimizerContext *ctx, IrBlock *latch, IrOperand latch_val, i64 *step)
{
    if (!ir_operand_is_vreg(latch_val))
    {
        return false;
    }
    /* The latch block must define the update directly; otherwise the loop is
       not the simple chain we can count. */
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
    (void) ctx;
    return false;
}

/* The loop's single predecessor outside the loop (the canonicalized
   preheader). The cached LoopInfo is rebuilt between passes and loses the
   `preheader` pointer, so recover it from the header's predecessors. */
static IrBlock *loop_preheader(Loop *loop)
{
    if (loop->preheader)
    {
        return loop->preheader;
    }
    IrBlock *outside = NULL;
    size_t npred = vec_size(loop->header->preds);
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(loop->header->preds, p);
        if (vec_has(loop->latches, pred))
        {
            continue;
        }
        if (outside)
        {
            return NULL;
        }
        outside = pred;
    }
    return outside;
}

static bool find_candidate(OptimizerContext *ctx, IrFunction *f, Loop *loop, UnrollCandidate *c)
{
    IrBlock *pre = loop_preheader(loop);
    if (!pre || vec_size(loop->latches) != 1)
    {
        return false;
    }
    IrBlock *header = loop->header;
    IrBlock *latch = (IrBlock *) vec_get(loop->latches, 0);
    IrInstr *hterm = terminator(header);
    if (!hterm || hterm->opcode != OP_BRCOND)
    {
        return false;
    }
    IrBlock *t = opt_block_by_label(f, hterm->extra.brcond.true_label);
    IrBlock *e = opt_block_by_label(f, hterm->extra.brcond.false_label);
    if (!t || !e)
    {
        return false;
    }
    /* Only the `cond true -> body` orientation is handled; the compared value is
       the loop-continue condition. */
    IrBlock *body_head = NULL, *exit = NULL;
    if (block_in_loop(loop, t) && !block_in_loop(loop, e))
    {
        body_head = t;
        exit = e;
    }
    else
    {
        return false;
    }
    if (vec_size(exit->preds) != 1 || vec_get(exit->preds, 0) != header)
    {
        return false;
    }
    if (vec_size(exit->instrs) > 0 && ((IrInstr *) vec_get(exit->instrs, 0))->opcode == OP_PHI)
    {
        return false;
    }

    Vec *chain = vec_new(ctx->scratch);
    if (!collect_chain(f, body_head, latch, chain))
    {
        return false;
    }
    size_t nchain = vec_size(chain);
    if (nchain == 0)
    {
        return false;
    }
    u32 body_instrs = 0;
    for (size_t i = 0; i < nchain; i++)
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
            if (!s || !block_in_loop(loop, s))
            {
                return false;
            }
        }
    }
    if (body_instrs > UNROLL_MAX_BODY_INSTRS)
    {
        return false;
    }

    Vec *phis = vec_new(ctx->scratch);
    IrInstr *cmp = NULL;
    IrOperand bound = ir_operand_imm(0);
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
            bound = in->ops[1];
        }
    }
    if (!cmp || vec_size(phis) == 0)
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
    /* The comparison feeds only the header branch; otherwise dropping the
       header would leave its result used with no definition. */
    {
        u32 uses = 0;
        size_t nbf = vec_size(f->blocks);
        for (size_t b = 0; b < nbf; b++)
        {
            IrBlock *ob = (IrBlock *) vec_get(f->blocks, b);
            size_t nin = vec_size(ob->instrs);
            for (size_t j = 0; j < nin; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(ob->instrs, j);
                for (u8 o = 0; o < in->nops; o++)
                {
                    if (ir_operand_is_vreg(in->ops[o]) && in->ops[o].u.vreg == cmp->result)
                    {
                        uses++;
                    }
                }
            }
        }
        if (uses != 1)
        {
            return false;
        }
    }
    /* The compared value must be one of the header phis: the induction var. */
    HeaderPhi *ind = NULL;
    for (size_t p = 0; p < vec_size(phis); p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(phis, p);
        if (hp->phi->result == cmp->ops[0].u.vreg)
        {
            ind = hp;
            break;
        }
    }
    if (!ind)
    {
        return false;
    }
    i64 step = 0;
    if (!induction_step(ctx, latch, ind->latch, &step))
    {
        return false;
    }
    u32 trip = 0;
    if (!trip_count(ind->init, bound, cmp->opcode, step, &trip))
    {
        return false;
    }

    c->header = header;
    c->preheader = pre;
    c->exit = exit;
    c->chain = chain;
    c->phis = phis;
    c->trip = trip;
    return true;
}

static const char *clone_label(IrFunction *f, const char *base, u32 iter, u32 idx)
{
    int n = snprintf(NULL, 0, "unroll_%s_%u_%u", base, iter, idx);
    char *buf = arena_alloc(f->arena, (size_t) n + 1, sizeof(char));
    snprintf(buf, (size_t) n + 1, "unroll_%s_%u_%u", base, iter, idx);
    return buf;
}

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

static bool unroll_candidate(OptimizerContext *ctx, IrFunction *f, UnrollCandidate *c)
{
    IrModule *mod = ctx->mod;
    u32 nvregs = mod->width_count;
    size_t nchain = vec_size(c->chain);
    size_t nphis = vec_size(c->phis);

    IrOperand *carry = arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));
    bool *is_phi = arena_alloc(ctx->scratch, nvregs * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        is_phi[v] = false;
    }
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        carry[p] = hp->init;
        is_phi[hp->phi->result] = true;
    }

    u32 *vmap = arena_alloc(ctx->scratch, nvregs * sizeof(u32), sizeof(u32));
    IrOperand *phi_val = arena_alloc(ctx->scratch, nvregs * sizeof(IrOperand), _Alignof(IrOperand));
    IrOperand *next_carry =
        arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));

    IrBlock *prev = c->preheader;
    IrBlock *last_clone = NULL;
    for (u32 iter = 0; iter < c->trip; iter++)
    {
        for (u32 v = 0; v < nvregs; v++)
        {
            vmap[v] = NO_VREG;
            phi_val[v] = ir_operand_imm(0);
        }
        /* Fresh vregs for this iteration's defs: each clone is SSA-disjoint. */
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
            nb->is_loop_header = false;
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
                /* The shallow copy aliases the call's argument array; remap a private copy. */
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
            const char *target = (b + 1 == nchain) ? c->header->label : clone[b + 1]->label;
            IrInstr *br = ir_emit_br(nb, target);
            opt_copy_line(br, terminator(src));
        }

        opt_retarget_terminator(prev, c->header->label, clone[0]->label);
        vec_push(clone[0]->preds, prev);
        for (size_t b = 1; b < nchain; b++)
        {
            vec_push(clone[b]->preds, clone[b - 1]);
        }

        last_clone = clone[nchain - 1];
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
        prev = last_clone;
    }

    IrInstr *last = terminator(last_clone);
    if (!last || last->opcode != OP_BR)
    {
        return false;
    }
    last->extra.br.target_label = c->exit->label;

    while (vec_size(c->exit->preds) > 0 &&
           (IrBlock *) vec_get(c->exit->preds, vec_size(c->exit->preds) - 1) == c->header)
    {
        vec_pop(c->exit->preds);
    }
    vec_push(c->exit->preds, last_clone);

    /* Values carried out of the loop are the header phis; after the header is
       gone every remaining use refers to the final iteration's value. Capture
       those before the blocks disappear. */
    IrOperand *final = arena_alloc(ctx->scratch, nphis * sizeof(IrOperand), _Alignof(IrOperand));
    for (size_t p = 0; p < nphis; p++)
    {
        HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
        final[p] = carry[p];
        (void) hp;
    }

    for (size_t b = nchain; b-- > 0;)
    {
        ir_func_remove_block(f, (IrBlock *) vec_get(c->chain, b));
    }
    ir_func_remove_block(f, c->header);

    /* Rewrite remaining references to any removed header phi. */
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            for (size_t p = 0; p < nphis; p++)
            {
                HeaderPhi *hp = (HeaderPhi *) vec_get(c->phis, p);
                for (u8 o = 0; o < in->nops; o++)
                {
                    if (ir_operand_is_vreg(in->ops[o]) && in->ops[o].u.vreg == hp->phi->result)
                    {
                        in->ops[o] = final[p];
                    }
                }
                if (in->opcode == OP_PHI)
                {
                    for (u32 e = 0; e < in->extra.phi.nentries; e++)
                    {
                        IrOperand v = in->extra.phi.entries[e].val;
                        if (ir_operand_is_vreg(v) && v.u.vreg == hp->phi->result)
                        {
                            in->extra.phi.entries[e].val = final[p];
                        }
                    }
                }
                else if (in->opcode == OP_CALL)
                {
                    for (u32 a = 0; a < in->extra.call.nargs; a++)
                    {
                        IrOperand v = in->extra.call.args[a];
                        if (ir_operand_is_vreg(v) && v.u.vreg == hp->phi->result)
                        {
                            in->extra.call.args[a] = final[p];
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool opt_pass_unroll(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        LoopInfo *loops = opt_get_loops(ctx, f);
        size_t nloops = vec_size(loops->loops);
        for (size_t li = 0; li < nloops; li++)
        {
            Loop *loop = (Loop *) vec_get(loops->loops, li);
            UnrollCandidate c;
            memset(&c, 0, sizeof(c));
            if (!find_candidate(ctx, f, loop, &c))
            {
                continue;
            }
            if (unroll_candidate(ctx, f, &c))
            {
                changed = true;
                break;
            }
        }
    }
    return changed;
}
