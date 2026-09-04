#include "harness.h"
#include "testdriver.h"

/* Phase 13a: character literals and escape sequences (§6.4.4.4). A char
   literal is an int constant, so it must fold everywhere an integer constant
   does. Every program must produce the same result through the interpreter
   and through the compiled ELF. */

TEST(char_lit, plain_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) { int c = 'A'; return c - 23; }\n", 42);
}

TEST(char_lit, octal_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) { return '\\52'; }\n", 42);
}

TEST(char_lit, hex_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) { return '\\x2a'; }\n", 42);
}

TEST(char_lit, simple_escapes)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int sum = '\\t' + '\\n' + '\\0' + '\\v' + '\\f' + '\\r' + '\\a' + '\\b';\n"
        "    if (sum != 9 + 10 + 0 + 11 + 12 + 13 + 7 + 8) return 1;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(char_lit, quote_and_escape_characters)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ('\\'' != 39) return 1;\n"
                          "    if ('\\\\' != 92) return 2;\n"
                          "    if ('\"' != 34) return 3;\n"
                          "    if ('\\?' != 63) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(char_lit, char_arithmetic)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 'a';\n"
                          "    c = c + 1;\n"
                          "    int x = c - 'b' + 42;\n"
                          "    return x;\n"
                          "}\n",
                          42);
}

TEST(char_lit, compare_chars)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char n = '0';\n"
                          "    if (n >= '0' && n <= '9') return 42;\n"
                          "    return 1;\n"
                          "}\n",
                          42);
}

TEST(char_lit, switch_on_char)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 'z';\n"
                          "    switch (c) {\n"
                          "        case 'a': return 1;\n"
                          "        case 'm': return 2;\n"
                          "        case 'z': return 42;\n"
                          "        default: return 3;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(char_lit, switch_on_int_from_char)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    switch (65) {\n"
                          "        case 'A': return 42;\n"
                          "        default: return 1;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(char_lit, char_init_from_escape)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char tab = '\\t';\n"
                          "    char nul = '\\0';\n"
                          "    int a[2];\n"
                          "    a[0] = tab + 33;\n"
                          "    a[1] = nul;\n"
                          "    if (a[0] != 42) return 1;\n"
                          "    if (sizeof(a) != 8) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(char_lit, string_escapes_share_decoder)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[] = { \"\\x41\\01\" };\n"
                          "    if (s[0] != 65) return 1;\n"
                          "    if (s[1] != 1) return 2;\n"
                          "    if (s[2] != 0) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

/* --- negatives (all must fail to build) --- */

TEST(char_lit, negative_empty)
{
    EXPECT_BUILD_FAIL("int main(void) { int c = ''; return c; }\n");
}

TEST(char_lit, negative_multi_char)
{
    EXPECT_BUILD_FAIL("int main(void) { int c = 'ab'; return c; }\n");
}

TEST(char_lit, negative_hex_no_digits)
{
    EXPECT_BUILD_FAIL("int main(void) { int c = '\\x'; return c; }\n");
}

TEST(char_lit, negative_out_of_range)
{
    EXPECT_BUILD_FAIL("int main(void) { int c = '\\400'; return c; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int c = '\\x400'; return c; }\n");
}

TEST(char_lit, negative_unterminated)
{
    EXPECT_BUILD_FAIL("int main(void) { int c = 'a; return c; }\n");
}