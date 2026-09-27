#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "elf.h"
#include "util/arena.h"
#include "util/vec.h"

#include <unistd.h>

TEST(const, const_local_read)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    const int x = 5;\n"
                            "    return x + 1;\n"
                            "}\n"),
              6);
}

TEST(const, const_local_spelling_int_const)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int const x = 5;\n"
                            "    return x + 1;\n"
                            "}\n"),
              6);
}

TEST(const, const_block_static_read)
{
    EXPECT_INTERP_AND_ELF("int f(int x) {\n"
                          "    static const int k = 7;\n"
                          "    return x + k;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(5);\n"
                          "}\n",
                          12);
}

TEST(const, const_global_read)
{
    EXPECT_INTERP_AND_ELF("const int g = 42;\n"
                          "int main(void) {\n"
                          "    return g;\n"
                          "}\n",
                          42);
}

TEST(const, const_global_zero_init)
{
    EXPECT_INTERP_AND_ELF("const int z;\n"
                          "int main(void) {\n"
                          "    return z + 3;\n"
                          "}\n",
                          3);
}

TEST(const, const_global_string_pointer)
{
    EXPECT_INTERP_AND_ELF("const char * const p = \"hi\";\n"
                          "int main(void) {\n"
                          "    return p[1];\n"
                          "}\n",
                          'i');
}

TEST(const, const_global_array_of_structs)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "const struct S a[2] = {{1, 2}, {3, 4}};\n"
                          "int main(void) {\n"
                          "    return a[0].x + a[1].y;\n"
                          "}\n",
                          5);
}

TEST(const, const_array_elements)
{
    EXPECT_EQ(tc_run_interp("const int a[3];\n"
                            "int sum(const int arr[3]) {\n"
                            "    return arr[0] + arr[2];\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return sum(a);\n"
                            "}\n"),
              0);
}

TEST(const, struct_const_pointer_arg)
{
    EXPECT_INTERP_AND_ELF("struct point { int x; int y; };\n"
                          "int sum(struct point const *p) {\n"
                          "    return p->x + p->y;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    struct point pt;\n"
                          "    pt.x = 3;\n"
                          "    pt.y = 4;\n"
                          "    return sum(&pt);\n"
                          "}\n",
                          7);
}

TEST(const, const_struct_value_assignment_is_read_only)
{
    EXPECT_EQ(tc_run_interp("struct point { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct point a;\n"
                            "    a.x = 5;\n"
                            "    a.y = 7;\n"
                            "    const struct point b = a;\n"
                            "    struct point c;\n"
                            "    c = b;\n"
                            "    return c.y;\n"
                            "}\n"),
              7);
}

TEST(const, const_pointer_to_scalar_global)
{
    EXPECT_INTERP_AND_ELF("int g = 100;\n"
                          "int main(void) {\n"
                          "    const int *p = &g;\n"
                          "    return *p - 50;\n"
                          "}\n",
                          50);
}

TEST(const, const_pointer_to_const_global)
{
    EXPECT_INTERP_AND_ELF("const int g = 8;\n"
                          "const int * const p = &g;\n"
                          "int main(void) {\n"
                          "    return *p;\n"
                          "}\n",
                          8);
}

TEST(const, add_qualifier_is_allowed)
{
    EXPECT_EQ(tc_run_interp("int g = 7;\n"
                            "void take(const int *p) {\n"
                            "    g = g + *p;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    int *q = &g;\n"
                            "    take(q);\n"
                            "    return g;\n"
                            "}\n"),
              14);
}

TEST(const, const_pointer_write_through_ok)
{
    EXPECT_INTERP_AND_ELF("int g = 5;\n"
                          "int main(void) {\n"
                          "    int * const cp = &g;\n"
                          "    *cp = 9;\n"
                          "    return g;\n"
                          "}\n",
                          9);
}

TEST(const, const_char_ptr_from_string_literal)
{
    EXPECT_EQ(tc_run_interp("const char *s = \"hey\";\n"
                            "int main(void) {\n"
                            "    return s[2];\n"
                            "}\n"),
              'y');
}

TEST(const, const_param_qualifiers_ignored_not_prototypes)
{
    EXPECT_EQ(tc_run_interp("int f(const int x) {\n"
                            "    return x * 2;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f(21);\n"
                            "}\n"),
              42);
}

TEST(const, const_global_in_rodata)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module("const int g = 42;\n"
                                    "int main(void) {\n"
                                    "    return g;\n"
                                    "}\n",
                                    arena);
    EXPECT_NOTNULL(mod);
    EXPECT_EQ(vec_size(mod->globals), 1);
    IrGlobal *g = (IrGlobal *) vec_get(mod->globals, 0);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);
    arena_free(arena);
}

TEST(const, const_zero_global_in_rodata_not_bss)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module("const int z;\n"
                                    "int main(void) {\n"
                                    "    return z;\n"
                                    "}\n",
                                    arena);
    EXPECT_NOTNULL(mod);
    IrGlobal *g = (IrGlobal *) vec_get(mod->globals, 0);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);
    EXPECT_NOTNULL(g->init_data);
    EXPECT_EQ(g->init_len, (size_t) 4);
    arena_free(arena);
}

TEST(const, const_global_elf_rodata_no_write_flag)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module("const int g = 42;\n"
                                    "int main(void) {\n"
                                    "    return g;\n"
                                    "}\n",
                                    arena);
    EXPECT_NOTNULL(mod);
    if (!mod)
    {
        arena_free(arena);
        return;
    }
    CodegenConfig cg_cfg = {0};
    CodegenModule *cm = codegen_ir_to_machine(mod, &cg_cfg, arena);
    EXPECT_NOTNULL(cm);
    if (!cm)
    {
        arena_free(arena);
        return;
    }
    elf_write(cm, "/tmp/ficc_p9_rd.o", NULL);
    int rc = tc_run_shell("objdump -h /tmp/ficc_p9_rd.o | grep -q '.rodata' && "
                          "! objdump -h /tmp/ficc_p9_rd.o | grep '.rodata' | grep -q WRITE");
    EXPECT_EQ(rc, 0);
    arena_free(arena);
    unlink("/tmp/ficc_p9_rd.o");
}

TEST(const, negative_assign_const_var)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const int x = 5;\n"
                      "    x = 6;\n"
                      "    return x;\n"
                      "}\n");
}

TEST(const, negative_assign_via_const_ptr)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "int main(void) {\n"
                      "    const int *p = &g;\n"
                      "    *p = 6;\n"
                      "    return g;\n"
                      "}\n");
}

TEST(const, negative_compound_assign_via_const_ptr)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "int main(void) {\n"
                      "    const int *p = &g;\n"
                      "    *p += 1;\n"
                      "    return g;\n"
                      "}\n");
}

TEST(const, negative_write_const_array_elem)
{
    EXPECT_BUILD_FAIL("const int a[3];\n"
                      "int main(void) {\n"
                      "    a[1] = 2;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_write_const_struct_member)
{
    EXPECT_BUILD_FAIL("struct point { int x; int y; };\n"
                      "int set(struct point const *p) {\n"
                      "    p->x = 9;\n"
                      "    return 0;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    struct point pt;\n"
                      "    return set(&pt);\n"
                      "}\n");
}

TEST(const, negative_write_const_member_dot)
{
    EXPECT_BUILD_FAIL("struct point { int x; int y; };\n"
                      "int main(void) {\n"
                      "    const struct point pt;\n"
                      "    pt.x = 1;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_write_const_field_of_const_struct)
{
    EXPECT_BUILD_FAIL("struct box { const int w; };\n"
                      "int main(void) {\n"
                      "    struct box b;\n"
                      "    b.w = 3;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_rebind_const_pointer)
{
    EXPECT_BUILD_FAIL("int g;\n"
                      "int main(void) {\n"
                      "    int * const cp = &g;\n"
                      "    cp = &g;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_write_const_local_param)
{
    EXPECT_BUILD_FAIL("void f(const int x) {\n"
                      "    x = 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    f(0);\n"
                      "    return 0;\n"
                      "}\n");
}

/* C11 §6.5.16.1: the left pointer type must carry every qualifier of the right. */
TEST(const, negative_discard_in_initializer)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "int main(void) {\n"
                      "    const int *p = &g;\n"
                      "    int *q = p;\n"
                      "    return *q;\n"
                      "}\n");
}

TEST(const, negative_discard_in_assignment)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "int main(void) {\n"
                      "    const int *p = &g;\n"
                      "    int *q;\n"
                      "    q = p;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_discard_in_argument)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "void f(int *p) {\n"
                      "    (void) p;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    const int *q = &g;\n"
                      "    f(q);\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_discard_in_return)
{
    EXPECT_BUILD_FAIL("int g = 5;\n"
                      "int *f(void) {\n"
                      "    const int *p = &g;\n"
                      "    return p;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_multi_level_discard)
{
    EXPECT_BUILD_FAIL("int g;\n"
                      "int *gp;\n"
                      "int main(void) {\n"
                      "    int **pp = &gp;\n"
                      "    const int **cpp = pp;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_int_ptr_to_char_ptr)
{
    EXPECT_BUILD_FAIL("int g;\n"
                      "char *cp;\n"
                      "int main(void) {\n"
                      "    cp = &g;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_conflicting_qualifiers_file_scope)
{
    EXPECT_BUILD_FAIL("int x;\n"
                      "const int x;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_consistency_ok_same_qualifiers)
{
    EXPECT_BUILD_SUCCEED("const int x = 5;\n"
                         "const int x;\n"
                         "int main(void) {\n"
                         "    return x;\n"
                         "}\n");
}

TEST(const, addr_of_scalar_write_through)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 3;\n"
                          "    int *p = &x;\n"
                          "    *p = 7;\n"
                          "    return x;\n"
                          "}\n",
                          7);
}

TEST(const, addr_of_const_scalar)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int x = 41;\n"
                          "    const int *p = &x;\n"
                          "    return *p + 1;\n"
                          "}\n",
                          42);
}

TEST(const, addr_of_in_loop)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 0;\n"
                          "    int s = 0;\n"
                          "    int *p = &i;\n"
                          "    while (i < 4) {\n"
                          "        i = i + 1;\n"
                          "        s = s + i;\n"
                          "    }\n"
                          "    return s + i;\n"
                          "}\n",
                          14);
}

TEST(const, addr_of_param)
{
    EXPECT_INTERP_AND_ELF("void bump(int *p) {\n"
                          "    *p = *p + 1;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int v = 9;\n"
                          "    bump(&v);\n"
                          "    return v;\n"
                          "}\n",
                          10);
}

TEST(const, addr_of_pointer_to_pointer)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 2;\n"
                          "    int *p = &x;\n"
                          "    int **q = &p;\n"
                          "    **q = 6;\n"
                          "    return x;\n"
                          "}\n",
                          6);
}

TEST(const, addr_of_nested_block)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r = 0;\n"
                          "    {\n"
                          "        int y = 3;\n"
                          "        int *p = &y;\n"
                          "        *p = 9;\n"
                          "        r = y;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          9);
}

TEST(const, addr_of_deref_of_addr)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1;\n"
                          "    *&x = 5;\n"
                          "    return x;\n"
                          "}\n",
                          5);
}

TEST(const, addr_of_struct_const_ptr_from_local)
{
    EXPECT_INTERP_AND_ELF("struct point { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct point pt;\n"
                          "    pt.x = 20;\n"
                          "    pt.y = 22;\n"
                          "    const struct point *p = &pt;\n"
                          "    return p->x + p->y;\n"
                          "}\n",
                          42);
}

TEST(const, addr_of_ifelse_through_pointer)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int v = 0;\n"
                          "    int *p = &v;\n"
                          "    if (1) {\n"
                          "        *p = 11;\n"
                          "    } else {\n"
                          "        *p = 22;\n"
                          "    }\n"
                          "    return v;\n"
                          "}\n",
                          11);
}

TEST(const, negative_addr_of_const_discard)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const int x = 5;\n"
                      "    int *p = &x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, negative_addr_of_const_write_through)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const int x = 5;\n"
                      "    const int *p = &x;\n"
                      "    *p = 6;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(const, addr_of_spills_to_alloca)
{
    /* The spill slot alloca must land in the function's entry block. */
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module("int main(void) {\n"
                                    "    int x = 3;\n"
                                    "    int *p = &x;\n"
                                    "    return *p;\n"
                                    "}\n",
                                    arena);
    EXPECT_NOTNULL(mod);
    IrFunction *f = (IrFunction *) vec_get(mod->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    bool found_alloca = false;
    size_t n = vec_size(entry->instrs);
    for (size_t i = 0; i < n; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(entry->instrs, i);
        if (in->opcode == OP_ALLOCA)
        {
            found_alloca = true;
        }
    }
    EXPECT_TRUE(found_alloca);
    arena_free(arena);
}

TEST(const, narrow_unsigned_char_load_no_sext)
{
    EXPECT_INTERP_AND_ELF("unsigned char g;\n"
                          "int main(void) {\n"
                          "    g = 200;\n"
                          "    return g == 200 ? 42 : 0;\n"
                          "}\n",
                          42);
}

TEST(const, narrow_unsigned_short_memory_exact)
{
    EXPECT_INTERP_AND_ELF("unsigned short g;\n"
                          "int main(void) {\n"
                          "    unsigned short *p = &g;\n"
                          "    *p = 65535;\n"
                          "    return *p == 65535 ? 42 : 0;\n"
                          "}\n",
                          42);
}

TEST(const, narrow_signed_short_return)
{
    EXPECT_INTERP_AND_ELF("short g;\n"
                          "int main(void) {\n"
                          "    g = -7;\n"
                          "    short *p = &g;\n"
                          "    return *p == -7 ? 42 : 0;\n"
                          "}\n",
                          42);
}

TEST(const, narrow_unsigned_member_load)
{
    EXPECT_INTERP_AND_ELF("struct P { unsigned char x; unsigned short y; };\n"
                          "struct P pt;\n"
                          "int main(void) {\n"
                          "    pt.x = 200;\n"
                          "    pt.y = 65535;\n"
                          "    return (pt.x == 200 && pt.y == 65535) ? 42 : 0;\n"
                          "}\n",
                          42);
}

TEST(const, narrow_unsigned_param_preserves_value)
{
    EXPECT_INTERP_AND_ELF("int f(unsigned char a) {\n"
                          "    return a == 200 ? 42 : 0;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(200);\n"
                          "}\n",
                          42);
}

TEST(const, narrow_signed_load_in_memory_vars)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = -7;\n"
                          "    char *cp = &c;\n"
                          "    short s;\n"
                          "    short *sp = &s;\n"
                          "    *sp = -1234;\n"
                          "    return (*cp == -7 && *sp == -1234) ? 42 : 0;\n"
                          "}\n",
                          42);
}