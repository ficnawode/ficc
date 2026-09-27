#include "harness.h"
#include "testdriver.h"

static const char *while_src = "int main(void) {\n"
                               "    int s = 0;\n"
                               "    int i = 0;\n"
                               "    while (i < 5) {\n"
                               "        s = s + i;\n"
                               "        i = i + 1;\n"
                               "    }\n"
                               "    return s;\n"
                               "}\n";

TEST(loops, interp_while)
{
    EXPECT_EQ(tc_run_interp(while_src), 10);
}

TEST(loops, elf_while)
{
    EXPECT_EQ(tc_run_elf(while_src), 10);
}

TEST(loops, while_false_skips_body)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    while (0) {\n"
                          "        x = 1;\n"
                          "    }\n"
                          "    if (x != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

static const char *for_src = "int main(void) {\n"
                             "    int s = 0;\n"
                             "    int i;\n"
                             "    for (i = 0; i < 5; i = i + 1) {\n"
                             "        s = s + i;\n"
                             "    }\n"
                             "    return s;\n"
                             "}\n";

TEST(loops, interp_for)
{
    EXPECT_EQ(tc_run_interp(for_src), 10);
}

TEST(loops, elf_for)
{
    EXPECT_EQ(tc_run_elf(for_src), 10);
}

TEST(loops, for_false_skips_body)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    for (int i = 7; i < 3; i++) {\n"
                          "        x = 1;\n"
                          "    }\n"
                          "    if (x != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(loops, for_continue_runs_post)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int sum = 0;\n"
                          "    for (int i = 0; i < 6; i++) {\n"
                          "        if (i == 2) continue;\n"
                          "        if (i == 5) continue;\n"
                          "        sum = sum + i;\n"
                          "    }\n"
                          "    if (sum != 8) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(loops, nested_break_continue_bind_inner)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int count = 0;\n"
                          "    for (int i = 0; i < 3; i++) {\n"
                          "        for (int j = 0; j < 3; j++) {\n"
                          "            if (j == 1) continue;\n"
                          "            if (i == 2) break;\n"
                          "            count++;\n"
                          "        }\n"
                          "    }\n"
                          "    if (count != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(loops, switch_break_continue_in_loop)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int sum = 0;\n"
                          "    for (int i = 0; i < 5; i++) {\n"
                          "        switch (i) {\n"
                          "        case 2: continue;\n"
                          "        case 3: break;\n"
                          "        default: sum = sum + i;\n"
                          "        }\n"
                          "    }\n"
                          "    if (sum != 5) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

static const char *do_while_src = "int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    do {\n"
                                  "        s = s + i;\n"
                                  "        i = i + 1;\n"
                                  "    } while (i < 5);\n"
                                  "    return s;\n"
                                  "}\n";

TEST(loops, interp_do_while)
{
    EXPECT_EQ(tc_run_interp(do_while_src), 10);
}

TEST(loops, elf_do_while)
{
    EXPECT_EQ(tc_run_elf(do_while_src), 10);
}

static const char *break_src = "int main(void) {\n"
                               "    int s = 0;\n"
                               "    int i = 0;\n"
                               "    while (i < 10) {\n"
                               "        if (i >= 5) {\n"
                               "            break;\n"
                               "        }\n"
                               "        s = s + i;\n"
                               "        i = i + 1;\n"
                               "    }\n"
                               "    return s;\n"
                               "}\n";

TEST(loops, interp_break)
{
    EXPECT_EQ(tc_run_interp(break_src), 10);
}

TEST(loops, elf_break)
{
    EXPECT_EQ(tc_run_elf(break_src), 10);
}

static const char *continue_src = "int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    while (i < 5) {\n"
                                  "        if (i == 2) {\n"
                                  "            i = i + 1;\n"
                                  "            continue;\n"
                                  "        }\n"
                                  "        s = s + i;\n"
                                  "        i = i + 1;\n"
                                  "    }\n"
                                  "    return s;\n"
                                  "}\n";

TEST(loops, interp_continue)
{
    EXPECT_EQ(tc_run_interp(continue_src), 8);
}

TEST(loops, elf_continue)
{
    EXPECT_EQ(tc_run_elf(continue_src), 8);
}

static const char *goto_src = "int main(void) {\n"
                              "    int x = 0;\n"
                              "    goto skip;\n"
                              "    x = 10;\n"
                              "skip:\n"
                              "    x = x + 5;\n"
                              "    return x;\n"
                              "}\n";

TEST(loops, interp_goto)
{
    EXPECT_EQ(tc_run_interp(goto_src), 5);
}

TEST(loops, elf_goto)
{
    EXPECT_EQ(tc_run_elf(goto_src), 5);
}

static const char *goto_backward_src = "int main(void) {\n"
                                       "    int x = 0;\n"
                                       "loop:\n"
                                       "    x = x + 1;\n"
                                       "    if (x < 5) {\n"
                                       "        goto loop;\n"
                                       "    }\n"
                                       "    return x;\n"
                                       "}\n";

TEST(loops, interp_goto_backward)
{
    EXPECT_EQ(tc_run_interp(goto_backward_src), 5);
}

TEST(loops, elf_goto_backward)
{
    EXPECT_EQ(tc_run_elf(goto_backward_src), 5);
}

static const char *goto_out_of_loop_src = "int main(void) {\n"
                                          "    int i = 0;\n"
                                          "    while (i < 10) {\n"
                                          "        if (i == 4) {\n"
                                          "            goto done;\n"
                                          "        }\n"
                                          "        i = i + 1;\n"
                                          "    }\n"
                                          "    i = 99;\n"
                                          "done:\n"
                                          "    return i;\n"
                                          "}\n";

TEST(loops, interp_goto_out_of_loop)
{
    EXPECT_EQ(tc_run_interp(goto_out_of_loop_src), 4);
}

TEST(loops, elf_goto_out_of_loop)
{
    EXPECT_EQ(tc_run_elf(goto_out_of_loop_src), 4);
}

static const char *for_empty_init_src = "int main(void) {\n"
                                        "    int c = 0;\n"
                                        "    for (; c < 3; c = c + 1) {\n"
                                        "        c = c;\n"
                                        "    }\n"
                                        "    return c;\n"
                                        "}\n";

TEST(loops, interp_for_empty_init)
{
    EXPECT_EQ(tc_run_interp(for_empty_init_src), 3);
}

TEST(loops, elf_for_empty_init)
{
    EXPECT_EQ(tc_run_elf(for_empty_init_src), 3);
}

TEST(loops, interp_for_no_clauses)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int i = 0;\n"
                            "    for (;;) {\n"
                            "        i = i + 1;\n"
                            "        if (i >= 5) break;\n"
                            "    }\n"
                            "    return i;\n"
                            "}\n"),
              5);
}

TEST(loops, interp_do_while_continue)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int i = 0;\n"
                            "    int s = 0;\n"
                            "    do {\n"
                            "        s = s + 1;\n"
                            "        if (i == 2) {\n"
                            "            i = i + 1;\n"
                            "            continue;\n"
                            "        }\n"
                            "        i = i + 1;\n"
                            "    } while (i < 5);\n"
                            "    return s;\n"
                            "}\n"),
              5);
}

TEST(loops, interp_var_assigned_in_one_branch)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int x = 0;\n"
                            "    int i = 0;\n"
                            "    while (i < 3) {\n"
                            "        if (i == 1) {\n"
                            "            x = 42;\n"
                            "        }\n"
                            "        i = i + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"),
              42);
}

TEST(loops, semantic_break_outside_loop)
{
    EXPECT_BUILD_FAIL("int main(void) { break; return 0; }");
}

TEST(loops, semantic_continue_outside_loop)
{
    EXPECT_BUILD_FAIL("int main(void) { continue; return 0; }");
}

TEST(loops, semantic_goto_undefined_label)
{
    EXPECT_BUILD_FAIL("int main(void) { goto nope; return 0; }");
}

TEST(loops, semantic_duplicate_label)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    goto a;\n"
                      "a:\n"
                      "    goto a;\n"
                      "a:\n"
                      "    return 0;\n"
                      "}\n");
}

static const char *short_circuit_guard_src =
    "int main(void) {\n"
    "    unsigned long align = 8;\n"
    "    unsigned long used = 130;\n"
    "    unsigned long mask, pos;\n"
    "    do { if (!(align >= 1 && align <= 16)) { return 1; } } while (0);\n"
    "    do { if (align < 1 || align > 16) { return 3; } } while (0);\n"
    "    mask = align - 1;\n"
    "    pos = (used + mask) & ~mask;\n"
    "    if (pos != 136) { return 2; }\n"
    "    return 0;\n"
    "}\n";

TEST(loops, short_circuit_guard)
{
    EXPECT_INTERP_AND_ELF(short_circuit_guard_src, 0);
}

static const char *phi_width_src = "#include <stdint.h>\n"
                                   "int main(void) {\n"
                                   "    unsigned long first = SIZE_MAX;\n"
                                   "    unsigned long i;\n"
                                   "    for (i = 0; i < 5; i++) {\n"
                                   "        if (i == 2) { first = i; }\n"
                                   "        if (i < 2) { if (first != SIZE_MAX) { return 1; } }\n"
                                   "    }\n"
                                   "    if (first != 2) { return 2; }\n"
                                   "    return 0;\n"
                                   "}\n";

TEST(loops, phi_sized_sentinel)
{
    EXPECT_INTERP_AND_ELF(phi_width_src, 0);
}

static const char *phi_parallel_cross_src = "int main(void) {\n"
                                            "    int i, j, sum = 0;\n"
                                            "    for (i = 0, j = 1; i < 3; i = j, j = i + 1)\n"
                                            "        sum = sum * 10 + i;\n"
                                            "    return sum;\n"
                                            "}\n";

TEST(loops, phi_parallel_cross_assignment)
{
    EXPECT_INTERP_AND_ELF(phi_parallel_cross_src, 12);
}

static const char *phi_parallel_list_src =
    "struct node { int v; struct node *next; };\n"
    "int main(void) {\n"
    "    struct node n3 = {3, 0}, n2 = {2, &n3}, n1 = {1, &n2};\n"
    "    int sum = 0;\n"
    "    struct node *pos = &n1, *tmp = n1.next;\n"
    "    for (; pos; pos = tmp, tmp = pos ? pos->next : 0)\n"
    "        sum = sum * 10 + pos->v;\n"
    "    return sum;\n"
    "}\n";

TEST(loops, phi_parallel_list_walk)
{
    EXPECT_INTERP_AND_ELF(phi_parallel_list_src, 123);
}
