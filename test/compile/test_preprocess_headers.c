#include "harness.h"
#include "testdriver.h"

TEST(pp_relational, ordering_comparisons_in_if)
{
    EXPECT_EQ(tc_run_elf("#if (30) <= (20)\n"
                         "#error \"le broken\"\n"
                         "#endif\n"
                         "#if 1 < 2 && 2 <= 3 && 3 >= 2 && 2 > 1 && 2 == 2 && 2 != 1\n"
                         "int main(void) { return 0; }\n"
                         "#elif\n"
                         "#error \"rel broken\"\n"
                         "#endif\n"),
              0);
}

TEST(pp_relational, compound_precedence)
{
    EXPECT_EQ(tc_run_elf("#define LIM (4 + 3)\n"
                         "#if LIM <= 8 && 9 >= LIM\n"
                         "int main(void) { return 0; }\n"
                         "#else\n"
                         "#error \"precedence broken\"\n"
                         "#endif\n"),
              0);
}

TEST(pp_relational, equality_binds_tighter_than_and)
{
    EXPECT_EQ(tc_run_elf("#if 2 + 2 == 4 && 2 * 3 == 6\n"
                         "int main(void) { return 0; }\n"
                         "#else\n"
                         "#error \"eq broken\"\n"
                         "#endif\n"),
              0);
}

TEST(pp_relational, short_circuit_skips_division)
{
    EXPECT_EQ(tc_run_elf("#if 0 && (1 / 0)\n"
                         "#error \"short circuit broken\"\n"
                         "#endif\n"
                         "int main(void) { return 0; }\n"),
              0);
}

TEST(pp_relational, negative_division_by_zero)
{
    EXPECT_BUILD_FAIL("#if 1 / 0\n"
                      "int main(void) { return 0; }\n"
                      "#endif\n");
}

TEST(pp_relational, ternary_binds_in_if)
{
    EXPECT_EQ(tc_run_elf("#if (1 ? 2 : 0) == 2 && (0 ? 1 : 3) == 3\n"
                         "int main(void) { return 0; }\n"
                         "#else\n"
                         "#error \"ternary broken\"\n"
                         "#endif\n"),
              0);
}

TEST(std_headers, intmax_typedefs)
{
    EXPECT_INTERP_AND_ELF("#include <stdint.h>\n"
                          "int main(void) {\n"
                          "    intmax_t x = 0;\n"
                          "    uintmax_t u = (uintmax_t) x;\n"
                          "    return u == 0 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(std_headers, intmax_macros)
{
    EXPECT_INTERP_AND_ELF("#include <stdint.h>\n"
                          "int main(void) {\n"
                          "    if (INTMAX_MAX < 0) return 1;\n"
                          "    if (UINTMAX_MAX != 18446744073709551615ULL) return 2;\n"
                          "    if (INTPTR_MIN > 0 || INTPTR_MAX < 0) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(std_headers, limits_macros)
{
    EXPECT_INTERP_AND_ELF("#include <limits.h>\n"
                          "int main(void) {\n"
                          "    if (INT_MAX != 2147483647) return 1;\n"
                          "    if (CHAR_BIT != 8) return 2;\n"
                          "    if (LONG_MIN >= 0) return 3;\n"
                          "    if (ULONG_MAX != 18446744073709551615UL) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(std_headers, ctype_declarations)
{
    EXPECT_BUILD_SUCCEED("#include <ctype.h>\n"
                         "int main(void) { return isalpha('a') ? 0 : 1; }\n");
}

TEST(std_headers, math_macros)
{
    EXPECT_INTERP_AND_ELF("#include <math.h>\n"
                          "int main(void) {\n"
                          "    double pi = M_PI;\n"
                          "    if (pi < 3.0 || pi > 4.0) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(std_headers, struct_tm_matches_the_host_abi)
{
    /* glibc struct tm adds tm_gmtoff and tm_zone beyond the nine standard ints. */
    EXPECT_INTERP_AND_ELF("#include <time.h>\n"
                          "_Static_assert(sizeof(struct tm) == 56, \"struct tm size\");\n"
                          "int main(void) {\n"
                          "    struct tm t;\n"
                          "    t.tm_gmtoff = 0;\n"
                          "    t.tm_zone = 0;\n"
                          "    return (int) sizeof(t) - 56;\n"
                          "}\n",
                          0);
}
