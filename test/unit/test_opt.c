#include "harness.h"
#include "testdriver.h"

#include "optpasses/opt_internal.h"

#include "ir.h"
#include "util/arena.h"

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
