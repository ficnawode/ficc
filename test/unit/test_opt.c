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

/* --- invariant 1: SSA (single def; every operand has one; params def'd at entry) --- */

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

/* --- invariant 2: block shape --- */

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

/* --- invariant 3: CFG coherence --- */

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

/* --- invariant 4: labels unique per function --- */

TEST(opt, duplicate_label_rejected)
{
    Arena *a = arena_new();
    IrModule *m = build_if_else(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    ir_func_add_block(f, "entry");
    EXPECT_FALSE(opt_verify(m));
    arena_free(a);
}

/* --- invariant 5: operands --- */

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

/* --- invariant 6: module value tables --- */

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

/* --- invariant 7: volatile as a memory-op barrier --- */

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

/* ---- CFG base, dominators, natural loops, canonical shape ---- */

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

/* ---- optimize() shell, mode tables, shared magic ---- */

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
