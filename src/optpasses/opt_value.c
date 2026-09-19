#include "opt_internal.h"

#include "ir.h"
#include "util/assert.h"

#include <stdint.h>
#include <string.h>

/* Scalar value semantics, matching ir_interp: ints wrap per class, FP re-rounds. */

bool opt_operand_eq(IrOperand a, IrOperand b)
{
    if (a.is_imm || b.is_imm)
    {
        return a.is_imm == b.is_imm && a.u.imm == b.u.imm;
    }
    if (a.is_global || b.is_global)
    {
        return a.is_global == b.is_global && a.u.global_index == b.u.global_index;
    }
    if (a.is_func || b.is_func)
    {
        return a.is_func == b.is_func && strcmp(a.u.func_name, b.u.func_name) == 0;
    }
    return a.u.vreg == b.u.vreg;
}

i64 opt_normalize(i64 raw, u8 width_bytes, bool is_signed)
{
    if (is_signed)
    {
        switch (width_bytes)
        {
            case 1:
                return (i64) (i8) raw;
            case 2:
                return (i64) (i16) raw;
            case 4:
                return (i64) (i32) raw;
            default:
                return raw;
        }
    }
    switch (width_bytes)
    {
        case 1:
            return raw & 0xFF;
        case 2:
            return raw & 0xFFFF;
        case 4:
            return raw & 0xFFFFFFFF;
        default:
            return raw;
    }
}

bool opt_fold_int(const IrModule *mod, const IrInstr *in, i64 *out)
{
    u8 w = mod->widths[in->result];
    bool is_signed = mod->signedness[in->result];
    i64 lhs = in->ops[0].u.imm;
    i64 raw = 0;
    if (in->opcode == OP_NEG || in->opcode == OP_NOT)
    {
        raw = in->opcode == OP_NEG ? -lhs : ~lhs;
    }
    else
    {
        i64 rhs = in->ops[1].u.imm;
        switch (in->opcode)
        {
            case OP_ADD:
                raw = lhs + rhs;
                break;
            case OP_SUB:
                raw = lhs - rhs;
                break;
            case OP_MUL:
                raw = lhs * rhs;
                break;
            case OP_AND:
                raw = lhs & rhs;
                break;
            case OP_OR:
                raw = lhs | rhs;
                break;
            case OP_XOR:
                raw = lhs ^ rhs;
                break;
            case OP_SDIV:
                if (rhs == 0)
                {
                    return false;
                }
                raw = lhs / rhs;
                break;
            case OP_UDIV:
                if (rhs == 0)
                {
                    return false;
                }
                raw = (i64) ((u64) lhs / (u64) rhs);
                break;
            case OP_SREM:
                if (rhs == 0)
                {
                    return false;
                }
                raw = lhs % rhs;
                break;
            case OP_UREM:
                if (rhs == 0)
                {
                    return false;
                }
                raw = (i64) ((u64) lhs % (u64) rhs);
                break;
            case OP_SHL:
                if (rhs < 0 || rhs >= (i64) w * 8)
                {
                    return false;
                }
                raw = lhs << rhs;
                break;
            case OP_ASHR:
                if (rhs < 0 || rhs >= (i64) w * 8)
                {
                    return false;
                }
                raw = lhs >> rhs;
                break;
            case OP_LSHR:
                if (rhs < 0 || rhs >= (i64) w * 8)
                {
                    return false;
                }
                raw = (i64) ((u64) lhs >> rhs);
                break;
            case OP_NEG:
                raw = -lhs;
                break;
            case OP_NOT:
                raw = ~lhs;
                break;
            case OP_ICMP_EQ:
                raw = lhs == rhs;
                break;
            case OP_ICMP_NE:
                raw = lhs != rhs;
                break;
            case OP_ICMP_ULT:
                raw = (u64) lhs < (u64) rhs;
                break;
            case OP_ICMP_ULE:
                raw = (u64) lhs <= (u64) rhs;
                break;
            case OP_ICMP_UGT:
                raw = (u64) lhs > (u64) rhs;
                break;
            case OP_ICMP_UGE:
                raw = (u64) lhs >= (u64) rhs;
                break;
            case OP_ICMP_SLT:
                raw = lhs < rhs;
                break;
            case OP_ICMP_SLE:
                raw = lhs <= rhs;
                break;
            case OP_ICMP_SGT:
                raw = lhs > rhs;
                break;
            case OP_ICMP_SGE:
                raw = lhs >= rhs;
                break;
            default:
                return false;
        }
    }
    *out = opt_normalize(raw, w, is_signed);
    return true;
}

/* FP imms ride the result-width pattern; compares/converts read them as doubles. */
static long double read_fp_bits(i64 bits, u8 w)
{
    if (w == 4)
    {
        float f;
        memcpy(&f, &bits, 4);
        return (long double) f;
    }
    double d;
    memcpy(&d, &bits, 8);
    return (long double) d;
}

static void store_fp_bits(i64 *out, long double v, u8 dw)
{
    if (dw == 4)
    {
        float f = (float) v;
        u32 bits = 0;
        memcpy(&bits, &f, 4);
        *out = (i64) (u64) bits;
        return;
    }
    double d = (double) v;
    *out = 0;
    memcpy(out, &d, 8);
}

bool opt_fold_fp(const IrModule *mod, const IrInstr *in, i64 *out)
{
    u8 dw = mod->widths[in->result];
    if (dw == 16 || !mod->floatness[in->result])
    {
        return false;
    }
    long double a = read_fp_bits(in->ops[0].u.imm, dw);
    long double rr = 0;
    if (in->opcode == OP_FNEG)
    {
        rr = -a;
    }
    else
    {
        long double b = read_fp_bits(in->ops[1].u.imm, dw);
        switch (in->opcode)
        {
            case OP_FADD:
                rr = a + b;
                break;
            case OP_FSUB:
                rr = a - b;
                break;
            case OP_FMUL:
                rr = a * b;
                break;
            case OP_FDIV:
                rr = a / b;
                break;
            default:
                return false;
        }
    }
    store_fp_bits(out, rr, dw);
    return true;
}

bool opt_fold_fcmp(const IrModule *mod, const IrInstr *in, i64 *out)
{
    (void) mod;
    long double a = read_fp_bits(in->ops[0].u.imm, 8);
    long double b = read_fp_bits(in->ops[1].u.imm, 8);
    bool r = false;
    switch (in->opcode)
    {
        case OP_FCMP_EQ:
            r = a == b;
            break;
        case OP_FCMP_NE:
            r = a != b;
            break;
        case OP_FCMP_LT:
            r = a < b;
            break;
        case OP_FCMP_GT:
            r = a > b;
            break;
        case OP_FCMP_LE:
            r = a <= b;
            break;
        case OP_FCMP_GE:
            r = a >= b;
            break;
        default:
            return false;
    }
    *out = r ? 1 : 0;
    return true;
}