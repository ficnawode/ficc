#include "harness.h"
#include "testdriver.h"

/* Loop conditions with side effects on loop variables (`while (i--)`, `do
   { } while (--n > 0)`) and short-circuited conditions that assign inside
   the `&&`/`||` RHS (`while (len > 0 && (p = f()) != NULL)`). The body's
   reads must resolve to the post-condition value, and phi copies on the back
   edge must observe the update. */

TEST(loop_cond, while_post_decrement_cond)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 5;\n"
                          "    int acc = 0;\n"
                          "    while (i--)\n"
                          "        acc += i;\n"
                          "    return acc == 10 && i == -1 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(loop_cond, do_while_pre_decrement_cond)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int n = 4;\n"
                          "    int acc = 0;\n"
                          "    do\n"
                          "        acc += n;\n"
                          "    while (--n > 0);\n"
                          "    return acc == 10 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(loop_cond, short_circuit_cond_assigns)
{
    EXPECT_INTERP_AND_ELF("int g;\n"
                          "int next(void) { g++; return g <= 3 ? g : 0; }\n"
                          "int main(void) {\n"
                          "    int x = 0;\n"
                          "    while (g < 3 && (x = next()) != 0)\n"
                          "        ;\n"
                          "    return x == 3 && g == 3 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(loop_cond, short_circuit_cond_loop_body_uses_var)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int lim = 3;\n"
                          "    int n = 0;\n"
                          "    int acc = 0;\n"
                          "    while (n < lim && (n += 1) && n <= lim)\n"
                          "        acc += n;\n"
                          "    return acc == 6 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(loop_cond, side_effect_in_cond_then_body_break)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int n = 10;\n"
                          "    int count = 0;\n"
                          "    while (n > 0) {\n"
                          "        n--;\n"
                          "        count++;\n"
                          "        if (count == 6) break;\n"
                          "    }\n"
                          "    return n == 4 && count == 6 ? 0 : 1;\n"
                          "}\n",
                          0);
}