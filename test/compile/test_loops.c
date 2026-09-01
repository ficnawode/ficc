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

static const char *break_continue_src = "int main(void) {\n"
                                        "    int s = 0;\n"
                                        "    int i = 0;\n"
                                        "    while (i < 10) {\n"
                                        "        if (i >= 5) {\n"
                                        "            break;\n"
                                        "        }\n"
                                        "        if (i == 2) {\n"
                                        "            i = i + 1;\n"
                                        "            continue;\n"
                                        "        }\n"
                                        "        s = s + i;\n"
                                        "        i = i + 1;\n"
                                        "    }\n"
                                        "    return s;\n"
                                        "}\n";

TEST(loops, interp_break_continue)
{
    /* 0 + 1 + 3 + 4 = 8 (skip 2 because of continue) */
    EXPECT_EQ(tc_run_interp(break_continue_src), 8);
}

TEST(loops, elf_break_continue)
{
    EXPECT_EQ(tc_run_elf(break_continue_src), 8);
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
