#include "opt_internal.h"

#include "ir.h"

static bool is_pure(IrInstr *in)
{
    switch (in->opcode)
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
        case OP_NEG:
        case OP_NOT:
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
        case OP_TRUNC:
        case OP_ZEXT:
        case OP_SEXT:
        case OP_GEP:
        case OP_ALLOCA:
        case OP_PHI:
        case OP_ITOF:
        case OP_FTOI:
        case OP_FCONV:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FNEG:
        case OP_FCMP_EQ:
        case OP_FCMP_NE:
        case OP_FCMP_LT:
        case OP_FCMP_GT:
        case OP_FCMP_LE:
        case OP_FCMP_GE:
            return true;
        default:
            return false;
    }
}

bool opt_pass_dce(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        for (;;)
        {
            opt_make_value_analysis(ctx, f);
            bool found = false;
            size_t nblocks = vec_size(f->blocks);
            for (size_t b = 0; b < nblocks; b++)
            {
                IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
                size_t ninstr = vec_size(bb->instrs);
                size_t write = 0;
                for (size_t j = 0; j < ninstr; j++)
                {
                    IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                    if (in->result != NO_VREG && is_pure(in) && ctx->use_count[in->result] == 0)
                    {
                        found = true;
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
            if (!found)
            {
                break;
            }
            changed = true;
        }
    }
    return changed;
}
