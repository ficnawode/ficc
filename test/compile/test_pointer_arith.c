#include "harness.h"
#include "testdriver.h"

/* Pointer arithmetic (§6.5.6): scaling by pointee size works with the
   pointer on either side of `+` (previously only `ptr + int`), plus
   `ptr - int`, `ptr - ptr`, and mixed constant expressions. */

TEST(pointer_arith, pointer_on_right_of_plus)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[4] = {10, 20, 30, 40};\n"
                          "    int *p = arr;\n"
                          "    int n = 2;\n"
                          "    if (*(p + n) != 30) return 1;\n"
                          "    if (*(n + p) != 30) return 2;\n"
                          "    if (*(n + p + 8 - 8) != 30) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(pointer_arith, pointer_minus_integer)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[4] = {10, 20, 30, 40};\n"
                          "    int *p = arr + 3;\n"
                          "    if (*(p - 2) != 20) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(pointer_arith, pointer_difference)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[4] = {0};\n"
                          "    int *a = arr;\n"
                          "    int *b = arr + 3;\n"
                          "    if (b - a != 3) return 1;\n"
                          "    if (a - b != -3) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(pointer_arith, byte_and_short_arrays)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c[6] = {0, 1, 2, 3, 4, 5};\n"
                          "    short s[4] = {10, 20, 30, 40};\n"
                          "    if (*(c + 5) != 5) return 1;\n"
                          "    if (*(2 + s) != 30) return 2;\n"
                          "    if (s + 4 - s != 4) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(pointer_arith, pointers_to_records)
{
    EXPECT_INTERP_AND_ELF("struct rec { int a; int b; };\n"
                          "int main(void) {\n"
                          "    struct rec r[2] = {{1, 2}, {3, 4}};\n"
                          "    struct rec *p = r;\n"
                          "    if ((p + 1)->b != 4) return 1;\n"
                          "    if ((*(1 + p)).a != p[1].a) return 2;\n"
                          "    if ((p + 2) - p != 2) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}