#include "harness.h"
#include "ir.h"
#include "ir_interp.h"
#include "util/arena.h"

TEST(ir_interp, arithmetic)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    u32 v2 = ir_alloc_vreg(m, 4);
    u32 v3 = ir_alloc_vreg(m, 4);

    ir_emit_binop(bb, OP_ADD, v2, ir_operand_imm(10), ir_operand_imm(3));
    ir_emit_binop(bb, OP_SUB, v3, ir_operand_vreg(v2), ir_operand_imm(2));
    ir_emit_ret(bb, ir_operand_vreg(v3));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 11); /* 10 + 3 - 2 */

    arena_free(a);
}

TEST(ir_interp, multiply_and_divide)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    u32 v1 = ir_alloc_vreg(m, 4);
    u32 v2 = ir_alloc_vreg(m, 4);

    ir_emit_binop(bb, OP_MUL, v1, ir_operand_imm(7), ir_operand_imm(3));
    ir_emit_binop(bb, OP_SDIV, v2, ir_operand_vreg(v1), ir_operand_imm(3));
    ir_emit_ret(bb, ir_operand_vreg(v2));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 7); /* 7 * 3 / 3 */

    arena_free(a);
}

TEST(ir_interp, negation)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    u32 v0 = ir_alloc_vreg(m, 4);
    ir_emit_unary(bb, OP_NEG, v0, ir_operand_imm(42));
    ir_emit_ret(bb, ir_operand_vreg(v0));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, -42);

    arena_free(a);
}

TEST(ir_interp, function_call)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);

    /* int add(int a, int b) { return a + b; } */
    IrFunction *add = ir_module_add_func(m, "add", type_int());
    IrBlock *add_bb = ir_func_add_block(add, "entry");
    u32 add_a = ir_alloc_vreg(m, 4);
    u32 add_b = ir_alloc_vreg(m, 4);
    u32 add_r = ir_alloc_vreg(m, 4);
    IrParam *pa = arena_alloc(a, sizeof(IrParam), sizeof(void *));
    pa->name = "a";
    pa->type = type_int();
    pa->vreg = add_a;
    IrParam *pb = arena_alloc(a, sizeof(IrParam), sizeof(void *));
    pb->name = "b";
    pb->type = type_int();
    pb->vreg = add_b;
    vec_push(add->params, pa);
    vec_push(add->params, pb);
    ir_emit_binop(add_bb, OP_ADD, add_r, ir_operand_vreg(add_a), ir_operand_vreg(add_b));
    ir_emit_ret(add_bb, ir_operand_vreg(add_r));

    /* int main(void) { return add(3, 4); } */
    IrFunction *main_fn = ir_module_add_func(m, "main", type_int());
    IrBlock *main_bb = ir_func_add_block(main_fn, "entry");
    u32 main_r = ir_alloc_vreg(m, 4);
    IrOperand args[2] = {ir_operand_imm(3), ir_operand_imm(4)};
    ir_emit_call(main_bb, main_r, "add", 2, args);
    ir_emit_ret(main_bb, ir_operand_vreg(main_r));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 7);

    arena_free(a);
}

TEST(ir_interp, phi_selection)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    IrBlock *merge = ir_func_add_block(f, "merge");

    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);
    then_bb->sealed = true;
    else_bb->sealed = true;
    merge->sealed = true;

    ir_emit_brcond(entry, ir_operand_imm(1), "then", "else");

    ir_emit_ret(then_bb, ir_operand_imm(10));
    ir_emit_ret(else_bb, ir_operand_imm(20));

    u32 phi_result = ir_alloc_vreg(m, 4);
    IrInstr *phi = ir_emit_phi(merge, phi_result, 2);
    ir_phi_add_entry(phi, ir_operand_imm(10), then_bb);
    ir_phi_add_entry(phi, ir_operand_imm(20), else_bb);
    ir_emit_ret(merge, ir_operand_vreg(phi_result));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

TEST(ir_interp, brcond_false)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");

    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    then_bb->sealed = true;
    else_bb->sealed = true;

    ir_emit_brcond(entry, ir_operand_imm(0), "then", "else");
    ir_emit_ret(then_bb, ir_operand_imm(10));
    ir_emit_ret(else_bb, ir_operand_imm(20));

    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 20);
    arena_free(a);
}
