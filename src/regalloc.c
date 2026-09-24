#include "regalloc.h"
#include "spill.h"
#include "target.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/vec.h"
#include <stdlib.h>

typedef struct
{
    const LiveInterval *iv;
    u8 reg;
} ActiveInterval;

typedef struct
{
    u32 pos;
    u16 mask;
} ClobberPos;

static ClobberPos *collect_clobbers(IrFunction *f, const IrPositions *pos, const TargetDesc *target,
                                    u32 *out_n, Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u32 count = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (target->instr_clobbers(target, (IrInstr *) vec_get(blk->instrs, ii)))
            {
                count++;
            }
        }
    }
    ClobberPos *out =
        arena_alloc(arena, (count ? count : 1) * sizeof(ClobberPos), _Alignof(ClobberPos));
    u32 n = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            u16 mask = target->instr_clobbers(target, (IrInstr *) vec_get(blk->instrs, ii));
            if (mask)
            {
                out[n].pos = pos->block_base[b] + (u32) 2 * (u32) ii;
                out[n].mask = mask;
                n++;
            }
        }
    }
    *out_n = n;
    return out;
}

/* GPR ids only: the mask is not meaningful for the XMM bank, whose ids share
   the same numbers. */
static u16 clobber_avoid(const ClobberPos *clob, u32 nclob, u32 start, u32 end)
{
    u16 avoid = 0;
    for (u32 c = 0; c < nclob; c++)
    {
        if (start <= clob[c].pos && clob[c].pos <= end)
        {
            avoid |= clob[c].mask;
        }
    }
    return avoid;
}

static RegAllocation *alloc_new(const LiveIntervals *set, Arena *arena)
{
    RegAllocation *alloc = arena_alloc(arena, sizeof(RegAllocation), _Alignof(RegAllocation));
    alloc->ivs = set->ivs;
    alloc->n = set->n;
    alloc->nvregs = set->nvregs;

    int *phys_map = arena_alloc(arena, set->nvregs * sizeof(int), sizeof(int));
    u32 *slot_map = arena_alloc(arena, set->nvregs * sizeof(u32), sizeof(u32));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        phys_map[v] = -1;
        slot_map[v] = 8;
    }
    alloc->phys_map = phys_map;
    alloc->slot_map = slot_map;
    alloc->saved_mask = 0;
    u8 *remat = arena_alloc(arena, set->nvregs * sizeof(u8), sizeof(u8));
    i32 *remat_disp = arena_alloc(arena, set->nvregs * sizeof(i32), sizeof(i32));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        remat[v] = 0;
        remat_disp[v] = 0;
    }
    alloc->remat = remat;
    alloc->remat_disp = remat_disp;
    return alloc;
}

static u32 *collect_call_positions(IrFunction *f, const IrPositions *pos, u32 *out_count,
                                   Arena *arena)
{
    size_t nblocks = vec_size(f->blocks);
    u32 count = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == OP_CALL)
            {
                count++;
            }
        }
    }
    u32 *calls = arena_alloc(arena, (count ? count : 1) * sizeof(u32), sizeof(u32));
    u32 n = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            if (((IrInstr *) vec_get(blk->instrs, ii))->opcode == OP_CALL)
            {
                calls[n++] = pos->block_base[b] + (u32) 2 * (u32) ii;
            }
        }
    }
    *out_count = count;
    return calls;
}

static Vec *call_site_vec(const u32 *calls, u32 ncall, Arena *arena)
{
    Vec *sites = vec_new(arena);
    for (u32 i = 0; i < ncall; i++)
    {
        u32 *p = arena_alloc(arena, sizeof(u32), sizeof(u32));
        *p = calls[i];
        vec_push(sites, p);
    }
    return sites;
}

static u32 pack_spills(const RegAllocation *alloc, const LiveIntervals *set,
                       const TargetDesc *target, Arena *arena)
{
    SlotRange *ranges =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(SlotRange), _Alignof(SlotRange));
    u32 n = 0;
    for (u32 i = 0; i < set->n; i++)
    {
        const LiveInterval *iv = &set->ivs[i];
        if (alloc->phys_map[iv->vreg] >= 0 || alloc->remat[iv->vreg])
        {
            continue;
        }
        ranges[n].vreg = iv->vreg;
        ranges[n].start = iv->start;
        ranges[n].end = iv->end;
        ranges[n].is16 = iv->width == 16;
        n++;
    }
    qsort(ranges, n, sizeof(SlotRange), spill_slot_range_cmp);
    return spill_pack_slots(target, ranges, n, alloc->slot_map, arena);
}

RegAllocation *regalloc_all_spilled(IrFunction *f, const LiveIntervals *set, Arena *arena)
{
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);
    alloc->frame_size = pack_spills(alloc, set, x86_64_target(), arena);
    return alloc;
}

static int interval_order_cmp(const void *a, const void *b)
{
    const LiveInterval *ia = *(const LiveInterval *const *) a;
    const LiveInterval *ib = *(const LiveInterval *const *) b;
    if (ia->start != ib->start)
    {
        return ia->start < ib->start ? -1 : 1;
    }
    if (ia->vreg != ib->vreg)
    {
        return ia->vreg < ib->vreg ? -1 : 1;
    }
    return 0;
}

static const LiveInterval **sorted_intervals(const LiveIntervals *set, Arena *arena)
{
    const LiveInterval **order =
        arena_alloc(arena, (set->n ? set->n : 1) * sizeof(LiveInterval *), sizeof(void *));
    for (u32 i = 0; i < set->n; i++)
    {
        order[i] = &set->ivs[i];
    }
    qsort(order, set->n, sizeof(LiveInterval *), interval_order_cmp);
    return order;
}

static int bank_callee_index(const RegBank *bank, u8 reg)
{
    for (u8 i = 0; i < bank->ncallee_saved; i++)
    {
        if (bank->callee_saved[i] == reg)
        {
            return i;
        }
    }
    return -1;
}

static bool bank_allows(const RegBank *bank, u8 reg)
{
    for (u8 i = 0; i < bank->nfixed; i++)
    {
        if (bank->fixed[i] == reg)
        {
            return false;
        }
    }
    return true;
}

/* The register of a predecessor operand that a PHI copy can share with its
   result. The first already-allocated operand of the same class wins; the
   active-set check in pick_register rejects a hint whose source is still live
   past the result's start, so only a source that dies at the copy is reused. */
static int phi_coalesce_hint(const IrInstr *phi, RegClass cls, const RegClass *vreg_cls,
                             const int *phys_map)
{
    for (u32 e = 0; e < phi->extra.phi.nentries; e++)
    {
        IrOperand v = phi->extra.phi.entries[e].val;
        if (!ir_operand_is_vreg(v))
        {
            continue;
        }
        u32 s = v.u.vreg;
        if (vreg_cls[s] == cls && phys_map[s] >= 0)
        {
            return phys_map[s];
        }
    }
    return -1;
}

/* The two-address form of these ops writes its result over `ops[0]`; the result
   can share that operand's register when the operand dies at the instruction.
   A PHI result likewise shares a predecessor operand's register. */
static int coalesce_hint(const IrInstr *def, const int *phys_map, RegClass cls,
                         const RegClass *vreg_cls)
{
    if (!def)
    {
        return -1;
    }
    switch (def->opcode)
    {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_LSHR:
        case OP_ASHR:
        case OP_NEG:
        case OP_NOT:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FNEG:
        case OP_GEP:
        case OP_LOAD:
            if (def->nops > 0 && ir_operand_is_vreg(def->ops[0]))
            {
                return phys_map[def->ops[0].u.vreg];
            }
            return -1;
        case OP_PHI:
            return phi_coalesce_hint(def, cls, vreg_cls, phys_map);
        default:
            return -1;
    }
}

static IrInstr **collect_defs(IrFunction *f, u32 nvregs, Arena *arena)
{
    IrInstr **defs = arena_alloc(arena, (nvregs ? nvregs : 1) * sizeof(IrInstr *), sizeof(void *));
    for (u32 v = 0; v < nvregs; v++)
    {
        defs[v] = NULL;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->result != NO_VREG)
            {
                defs[in->result] = in;
            }
        }
    }
    return defs;
}

/* A value live at a call must ride a callee-saved register or a slot. The start
   bound is inclusive: an entry parameter (position 0) survives an entry call. */
static bool crosses_call(const u32 *calls, u32 ncall, u32 start, u32 end)
{
    for (u32 i = 0; i < ncall; i++)
    {
        if (calls[i] >= start && calls[i] < end)
        {
            return true;
        }
    }
    return false;
}

static int pick_register(const RegBank *bank, const ActiveInterval *active, u32 nactive,
                         bool crossing, u16 avoid, int hint, u32 start)
{
    /* A coalescing hint may reuse a register still held by an interval that dies
       exactly here (end == start); any interval that lives past `start` blocks it. */
    if (hint >= 0 && bank_allows(bank, (u8) hint) && !(avoid & (u16) (1u << hint)) &&
        (!crossing || bank_callee_index(bank, (u8) hint) >= 0))
    {
        bool blocked = false;
        for (u32 a = 0; a < nactive; a++)
        {
            if (active[a].reg == (u8) hint && active[a].iv->end > start)
            {
                blocked = true;
                break;
            }
        }
        if (!blocked)
        {
            return hint;
        }
    }

    bool used[16] = {false};
    for (u32 a = 0; a < nactive; a++)
    {
        used[active[a].reg] = true;
    }
    /* A non-crossing value prefers a caller-saved register (no prologue save);
       only fall back to a callee-saved one when the caller-saved bank is full. */
    for (u8 pass = 0; pass < 2; pass++)
    {
        for (u8 i = 0; i < bank->num_regs; i++)
        {
            u8 reg = bank->names[i];
            if (used[reg] || !bank_allows(bank, reg) || (avoid & (u16) (1u << reg)))
            {
                continue;
            }
            bool callee = bank_callee_index(bank, reg) >= 0;
            if (crossing)
            {
                if (!callee)
                {
                    continue;
                }
            }
            else if ((pass == 0) == callee)
            {
                continue; /* pass 0 takes caller-saved, pass 1 callee-saved */
            }
            return reg;
        }
        if (crossing)
        {
            break; /* no second pass */
        }
    }
    return -1;
}

/* Registers a call's argument setup writes that the allocator may otherwise
   hand out. A call operand must avoid them: another argument's move could
   otherwise clobber it before it is read. */
static u16 call_arg_avoid_mask(const RegBank *bank, const u8 *args, u8 nargs)
{
    u16 mask = 0;
    for (u8 i = 0; i < nargs; i++)
    {
        if (bank_allows(bank, args[i]))
        {
            mask |= (u16) (1u << args[i]);
        }
    }
    return mask;
}

/* A call whose arguments include a by-value record: the record's stack copy
   runs through rep movsb, clobbering %rsi/%rdi/%rcx, so every argument of such
   a call must keep off the argument lanes. Unknown types stay conservative. */
static bool call_has_record_arg(const IrInstr *in)
{
    if (!in->extra.call.arg_types)
    {
        return true;
    }
    for (u32 a = 0; a < in->extra.call.nargs; a++)
    {
        Type *t = in->extra.call.arg_types[a];
        if (t && type_is_record(t))
        {
            return true;
        }
    }
    return false;
}

/* Mark every vreg whose value arrives in (caller side) or is loaded from
   (callee side) an ABI argument register. Such a vreg must not itself be
   allocated to an argument register, or one argument's move would clobber
   another's incoming value. A scalar GP argument of a record-free call is the
   exception: lowering schedules those moves as a parallel copy, so the value
   may ride its own argument lane (see collect_arg_prefs). */
static bool *mark_arg_reg_vregs(IrFunction *f, u32 nvregs, Arena *arena)
{
    bool *op = arena_alloc(arena, nvregs * sizeof(bool), sizeof(bool));
    for (u32 v = 0; v < nvregs; v++)
    {
        op[v] = false;
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        u32 pv = ((IrParam *) vec_get(f->params, i))->vreg;
        if (pv != NO_VREG)
        {
            op[pv] = true;
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
            if (in->opcode != OP_CALL)
            {
                continue;
            }
            if (in->extra.call.is_indirect && ir_operand_is_vreg(in->extra.call.callee))
            {
                op[in->extra.call.callee.u.vreg] = true;
            }
            bool conservative = call_has_record_arg(in);
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                IrOperand arg = in->extra.call.args[a];
                if (!ir_operand_is_vreg(arg))
                {
                    continue;
                }
                Type *t = in->extra.call.arg_types ? in->extra.call.arg_types[a] : NULL;
                if (conservative || (t && type_is_fp(t)))
                {
                    op[arg.u.vreg] = true;
                }
            }
        }
    }
    return op;
}

/* Preferred physical lane per vreg: a scalar GP argument of a record-free call
   is born in the lane it will be passed in, so its argument move is a no-op.
   Returns -1 for every other value. */
static int *collect_arg_prefs(IrFunction *f, u32 nvregs, const TargetDesc *target, Arena *arena)
{
    int *pref = arena_alloc(arena, nvregs * sizeof(int), sizeof(int));
    for (u32 v = 0; v < nvregs; v++)
    {
        pref[v] = -1;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_CALL || call_has_record_arg(in))
            {
                continue;
            }
            u32 gpi = 0;
            for (u32 a = 0; a < in->extra.call.nargs; a++)
            {
                Type *t = in->extra.call.arg_types ? in->extra.call.arg_types[a] : NULL;
                if (t && type_is_fp(t))
                {
                    continue; /* rides an XMM lane, not a GP one */
                }
                IrOperand arg = in->extra.call.args[a];
                if (ir_operand_is_vreg(arg) && gpi < target->ngp)
                {
                    pref[arg.u.vreg] = target->gp_args[gpi];
                }
                gpi++;
            }
        }
    }
    return pref;
}

/* When no register is free, a shorter-lived interval can take the register of
   the active interval that ends farthest. Returns the index to evict, or -1. */
static int pick_eviction(const RegBank *bank, const ActiveInterval *active, u32 nactive,
                         bool crossing, u16 avoid, u32 iv_end)
{
    int best = -1;
    u32 best_end = iv_end;
    for (u32 a = 0; a < nactive; a++)
    {
        u8 reg = active[a].reg;
        if (avoid & (u16) (1u << reg))
        {
            continue;
        }
        if (crossing && bank_callee_index(bank, reg) < 0)
        {
            continue;
        }
        if (active[a].iv->end > best_end)
        {
            best_end = active[a].iv->end;
            best = (int) a;
        }
    }
    return best;
}

static void linear_scan_class(RegAllocation *alloc, const LiveInterval **order, u32 nintervals,
                              const RegBank *bank, const u32 *calls, u32 ncall, const bool *call_op,
                              u16 arg_avoid, const ClobberPos *clob, u32 nclob, IrInstr **defs,
                              const RegClass *vreg_cls, const int *pref, Arena *arena)
{
    ActiveInterval *active = arena_alloc(
        arena, (nintervals ? nintervals : 1) * sizeof(ActiveInterval), _Alignof(ActiveInterval));
    u32 nactive = 0;
    for (u32 k = 0; k < nintervals; k++)
    {
        const LiveInterval *iv = order[k];
        if (iv->cls != bank->cls || alloc->remat[iv->vreg])
        {
            continue;
        }
        for (u32 a = 0; a < nactive;)
        {
            if (active[a].iv->end < iv->start)
            {
                active[a] = active[--nactive];
            }
            else
            {
                a++;
            }
        }
        bool crossing = crosses_call(calls, ncall, iv->start, iv->end);
        u16 avoid = call_op[iv->vreg] ? arg_avoid : 0;
        if (bank->cls == RC_GPR)
        {
            avoid |= clobber_avoid(clob, nclob, iv->start, iv->end);
        }
        int hint = coalesce_hint(defs[iv->vreg], alloc->phys_map, iv->cls, vreg_cls);
        if (hint < 0)
        {
            hint = pref[iv->vreg];
        }
        int reg = pick_register(bank, active, nactive, crossing, avoid, hint, iv->start);
        if (reg < 0)
        {
            int ev = pick_eviction(bank, active, nactive, crossing, avoid, iv->end);
            if (ev >= 0)
            {
                reg = active[ev].reg;
                alloc->phys_map[active[ev].iv->vreg] = -1;
                active[ev] = active[--nactive];
            }
        }
        if (reg < 0)
        {
            continue;
        }
        alloc->phys_map[iv->vreg] = reg;
        active[nactive].iv = iv;
        active[nactive].reg = (u8) reg;
        nactive++;
    }
}

/* %rbp without the frame-pointer reservation: it joins the allocatable
   callee-saved bank. */
static RegBank bank_with_rbp_allocatable(const RegBank *bank, u8 frame_reg)
{
    RegBank out = *bank;
    u8 n = 0;
    for (u8 i = 0; i < out.nfixed; i++)
    {
        if (out.fixed[i] != frame_reg)
        {
            out.fixed[n++] = out.fixed[i];
        }
    }
    out.nfixed = n;
    return out;
}

/* Linear scan in interval-start order: assign the lowest free register, else
   spill.  Reserved registers (implicit operands and scratch) are never handed
   out, so a fixed-encoding instruction's operands can be coerced in place. */
RegAllocation *regalloc_linear(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                               Arena *arena)
{
    return regalloc_linear_ex(f, set, target, arena, false);
}

RegAllocation *regalloc_linear_ex(IrFunction *f, const LiveIntervals *set, const TargetDesc *target,
                                  Arena *arena, bool allow_rbp)
{
    RegBank gpr =
        allow_rbp ? bank_with_rbp_allocatable(&target->gpr, target->frame_reg) : target->gpr;
    RegAllocation *alloc = alloc_new(set, arena);
    u32 ncall = 0;
    u32 *calls = collect_call_positions(f, &set->pos, &ncall, arena);
    alloc->call_sites = call_site_vec(calls, ncall, arena);

    const LiveInterval **order = sorted_intervals(set, arena);
    bool *call_op = mark_arg_reg_vregs(f, set->nvregs, arena);
    IrInstr **defs = collect_defs(f, set->nvregs, arena);
    for (u32 v = 0; v < set->nvregs; v++)
    {
        if (defs[v] && defs[v]->opcode == OP_ALLOCA)
        {
            alloc->remat[v] = 1;
        }
    }
    /* An address formed by adding a constant to a static alloca is itself a
       constant %rbp-relative address, so rematerialize it too. */
    for (u32 v = 0; v < set->nvregs; v++)
    {
        IrInstr *d = defs[v];
        if (!d || d->opcode != OP_GEP || !d->ops[1].is_imm || !ir_operand_is_vreg(d->ops[0]))
        {
            continue;
        }
        u32 bv = d->ops[0].u.vreg;
        if (bv < set->nvregs && defs[bv] && defs[bv]->opcode == OP_ALLOCA)
        {
            alloc->remat[v] = 1;
        }
    }
    RegClass *vreg_cls = arena_alloc(arena, set->nvregs * sizeof(RegClass), _Alignof(RegClass));
    for (u32 v = 0; v < set->nvregs; v++)
    {
        vreg_cls[v] = RC_NONE;
    }
    for (u32 i = 0; i < set->n; i++)
    {
        vreg_cls[set->ivs[i].vreg] = set->ivs[i].cls;
    }
    u32 nclob = 0;
    ClobberPos *clob = collect_clobbers(f, &set->pos, target, &nclob, arena);
    int *pref = collect_arg_prefs(f, set->nvregs, target, arena);
    u16 gpr_avoid = call_arg_avoid_mask(&gpr, target->gp_args, target->ngp);
    u16 xmm_avoid = call_arg_avoid_mask(&target->xmm, target->fp_args, target->nfp);
    linear_scan_class(alloc, order, set->n, &gpr, calls, ncall, call_op, gpr_avoid, clob, nclob,
                      defs, vreg_cls, pref, arena);
    linear_scan_class(alloc, order, set->n, &target->xmm, calls, ncall, call_op, xmm_avoid, clob,
                      nclob, defs, vreg_cls, pref, arena);
    u8 saved_mask = 0;
    for (u32 v = 0; v < set->nvregs; v++)
    {
        int reg = alloc->phys_map[v];
        int callee = reg >= 0 ? bank_callee_index(&gpr, (u8) reg) : -1;
        if (callee >= 0)
        {
            saved_mask |= (u8) (1u << callee);
        }
    }
    alloc->saved_mask = saved_mask;
    alloc->frame_size = pack_spills(alloc, set, target, arena);
    return alloc;
}

static const LiveInterval *interval_of(const RegAllocation *alloc, u32 vreg)
{
    u32 lo = 0;
    u32 hi = alloc->n;
    while (lo < hi)
    {
        u32 mid = lo + (hi - lo) / 2;
        u32 mv = alloc->ivs[mid].vreg;
        if (mv == vreg)
        {
            return &alloc->ivs[mid];
        }
        if (mv < vreg)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return NULL;
}

RegLoc loc_of(const RegAllocation *alloc, IrOperand op)
{
    RegLoc loc = {0};
    if (op.is_imm)
    {
        loc.kind = LOC_IMM;
        return loc;
    }
    ASSERT(ir_operand_is_vreg(op) && "globals/functions resolve to addresses, not locations");
    ASSERT(op.u.vreg < alloc->nvregs && "operand vreg within the module's table");
    const LiveInterval *iv = interval_of(alloc, op.u.vreg);
    loc.cls = iv ? iv->cls : RC_GPR;
    int phys = alloc->phys_map[op.u.vreg];
    if (phys >= 0)
    {
        loc.kind = LOC_REG;
        loc.reg = (u8) phys;
        return loc;
    }
    if (alloc->remat[op.u.vreg])
    {
        loc.kind = LOC_REMAT;
        loc.cls = RC_GPR;
        loc.disp = alloc->remat_disp[op.u.vreg];
        return loc;
    }
    loc.kind = LOC_MEM;
    loc.disp = -(i32) alloc->slot_map[op.u.vreg];
    return loc;
}
