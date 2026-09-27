#include "harness.h"
#include "testdriver.h"

#include "ir.h"
#include "ir_builder.h"

static IrModule *lines_module(const char *src, Arena *a)
{
    IrModule *m = tc_build_module(src, a);
    EXPECT_NOTNULL(m);
    return m;
}

static IrInstr *first_instr(IrModule *m, size_t func_i)
{
    IrFunction *f = (IrFunction *) vec_get(m->funcs, func_i);
    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    return (IrInstr *) vec_get(bb->instrs, 0);
}

TEST(ir_lines, statement_line_stamped)
{
    Arena *a = arena_new();
    IrModule *m = lines_module("int f(void)\n{\n  return 7;\n}\n", a);
    IrInstr *ret = first_instr(m, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->line, 3);

    arena_free(a);
}

TEST(ir_lines, statement_line_after_blank_lines)
{
    Arena *a = arena_new();
    IrModule *m = lines_module("int f(void)\n{\n\n\n  return 7;\n}\n", a);
    IrInstr *ret = first_instr(m, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->line, 5);

    arena_free(a);
}

TEST(ir_lines, function_boundary_resets_line)
{
    Arena *a = arena_new();
    IrModule *m = lines_module("int a(void)\n{\n  return 1;\n}\n"
                               "int b(void)\n{\n  return 2;\n}\n",
                               a);
    IrInstr *ra = first_instr(m, 0);
    IrInstr *rb = first_instr(m, 1);
    EXPECT_EQ(ra->line, 3);
    /* Function b starts fresh, not with a stale line from function a. */
    EXPECT_EQ(rb->line, 7);

    arena_free(a);
}

TEST(ir_lines, distinct_statements_get_distinct_lines)
{
    Arena *a = arena_new();
    IrModule *m = lines_module("void f(int *p)\n{\n  *p = 1;\n  *p = 2;\n}\n", a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    bool saw_line3 = false;
    bool saw_line4 = false;
    for (size_t i = 0; i < vec_size(bb->instrs); i++)
    {
        u32 line = ((IrInstr *) vec_get(bb->instrs, i))->line;
        if (line == 3)
        {
            saw_line3 = true;
        }
        if (line == 4)
        {
            saw_line4 = true;
        }
    }
    EXPECT_TRUE(saw_line3);
    EXPECT_TRUE(saw_line4);

    arena_free(a);
}

TEST(ir_lines, block_knows_its_function)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "f", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    EXPECT_TRUE(bb->func == f);

    arena_free(a);
}