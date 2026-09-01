#include "harness.h"
#include "testdriver.h"

static const char *data_bss_src = "int counter = 5;\n"
                                  "static int acc;\n"
                                  "int main(void) {\n"
                                  "    counter = counter + 2;\n"
                                  "    acc = acc + counter + 1;\n"
                                  "    return acc;\n"
                                  "}\n";

TEST(globals, interp_data_bss)
{
    EXPECT_EQ(tc_run_interp(data_bss_src), 8);
}

TEST(globals, interp_accumulate_across_calls)
{
    EXPECT_EQ(tc_run_interp("static int total;\n"
                            "int add(int v) {\n"
                            "    total = total + v;\n"
                            "    return total;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    add(3);\n"
                            "    add(4);\n"
                            "    return add(5);\n"
                            "}\n"),
              12);
}

TEST(globals, interp_record_array_globals)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "struct Pt origin;\n"
                            "int arr[3];\n"
                            "int main(void) {\n"
                            "    origin.x = 4;\n"
                            "    origin.y = 5;\n"
                            "    arr[1] = 7;\n"
                            "    return origin.x + origin.y + arr[1];\n"
                            "}\n"),
              16);
}

TEST(globals, interp_global_enum)
{
    EXPECT_EQ(tc_run_interp("enum Color { RED, GREEN, BLUE };\n"
                            "enum Color state;\n"
                            "int main(void) {\n"
                            "    state = BLUE;\n"
                            "    return state + RED;\n"
                            "}\n"),
              2);
}

TEST(globals, interp_addr_of_scalar_global)
{
    EXPECT_EQ(tc_run_interp("int g = 9;\n"
                            "int *p;\n"
                            "int main(void) {\n"
                            "    p = &g;\n"
                            "    return *p;\n"
                            "}\n"),
              9);
}

TEST(globals, interp_pointer_global_string)
{
    EXPECT_EQ(tc_run_interp("char *msg = \"hello\";\n"
                            "int main(void) {\n"
                            "    return msg[1] + msg[4] + msg[0];\n"
                            "}\n"),
              'e' + 'o' + 'h');
}

TEST(globals, interp_extern_is_zero)
{
    EXPECT_EQ(tc_run_interp("extern int e;\n"
                            "int main(void) {\n"
                            "    return e;\n"
                            "}\n"),
              0);
}

TEST(globals, interp_tentative_defs_merge)
{
    EXPECT_EQ(tc_run_interp("int x;\n"
                            "int x;\n"
                            "int main(void) {\n"
                            "    x = 9;\n"
                            "    return x;\n"
                            "}\n"),
              9);
}

TEST(globals, interp_zero_init)
{
    EXPECT_EQ(tc_run_interp("int z;\n"
                            "long l;\n"
                            "char c;\n"
                            "int main(void) {\n"
                            "    l = 1;\n"
                            "    if (z != 0) {\n"
                            "        return 1;\n"
                            "    }\n"
                            "    if (c != 0) {\n"
                            "        return 2;\n"
                            "    }\n"
                            "    if (l != 1) {\n"
                            "        return 3;\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n"),
              0);
}

TEST(globals, elf_data_bss)
{
    EXPECT_EQ(tc_run_elf(data_bss_src), 8);
}

TEST(globals, elf_pointer_global_string)
{
    /* exit code truncates to the low byte: 'e'+'o'+'h' = 316 & 0xFF = 60 */
    EXPECT_EQ(tc_run_elf("char *msg = \"hello\";\n"
                         "int main(void) {\n"
                         "    return msg[1] + msg[4] + msg[0];\n"
                         "}\n"),
              60);
}

TEST(globals, elf_extern_two_tu)
{
    /* A ficc-compiled unit can reference extern symbols that are defined in a
       second, host-compiled translation unit. */
    EXPECT_EQ(tc_run_elf_with_extra_tu("extern int shared;\n"
                                       "extern int other_ext;\n"
                                       "int main(void) {\n"
                                       "    shared = shared + 2;\n"
                                       "    return shared + other_ext;\n"
                                       "}\n",
                                       "int shared = 40;\n"
                                       "int other_ext = 5;\n"),
              47);
}

TEST(globals, negative_global_nonconst_init)
{
    EXPECT_BUILD_FAIL("int v = 1 + 2 + main;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_global_redefine)
{
    EXPECT_BUILD_FAIL("int x = 1;\n"
                      "int x = 2;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

static const char *block_static_src = "int counter(void) {\n"
                                      "    static int n = 5;\n"
                                      "    n = n + 1;\n"
                                      "    return n;\n"
                                      "}\n"
                                      "int main(void) {\n"
                                      "    return counter() + counter();\n"
                                      "}\n";

TEST(globals, interp_block_static)
{
    EXPECT_EQ(tc_run_interp(block_static_src), 13);
}

TEST(globals, elf_block_static)
{
    EXPECT_EQ(tc_run_elf(block_static_src), 13);
}

TEST(globals, interp_block_static_distinct)
{
    EXPECT_EQ(tc_run_interp("int f(void) {\n"
                            "    static int n;\n"
                            "    n = n + 2;\n"
                            "    return n;\n"
                            "}\n"
                            "int g(void) {\n"
                            "    static int n;\n"
                            "    n = n + 3;\n"
                            "    return n;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f() + f() + g();\n"
                            "}\n"),
              9);
}

TEST(globals, local_shadows_global)
{
    EXPECT_EQ(tc_run_interp("int counter = 100;\n"
                            "int main(void) {\n"
                            "    int counter = 7;\n"
                            "    counter = counter + 1;\n"
                            "    return counter;\n"
                            "}\n"),
              8);
}

static const char *static_function_src = "static int helper(int x) {\n"
                                         "    return x * 2;\n"
                                         "}\n"
                                         "int visible(int x) {\n"
                                         "    return helper(x) + 1;\n"
                                         "}\n"
                                         "int main(void) {\n"
                                         "    return visible(5);\n"
                                         "}\n";

TEST(globals, interp_static_function)
{
    EXPECT_EQ(tc_run_interp(static_function_src), 11);
}

TEST(globals, elf_static_function_binding)
{
    EXPECT_EQ(tc_run_elf(static_function_src), 11);
}

TEST(globals, extern_with_init_is_definition)
{
    EXPECT_INTERP_AND_ELF("extern int x = 5;\n"
                          "int main(void) {\n"
                          "    return x;\n"
                          "}\n",
                          5);
}

TEST(globals, negative_linkage_mixing)
{
    EXPECT_BUILD_FAIL("int x;\n"
                      "static int x;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
    EXPECT_BUILD_FAIL("static int f(void) {\n"
                      "    return 0;\n"
                      "}\n"
                      "int f(void) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_static_nonconst_init)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    static int x = 1 + main;\n"
                      "    return x;\n"
                      "}\n");
}

TEST(globals, negative_aggregate_init)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "struct Pt p = 5;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_string_init_non_char_ptr)
{
    EXPECT_BUILD_FAIL("int *p = \"hi\";\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_incomplete_global)
{
    EXPECT_BUILD_FAIL("struct S;\n"
                      "struct S s;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, interp_block_extern)
{
    EXPECT_EQ(tc_run_interp("int shared = 10;\n"
                            "int main(void) {\n"
                            "    extern int shared;\n"
                            "    shared = shared + 5;\n"
                            "    return shared;\n"
                            "}\n"),
              15);
}

TEST(globals, elf_block_extern_shared)
{
    EXPECT_EQ(tc_run_elf("int n;\n"
                         "int a(void) {\n"
                         "    extern int n;\n"
                         "    n = n + 1;\n"
                         "    return n;\n"
                         "}\n"
                         "int b(void) {\n"
                         "    extern int n;\n"
                         "    n = n + 2;\n"
                         "    return n;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return a() + a() + b();\n"
                         "}\n"),
              7);
}

TEST(globals, negative_block_extern_with_init)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    extern int q = 5;\n"
                      "    return q;\n"
                      "}\n");
}

static const char *nested_shadow_src = "int main(void) {\n"
                                       "    int x = 1;\n"
                                       "    {\n"
                                       "        int x = 2;\n"
                                       "        x = x + 1;\n"
                                       "    }\n"
                                       "    return x;\n"
                                       "}\n";

TEST(globals, interp_nested_shadow)
{
    EXPECT_EQ(tc_run_interp(nested_shadow_src), 1);
}

TEST(globals, interp_shadow_param_nested)
{
    EXPECT_EQ(tc_run_interp("int f(int x) {\n"
                            "    {\n"
                            "        int x = 5;\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f(10);\n"
                            "}\n"),
              10);
}

TEST(globals, interp_shadow_across_branches)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int x = 1;\n"
                            "    if (x) {\n"
                            "        int x = 2;\n"
                            "        x = x + 1;\n"
                            "    } else {\n"
                            "        int x = 3;\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"),
              1);
}

TEST(globals, interp_shadow_different_type)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int x = 10;\n"
                            "    {\n"
                            "        char *x = \"ab\";\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"),
              10);
}

static const char *shadow_in_loop_src = "int main(void) {\n"
                                        "    int total = 0;\n"
                                        "    int i = 0;\n"
                                        "    while (i < 5) {\n"
                                        "        int total = 7;\n"
                                        "        total = total + 1;\n"
                                        "        i = i + 1;\n"
                                        "    }\n"
                                        "    return total;\n"
                                        "}\n";

TEST(globals, interp_shadow_in_loop)
{
    EXPECT_EQ(tc_run_interp(shadow_in_loop_src), 0);
}

TEST(globals, interp_local_shadows_block_static)
{
    EXPECT_EQ(tc_run_interp("int counter(void) {\n"
                            "    static int n = 5;\n"
                            "    {\n"
                            "        int n = 50;\n"
                            "        n = n + 1;\n"
                            "    }\n"
                            "    n = n + 1;\n"
                            "    return n;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return counter() + counter();\n"
                            "}\n"),
              13);
}

TEST(globals, elf_nested_shadow)
{
    EXPECT_EQ(tc_run_elf(nested_shadow_src), 1);
}

TEST(globals, elf_shadow_in_loop)
{
    EXPECT_EQ(tc_run_elf(shadow_in_loop_src), 0);
}
