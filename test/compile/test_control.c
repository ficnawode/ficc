#include "harness.h"
#include "testdriver.h"

static const char *if_then_src = "int main(void) {\n"
                                 "    int x = 5;\n"
                                 "    if (x) {\n"
                                 "        x = 10;\n"
                                 "    } else {\n"
                                 "        x = 20;\n"
                                 "    }\n"
                                 "    return x;\n"
                                 "}\n";

TEST(control, interp_if_then)
{
    EXPECT_EQ(tc_run_interp(if_then_src), 10);
}

TEST(control, elf_if_then)
{
    EXPECT_EQ(tc_run_elf(if_then_src), 10);
}

static const char *if_else_src = "int main(void) {\n"
                                 "    int x = 0;\n"
                                 "    if (x) {\n"
                                 "        x = 10;\n"
                                 "    } else {\n"
                                 "        x = 20;\n"
                                 "    }\n"
                                 "    return x;\n"
                                 "}\n";

TEST(control, interp_if_else)
{
    EXPECT_EQ(tc_run_interp(if_else_src), 20);
}

TEST(control, elf_if_else)
{
    EXPECT_EQ(tc_run_elf(if_else_src), 20);
}

static const char *if_without_else_src = "int main(void) {\n"
                                         "    int x = 0;\n"
                                         "    if (1) x = 10;\n"
                                         "    if (0) x = 99;\n"
                                         "    return x;\n"
                                         "}\n";

TEST(control, interp_if_without_else)
{
    EXPECT_EQ(tc_run_interp(if_without_else_src), 10);
}

TEST(control, elf_if_without_else)
{
    EXPECT_EQ(tc_run_elf(if_without_else_src), 10);
}

static const char *dangling_else_src = "int main(void) {\n"
                                       "    int x = 0;\n"
                                       "    if (1) if (0) x = 1; else x = 2;\n"
                                       "    return x;\n"
                                       "}\n";

TEST(control, interp_dangling_else)
{
    EXPECT_EQ(tc_run_interp(dangling_else_src), 2);
}

TEST(control, elf_dangling_else)
{
    EXPECT_EQ(tc_run_elf(dangling_else_src), 2);
}