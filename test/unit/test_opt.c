#include "harness.h"
#include "testdriver.h"

#include "opt.h"
#include "optpasses/opt_internal.h"

#include "ir.h"
#include "ir_interp.h"
#include "util/arena.h"

#include <stdint.h>
#include <string.h>

/* Exercised one invariant per TEST on hand-built, then corrupted, modules. */

/* A fully-formed if/else merge carrying a phi; each corruption mutates this. */
static IrModule *build_if_else(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    IrBlock *merge = ir_func_add_block(f, "merge");

    u32 cond = ir_alloc_vreg(m, 8, false, false);
    u32 val = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ICMP_SLT, cond, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_brcond(entry, ir_operand_vreg(cond), then_bb->label, else_bb->label);
    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);

    ir_emit_br(then_bb, merge->label);
    ir_emit_br(else_bb, merge->label);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);

    IrInstr *phi = ir_emit_phi_at_start(merge, val, 2);
    ir_phi_add_entry(phi, ir_operand_imm(10), then_bb);
    ir_phi_add_entry(phi, ir_operand_imm(20), else_bb);
    ir_emit_ret(merge, ir_operand_vreg(val));
    return m;
}

TEST(opt, clean_if_else_passes)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, builder_fixtures_verify)
{
    Arena *a = arena_new();
    EXPECT_TRUE(opt_verify(tc_build_module("int main(void) { return 42; }", a)));
    EXPECT_TRUE(opt_verify(tc_build_module("int main(void) {\n"
                                           "    int s = 0;\n"
                                           "    for (int i = 0; i < 4; i = i + 1) s = s + i;\n"
                                           "    return s;\n"
                                           "}\n",
                                           a)));
    EXPECT_TRUE(
        opt_verify(tc_build_module("int main(void) {\n"
                                   "    switch (1) { case 1: return 1; default: return 0; }\n"
                                   "}\n",
                                   a)));
    EXPECT_TRUE(opt_verify(tc_build_module("long double g;\n"
                                           "int main(void) { g = 1.5L; return g > 0; }\n",
                                           a)));
    arena_free(a);
}

/* invariant 1: SSA (single def; every operand has one; params def'd at entry) */

TEST(opt, double_def_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *then_bb = (IrBlock *) vec_get(f->blocks, 1);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    IrInstr *add =
        ir_emit_binop(then_bb, OP_ADD, phi->result, ir_operand_imm(1), ir_operand_imm(2));
    vec_pop(then_bb->instrs);
    vec_insert(then_bb->instrs, 0, add);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, undef_operand_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    u32 ghost = ir_alloc_vreg(m, 8, false, false);
    IrInstr *brcond = (IrInstr *) vec_last(entry->instrs);
    brcond->ops[0] = ir_operand_vreg(ghost);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, param_use_ok)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 p = ir_alloc_vreg(m, 8, true, false);
    IrParam *param = arena_alloc(a, sizeof(IrParam), sizeof(void *));
    param->name = "x";
    param->type = type_int();
    param->vreg = p;
    vec_push(f->params, param);
    IrBlock *entry = ir_func_add_block(f, "entry");
    ir_emit_ret(entry, ir_operand_vreg(p));
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, param_redefined_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 p = ir_alloc_vreg(m, 8, true, false);
    IrParam *param = arena_alloc(a, sizeof(IrParam), sizeof(void *));
    param->name = "x";
    param->type = type_int();
    param->vreg = p;
    vec_push(f->params, param);
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 extra = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, p, ir_operand_vreg(extra), ir_operand_imm(1));
    ir_emit_ret(entry, ir_operand_imm(0));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 2: block shape */

TEST(opt, missing_terminator_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 dst = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, dst, ir_operand_imm(1), ir_operand_imm(2));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, instr_after_terminator_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 dst = ir_alloc_vreg(m, 8, true, false);
    ir_emit_ret(entry, ir_operand_imm(0));
    ir_emit_binop(entry, OP_ADD, dst, ir_operand_imm(1), ir_operand_imm(2));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, phi_not_at_start_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    IrBlock *merge = ir_func_add_block(f, "merge");
    ir_emit_br(entry, then_bb->label);
    ir_emit_br(then_bb, merge->label);
    ir_emit_br(else_bb, merge->label);
    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);
    u32 w = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(merge, OP_ADD, w, ir_operand_imm(1), ir_operand_imm(1));
    IrInstr *phi = ir_emit_phi(merge, w, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), then_bb);
    ir_phi_add_entry(phi, ir_operand_imm(2), else_bb);
    ir_emit_ret(merge, ir_operand_imm(0));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 3: CFG coherence */

TEST(opt, brcond_unknown_target_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrInstr *brcond = (IrInstr *) vec_last(entry->instrs);
    brcond->extra.brcond.true_label = "no_such_block";
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, phi_entry_not_pred_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    phi->extra.phi.entries[0].label = "stranger";
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, duplicate_phi_entry_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    phi->extra.phi.entries[1].label = phi->extra.phi.entries[0].label;
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, pred_without_phi_entry_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    vec_push(merge->preds, entry);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, succ_not_recorded_pred_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *dest = ir_func_add_block(f, "dest");
    ir_emit_br(entry, dest->label);
    ir_emit_ret(dest, ir_operand_imm(0));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 4: labels unique per function */

TEST(opt, duplicate_label_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    ir_func_add_block(f, "entry");
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 5: operands */

TEST(opt, arity_mismatch_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrInstr *ret = ir_emit_ret(entry, ir_operand_imm(0));
    ret->nops = 3;
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, result_bounds_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    ir_emit_binop(entry, OP_ADD, 12345, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_ret(entry, ir_operand_imm(0));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, operand_bounds_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrInstr *brcond = (IrInstr *) vec_last(entry->instrs);
    brcond->ops[0] = ir_operand_vreg(12345);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* A well-formed width-16 merge: only the wide-operand rule varies below. */
static IrModule *build_wide_phi(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    IrBlock *merge = ir_func_add_block(f, "merge");
    ir_emit_br(entry, merge->label);
    ir_emit_br(then_bb, merge->label);
    ir_emit_br(else_bb, merge->label);
    vec_push(merge->preds, entry);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);
    u32 ld = ir_alloc_vreg(m, 16, true, true);
    IrInstr *phi = ir_emit_phi_at_start(merge, ld, 3);
    ir_phi_add_entry(phi, ir_operand_imm(0), entry);
    ir_phi_add_entry(phi, ir_operand_imm(0), then_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), else_bb);
    ir_emit_ret(merge, ir_operand_imm(0));
    return m;
}

TEST(opt, wide_phi_zero_immediate_allowed)
{
    Arena *a = arena_new();
    EXPECT_TRUE(opt_verify(build_wide_phi(a)));
    arena_free(a);
}

TEST(opt, wide_phi_nonzero_immediate_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_wide_phi(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    phi->extra.phi.entries[0].val = ir_operand_imm(5);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 6: module value tables */

TEST(opt, width_count_mismatch_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    m->next_vreg++;
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, float_width_mismatch_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    ir_alloc_vreg(m, 3, false, true);
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

TEST(opt, width_zero_result_rejected)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 zero = ir_alloc_vreg(m, 0, false, false);
    ir_emit_binop(entry, OP_ADD, zero, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_ret(entry, ir_operand_imm(0));
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* invariant 7: volatile as a memory-op barrier */

/* OP_LOAD/OP_STORE counts from an inspected module, split by the volatile flag. */
typedef struct
{
    u32 volatile_count;
    u32 plain_count;
} MemopCounts;

static MemopCounts count_memops(IrModule *m)
{
    MemopCounts counts = {0};
    size_t nfuncs = vec_size(m->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, i);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t it = 0; it < ninstr; it++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, it);
                if (in->opcode != OP_LOAD && in->opcode != OP_STORE)
                {
                    continue;
                }
                if (in->extra.mem.is_volatile)
                {
                    counts.volatile_count++;
                }
                else
                {
                    counts.plain_count++;
                }
            }
        }
    }
    return counts;
}

/* Volatile programs must flag every memory op and still verify as well-formed. */
TEST(opt, volatile_global_increment_flags)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("volatile int g;\n"
                                  "int main(void) {\n"
                                  "    for (int i = 0; i < 10; i = i + 1) g = g + 1;\n"
                                  "    return g;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    MemopCounts counts = count_memops(m);
    EXPECT_TRUE(counts.volatile_count >= 2); /* the load and the store in the loop */
    EXPECT_TRUE(counts.plain_count == 0);
    arena_free(a);
}

TEST(opt, volatile_deref_in_loop_flags)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    volatile int x = 0;\n"
                                  "    volatile int *p = &x;\n"
                                  "    for (int i = 0; i < 5; i = i + 1) *p = *p + 2;\n"
                                  "    return x;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    MemopCounts counts = count_memops(m);
    EXPECT_TRUE(counts.volatile_count >= 2); /* deref load + deref store inside the loop */
    arena_free(a);
}

/* A plain non-volatile program carries the flag clear (even on globals). */
TEST(opt, plain_global_ops_unflagged)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int g;\n"
                                  "int main(void) {\n"
                                  "    g = 40;\n"
                                  "    return g + 2;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    MemopCounts counts = count_memops(m);
    EXPECT_TRUE(counts.volatile_count == 0);
    EXPECT_TRUE(counts.plain_count >= 2);
    arena_free(a);
}

/* CFG base, dominators, natural loops, canonical shape */

static IrFunction *opt_main_fn(IrModule *m)
{
    return (IrFunction *) vec_get(m->funcs, 0);
}

/* Block whose label starts with `prefix`; the builder suffixes `_N`. */
static IrBlock *block_with_prefix(IrFunction *f, const char *prefix)
{
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (strncmp(bb->label, prefix, strlen(prefix)) == 0)
        {
            return bb;
        }
    }
    return NULL;
}

static bool succ_contains(CfgInfo *cfg, IrFunction *f, const char *label, IrBlock *target)
{
    IrBlock *bb = block_with_prefix(f, label);
    if (!bb)
    {
        return false;
    }
    Vec *succs = cfg->succs[opt_block_index(f, bb)];
    size_t n = vec_size(succs);
    for (size_t i = 0; i < n; i++)
    {
        if (vec_get(succs, i) == target)
        {
            return true;
        }
    }
    return false;
}

TEST(opt, cfg_successors_of_control)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x = 1;\n"
                                  "    if (x) return 1; else return 2;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    EXPECT_TRUE(cfg->nblocks == 4);
    IrBlock *then_bb = block_with_prefix(f, "then");
    IrBlock *else_bb = block_with_prefix(f, "else");
    EXPECT_TRUE(succ_contains(cfg, f, "entry", then_bb));
    EXPECT_TRUE(succ_contains(cfg, f, "entry", else_bb));
    EXPECT_TRUE(vec_size(cfg->succs[opt_block_index(f, then_bb)]) ==
                0); /* `ret` has no successors */
    arena_free(a);
}

TEST(opt, cfg_successors_of_switch)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(
        "int main(void) {\n"
        "    switch (2) { case 1: return 1; case 2: return 2; default: return 0; }\n"
        "}\n",
        a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    size_t nsucc = vec_size(cfg->succs[0]);
    EXPECT_TRUE(nsucc == 3); /* two cases + default */
    arena_free(a);
}

TEST(opt, rpo_entry_first_and_dead_block_excluded)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    return 0;\n"
                                  "    return 1;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    EXPECT_TRUE(cfg->rpo[0] == (IrBlock *) vec_get(f->blocks, 0));
    EXPECT_TRUE(cfg->rpo_index[0] == 0);
    /* the dead second block stays out of the RPO */
    EXPECT_TRUE(cfg->rpo_index[1] == UINT32_MAX);
    arena_free(a);
}

TEST(opt, dominators_if_else)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    if (1) return 1; else return 2;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrBlock *then_bb = block_with_prefix(f, "then");
    IrBlock *else_bb = block_with_prefix(f, "else");
    u32 ei = opt_block_index(f, entry);
    u32 ti = opt_block_index(f, then_bb);
    u32 ai = opt_block_index(f, else_bb);
    EXPECT_TRUE(doms->idom[ti] == ei);
    EXPECT_TRUE(doms->idom[ai] == ei);
    EXPECT_TRUE(doms->depth[ti] == 1);
    EXPECT_TRUE(doms->depth[ai] == 1);
    EXPECT_TRUE(opt_doms_dominates(doms, ei, ti));
    EXPECT_FALSE(opt_doms_dominates(doms, ti, ei));
    arena_free(a);
}

TEST(opt, natural_loop_while_recognized)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    while (i < 10) { i = i + 1; s = s + i; }\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 1);
    Loop *l = (Loop *) vec_get(loops->loops, 0);
    IrBlock *hdr = block_with_prefix(f, "while_header");
    EXPECT_TRUE(l->header == hdr);
    IrBlock *body = block_with_prefix(f, "while_body");
    EXPECT_TRUE(opt_loops_contains(l, body));
    EXPECT_TRUE(vec_size(l->latches) >= 1);
    EXPECT_TRUE(doms->idom[opt_block_index(f, hdr)] == 0);
    arena_free(a);
}

TEST(opt, nested_loops_nest)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 2; i = i + 1)\n"
                                  "        for (int j = 0; j < 2; j = j + 1) s = s + 1;\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 2);
    Loop *outer = NULL;
    Loop *inner = NULL;
    for (size_t i = 0; i < 2; i++)
    {
        Loop *l = (Loop *) vec_get(loops->loops, i);
        if (strncmp(l->header->label, "for_header", 10) == 0)
        {
            if (!outer)
            {
                outer = l;
            }
            else
            {
                inner = l;
            }
        }
    }
    EXPECT_NOTNULL(outer);
    EXPECT_NOTNULL(inner);
    if (opt_doms_dominates(doms, opt_block_index(f, outer->header),
                           opt_block_index(f, inner->header)))
    {
        /* outer already wraps inner */
    }
    else
    {
        Loop *tmp = outer;
        outer = inner;
        inner = tmp;
    }
    EXPECT_TRUE(inner->outer == outer);
    arena_free(a);
}

TEST(opt, do_while_latch_shape)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    do { i = i + 1; s = s + i; } while (i < 3);\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 1);
    Loop *l = (Loop *) vec_get(loops->loops, 0);
    /* Do-while's natural header is the first-executed body block. */
    EXPECT_TRUE(l->header == block_with_prefix(f, "do_body"));
    EXPECT_TRUE(vec_size(l->latches) == 1);
    EXPECT_TRUE((IrBlock *) vec_get(l->latches, 0) == block_with_prefix(f, "do_header"));
    EXPECT_TRUE(opt_loops_canonicalize(m, f, loops) == false);
    EXPECT_TRUE(opt_loops_verify_shapes(loops));
    EXPECT_TRUE(l->preheader == (IrBlock *) vec_get(f->blocks, 0));
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

/* A continue in a while loop sends the body end and the continue block
   both back into the header: a multi-latch loop to canonicalize. */
TEST(opt, canonicalize_multi_latch_preserves_semantics)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    while (i < 10) {\n"
                                  "        i = i + 1;\n"
                                  "        if (i == 3) continue;\n"
                                  "        s = s + i;\n"
                                  "    }\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 1);
    Loop *l = (Loop *) vec_get(loops->loops, 0);
    EXPECT_TRUE(vec_size(l->latches) >= 2);

    i64 before = ir_interp_run(m);
    EXPECT_TRUE(opt_loops_canonicalize(m, f, loops));
    EXPECT_TRUE(opt_loops_verify_shapes(loops));
    EXPECT_TRUE(vec_size(l->latches) == 1);
    EXPECT_TRUE(opt_verify(m));
    i64 after = ir_interp_run(m);
    EXPECT_TRUE(after == 52);
    EXPECT_TRUE(after == before);
    arena_free(a);
}

TEST(opt, multi_latch_interp_elf_oracle)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int s = 0;\n"
                          "    int i = 0;\n"
                          "    while (i < 10) {\n"
                          "        i = i + 1;\n"
                          "        if (i == 3) continue;\n"
                          "        s = s + i;\n"
                          "    }\n"
                          "    return s;\n"
                          "}\n",
                          52);
}

/* A header with two outside predecessors needs a synthetic preheader. */
static IrModule *build_two_outside_loop(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *a_bb = ir_func_add_block(f, "a");
    IrBlock *b_bb = ir_func_add_block(f, "b");
    IrBlock *hdr = ir_func_add_block(f, "hdr");
    IrBlock *latch = ir_func_add_block(f, "latch");
    IrBlock *exit_bb = ir_func_add_block(f, "exit");

    u32 cond = ir_alloc_vreg(m, 8, false, false);
    u32 x = ir_alloc_vreg(m, 8, true, false);

    ir_emit_br(entry, a_bb->label);
    vec_push(a_bb->preds, entry);

    ir_emit_binop(a_bb, OP_ICMP_SLT, cond, ir_operand_imm(0), ir_operand_imm(1));
    ir_emit_brcond(a_bb, ir_operand_vreg(cond), hdr->label, b_bb->label);
    vec_push(hdr->preds, a_bb);
    vec_push(b_bb->preds, a_bb);

    ir_emit_br(b_bb, hdr->label);
    vec_push(hdr->preds, b_bb);

    hdr->is_loop_header = true;
    IrInstr *phi = ir_emit_phi_at_start(hdr, x, 3);
    ir_phi_add_entry(phi, ir_operand_imm(10), a_bb);
    ir_phi_add_entry(phi, ir_operand_imm(20), b_bb);
    ir_phi_add_entry(phi, ir_operand_imm(1), latch);
    u32 c2 = ir_alloc_vreg(m, 8, false, false);
    ir_emit_binop(hdr, OP_ICMP_SLT, c2, ir_operand_imm(1), ir_operand_imm(0));
    ir_emit_brcond(hdr, ir_operand_vreg(c2), latch->label, exit_bb->label);
    vec_push(latch->preds, hdr);
    vec_push(exit_bb->preds, hdr);

    ir_emit_br(latch, hdr->label);
    vec_push(hdr->preds, latch);

    ir_emit_ret(exit_bb, ir_operand_vreg(x));
    return m;
}

TEST(opt, canonicalize_preheader_synthesis)
{
    Arena *a = arena_new();
    IrModule *m = build_two_outside_loop(a);
    EXPECT_TRUE(opt_verify(m));
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 1);
    Loop *l = (Loop *) vec_get(loops->loops, 0);
    EXPECT_NULL(l->preheader);

    i64 before = ir_interp_run(m);
    EXPECT_TRUE(opt_loops_canonicalize(m, f, loops));
    EXPECT_TRUE(opt_loops_verify_shapes(loops));
    EXPECT_NOTNULL(l->preheader);
    EXPECT_TRUE(opt_verify(m));
    i64 after = ir_interp_run(m);
    EXPECT_TRUE(after == 10);
    EXPECT_TRUE(after == before);
    arena_free(a);
}

TEST(opt, for_loop_preheader_is_entry)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 4; i = i + 1) s = s + i;\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_cfg_build(f, a);
    Dominators *doms = opt_doms_build(cfg, a);
    LoopInfo *loops = opt_loops_find(f, cfg, doms, a);
    EXPECT_TRUE(vec_size(loops->loops) == 1);
    Loop *l = (Loop *) vec_get(loops->loops, 0);
    EXPECT_TRUE(vec_size(l->latches) == 1);
    opt_loops_canonicalize(m, f, loops);
    EXPECT_TRUE(l->preheader == (IrBlock *) vec_get(f->blocks, 0));
    EXPECT_TRUE(opt_loops_verify_shapes(loops));
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

/* optimize() shell, mode tables, shared magic */

static OptimizerContext make_ctx(IrModule *m, Arena *a)
{
    OptimizerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod = m;
    ctx.arena = a;
    ctx.opts = opt_config_for(OPT_LEVEL_0);
    return ctx;
}

TEST(opt, optimize_preserves_with_empty_tables)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 6; i = i + 1) s = s + i;\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_0, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), before);

    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, optimize_tolerates_null_module)
{
    Arena *a = arena_new();
    optimize(NULL, OPT_LEVEL_2, a);
    arena_free(a);
}

TEST(opt, cfg_cache_reused_and_rebuilt_per_function)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int a(void) { return 1; }\n"
                                  "int b(void) { return 2; }\n"
                                  "int main(void) { return a() + b(); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    OptimizerContext ctx = make_ctx(m, a);
    IrFunction *f0 = (IrFunction *) vec_get(m->funcs, 0);
    IrFunction *f1 = (IrFunction *) vec_get(m->funcs, 1);
    CfgInfo *c0 = opt_get_cfg(&ctx, f0);
    EXPECT_TRUE(c0->func == f0);
    EXPECT_TRUE(opt_get_cfg(&ctx, f0) == c0); /* same function, same epoch: cached */
    CfgInfo *c1 = opt_get_cfg(&ctx, f1);
    EXPECT_TRUE(c1 != c0);
    EXPECT_TRUE(c1->func == f1);
    arena_free(a);
}

TEST(opt, cfg_epoch_invalidation_rebuilds_cache)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    if (1) return 1; else return 2;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    OptimizerContext ctx = make_ctx(m, a);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *c0 = opt_get_cfg(&ctx, f);
    ctx.cfg_epoch++;
    CfgInfo *c1 = opt_get_cfg(&ctx, f);
    EXPECT_TRUE(c1 != c0);
    EXPECT_TRUE(c0->func == f);
    EXPECT_TRUE(c1->func == f);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, rpo_order_matches_cfg)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    OptimizerContext ctx = make_ctx(m, a);
    IrFunction *f = opt_main_fn(m);
    CfgInfo *cfg = opt_get_cfg(&ctx, f);
    Vec *order = opt_rpo_order(&ctx, f);
    EXPECT_TRUE(vec_size(order) == cfg->nreach);
    for (u32 k = 0; k < cfg->nreach; k++)
    {
        EXPECT_TRUE(vec_get(order, k) == cfg->rpo[k]);
    }
    arena_free(a);
}

TEST(opt, value_analysis_defs_and_uses)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    OptimizerContext ctx = make_ctx(m, a);
    IrFunction *f = opt_main_fn(m);
    opt_make_value_analysis(&ctx, f);

    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrInstr *binc = (IrInstr *) vec_get(entry->instrs, 0); /* icmp_slt -> cond */
    u32 cond = binc->result;
    EXPECT_TRUE(ctx.def_vreg[cond] == binc);
    EXPECT_EQ(ctx.use_count[cond], 1); /* the brcond consumes it */

    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    u32 val = phi->result;
    EXPECT_TRUE(ctx.def_vreg[val] == phi);
    EXPECT_EQ(ctx.use_count[val], 1); /* the ret consumes it */
    arena_free(a);
}

/* Whether the current terminator of `bb` leaves for `target`. */
static bool succ_contains_bb(IrBlock *bb, IrBlock *target)
{
    if (vec_size(bb->instrs) == 0)
    {
        return false;
    }
    IrInstr *last = (IrInstr *) vec_last(bb->instrs);
    switch (last->opcode)
    {
        case OP_BR:
            return strcmp(last->extra.br.target_label, target->label) == 0;
        case OP_BRCOND:
            return strcmp(last->extra.brcond.true_label, target->label) == 0 ||
                   strcmp(last->extra.brcond.false_label, target->label) == 0;
        default:
            return false;
    }
}

TEST(opt, insert_empty_block_splits_ifelse_merge)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    EXPECT_EQ(ir_interp_run(m), 10);
    IrFunction *f = opt_main_fn(m);
    IrBlock *then_bb = (IrBlock *) vec_get(f->blocks, 1);
    IrBlock *else_bb = (IrBlock *) vec_get(f->blocks, 2);
    IrBlock *merge = (IrBlock *) vec_get(f->blocks, 3);

    Vec *preds = vec_new(a);
    vec_push(preds, then_bb);
    vec_push(preds, else_bb);
    IrBlock *spliced = opt_insert_empty_block(m, f, preds, merge, "merged");
    EXPECT_NOTNULL(spliced);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 10);

    /* both original edges now land on the splice; merge's phi has one entry */
    EXPECT_TRUE(succ_contains_bb(then_bb, spliced));
    EXPECT_TRUE(succ_contains_bb(else_bb, spliced));
    IrInstr *phi = (IrInstr *) vec_get(merge->instrs, 0);
    EXPECT_EQ(phi->extra.phi.nentries, 1);
    EXPECT_STR_EQ(phi->extra.phi.entries[0].label, spliced->label);
    arena_free(a);
}

TEST(opt, insert_preheader_single_edge)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 4; i = i + 1) s = s + i;\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    IrFunction *f = opt_main_fn(m);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrBlock *header = block_with_prefix(f, "for_header");
    EXPECT_EQ(ir_interp_run(m), 6);
    IrBlock *pre = opt_insert_preheader(m, f, entry, header, "ph");
    EXPECT_NOTNULL(pre);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 6);
    EXPECT_TRUE(succ_contains_bb(entry, pre));
    EXPECT_TRUE(succ_contains_bb(pre, header));
    arena_free(a);
}

TEST(opt, insert_preheader_through_brcond)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    EXPECT_EQ(ir_interp_run(m), 10);
    IrFunction *f = opt_main_fn(m);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrBlock *then_bb = (IrBlock *) vec_get(f->blocks, 1);

    IrBlock *pre = opt_insert_preheader(m, f, entry, then_bb, "pre");
    EXPECT_NOTNULL(pre);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 10);
    EXPECT_TRUE(succ_contains_bb(entry, pre));
    EXPECT_TRUE(succ_contains_bb(pre, then_bb));
    arena_free(a);
}

TEST(opt, erase_and_reinsert_instr)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = opt_main_fn(m);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrInstr *brcond = (IrInstr *) vec_last(entry->instrs);
    EXPECT_TRUE(opt_instr_index(entry, brcond) != UINT32_MAX);

    opt_erase_instr(entry, brcond);
    EXPECT_TRUE(opt_instr_index(entry, brcond) == UINT32_MAX);
    EXPECT_FALSE(opt_verify(m)); /* the entry block lost its terminator */

    opt_insert_instr(entry, (u32) vec_size(entry->instrs), brcond);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 10);
    arena_free(a);
}

/* canonicalize passes */

static u32 count_opcode(IrModule *m, IrOpcode op)
{
    u32 n = 0;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                if (((IrInstr *) vec_get(bb->instrs, j))->opcode == op)
                {
                    n++;
                }
            }
        }
    }
    return n;
}

static u32 count_all_instrs(IrModule *m)
{
    u32 n = 0;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            n += (u32) vec_size(((IrBlock *) vec_get(f->blocks, b))->instrs);
        }
    }
    return n;
}

static u32 count_add_mul_or_sub(IrModule *m)
{
    return count_opcode(m, OP_ADD) + count_opcode(m, OP_MUL) + count_opcode(m, OP_OR) +
           count_opcode(m, OP_SUB);
}

static i64 fp_bits(double d)
{
    i64 bits = 0;
    memcpy(&bits, &d, sizeof(d));
    return bits;
}

static IrFunction *add_int_param(IrModule *m, const char *name, u8 width, bool is_signed)
{
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 p = ir_alloc_vreg(m, width, is_signed, false);
    IrParam *param = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    param->name = name;
    param->type = type_int();
    param->vreg = p;
    vec_push(f->params, param);
    ir_func_add_block(f, "entry");
    return f;
}

TEST(opt, fold_both_immediate_binops)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 8, true, false);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, v0, ir_operand_imm(3), ir_operand_imm(4));
    ir_emit_binop(entry, OP_MUL, v1, ir_operand_vreg(v0), ir_operand_imm(6));
    ir_emit_ret(entry, ir_operand_vreg(v1));
    EXPECT_EQ(ir_interp_run(m), 42);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ADD), 0);
    EXPECT_EQ(count_opcode(m, OP_MUL), 0);
    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

TEST(opt, fold_normalizes_at_result_width)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 1, true, false); /* signed i8: 120+8 wraps to -128 */
    ir_emit_binop(entry, OP_ADD, v0, ir_operand_imm(120), ir_operand_imm(8));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    EXPECT_EQ(ir_interp_run(m), -128);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ADD), 0);
    EXPECT_EQ(ir_interp_run(m), -128);
    arena_free(a);
}

TEST(opt, fold_unsigned_masks_to_width)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 1, false, false); /* u8: 250+10 wraps to 4 */
    ir_emit_binop(entry, OP_ADD, v0, ir_operand_imm(250), ir_operand_imm(10));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    EXPECT_EQ(ir_interp_run(m), 4);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 4);
    arena_free(a);
}

/* Zero divisors and out-of-range shift counts are UB; the fold leaves them. */
TEST(opt, fold_keeps_ub_shapes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 8, true, false);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    u32 v2 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_SDIV, v0, ir_operand_imm(4), ir_operand_imm(0));
    ir_emit_binop(entry, OP_SHL, v1, ir_operand_imm(1), ir_operand_imm(65));
    ir_emit_binop(entry, OP_ADD, v2, ir_operand_vreg(v0), ir_operand_vreg(v1));
    ir_emit_ret(entry, ir_operand_vreg(v2));
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_SDIV), 1);
    EXPECT_EQ(count_opcode(m, OP_SHL), 1);
    arena_free(a);
}

TEST(opt, fold_fp_arithmetic)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_fp_vreg(m, 8);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_FADD, v0, ir_operand_imm(fp_bits(2.0)), ir_operand_imm(fp_bits(3.0)));
    ir_emit_unary(entry, OP_FTOI, v1, ir_operand_vreg(v0));
    ir_emit_ret(entry, ir_operand_vreg(v1));
    EXPECT_EQ(ir_interp_run(m), 5);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_FADD), 0);
    EXPECT_EQ(ir_interp_run(m), 5);
    arena_free(a);
}

TEST(opt, fold_icmp_feeds_constant_branch)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    if (1 < 2) return 7;\n"
                                  "    return 9;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 7);
    EXPECT_TRUE(count_opcode(m, OP_ICMP_SLT) >= 1);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ICMP_SLT), 0);
    EXPECT_EQ(count_opcode(m, OP_BRCOND), 0);
    EXPECT_EQ(ir_interp_run(m), 7);
    arena_free(a);
}

TEST(opt, identity_neutral_elements)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 a0 = ir_alloc_vreg(m, 4, true, false);
    u32 a1 = ir_alloc_vreg(m, 4, true, false);
    u32 a2 = ir_alloc_vreg(m, 4, true, false);
    u32 a3 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_ADD, a0, ir_operand_vreg(p->vreg), ir_operand_imm(0));
    ir_emit_binop(entry, OP_MUL, a1, ir_operand_vreg(a0), ir_operand_imm(1));
    ir_emit_binop(entry, OP_OR, a2, ir_operand_vreg(a1), ir_operand_imm(0));
    ir_emit_binop(entry, OP_SUB, a3, ir_operand_vreg(a2), ir_operand_vreg(a2));
    ir_emit_ret(entry, ir_operand_vreg(a3));
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_add_mul_or_sub(m), 0);
    EXPECT_EQ(count_all_instrs(m), 1); /* just the ret */
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

TEST(opt, identity_mul_by_zero)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_MUL, v0, ir_operand_vreg(p->vreg), ir_operand_imm(0));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_MUL), 0);
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* x + 0.0 and x * 1.0 are not FP identities (-0.0/NaN); they must survive. */
TEST(opt, identity_skips_fp)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 fp = ir_alloc_fp_vreg(m, 4);
    IrParam *param = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    param->name = "x";
    param->type = type_float();
    param->vreg = fp;
    vec_push(f->params, param);
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_fp_vreg(m, 4);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_FADD, v0, ir_operand_vreg(fp), ir_operand_imm(fp_bits(0.0)));
    ir_emit_unary(entry, OP_FTOI, v1, ir_operand_vreg(v0));
    ir_emit_ret(entry, ir_operand_vreg(v1));
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_FADD), 1);
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, cast_same_width_removed)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_unary(entry, OP_ZEXT, v0, ir_operand_vreg(p->vreg));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ZEXT), 0);
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, cast_trunc_imm_normalizes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 1, true, false);
    ir_emit_unary(entry, OP_TRUNC, v0, ir_operand_imm(300));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    EXPECT_EQ(ir_interp_run(m), 44);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_TRUNC), 0);
    EXPECT_EQ(ir_interp_run(m), 44);
    arena_free(a);
}

TEST(opt, cast_widening_kept)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 1, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_unary(entry, OP_SEXT, v0, ir_operand_vreg(p->vreg));
    ir_emit_ret(entry, ir_operand_vreg(v0));
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_SEXT), 1);
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, cprop_double_neg)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 n1 = ir_alloc_vreg(m, 4, true, false);
    u32 n2 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_unary(entry, OP_NEG, n1, ir_operand_vreg(p->vreg));
    ir_emit_unary(entry, OP_NEG, n2, ir_operand_vreg(n1));
    ir_emit_ret(entry, ir_operand_vreg(n2));
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_NEG), 0);
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

static IrModule *build_equal_phi(Arena *a, i64 entry_val, i64 latch_val)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 x = ir_alloc_vreg(m, 8, true, false);
    IrParam *param = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    param->name = "x";
    param->type = type_int();
    param->vreg = x;
    vec_push(f->params, param);
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *left = ir_func_add_block(f, "left");
    IrBlock *right = ir_func_add_block(f, "right");
    IrBlock *exit_bb = ir_func_add_block(f, "exit");
    u32 cond = ir_alloc_vreg(m, 8, true, false);
    u32 p = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ICMP_SLT, cond, ir_operand_vreg(x), ir_operand_imm(1));
    ir_emit_brcond(entry, ir_operand_vreg(cond), left->label, right->label);
    vec_push(left->preds, entry);
    vec_push(right->preds, entry);
    ir_emit_br(left, exit_bb->label);
    vec_push(exit_bb->preds, left);
    ir_emit_br(right, exit_bb->label);
    vec_push(exit_bb->preds, right);
    IrInstr *phi = ir_emit_phi_at_start(exit_bb, p, 2);
    ir_phi_add_entry(phi, ir_operand_imm(entry_val), left);
    ir_phi_add_entry(phi, ir_operand_imm(latch_val), right);
    ir_emit_ret(exit_bb, ir_operand_vreg(p));
    return m;
}

TEST(opt, phi_simp_equal_entries_collapse)
{
    Arena *a = arena_new();
    IrModule *m = build_equal_phi(a, 10, 10);
    EXPECT_EQ(ir_interp_run(m), 10);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_PHI), 0);
    EXPECT_EQ(ir_interp_run(m), 10);
    arena_free(a);
}

/* A natural loop whose latch keeps the value (self entry) collapses to the preheader's. */
TEST(opt, phi_simp_self_latch_invariant)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *head = ir_func_add_block(f, "head");
    IrBlock *latch = ir_func_add_block(f, "latch");
    IrBlock *exit_bb = ir_func_add_block(f, "exit");
    u32 p = ir_alloc_vreg(m, 8, true, false);
    u32 cond = ir_alloc_vreg(m, 8, true, false);
    ir_emit_br(entry, head->label);
    vec_push(head->preds, entry);
    IrInstr *phi = ir_emit_phi_at_start(head, p, 2);
    ir_phi_add_entry(phi, ir_operand_imm(5), entry);
    ir_phi_add_entry(phi, ir_operand_vreg(p), latch);
    ir_emit_binop(head, OP_ICMP_SLT, cond, ir_operand_imm(1), ir_operand_imm(0));
    ir_emit_brcond(head, ir_operand_vreg(cond), latch->label, exit_bb->label);
    vec_push(latch->preds, head);
    vec_push(exit_bb->preds, head);
    ir_emit_br(latch, head->label);
    vec_push(head->preds, latch);
    ir_emit_ret(exit_bb, ir_operand_vreg(p));
    EXPECT_EQ(ir_interp_run(m), 5);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_PHI), 0);
    EXPECT_EQ(ir_interp_run(m), 5);
    arena_free(a);
}

TEST(opt, phi_simp_distinct_values_kept)
{
    Arena *a = arena_new();
    IrModule *m = build_equal_phi(a, 10, 20);
    EXPECT_EQ(ir_interp_run(m), 10);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_PHI), 1);
    EXPECT_EQ(ir_interp_run(m), 10);
    arena_free(a);
}

TEST(opt, dce_removes_unused_arithmetic)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 a0 = ir_alloc_vreg(m, 4, true, false);
    u32 a1 = ir_alloc_vreg(m, 4, true, false);
    u32 c = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_ADD, a0, ir_operand_vreg(p->vreg), ir_operand_imm(1));
    ir_emit_binop(entry, OP_MUL, a1, ir_operand_vreg(a0), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, c, ir_operand_vreg(p->vreg), ir_operand_imm(3));
    ir_emit_ret(entry, ir_operand_vreg(c));
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ADD), 1); /* a0 (via a1) and a1 are gone */
    EXPECT_EQ(count_opcode(m, OP_MUL), 0);
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, dce_removes_unused_alloca)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 sp = ir_alloc_vreg(m, 8, false, false);
    ir_emit_alloca(entry, sp, 8);
    ir_emit_ret(entry, ir_operand_imm(0));
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ALLOCA), 0);
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

TEST(opt, cfg_clean_prunes_dead_blocks)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    return 42;\n"
                                  "    return 7;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 42);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    IrFunction *f = opt_main_fn(m);
    EXPECT_EQ(vec_size(f->blocks), 1);
    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

TEST(opt, preheader_canonical_shape_after_optimize)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 6; i = i + 1) s = s + i;\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 15);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 15);
    arena_free(a);
}

/* optimize passes (GVN, LICM, mem_fwd) */

static IrModule *build_add_add(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 x = ir_alloc_vreg(m, 4, true, false);
    u32 y = ir_alloc_vreg(m, 4, true, false);
    IrParam *px = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    px->name = "x";
    px->type = type_int();
    px->vreg = x;
    IrParam *py = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    py->name = "y";
    py->type = type_int();
    py->vreg = y;
    vec_push(f->params, px);
    vec_push(f->params, py);
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    u32 v1 = ir_alloc_vreg(m, 4, true, false);
    u32 v2 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_ADD, v0, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_binop(entry, OP_ADD, v1, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_binop(entry, OP_ADD, v2, ir_operand_vreg(v0), ir_operand_vreg(v1));
    ir_emit_ret(entry, ir_operand_vreg(v2));
    return m;
}

TEST(opt, gvn_merges_equal_defs)
{
    Arena *a = arena_new();
    IrModule *m = build_add_add(a);
    EXPECT_EQ(count_opcode(m, OP_ADD), 3);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ADD), 2); /* the second a+b folds into the first */
    arena_free(a);
}

TEST(opt, gvn_commutative_merge)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 x = ir_alloc_vreg(m, 4, true, false);
    u32 y = ir_alloc_vreg(m, 4, true, false);
    IrParam *px = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    px->name = "x";
    px->type = type_int();
    px->vreg = x;
    IrParam *py = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    py->name = "y";
    py->type = type_int();
    py->vreg = y;
    vec_push(f->params, px);
    vec_push(f->params, py);
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    u32 v1 = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_ADD, v0, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_binop(entry, OP_ADD, v1, ir_operand_vreg(y), ir_operand_vreg(x));
    ir_emit_ret(entry, ir_operand_vreg(v1));
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_ADD), 1);
    arena_free(a);
}

/* Equal expressions in non-dominating siblings are never merged. */
TEST(opt, gvn_skips_sibling_defs)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 x = ir_alloc_vreg(m, 8, true, false);
    u32 y = ir_alloc_vreg(m, 8, true, false);
    IrParam *px = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    px->name = "x";
    px->type = type_int();
    px->vreg = x;
    IrParam *py = arena_alloc(m->arena, sizeof(IrParam), sizeof(void *));
    py->name = "y";
    py->type = type_int();
    py->vreg = y;
    vec_push(f->params, px);
    vec_push(f->params, py);
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    u32 c = ir_alloc_vreg(m, 8, false, false);
    u32 v0 = ir_alloc_vreg(m, 8, true, false);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ICMP_SLT, c, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_brcond(entry, ir_operand_vreg(c), then_bb->label, else_bb->label);
    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    ir_emit_binop(then_bb, OP_ADD, v0, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_ret(then_bb, ir_operand_vreg(v0));
    ir_emit_binop(else_bb, OP_ADD, v1, ir_operand_vreg(x), ir_operand_vreg(y));
    ir_emit_ret(else_bb, ir_operand_vreg(v1));
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    if (count_opcode(m, OP_ADD) != 2)
    {
        fprintf(stderr, "add count: %u\n", count_opcode(m, OP_ADD));
        test_fail();
    }
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* The block that owns the sole surviving MUL is the loop's preheader. */
static IrBlock *block_of_instr(IrFunction *f, IrInstr *in)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        if (opt_instr_index(bb, in) != UINT32_MAX)
        {
            return bb;
        }
    }
    return NULL;
}

TEST(opt, licm_hoists_invariant_mul)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int helper(int k) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 100; i = i + 1) s = s + k * 3;\n"
                                  "    return s;\n"
                                  "}\n"
                                  "int main(void) { return helper(5) == 1500; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 1);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0); /* helper */
    EXPECT_EQ(count_opcode(m, OP_MUL), 1);
    IrInstr *mul = NULL;
    size_t nblocks = vec_size(f->blocks);
    for (size_t b = 0; b < nblocks; b++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t j = 0; j < ninstr; j++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
            if (in->opcode == OP_MUL)
            {
                mul = in;
            }
        }
    }
    EXPECT_TRUE(mul != NULL);
    /* the invariant k*3 lands in blocks[0] (the preheader) */
    EXPECT_TRUE(block_of_instr(f, mul) == (IrBlock *) vec_get(f->blocks, 0));
    EXPECT_EQ(ir_interp_run(m), 1);
    arena_free(a);
}

TEST(opt, licm_keeps_loop_carried_defs)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int helper(int k) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 100; i = i + 1) s = s + k;\n"
                                  "    return s;\n"
                                  "}\n"
                                  "int main(void) { return helper(5) == 500; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 1);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    /* the add uses `s` via a header phi, so it must stay in the loop */
    size_t nadd = 0;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                if (in->opcode == OP_ADD)
                {
                    nadd++;
                }
            }
        }
    }
    EXPECT_TRUE(nadd >= 1);
    EXPECT_EQ(ir_interp_run(m), 1);
    arena_free(a);
}

TEST(opt, mem_fwd_runs_at_level_1)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int g;\n"
                                  "int main(void) { g = 40; return g; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 40);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_LOAD), 0);
    EXPECT_EQ(count_opcode(m, OP_STORE), 1);
    EXPECT_EQ(ir_interp_run(m), 40);
    arena_free(a);
}

/* Store-then-load of the same global forwards the value; the load dies. */
TEST(opt, mem_fwd_forwards_store_to_load)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int g;\n"
                                  "int main(void) { g = 40; return g; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 40);
    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_LOAD), 0);
    EXPECT_EQ(count_opcode(m, OP_STORE), 1);
    EXPECT_EQ(ir_interp_run(m), 40);
    arena_free(a);
}

/* A volatile store is a barrier: the following load must survive. */
TEST(opt, mem_fwd_volatile_barrier)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("volatile int g;\n"
                                  "int main(void) { g = 40; return g; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 40);
    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_LOAD), 1);
    EXPECT_EQ(ir_interp_run(m), 40);
    arena_free(a);
}

/* A call between store and load is a barrier: the load must survive. */
TEST(opt, mem_fwd_call_barrier)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int g;\n"
                                  "int f(void) { return 1; }\n"
                                  "int main(void) {\n"
                                  "    g = 40;\n"
                                  "    (void) f();\n"
                                  "    return g;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 40);
    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_LOAD), 1);
    EXPECT_EQ(ir_interp_run(m), 40);
    arena_free(a);
}

/* Two identical loads of one global collapse to one (redundant-load elim). */
TEST(opt, mem_fwd_redundant_load)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int g = 20;\n"
                                  "int main(void) { int a = g; int b = g; return a + b; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 40);
    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(count_opcode(m, OP_LOAD), 1);
    EXPECT_EQ(ir_interp_run(m), 40);
    arena_free(a);
}

/* The body of a `for (k = n; k-- > 1;)` loop reads the decremented value. */
TEST(opt, for_postdec_condition_visits_decremented_values)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static unsigned char data[8];\n"
                                  "int main(void) {\n"
                                  "    int n = 8;\n"
                                  "    int k;\n"
                                  "    for (k = n; k-- > 1;)\n"
                                  "        data[k] = 1;\n"
                                  "    int ok = 1;\n"
                                  "    for (int i = 0; i < 8; i = i + 1)\n"
                                  "        if (i >= 1 && i <= 7 && data[i] != 1)\n"
                                  "            ok = 0;\n"
                                  "    return ok ? 0 : 1;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_3, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* The sext of an immediate must survive optimization (folding it would
   narrow the value at its uses). */
TEST(opt, sext_imm_kept_across_optimize)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int emit(long long addend)\n"
                                  "{\n"
                                  "    return addend == -4 ? 0 : 1;\n"
                                  "}\n"
                                  "int main(void) { return emit(-4); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    optimize(m, OPT_LEVEL_2, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_opcode(m, OP_SEXT) >= 1);
    arena_free(a);
}

/* A latch stub feeding a header phi must stay a single-successor block. */
TEST(opt, cfg_clean_keeps_latch_for_header_phi_copy)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static unsigned char data[8];\n"
                                  "int main(void) {\n"
                                  "    int end = 8;\n"
                                  "    int k;\n"
                                  "    for (k = 0; k < end; k = k + 1)\n"
                                  "        if (k == 4)\n"
                                  "            break;\n"
                                  "        else\n"
                                  "            data[k] = 1;\n"
                                  "    int ok = 1;\n"
                                  "    for (int i = 0; i < 8; i = i + 1)\n"
                                  "        if (i <= 3 && data[i] != 1)\n"
                                  "            ok = 0;\n"
                                  "    return ok ? 0 : 1;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 0);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* inline pass (F1: tier 1 user-directed + tier 2 size filter) */

static u32 count_calls_to(IrModule *m, const char *fname)
{
    u32 n = 0;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        size_t nb = vec_size(f->blocks);
        for (size_t b = 0; b < nb; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t nin = vec_size(bb->instrs);
            for (size_t j = 0; j < nin; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                if (in->opcode == OP_CALL && !in->extra.call.is_indirect &&
                    strcmp(in->extra.call.name, fname) == 0)
                {
                    n++;
                }
            }
        }
    }
    return n;
}

static u32 count_opcode_in_fn(IrModule *m, const char *fname, IrOpcode op)
{
    u32 n = 0;
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        if (fname && strcmp(f->name, fname) != 0)
        {
            continue;
        }
        size_t nb = vec_size(f->blocks);
        for (size_t b = 0; b < nb; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t nin = vec_size(bb->instrs);
            for (size_t j = 0; j < nin; j++)
            {
                if (((IrInstr *) vec_get(bb->instrs, j))->opcode == op)
                {
                    n++;
                }
            }
        }
    }
    return n;
}

static IrFunction *fn_by_name(IrModule *m, const char *name)
{
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        if (strcmp(f->name, name) == 0)
        {
            return f;
        }
    }
    return NULL;
}

/* Tier 1: a `static inline` leaf disappears entirely at -O1. */
TEST(opt, inline_tier1_removes_call)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int sq(int x) { return x * x; }\n"
                                  "int main(void) { return sq(6); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_TRUE(count_calls_to(m, "sq") >= 1);
    EXPECT_EQ(ir_interp_run(m), 36);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "sq") == 0);
    EXPECT_EQ(ir_interp_run(m), 36);
    arena_free(a);
}

/* A `static inline` keeps its addressable copy (C11 §6.7.4). */
TEST(opt, inline_static_copy_kept)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int add(int x) { return x + 1; }\n"
                                  "int main(void) { return add(41); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    IrFunction *add = fn_by_name(m, "add");
    EXPECT_NOTNULL(add);
    EXPECT_TRUE(vec_size(add->blocks) > 0);
    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

/* Direct or transitive recursion is never expanded: a self-call stays a call. */
TEST(opt, inline_recursion_not_expanded)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int down(int n)\n"
                                  "{\n"
                                  "    return n <= 0 ? n : down(n - 1) + 1;\n"
                                  "}\n"
                                  "int main(void) { return down(5); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 5);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "down") >= 1); /* the recursion remains */
    EXPECT_EQ(ir_interp_run(m), 5);
    arena_free(a);
}

/* Mutual recursion f <-> g is equally inert under the chain guard. */
TEST(opt, inline_transitive_recursion_blocked)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int g(int n);\n"
                                  "static inline int f(int n)\n"
                                  "{\n"
                                  "    return n <= 0 ? 1 : g(n - 1);\n"
                                  "}\n"
                                  "static inline int g(int n)\n"
                                  "{\n"
                                  "    return f(n - 1);\n"
                                  "}\n"
                                  "int main(void) { return f(3); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 1);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "f") + count_calls_to(m, "g") >= 1);
    EXPECT_EQ(ir_interp_run(m), 1);
    arena_free(a);
}

/* Tier 2: a non-inline helper that is too cheap to stay a call inlines too. */
TEST(opt, inline_tier2_small_callee)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int twice(int x) { return x + x; }\n"
                                  "int main(void) { return twice(21); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_TRUE(count_calls_to(m, "twice") >= 1);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "twice") == 0);
    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

/* Tier 2 size filter: a body above the cap stays a call. */
TEST(opt, inline_tier2_big_callee_kept)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int many(int k)\n"
                                  "{\n"
                                  "    int s = 0;\n"
                                  "    s = s + k; s = s + k; s = s + k; s = s + k; s = s + k;\n"
                                  "    s = s + k; s = s + k; s = s + k; s = s + k; s = s + k;\n"
                                  "    s = s + k; s = s + k; s = s + k; s = s + k; s = s + k;\n"
                                  "    s = s + k; s = s + k; s = s + k; s = s + k; s = s + k;\n"
                                  "    s = s + k; s = s + k;\n"
                                  "    return s;\n"
                                  "}\n"
                                  "int main(void) { return many(1) == 22 ? 0 : 1; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "many") >= 1); /* over the size cap */
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* A multi-exit inline body merges its returns into the call result phi. */
TEST(opt, inline_multi_ret_phi)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int cls(int v)\n"
                                  "{\n"
                                  "    if (v < 0) return -1;\n"
                                  "    if (v == 0) return 0;\n"
                                  "    return 1;\n"
                                  "}\n"
                                  "int main(void) { return cls(-3) + cls(0) + cls(4) + 2; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 2);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "cls") == 0);
    EXPECT_EQ(ir_interp_run(m), 2);
    arena_free(a);
}

/* A void inline with a loop-local alloca and a pointer param. */
TEST(opt, inline_void_alloca)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline void bump(int *p, int n)\n"
                                  "{\n"
                                  "    for (int k = 0; k < n; k = k + 1) *p = *p + 1;\n"
                                  "}\n"
                                  "int main(void) { int v = 0; bump(&v, 3); return v; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 3);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "bump") == 0);
    EXPECT_EQ(ir_interp_run(m), 3);
    arena_free(a);
}

/* A direct call to a non-inline leaf nested inside another inline callee is
   itself inlined (the clone's call sites fall under a fresh chain). */
TEST(opt, inline_nested_chain)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int seven(void) { return 7; }\n"
                                  "static inline int bump7(int x) { return x + seven(); }\n"
                                  "int main(void) { return bump7(10); }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "bump7") == 0);
    EXPECT_TRUE(count_calls_to(m, "seven") == 0);
    EXPECT_EQ(ir_interp_run(m), 17);
    arena_free(a);
}

/* Taking a function's address routes the call through an operand: that call
   is indirect and stays, even if the target is `inline`. */
TEST(opt, inline_indirect_stays)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("static inline int sel(int v) { return v * 2; }\n"
                                  "int main(void)\n"
                                  "{\n"
                                  "    int (*fp)(int) = &sel;\n"
                                  "    return fp(21) == 42 ? 0 : 1;\n"
                                  "}\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "sel") == 0);               /* no direct calls remain */
    EXPECT_TRUE(count_opcode_in_fn(m, "main", OP_CALL) >= 1); /* the fp() */
    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

/* An sret (record-return) inline writes through the caller's slot pointer. */
TEST(opt, inline_sret_record)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("typedef struct { int a; int b; } Pair;\n"
                                  "static inline Pair mk(int a, int b)\n"
                                  "{\n"
                                  "    Pair p;\n"
                                  "    p.a = a;\n"
                                  "    p.b = b;\n"
                                  "    return p;\n"
                                  "}\n"
                                  "int main(void) { Pair q = mk(20, 22); return q.a + q.b; }\n",
                                  a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(ir_interp_run(m), 42);
    optimize(m, OPT_LEVEL_1, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_TRUE(count_calls_to(m, "mk") == 0);
    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

/* Phase 25 J: strength reduction, reassociation, and dead-store elimination. */

static IrInstr *def_of(IrModule *m, u32 vreg)
{
    size_t nfuncs = vec_size(m->funcs);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        IrFunction *f = (IrFunction *) vec_get(m->funcs, fi);
        size_t nblocks = vec_size(f->blocks);
        for (size_t b = 0; b < nblocks; b++)
        {
            IrBlock *bb = (IrBlock *) vec_get(f->blocks, b);
            size_t ninstr = vec_size(bb->instrs);
            for (size_t j = 0; j < ninstr; j++)
            {
                IrInstr *in = (IrInstr *) vec_get(bb->instrs, j);
                if (in->result == vreg)
                {
                    return in;
                }
            }
        }
    }
    return NULL;
}

static u32 add_param_vreg(Arena *a, IrModule *m, IrFunction *f, const char *name)
{
    u32 v = ir_alloc_vreg(m, 8, true, false);
    IrParam *p = arena_alloc(a, sizeof(IrParam), _Alignof(IrParam));
    p->name = name;
    p->type = type_int();
    p->vreg = v;
    vec_push(f->params, p);
    return v;
}

TEST(opt, strength_rewrites_power_of_two_multiply)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 x = ir_alloc_vreg(m, 4, true, false);
    u32 r = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(bb, OP_MUL, x, ir_operand_imm(3), ir_operand_imm(1));
    ir_emit_binop(bb, OP_MUL, r, ir_operand_vreg(x), ir_operand_imm(8));
    ir_emit_ret(bb, ir_operand_vreg(r));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_strength(&ctx));
    EXPECT_EQ(def_of(m, r)->opcode, OP_SHL);
    EXPECT_EQ(def_of(m, r)->ops[1].u.imm, 3);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, strength_rewrites_unsigned_modulo_by_power_of_two)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 x = ir_alloc_vreg(m, 4, false, false);
    u32 r = ir_alloc_vreg(m, 4, false, false);
    ir_emit_binop(bb, OP_UDIV, x, ir_operand_imm(9), ir_operand_imm(1));
    ir_emit_binop(bb, OP_UREM, r, ir_operand_vreg(x), ir_operand_imm(16));
    ir_emit_ret(bb, ir_operand_vreg(r));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_strength(&ctx));
    EXPECT_EQ(def_of(m, r)->opcode, OP_AND);
    EXPECT_EQ(def_of(m, r)->ops[1].u.imm, 15);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, reassoc_combines_addends)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 base = add_param_vreg(a, m, f, "base");
    u32 t = ir_alloc_vreg(m, 4, true, false);
    u32 r = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(bb, OP_ADD, t, ir_operand_vreg(base), ir_operand_imm(3));
    ir_emit_binop(bb, OP_SUB, r, ir_operand_vreg(t), ir_operand_imm(9));
    ir_emit_ret(bb, ir_operand_vreg(r));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_reassoc(&ctx));
    EXPECT_EQ(def_of(m, r)->opcode, OP_ADD);
    EXPECT_EQ(def_of(m, r)->ops[0].u.vreg, base);
    EXPECT_EQ(def_of(m, r)->ops[1].u.imm, -6);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, canon_moves_immediates_to_operand_one)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 sum = ir_alloc_vreg(m, 4, true, false);
    u32 prod = ir_alloc_vreg(m, 4, true, false);
    ir_emit_binop(entry, OP_ADD, sum, ir_operand_imm(5), ir_operand_vreg(p->vreg));
    ir_emit_binop(entry, OP_MUL, prod, ir_operand_imm(7), ir_operand_vreg(sum));
    ir_emit_ret(entry, ir_operand_vreg(prod));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_canon(&ctx));
    EXPECT_EQ(def_of(m, sum)->ops[0].u.vreg, p->vreg);
    EXPECT_EQ(def_of(m, sum)->ops[1].u.imm, 5);
    EXPECT_EQ(def_of(m, prod)->ops[0].u.vreg, sum);
    EXPECT_EQ(def_of(m, prod)->ops[1].u.imm, 7);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, canon_mirrors_ordered_comparisons)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = add_int_param(m, "x", 4, true);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    IrParam *p = (IrParam *) vec_get(f->params, 0);
    u32 slt = ir_alloc_vreg(m, 1, false, false);
    u32 uge = ir_alloc_vreg(m, 1, false, false);
    ir_emit_binop(entry, OP_ICMP_SLT, slt, ir_operand_imm(5), ir_operand_vreg(p->vreg));
    ir_emit_binop(entry, OP_ICMP_UGE, uge, ir_operand_imm(9), ir_operand_vreg(p->vreg));
    ir_emit_ret(entry, ir_operand_vreg(slt));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_canon(&ctx));
    EXPECT_EQ(def_of(m, slt)->opcode, OP_ICMP_SGT);
    EXPECT_EQ(def_of(m, slt)->ops[0].u.vreg, p->vreg);
    EXPECT_EQ(def_of(m, slt)->ops[1].u.imm, 5);
    EXPECT_EQ(def_of(m, uge)->opcode, OP_ICMP_ULE);
    EXPECT_EQ(def_of(m, uge)->ops[0].u.vreg, p->vreg);
    EXPECT_EQ(def_of(m, uge)->ops[1].u.imm, 9);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, canon_pipeline_preserves_value)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int f(int x) { return 5 * x + (0 == x) + (5 < x); }\n"
                                  "int main(void) { return f(6); }\n",
                                  a);
    EXPECT_NOTNULL(m);
    i64 before = ir_interp_run(m);
    optimize(m, OPT_LEVEL_3, a);
    EXPECT_TRUE(opt_verify(m));
    EXPECT_EQ(ir_interp_run(m), before);
    arena_free(a);
}

TEST(opt, dse_drops_an_overwritten_store)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 p = add_param_vreg(a, m, f, "p");
    ir_emit_store(bb, ir_operand_imm(1), ir_operand_vreg(p), 4, false);
    ir_emit_store(bb, ir_operand_imm(2), ir_operand_vreg(p), 4, false);
    ir_emit_ret(bb, ir_operand_imm(0));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_TRUE(opt_pass_dse(&ctx));
    EXPECT_EQ(count_opcode(m, OP_STORE), 1u);
    EXPECT_TRUE(opt_verify(m));
    arena_free(a);
}

TEST(opt, dse_keeps_a_store_a_load_may_read)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 p = add_param_vreg(a, m, f, "p");
    u32 v = ir_alloc_vreg(m, 4, true, false);
    ir_emit_store(bb, ir_operand_imm(1), ir_operand_vreg(p), 4, false);
    ir_emit_load(bb, v, ir_operand_vreg(p), false);
    ir_emit_store(bb, ir_operand_imm(2), ir_operand_vreg(p), 4, false);
    ir_emit_ret(bb, ir_operand_vreg(v));
    OptimizerContext ctx = make_ctx(m, a);
    EXPECT_FALSE(opt_pass_dse(&ctx));
    EXPECT_EQ(count_opcode(m, OP_STORE), 2u);
    arena_free(a);
}

TEST(opt, o3_strength_is_distinct_from_o2)
{
    Arena *a1 = arena_new();
    Arena *a2 = arena_new();
    const char *src = "int main(void)\n"
                      "{\n"
                      "    int s = 0;\n"
                      "    for (int i = 0; i < 8; i = i + 1) s = s + i * 8;\n"
                      "    return s;\n"
                      "}\n";
    IrModule *m2 = tc_build_module(src, a1);
    IrModule *m3 = tc_build_module(src, a2);
    optimize(m2, OPT_LEVEL_2, a1);
    optimize(m3, OPT_LEVEL_3, a2);
    EXPECT_TRUE(m2 && m3);
    EXPECT_TRUE(count_opcode(m2, OP_MUL) > 0);
    EXPECT_EQ(count_opcode(m3, OP_MUL), 0u);
    EXPECT_EQ(ir_interp_run(m2), ir_interp_run(m3));
    arena_free(a1);
    arena_free(a2);
}
