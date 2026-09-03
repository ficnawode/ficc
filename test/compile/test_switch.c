#include "harness.h"
#include "testdriver.h"

TEST(switch, basic_match)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 2;\n"
                          "    int r;\n"
                          "    switch (x) {\n"
                          "    case 1:\n"
                          "        r = 10;\n"
                          "        break;\n"
                          "    case 2:\n"
                          "        r = 20;\n"
                          "        break;\n"
                          "    case 3:\n"
                          "        r = 30;\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 99;\n"
                          "        break;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          20);
}

TEST(switch, default_taken)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r;\n"
                          "    switch (42) {\n"
                          "    case 1:\n"
                          "        r = 10;\n"
                          "        break;\n"
                          "    case 2:\n"
                          "        r = 20;\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 7;\n"
                          "        break;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          7);
}

TEST(switch, no_default)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (7) {\n"
                          "    case 1:\n"
                          "        return 11;\n"
                          "    case 2:\n"
                          "        return 22;\n"
                          "    }\n"
                          "    return 99;\n"
                          "}\n",
                          99);
}

TEST(switch, fall_through)
{
    /* case 1 falls into case 2 (no break); x=2 skips case 1 entirely. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r = 0;\n"
                          "    int x = 2;\n"
                          "    switch (x) {\n"
                          "    case 1:\n"
                          "        r = 10;\n"
                          "    case 2:\n"
                          "        r = r + 5;\n"
                          "        break;\n"
                          "    case 3:\n"
                          "        r = 30;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          5);
}

TEST(switch, fall_through_all)
{
    /* A matched case with no break falls through every later label. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r = 0;\n"
                          "    switch (1) {\n"
                          "    case 1:\n"
                          "        r = r + 1;\n"
                          "    case 2:\n"
                          "        r = r + 2;\n"
                          "    case 3:\n"
                          "        r = r + 4;\n"
                          "    default:\n"
                          "        r = r + 8;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          15);
}

TEST(switch, default_in_middle)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (5) {\n"
                          "    case 1:\n"
                          "        return 11;\n"
                          "    default:\n"
                          "        return 77;\n"
                          "    case 2:\n"
                          "        return 22;\n"
                          "    }\n"
                          "}\n",
                          77);
}

TEST(switch, in_loop_with_continue)
{
    /* i=0 -> default s+=0; 1 -> +10; 2 -> +2; 3 -> continue; 4 -> +4 => 16 */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int s = 0;\n"
                          "    int i;\n"
                          "    for (i = 0; i < 5; i = i + 1) {\n"
                          "        switch (i) {\n"
                          "        case 1:\n"
                          "            s = s + 10;\n"
                          "            break;\n"
                          "        case 3:\n"
                          "            continue;\n"
                          "        default:\n"
                          "            s = s + i;\n"
                          "        }\n"
                          "    }\n"
                          "    return s;\n"
                          "}\n",
                          16);
}

TEST(switch, nested_switch)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r;\n"
                          "    switch (2) {\n"
                          "    case 1:\n"
                          "        r = 1;\n"
                          "        break;\n"
                          "    case 2:\n"
                          "        switch (1) {\n"
                          "        case 0:\n"
                          "            r = 20;\n"
                          "            break;\n"
                          "        case 1:\n"
                          "            r = 21;\n"
                          "            break;\n"
                          "        default:\n"
                          "            r = 22;\n"
                          "        }\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 99;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          21);
}

TEST(switch, enum_case_values)
{
    EXPECT_INTERP_AND_ELF("enum Color { RED, GREEN, BLUE };\n"
                          "int main(void) {\n"
                          "    switch (2) {\n"
                          "    case RED:\n"
                          "        return 1;\n"
                          "    case GREEN:\n"
                          "        return 2;\n"
                          "    case BLUE:\n"
                          "        return 3;\n"
                          "    default:\n"
                          "        return 4;\n"
                          "    }\n"
                          "}\n",
                          3);
}

TEST(switch, constant_expr_cases)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (10) {\n"
                          "    case 4 + 6:\n"
                          "        return 1;\n"
                          "    case 2 * 7:\n"
                          "        return 2;\n"
                          "    default:\n"
                          "        return 3;\n"
                          "    }\n"
                          "}\n",
                          1);
}

TEST(switch, assignment_after_switch)
{
    /* A variable written in every case and read after the switch needs a
       merge PHI. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r;\n"
                          "    switch (3) {\n"
                          "    case 1:\n"
                          "        r = 10;\n"
                          "        break;\n"
                          "    case 2:\n"
                          "        r = 20;\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 30;\n"
                          "    }\n"
                          "    return r;\n"
                          "}\n",
                          30);
}

TEST(switch, bare_single_case)
{
    /* Non-compound switch body. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (1) {\n"
                          "    case 1:\n"
                          "        return 5;\n"
                          "    }\n"
                          "}\n",
                          5);
}

TEST(switch, if_inside_case)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (2) {\n"
                          "    case 1:\n"
                          "        if (1) {\n"
                          "            return 10;\n"
                          "        }\n"
                          "    case 2:\n"
                          "        if (0) {\n"
                          "            return 20;\n"
                          "        }\n"
                          "        return 30;\n"
                          "    }\n"
                          "}\n",
                          30);
}

TEST(switch, negative_case_outside_switch)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    case 1:\n"
                      "        return 0;\n"
                      "}\n");
}

TEST(switch, negative_default_outside_switch)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    default:\n"
                      "        return 0;\n"
                      "}\n");
}

TEST(switch, negative_duplicate_case)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    case 1:\n"
                      "        return 0;\n"
                      "    case 1:\n"
                      "        return 1;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, negative_duplicate_default)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    default:\n"
                      "        return 0;\n"
                      "    default:\n"
                      "        return 1;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, negative_nonconstant_case)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 1;\n"
                      "    switch (1) {\n"
                      "    case x:\n"
                      "        return 0;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, negative_continue_not_in_loop)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    case 1:\n"
                      "        continue;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, case_sizeof_type)
{
    /* sizeof(type) is an integer constant expression (§6.6). */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (8) {\n"
                          "    case sizeof(long):\n"
                          "        return 42;\n"
                          "    case sizeof(short) + 1:\n"
                          "        return 7;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, case_conditional_constant)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (2) {\n"
                          "    case 1 ? 2 : 3:\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, case_logical_constant)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (1) {\n"
                          "    case 1 && 1:\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, case_sizeof_variable)
{
    /* `sizeof(variable)` is an integer constant expression (§6.6p6); its
       type is only known after parsing, so it folds during semantic. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x;\n"
                          "    switch (4) {\n"
                          "    case sizeof(x):\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, case_sizeof_array)
{
    /* Array-to-pointer decay is suppressed under sizeof (§6.3.2.1p3). */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[10];\n"
                          "    switch (40) {\n"
                          "    case sizeof(a):\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, case_converted_to_promoted_type)
{
    /* The case constant is converted to the promoted controlling-expression
       type (§6.8.4.2p5): 2147483648 becomes -2147483648 in a 32-bit int. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = -2147483648;\n"
                          "    switch (x) {\n"
                          "    case 2147483648:\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(switch, negative_duplicate_after_conversion)
{
    /* After conversion to int, `4294967295` equals `-1`, so this is a
       duplicate case value (§6.8.4.2p3). */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    case -1:\n"
                      "        return 1;\n"
                      "    case 4294967295:\n"
                      "        return 2;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, case_label_in_if_body)
{
    /* A case label nested inside the body of an if statement; trailing
       statements under the label (including ones outside the if) run. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1;\n"
                          "    int s = 0;\n"
                          "    switch (x) {\n"
                          "    if (1) {\n"
                          "        case 1:\n"
                          "            s = s + 1;\n"
                          "            s = s + 2;\n"
                          "    }\n"
                          "    }\n"
                          "    return s;\n"
                          "}\n",
                          3);
}

TEST(switch, case_label_in_loop_body)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i;\n"
                          "    int s = 0;\n"
                          "    for (i = 0; i < 3; i = i + 1) {\n"
                          "        switch (i) {\n"
                          "        case 0:\n"
                          "            {\n"
                          "                s = s + 1;\n"
                          "                break;\n"
                          "            }\n"
                          "        case 1:\n"
                          "            s = s + 2;\n"
                          "            break;\n"
                          "        default:\n"
                          "            s = s + 100;\n"
                          "        }\n"
                          "    }\n"
                          "    return s;\n"
                          "}\n",
                          103);
}

TEST(switch, negative_switch_cond_not_integer)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int *p;\n"
                      "    switch (p) {\n"
                      "    case 1:\n"
                      "        return 0;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, negative_case_string_constant)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    case \"abc\":\n"
                      "        return 1;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, negative_duplicate_via_expression)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    switch (1) {\n"
                      "    case 1:\n"
                      "        return 1;\n"
                      "    case 1 + 0:\n"
                      "        return 2;\n"
                      "    }\n"
                      "}\n");
}

TEST(switch, nested_switch_may_reuse_values)
{
    /* C11 §6.8.4.2p3: an enclosed switch may duplicate case constant
       expressions of an enclosing switch. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (2) {\n"
                          "    case 5:\n"
                          "        return 1;\n"
                          "    case 2:\n"
                          "        switch (5) {\n"
                          "        case 5:\n"
                          "            return 42;\n"
                          "        default:\n"
                          "            return 3;\n"
                          "        }\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n",
                          42);
}

TEST(switch, empty_body_falls_through)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 3;\n"
                          "    switch (x) {\n"
                          "    }\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(switch, break_exits_switch_in_loop)
{
    /* `break` inside the switch (itself inside a loop) exits the switch,
       not the loop. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i;\n"
                          "    int n = 0;\n"
                          "    for (i = 0; i < 5; i = i + 1) {\n"
                          "        switch (i) {\n"
                          "        case 1:\n"
                          "            break;\n"
                          "        case 3:\n"
                          "            n = n + 100;\n"
                          "            break;\n"
                          "        default:\n"
                          "            n = n + 1;\n"
                          "        }\n"
                          "    }\n"
                          "    return n;\n"
                          "}\n",
                          103);
}

TEST(switch, negative_case_value_matches)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (-1) {\n"
                          "    case -1:\n"
                          "        return 42;\n"
                          "    default:\n"
                          "        return 0;\n"
                          "    }\n"
                          "}\n",
                          42);
}