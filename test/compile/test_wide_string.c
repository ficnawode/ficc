#include "harness.h"
#include "testdriver.h"

/* Wide/UTF string and character literals (C11 §6.4.4.4, §6.4.5). */

TEST(wide_string, content_and_indexing)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    const int *w = L\"NULL\";\n"
        "    if (w[0] != 78 || w[1] != 85 || w[2] != 76 || w[3] != 76) return 1;\n"
        "    if (w[4] != 0) return 2;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(wide_string, sizeof_counts_elements)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (sizeof(L\"abc\") != 16) return 1;\n"
                          "    if (sizeof(\"abc\") != 4) return 2;\n"
                          "    if (sizeof(u\"abc\") != 8) return 3;\n"
                          "    if (sizeof(U\"abc\") != 16) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(wide_string, wide_char_literal_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) { int c = L'A'; return c == 'A' ? 42 : 1; }\n", 42);
}

TEST(wide_string, array_initialization)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[5] = L\"NULL\";\n"
        "    if (a[0] != 78 || a[1] != 85 || a[2] != 76 || a[3] != 76) return 1;\n"
        "    if (a[4] != 0) return 2;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(wide_string, inferred_array_length)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int b[] = L\"hi\";\n"
                          "    if (sizeof(b) != 12) return 1;\n"
                          "    if (b[0] != 'h' || b[1] != 'i' || b[2] != 0) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(wide_string, utf16_array_init)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned short c[] = u\"hi\";\n"
                          "    if (c[0] != 'h' || c[1] != 'i' || sizeof(c) != 6) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(wide_string, utf32_array_init)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned int d[] = U\"hi\";\n"
                          "    if (d[0] != 'h' || d[1] != 'i' || sizeof(d) != 12) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(wide_string, adjacent_literal_concatenation)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int *w = L\"a\" L\"b\";\n"
                          "    if (w[0] != 'a' || w[1] != 'b' || w[2] != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(wide_string, pointer_arithmetic)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int *w = L\"abc\";\n"
                          "    if (*(w + 1) != 'b') return 1;\n"
                          "    if (w[2] != 'c') return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}
