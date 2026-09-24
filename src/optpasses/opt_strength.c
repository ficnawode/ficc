#include "opt_internal.h"

#include "ir.h"

/* The low `width` bytes of an immediate, as the instruction sees them. */
static u64 low_bits(i64 v, u8 width)
{
    if (width >= 8)
    {
        return (u64) v;
    }
    return (u64) v & ((1ULL << (width * 8)) - 1);
}

static bool pow2_shift(u64 bits, u8 width, u8 *shift)
{
    if (bits == 0 || (bits & (bits - 1)) != 0)
    {
        return false;
    }
    u8 k = 0;
    while ((bits & 1) == 0)
    {
        bits >>= 1;
        k++;
    }
    if (k >= (u8) (width * 8))
    {
        return false;
    }
    *shift = k;
    return true;
}

/* Rewrite a multiply or unsigned divide/remainder by a power of two into a
   shift or mask. In place: the result vreg keeps its uses. */
bool opt_pass_strength(OptimizerContext *ctx)
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
                if (in->result == NO_VREG)
                {
                    continue;
                }
                IrOpcode op = in->opcode;
                if (op != OP_MUL && op != OP_UDIV && op != OP_UREM)
                {
                    continue;
                }
                int imm_idx = in->ops[1].is_imm ? 1 : (in->ops[0].is_imm ? 0 : -1);
                if (imm_idx < 0)
                {
                    continue;
                }
                IrOperand var = in->ops[1 - imm_idx];
                if (!ir_operand_is_vreg(var))
                {
                    continue;
                }
                u8 width = ctx->mod->widths[in->result];
                u64 bits = low_bits(in->ops[imm_idx].u.imm, width);
                u8 k = 0;
                if (!pow2_shift(bits, width, &k))
                {
                    continue;
                }
                if (op == OP_MUL)
                {
                    if (k == 0)
                    {
                        continue; /* multiply by one is an identity */
                    }
                    in->opcode = OP_SHL;
                    in->ops[1] = ir_operand_imm((i64) k);
                }
                else if (op == OP_UDIV)
                {
                    in->opcode = OP_LSHR;
                    in->ops[1] = ir_operand_imm((i64) k);
                }
                else
                {
                    in->opcode = OP_AND;
                    in->ops[1] = ir_operand_imm((i64) (bits - 1)); /* x % 2^k == x & (2^k - 1) */
                }
                in->ops[0] = var;
                changed = true;
            }
        }
    }
    return changed;
}
