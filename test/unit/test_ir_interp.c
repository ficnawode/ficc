#include "harness.h"
#include "ir.h"
#include "ir_interp.h"
#include "util/arena.h"

TEST(ir_interp, arithmetic)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    u32 v2 = ir_alloc_vreg(m, 4);
    u32 v3 = ir_alloc_vreg(m, 4);

    ir_emit_add(bb, a, v2, ir_operand_imm(10), ir_operand_imm(3));
    ir_emit_sub(bb, a, v3, ir_operand_vreg(v2), ir_operand_imm(2));
    ir_emit_ret(bb, a, ir_operand_vreg(v3));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 11); /* 10 + 3 - 2 */

    arena_free(a);
}

TEST(ir_interp, multiply_and_divide)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    u32 v1 = ir_alloc_vreg(m, 4);
    u32 v2 = ir_alloc_vreg(m, 4);

    ir_emit_mul(bb, a, v1, ir_operand_imm(7), ir_operand_imm(3));
    ir_emit_sdiv(bb, a, v2, ir_operand_vreg(v1), ir_operand_imm(3));
    ir_emit_ret(bb, a, ir_operand_vreg(v2));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 7); /* 7 * 3 / 3 */

    arena_free(a);
}

TEST(ir_interp, negation)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    u32 v0 = ir_alloc_vreg(m, 4);
    ir_emit_neg(bb, a, v0, ir_operand_imm(42));
    ir_emit_ret(bb, a, ir_operand_vreg(v0));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, -42);

    arena_free(a);
}

TEST(ir_interp, function_call)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);

    /* int add(int a, int b) { return a + b; } */
    Function *add = ir_module_add_func(m, a, "add", type_int());
    Block *add_bb = ir_func_add_block(add, a, "entry");
    u32 add_a = ir_alloc_vreg(m, 4);
    u32 add_b = ir_alloc_vreg(m, 4);
    u32 add_r = ir_alloc_vreg(m, 4);
    Param *pa = arena_alloc(a, sizeof(Param), sizeof(void *));
    pa->name = "a";
    pa->type = type_int();
    pa->vreg = add_a;
    Param *pb = arena_alloc(a, sizeof(Param), sizeof(void *));
    pb->name = "b";
    pb->type = type_int();
    pb->vreg = add_b;
    vec_push(add->params, pa);
    vec_push(add->params, pb);
    ir_emit_add(add_bb, a, add_r, ir_operand_vreg(add_a), ir_operand_vreg(add_b));
    ir_emit_ret(add_bb, a, ir_operand_vreg(add_r));

    /* int main(void) { return add(3, 4); } */
    Function *main_fn = ir_module_add_func(m, a, "main", type_int());
    Block *main_bb = ir_func_add_block(main_fn, a, "entry");
    u32 main_r = ir_alloc_vreg(m, 4);
    Operand args[2] = {ir_operand_imm(3), ir_operand_imm(4)};
    ir_emit_call(main_bb, a, main_r, "add", 2, args);
    ir_emit_ret(main_bb, a, ir_operand_vreg(main_r));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 7);

    arena_free(a);
}

TEST(ir_interp, phi_selection)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *entry = ir_func_add_block(f, a, "entry");
    Block *then_bb = ir_func_add_block(f, a, "then");
    Block *else_bb = ir_func_add_block(f, a, "else");
    Block *merge = ir_func_add_block(f, a, "merge");

    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);
    then_bb->sealed = true;
    else_bb->sealed = true;
    merge->sealed = true;

    ir_emit_brcond(entry, a, ir_operand_imm(1), "then", "else");

    ir_emit_ret(then_bb, a, ir_operand_imm(10));
    ir_emit_ret(else_bb, a, ir_operand_imm(20));

    u32 phi_result = ir_alloc_vreg(m, 4);
    Instr *phi = ir_emit_phi(merge, a, phi_result, 2);
    phi_add_entry(phi, ir_operand_imm(10), then_bb);
    phi_add_entry(phi, ir_operand_imm(20), else_bb);
    ir_emit_ret(merge, a, ir_operand_vreg(phi_result));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

TEST(ir_interp, brcond_false)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *entry = ir_func_add_block(f, a, "entry");
    Block *then_bb = ir_func_add_block(f, a, "then");
    Block *else_bb = ir_func_add_block(f, a, "else");

    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    then_bb->sealed = true;
    else_bb->sealed = true;

    ir_emit_brcond(entry, a, ir_operand_imm(0), "then", "else");
    ir_emit_ret(then_bb, a, ir_operand_imm(10));
    ir_emit_ret(else_bb, a, ir_operand_imm(20));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 20);
    arena_free(a);
}
