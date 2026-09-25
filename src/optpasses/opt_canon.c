#include "opt_internal.h"

#include "ir.h"

static bool is_commutative(IrOpcode op)
{
    switch (op)
    {
        case OP_ADD:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_ICMP_EQ:
        case OP_ICMP_NE:
            return true;
        default:
            return false;
    }
}

static IrOpcode mirrored_cmp(IrOpcode op)
{
    switch (op)
    {
        case OP_ICMP_ULT:
            return OP_ICMP_UGT;
        case OP_ICMP_ULE:
            return OP_ICMP_UGE;
        case OP_ICMP_UGT:
            return OP_ICMP_ULT;
        case OP_ICMP_UGE:
            return OP_ICMP_ULE;
        case OP_ICMP_SLT:
            return OP_ICMP_SGT;
        case OP_ICMP_SLE:
            return OP_ICMP_SGE;
        case OP_ICMP_SGT:
            return OP_ICMP_SLT;
        case OP_ICMP_SGE:
            return OP_ICMP_SLE;
        default:
            return op;
    }
}

/* An immediate belongs in operand 1 so codegen can use the short encodings. */
bool opt_pass_canon(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t i = 0; i < ninstr; i++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->nops != 2 || !in->ops[0].is_imm || in->ops[1].is_imm)
                {
                    continue;
                }
                if (!is_commutative(in->opcode))
                {
                    IrOpcode op = mirrored_cmp(in->opcode);
                    if (op == in->opcode)
                    {
                        continue;
                    }
                    in->opcode = op;
                }
                IrOperand tmp = in->ops[0];
                in->ops[0] = in->ops[1];
                in->ops[1] = tmp;
                changed = true;
            }
        }
    }
    return changed;
}
