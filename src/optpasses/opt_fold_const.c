#include "opt_internal.h"

#include "ir.h"

#include <string.h>

static bool is_int_binop(IrOpcode op)
{
    switch (op)
    {
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
            return true;
        default:
            return false;
    }
}

static bool is_fp_binop(IrOpcode op)
{
    return op == OP_FADD || op == OP_FSUB || op == OP_FMUL || op == OP_FDIV;
}

static bool is_fp_cmp(IrOpcode op)
{
    return op == OP_FCMP_EQ || op == OP_FCMP_NE || op == OP_FCMP_LT || op == OP_FCMP_GT ||
           op == OP_FCMP_LE || op == OP_FCMP_GE;
}

bool opt_pass_fold_const(OptimizerContext *ctx)
{
    IrModule *mod = ctx->mod;
    bool changed = false;
    size_t nfuncs = vec_size(mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(mod->funcs, fi);
        opt_repl_begin(ctx);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t i = 0; i < ninstr; i++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->result == NO_VREG)
                {
                    continue;
                }
                i64 val = 0;
                bool foldable = false;
                if ((in->opcode == OP_NEG || in->opcode == OP_NOT) && in->ops[0].is_imm)
                {
                    foldable = opt_fold_int(mod, in, &val);
                }
                else if (is_int_binop(in->opcode) && in->ops[0].is_imm && in->ops[1].is_imm)
                {
                    foldable = opt_fold_int(mod, in, &val);
                }
                else if (is_fp_binop(in->opcode) && in->ops[0].is_imm && in->ops[1].is_imm)
                {
                    foldable = opt_fold_fp(mod, in, &val);
                }
                else if (in->opcode == OP_FNEG && in->ops[0].is_imm)
                {
                    foldable = opt_fold_fp(mod, in, &val);
                }
                else if (is_fp_cmp(in->opcode) && in->ops[0].is_imm && in->ops[1].is_imm)
                {
                    foldable = opt_fold_fcmp(mod, in, &val);
                }
                if (foldable)
                {
                    opt_repl_set(ctx, in->result, ir_operand_imm(val));
                    changed = true;
                }
            }
        }
        if (opt_repl_apply(ctx, f))
        {
            changed = true;
        }
    }
    return changed;
}
