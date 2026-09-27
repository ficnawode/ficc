#include "harness.h"
#include "testdriver.h"

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

TEST(loop_cond, for_post_decrement_cond)
{
    EXPECT_INTERP_AND_ELF("static unsigned char data[8];\n"
                          "int main(void) {\n"
                          "    int n = 8;\n"
                          "    int k;\n"
                          "    for (k = n; k-- > 1;)\n"
                          "        data[k] = 1;\n"
                          "    int ok = 1;\n"
                          "    for (int i = 0; i < 8; i = i + 1)\n"
                          "        if (i >= 1 && i <= 7 && data[i] != 1)\n"
                          "            ok = 0;\n"
                          "    return ok ? 0 : 1;\n"
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

TEST(loop_cond, break_keeps_loop_var_from_latched_value)
{
    EXPECT_INTERP_AND_ELF("static unsigned char data[8];\n"
                          "int main(void) {\n"
                          "    int end = 8;\n"
                          "    int k;\n"
                          "    for (k = 0; k < end; k = k + 1)\n"
                          "        if (k == 4)\n"
                          "            break;\n"
                          "        else\n"
                          "            data[k] = 1;\n"
                          "    int ok = 1;\n"
                          "    for (int i = 0; i < 8; i = i + 1)\n"
                          "        if (i <= 3 && data[i] != 1)\n"
                          "            ok = 0;\n"
                          "    return ok ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(loop_cond, or_short_circuit_condition)
{
    EXPECT_INTERP_AND_ELF("int calls;\n"
                          "int rhs(void) { calls = calls + 1; return 5; }\n"
                          "int main(void) {\n"
                          "    int n = 0;\n"
                          "    int total = 0;\n"
                          "    while (n < 3 || rhs() < 4) {\n"
                          "        total = total + n;\n"
                          "        n = n + 1;\n"
                          "    }\n"
                          "    if (total != 3) return 1;\n"
                          "    if (calls != 1) return 2;\n"
                          "    if (n != 3) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(loop_cond, do_while_continue_rechecks_condition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int n = 0;\n"
                          "    int sum = 0;\n"
                          "    do {\n"
                          "        n++;\n"
                          "        if (n == 2) continue;\n"
                          "        sum = sum + n;\n"
                          "    } while (n < 4 && sum < 100);\n"
                          "    if (sum != 8) return 1;\n"
                          "    if (n != 4) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}
