#include "opt_internal.h"

#include "ir.h"
#include "util/assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Byte width of the x87 long double value class. */
#define LD_BYTES 16

static void verify_error(const char *fmt, ...)
{
    fprintf(stderr, "[ir] error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool is_terminator_op(IrOpcode op)
{
    return op == OP_RET || op == OP_UNREACHABLE || op == OP_BR || op == OP_BRCOND ||
           op == OP_SWITCH;
}

static const char *opcode_short_name(IrOpcode op)
{
    return ir_opcode_name(op) + 3;
}

typedef enum
{
    RESULT_NONE,
    RESULT_REQUIRED,
    RESULT_OPTIONAL,
} ResultKind;

typedef struct
{
    u8 min_ops;
    u8 max_ops;
    bool is_legal;
    ResultKind result;
} OpShape;

static OpShape op_shape(IrOpcode op)
{
    switch (op)
    {
        case OP_RET:
            return (OpShape) {.min_ops = 0, .max_ops = 1, .result = RESULT_NONE, .is_legal = true};
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_SDIV:
        case OP_UDIV:
        case OP_SREM:
        case OP_UREM:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_LSHR:
        case OP_ASHR:
        case OP_ICMP_EQ:
        case OP_ICMP_NE:
        case OP_ICMP_ULT:
        case OP_ICMP_ULE:
        case OP_ICMP_UGT:
        case OP_ICMP_UGE:
        case OP_ICMP_SLT:
        case OP_ICMP_SLE:
        case OP_ICMP_SGT:
        case OP_ICMP_SGE:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FCMP_EQ:
        case OP_FCMP_NE:
        case OP_FCMP_LT:
        case OP_FCMP_GT:
        case OP_FCMP_LE:
        case OP_FCMP_GE:
            return (OpShape) {
                .min_ops = 2, .max_ops = 2, .result = RESULT_REQUIRED, .is_legal = true};
        case OP_NEG:
        case OP_NOT:
        case OP_TRUNC:
        case OP_ZEXT:
        case OP_SEXT:
        case OP_LOAD:
        case OP_ALLOCA:
        case OP_VA_ARG:
        case OP_ITOF:
        case OP_FTOI:
        case OP_FCONV:
        case OP_FNEG:
            return (OpShape) {
                .min_ops = 1, .max_ops = 1, .result = RESULT_REQUIRED, .is_legal = true};
        case OP_BRCOND:
        case OP_SWITCH:
        case OP_VA_START:
        case OP_VA_END:
            return (OpShape) {.min_ops = 1, .max_ops = 1, .result = RESULT_NONE, .is_legal = true};
        case OP_GEP:
            return (OpShape) {
                .min_ops = 3, .max_ops = 3, .result = RESULT_REQUIRED, .is_legal = true};
        case OP_STORE:
        case OP_MEMCPY:
            return (OpShape) {.min_ops = 3, .max_ops = 3, .result = RESULT_NONE, .is_legal = true};
        case OP_UNREACHABLE:
        case OP_BR:
            return (OpShape) {.min_ops = 0, .max_ops = 0, .result = RESULT_NONE, .is_legal = true};
        case OP_CALL:
            return (OpShape) {
                .min_ops = 0, .max_ops = 0, .result = RESULT_OPTIONAL, .is_legal = true};
        case OP_PHI:
            return (OpShape) {
                .min_ops = 0, .max_ops = 0, .result = RESULT_REQUIRED, .is_legal = true};
        default:
            return (OpShape) {.is_legal = false};
    }
}

static bool is_param_vreg(IrFunction *f, u32 vreg)
{
    size_t n = vec_size(f->params);
    for (size_t i = 0; i < n; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        if (p->vreg == vreg)
        {
            return true;
        }
    }
    return false;
}

static void check_operand(IrModule *mod, IrFunction *f, IrInstr **defs, IrFunction **def_funcs,
                          IrOperand op, bool *ok)
{
    if (op.is_global)
    {
        if (op.u.global_index >= vec_size(mod->globals))
        {
            verify_error("operand references global %zu, but the module has %zu globals",
                         (size_t) op.u.global_index, vec_size(mod->globals));
            *ok = false;
        }
        return;
    }
    if (op.is_imm || op.is_func)
    {
        return;
    }
    if (op.u.vreg >= mod->width_count)
    {
        verify_error("operand references vreg %u, but width_count is %u", op.u.vreg,
                     mod->width_count);
        *ok = false;
        return;
    }
    if (!defs[op.u.vreg])
    {
        if (!is_param_vreg(f, op.u.vreg))
        {
            verify_error("operand uses vreg %u, which no instruction defines", op.u.vreg);
            *ok = false;
        }
        return;
    }
    if (def_funcs[op.u.vreg] != f)
    {
        verify_error("operand uses vreg %u, defined in function '%s'", op.u.vreg,
                     def_funcs[op.u.vreg]->name);
        *ok = false;
    }
}

/* Long-double values ride vregs, except the zero literal the builder emits. */
static void check_wide_operand(IrOperand op, IrBlock *bb, IrOpcode opcode, bool *ok)
{
    if (op.is_imm && op.u.imm == 0)
    {
        return;
    }
    if (op.is_imm || op.is_global || op.is_func)
    {
        verify_error("block '%s': %s width-16 value must be a vreg", bb->label,
                     opcode_short_name(opcode));
        *ok = false;
    }
}

static void check_call_args(IrModule *mod, IrFunction *f, IrInstr *in, IrInstr **defs,
                            IrFunction **def_funcs, bool *ok)
{
    for (u32 a = 0; a < in->extra.call.nargs; a++)
    {
        check_operand(mod, f, defs, def_funcs, in->extra.call.args[a], ok);
    }
    if (in->extra.call.is_indirect)
    {
        check_operand(mod, f, defs, def_funcs, in->extra.call.callee, ok);
    }
}

static void check_call_ld_args(IrModule *mod, IrBlock *bb, IrInstr *in, bool *ok)
{
    if (in->extra.call.is_indirect)
    {
        return;
    }
    size_t nfuncs = vec_size(mod->funcs);
    IrFunction *callee = NULL;
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *c = (IrFunction *) vec_get(mod->funcs, i);
        if (strcmp(c->name, in->extra.call.name) == 0)
        {
            callee = c;
            break;
        }
    }
    if (!callee)
    {
        return;
    }
    size_t nparams = vec_size(callee->params);
    u32 nargs = in->extra.call.nargs;
    for (u32 a = 0; a < nargs && a < nparams; a++)
    {
        IrParam *p = (IrParam *) vec_get(callee->params, a);
        if (p->vreg >= mod->width_count || mod->widths[p->vreg] != LD_BYTES)
        {
            continue;
        }
        IrOperand arg = in->extra.call.args[a];
        if (arg.is_imm || arg.is_global || arg.is_func)
        {
            verify_error("block '%s': call to '%s' has a long-double argument that is not a vreg",
                         bb->label, callee->name);
            *ok = false;
        }
    }
}

static IrBlock *find_block_by_label(IrFunction *f, const char *label)
{
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (strcmp(bb->label, label) == 0)
        {
            return bb;
        }
    }
    return NULL;
}

static void check_targets_exist(IrFunction *f, IrInstr *in, IrBlock *bb, bool *ok)
{
    switch (in->opcode)
    {
        case OP_BR:
            if (!find_block_by_label(f, in->extra.br.target_label))
            {
                verify_error("block '%s': br targets unknown block '%s'", bb->label,
                             in->extra.br.target_label);
                *ok = false;
            }
            break;
        case OP_BRCOND:
            if (!find_block_by_label(f, in->extra.brcond.true_label) ||
                !find_block_by_label(f, in->extra.brcond.false_label))
            {
                verify_error("block '%s': brcond targets unknown block", bb->label);
                *ok = false;
            }
            break;
        case OP_SWITCH:
            for (u32 c = 0; c < in->extra.sw.ncases; c++)
            {
                if (!find_block_by_label(f, in->extra.sw.cases[c].label))
                {
                    verify_error("block '%s': switch case targets unknown block '%s'", bb->label,
                                 in->extra.sw.cases[c].label);
                    *ok = false;
                }
            }
            if (in->extra.sw.default_label && !find_block_by_label(f, in->extra.sw.default_label))
            {
                verify_error("block '%s': switch default targets unknown block '%s'", bb->label,
                             in->extra.sw.default_label);
                *ok = false;
            }
            break;
        default:
            break;
    }
}

static void check_payloads(IrModule *mod, IrFunction *f, IrBlock *bb, IrInstr *in, IrInstr **defs,
                           IrFunction **def_funcs, bool *ok)
{
    switch (in->opcode)
    {
        case OP_STORE:
            if (in->nops >= 3 && in->ops[2].is_imm && (u32) in->ops[2].u.imm == LD_BYTES)
            {
                check_wide_operand(in->ops[0], bb, in->opcode, ok);
            }
            break;
        case OP_PHI:
            if (in->extra.phi.nfilled != in->extra.phi.nentries)
            {
                verify_error("block '%s': PHI has %u of %u entries filled", bb->label,
                             in->extra.phi.nfilled, in->extra.phi.nentries);
                *ok = false;
            }
            else if (in->result < mod->width_count && mod->widths[in->result] == LD_BYTES)
            {
                for (u32 e = 0; e < in->extra.phi.nentries; e++)
                {
                    check_wide_operand(in->extra.phi.entries[e].val, bb, in->opcode, ok);
                }
            }
            break;
        case OP_CALL:
            check_call_args(mod, f, in, defs, def_funcs, ok);
            check_call_ld_args(mod, bb, in, ok);
            break;
        default:
            check_targets_exist(f, in, bb, ok);
            break;
    }
}

static void check_block(IrModule *mod, IrFunction *f, IrBlock *bb, IrInstr **defs,
                        IrFunction **def_funcs, bool *ok)
{
    size_t ninstr = vec_size(bb->instrs);
    if (ninstr == 0)
    {
        verify_error("block '%s' is empty", bb->label);
        *ok = false;
        return;
    }

    bool saw_non_phi = false;
    for (size_t i = 0; i < ninstr; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
        IrOpcode op = in->opcode;

        if (op == OP_SELECT)
        {
            verify_error("reserved opcode OP_SELECT is not lowered by any backend");
            *ok = false;
        }
        if (op == OP_PHI && saw_non_phi)
        {
            verify_error("block '%s': PHI after a non-PHI instruction", bb->label);
            *ok = false;
        }
        if (is_terminator_op(op) && i != ninstr - 1)
        {
            verify_error("block '%s': %s is not the final instruction", bb->label,
                         opcode_short_name(op));
            *ok = false;
        }
        if (op != OP_PHI)
        {
            saw_non_phi = true;
        }

        OpShape shape = op_shape(op);
        if (!shape.is_legal)
        {
            verify_error("block '%s': opcode '%s' is not a legal instruction shape", bb->label,
                         ir_opcode_name(op));
            *ok = false;
        }
        else if (in->nops < shape.min_ops || in->nops > shape.max_ops)
        {
            verify_error("block '%s': %s has %u operands, expected %u to %u", bb->label,
                         opcode_short_name(op), in->nops, shape.min_ops, shape.max_ops);
            *ok = false;
        }

        if (shape.is_legal && shape.result == RESULT_REQUIRED && in->result == NO_VREG)
        {
            verify_error("block '%s': %s requires a result vreg", bb->label, opcode_short_name(op));
            *ok = false;
        }
        if (shape.is_legal && shape.result == RESULT_NONE && in->result != NO_VREG)
        {
            verify_error("block '%s': %s must not define vreg %u", bb->label, opcode_short_name(op),
                         in->result);
            *ok = false;
        }

        for (u8 oi = 0; oi < in->nops; oi++)
        {
            check_operand(mod, f, defs, def_funcs, in->ops[oi], ok);
        }

        check_payloads(mod, f, bb, in, defs, def_funcs, ok);
    }

    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    if (!is_terminator_op(last->opcode))
    {
        verify_error("block '%s' has no terminator", bb->label);
        *ok = false;
    }
}

static bool pred_has_label(IrBlock *bb, const char *label)
{
    size_t npred = vec_size(bb->preds);
    for (size_t p = 0; p < npred; p++)
    {
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, p);
        if (strcmp(pred->label, label) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool phi_has_label(IrInstr *phi, const char *label)
{
    u32 nentries = phi->extra.phi.nentries;
    for (u32 e = 0; e < nentries; e++)
    {
        if (strcmp(phi->extra.phi.entries[e].label, label) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool phi_has_duplicate_label(IrInstr *phi)
{
    u32 nentries = phi->extra.phi.nentries;
    for (u32 e1 = 0; e1 < nentries; e1++)
    {
        for (u32 e2 = e1 + 1; e2 < nentries; e2++)
        {
            if (strcmp(phi->extra.phi.entries[e1].label, phi->extra.phi.entries[e2].label) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* Every phi lists exactly the block's preds; the interpreter selects an incoming
   value by predecessor label. */
static void check_phi_preds(IrBlock *bb, bool *ok)
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
        for (u32 e = 0; e < nentries; e++)
        {
            if (!pred_has_label(bb, in->extra.phi.entries[e].label))
            {
                verify_error("block '%s': phi entry '%s' is not a predecessor", bb->label,
                             in->extra.phi.entries[e].label);
                *ok = false;
            }
        }
        if (phi_has_duplicate_label(in))
        {
            verify_error("block '%s': phi entries repeat a predecessor label", bb->label);
            *ok = false;
        }
        size_t npred = vec_size(bb->preds);
        for (size_t p = 0; p < npred; p++)
        {
            IrBlock *pred = (IrBlock *) vec_get(bb->preds, p);
            if (!phi_has_label(in, pred->label))
            {
                verify_error("block '%s': predecessor '%s' has no phi entry", bb->label,
                             pred->label);
                *ok = false;
            }
        }
    }
}

/* Pred lists over-approximate, so only a real edge must appear. */
static bool is_recorded_pred(IrBlock *to, IrBlock *bb)
{
    size_t npred = vec_size(to->preds);
    for (size_t p = 0; p < npred; p++)
    {
        if (vec_get(to->preds, p) == bb)
        {
            return true;
        }
    }
    return false;
}

static void check_succ_recorded(IrBlock *bb, const char *label, bool *ok)
{
    IrBlock *to = find_block_by_label(bb->func, label);
    if (to && !is_recorded_pred(to, bb))
    {
        verify_error("block '%s' branches to '%s', which lists no such predecessor", bb->label,
                     label);
        *ok = false;
    }
}

static void check_edges(IrBlock *bb, bool *ok)
{
    if (vec_size(bb->instrs) == 0)
    {
        return;
    }
    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    switch (last->opcode)
    {
        case OP_BR:
            check_succ_recorded(bb, last->extra.br.target_label, ok);
            break;
        case OP_BRCOND:
            check_succ_recorded(bb, last->extra.brcond.true_label, ok);
            check_succ_recorded(bb, last->extra.brcond.false_label, ok);
            break;
        case OP_SWITCH:
            for (u32 c = 0; c < last->extra.sw.ncases; c++)
            {
                check_succ_recorded(bb, last->extra.sw.cases[c].label, ok);
            }
            if (last->extra.sw.default_label)
            {
                check_succ_recorded(bb, last->extra.sw.default_label, ok);
            }
            break;
        default:
            break;
    }
}

static void check_label_unique(IrFunction *f, bool *ok)
{
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *a = (IrBlock *) vec_get(f->blocks, i);
        for (size_t j = i + 1; j < n; j++)
        {
            IrBlock *b = (IrBlock *) vec_get(f->blocks, j);
            if (strcmp(a->label, b->label) == 0)
            {
                verify_error("duplicate block label '%s' in function '%s'", a->label, f->name);
                *ok = false;
            }
        }
    }
}

static void check_function(IrModule *mod, IrFunction *f, IrInstr **defs, IrFunction **def_funcs,
                           bool *ok)
{
    if (vec_size(f->blocks) == 0)
    {
        verify_error("function '%s' has no blocks", f->name);
        *ok = false;
        return;
    }

    check_label_unique(f, ok);

    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(f->params, i);
        if (p->vreg >= mod->width_count)
        {
            verify_error("function '%s': parameter '%s' has out-of-range vreg %u", f->name, p->name,
                         p->vreg);
            *ok = false;
        }
        else if (defs[p->vreg])
        {
            verify_error("function '%s': parameter '%s' is redefined by an instruction", f->name,
                         p->name);
            *ok = false;
        }
    }

    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        check_block(mod, f, bb, defs, def_funcs, ok);
    }
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        check_phi_preds(bb, ok);
        check_edges(bb, ok);
    }
}

static void record_result(IrModule *mod, IrFunction *f, IrInstr *in, IrInstr **defs,
                          IrFunction **def_funcs, bool *ok)
{
    u32 vreg = in->result;
    if (vreg >= mod->width_count)
    {
        verify_error("instruction result vreg %u out of range (width_count %u)", vreg,
                     mod->width_count);
        *ok = false;
        return;
    }
    if (mod->widths[vreg] == 0)
    {
        verify_error("instruction defines vreg %u, which has width 0", vreg);
        *ok = false;
    }
    if (defs[vreg])
    {
        verify_error("vreg %u is defined by more than one instruction", vreg);
        *ok = false;
    }
    else
    {
        defs[vreg] = in;
        def_funcs[vreg] = f;
    }
}

bool opt_verify(IrModule *mod)
{
    bool ok = true;

    if (mod->width_count != mod->next_vreg)
    {
        verify_error("module value tables are inconsistent: width_count %u != next_vreg %u",
                     mod->width_count, mod->next_vreg);
        ok = false;
    }

    IrInstr **defs = arena_alloc(mod->arena, mod->width_count * sizeof(IrInstr *), sizeof(void *));
    IrFunction **def_funcs =
        arena_alloc(mod->arena, mod->width_count * sizeof(IrFunction *), sizeof(void *));
    for (u32 v = 0; v < mod->width_count; v++)
    {
        defs[v] = NULL;
        def_funcs[v] = NULL;
        if (ir_vreg_float(mod, v) && mod->widths[v] != 4 && mod->widths[v] != 8 &&
            mod->widths[v] != LD_BYTES)
        {
            verify_error("vreg %u is float with width %u, expected 4, 8 or %u", v, mod->widths[v],
                         LD_BYTES);
            ok = false;
        }
    }

    size_t nfuncs = vec_size(mod->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, i);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t it = 0; it < ninstr; it++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, it);
                if (in->result != NO_VREG)
                {
                    record_result(mod, f, in, defs, def_funcs, &ok);
                }
            }
        }
    }

    for (size_t i = 0; i < nfuncs; i++)
    {
        check_function(mod, (IrFunction *) vec_get(mod->funcs, i), defs, def_funcs, &ok);
    }

    return ok;
}