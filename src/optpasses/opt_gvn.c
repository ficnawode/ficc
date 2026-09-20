#include "opt_internal.h"

#include "ir.h"
#include "util/hashmap.h"

#include <stdint.h>
#include <string.h>

typedef enum
{
    VN_NONE = 0,
    VN_IMM,
    VN_VREG,
    VN_GLOBAL,
    VN_FUNC,
} VnOpKind;

typedef struct
{
    u32 opcode;
    u8 width;
    u8 is_signed;
    u8 nops;
    u8 kind[3];
    u64 val[3];
} VnKey;

static u64 vn_key_hash(const void *key)
{
    const VnKey *k = key;
    const u8 *p = (const u8 *) k;
    u64 h = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(VnKey); i++)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool vn_key_eq(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(VnKey)) == 0;
}

static bool vn_numberable(IrOpcode op)
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
        case OP_NEG:
        case OP_NOT:
        case OP_TRUNC:
        case OP_ZEXT:
        case OP_SEXT:
        case OP_GEP:
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

static bool vn_commutative(IrOpcode op)
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

static VnOpKind operand_kind(IrOperand op)
{
    if (op.is_imm)
    {
        return VN_IMM;
    }
    if (op.is_global)
    {
        return VN_GLOBAL;
    }
    if (op.is_func)
    {
        return VN_FUNC;
    }
    return VN_VREG;
}

static u64 operand_val(IrOperand op)
{
    if (op.is_imm)
    {
        return (u64) op.u.imm;
    }
    if (op.is_global)
    {
        return (u64) op.u.global_index;
    }
    if (op.is_func)
    {
        return (u64) (uintptr_t) op.u.func_name;
    }
    return (u64) op.u.vreg;
}

static void build_key(const IrModule *mod, const IrInstr *in, VnKey *key)
{
    memset(key, 0, sizeof(VnKey));
    key->opcode = in->opcode;
    key->width = mod->widths[in->result];
    key->is_signed = mod->signedness[in->result];
    key->nops = in->nops;
    for (u8 o = 0; o < in->nops; o++)
    {
        key->kind[o] = (u8) operand_kind(in->ops[o]);
        key->val[o] = operand_val(in->ops[o]);
    }
    if (vn_commutative(in->opcode) && in->nops == 2 &&
        (key->kind[0] > key->kind[1] ||
         (key->kind[0] == key->kind[1] && key->val[0] > key->val[1])))
    {
        u8 k = key->kind[0];
        key->kind[0] = key->kind[1];
        key->kind[1] = k;
        u64 v = key->val[0];
        key->val[0] = key->val[1];
        key->val[1] = v;
    }
}

/* def_block per vreg, for the leader's dominance test; params map to entry. */
static void build_def_block(IrFunction *f, IrBlock **def_block)
{
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    size_t nparams = vec_size(f->params);
    for (size_t p = 0; p < nparams; p++)
    {
        IrParam *param = (IrParam *) vec_get(f->params, p);
        def_block[param->vreg] = entry;
    }
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->result != NO_VREG)
            {
                def_block[in->result] = bb;
            }
        }
    }
}

bool opt_pass_gvn(OptimizerContext *ctx)
{
    bool changed = false;
    size_t nfuncs = vec_size(ctx->mod->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(ctx->mod->funcs, fi);
        CfgInfo *cfg = opt_get_cfg(ctx, f);
        Dominators *doms = opt_get_doms(ctx, f);

        u32 nvregs = ctx->mod->width_count;
        IrBlock **def_block = arena_alloc(ctx->arena, nvregs * sizeof(IrBlock *), sizeof(void *));
        for (u32 v = 0; v < nvregs; v++)
        {
            def_block[v] = NULL;
        }
        build_def_block(f, def_block);

        HashMap *table = hashmap_new(ctx->arena, vn_key_hash, vn_key_eq);
        for (u32 k = 0; k < cfg->nreach; k++)
        {
            IrBlock *bb = cfg->rpo[k];
            size_t i = 0;
            while (i < vec_size(bb->instrs))
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, i);
                if (in->result == NO_VREG || !vn_numberable(in->opcode))
                {
                    i++;
                    continue;
                }
                VnKey key;
                u32 cur_block = opt_block_index(f, bb);
                build_key(ctx->mod, in, &key);
                void *hit = hashmap_get(table, &key);
                if (hit)
                {
                    u32 leader = (u32) (uintptr_t) hit - 1;
                    u32 leader_block = opt_block_index(f, def_block[leader]);
                    if (opt_doms_dominates(doms, leader_block, cur_block))
                    {
                        opt_replace_def(ctx, f, in, ir_operand_vreg(leader));
                        changed = true;
                        continue; /* def erased; re-check the shifted slot */
                    }
                    i++;
                }
                else
                {
                    VnKey *stored = arena_alloc(ctx->arena, sizeof(VnKey), _Alignof(VnKey));
                    *stored = key;
                    hashmap_set(table, stored, (void *) (uintptr_t) (in->result + 1));
                    i++;
                }
            }
        }
    }
    return changed;
}
