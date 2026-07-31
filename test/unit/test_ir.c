#include "harness.h"
#include <string.h>
#include "ir.h"
#include "util/arena.h"

TEST(ir, module_creation)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(m->next_vreg, 0);
    EXPECT_EQ(m->width_count, 0);
    EXPECT_TRUE(m->funcs != NULL);
    EXPECT_TRUE(m->globals != NULL);
    arena_free(a);
}

TEST(ir, vreg_allocation)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);

    u32 v0 = ir_alloc_vreg(m, 32);
    u32 v1 = ir_alloc_vreg(m, 64);
    u32 v2 = ir_alloc_vreg(m, 8);

    EXPECT_EQ(v0, 0);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(v2, 2);
    EXPECT_EQ(m->width_count, 3);
    EXPECT_EQ(m->widths[0], 32);
    EXPECT_EQ(m->widths[1], 64);
    EXPECT_EQ(m->widths[2], 8);

    arena_free(a);
}

TEST(ir, vreg_table_growth)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);

    for (int i = 0; i < 20; i++)
        ir_alloc_vreg(m, (u8)(i + 1));

    EXPECT_EQ(m->width_count, 20);
    for (int i = 0; i < 20; i++)
        EXPECT_EQ(m->widths[i], (u8)(i + 1));

    arena_free(a);
}

TEST(ir, function_and_block)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    EXPECT_TRUE(f != NULL);
    EXPECT_TRUE(strcmp(f->name, "main") == 0);
    EXPECT_EQ(vec_size(f->blocks), 1);
    EXPECT_TRUE(bb != NULL);
    EXPECT_TRUE(strcmp(bb->label, "entry") == 0);

    arena_free(a);
}

TEST(ir, emit_ret)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    Instr *i = ir_emit_ret(bb, a, ir_operand_imm(42));
    EXPECT_TRUE(i != NULL);
    EXPECT_EQ(i->opcode, OP_RET);
    EXPECT_EQ(i->nops, 1);
    EXPECT_TRUE(i->ops[0].is_imm);
    EXPECT_EQ(i->ops[0].u.imm, 42);

    arena_free(a);
}

TEST(ir, emit_unreachable)
{
    Arena *a = arena_new();
    Module *m = ir_module_new(a);
    Function *f = ir_module_add_func(m, a, "main", type_int());
    Block *bb = ir_func_add_block(f, a, "entry");

    Instr *i = ir_emit_unreachable(bb, a);
    EXPECT_TRUE(i != NULL);
    EXPECT_EQ(i->opcode, OP_UNREACHABLE);
    EXPECT_EQ(i->nops, 0);

    arena_free(a);
}

TEST(ir, operand_vreg)
{
    Operand o = ir_operand_vreg(7);
    EXPECT_FALSE(o.is_imm);
    EXPECT_EQ(o.u.vreg, 7);
}

TEST(ir, operand_imm)
{
    Operand o = ir_operand_imm(-123);
    EXPECT_TRUE(o.is_imm);
    EXPECT_EQ(o.u.imm, -123);
}

TEST(ir, opcode_names)
{
    EXPECT_TRUE(strcmp(ir_opcode_name(OP_RET), "OP_RET") == 0);
    EXPECT_TRUE(strcmp(ir_opcode_name(OP_ADD), "OP_ADD") == 0);
    EXPECT_TRUE(strcmp(ir_opcode_name(OP_ICMP_SLT), "OP_ICMP_SLT") == 0);
}
