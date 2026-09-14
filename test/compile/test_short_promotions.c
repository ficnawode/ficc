#include "harness.h"
#include "testdriver.h"

/* Extension into 16-bit destinations: `short`/`unsigned short` intermediate
   results from a smaller operand must sign-/zero-extend into a 16-bit
   register (66-prefixed movsx/movzx), then widen again to int without
   corrupting the sign. */

TEST(short_promotions, char_zero_extends_to_ushort_then_int)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned short a = 0xFF;\n"
                          "    char b = (char) 0xFF;\n"
                          "    int x = (unsigned short) b;\n"
                          "    if (x != 0xFFFF) return 1;\n"
                          "    if (a != 0xFF) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(short_promotions, char_sign_extends_to_short)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    signed char b = -3;\n"
                          "    signed char big = -128;\n"
                          "    short s = (short) b;\n"
                          "    short sg = (short) big;\n"
                          "    unsigned short u = (unsigned short) big;\n"
                          "    if (s != -3) return 1;\n"
                          "    if (sg != -128) return 2;\n"
                          "    if (u != 0xFF80) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(short_promotions, short_through_function_and_back)
{
    EXPECT_INTERP_AND_ELF("short neg(short x) { return -x; }\n"
                          "unsigned short cap(unsigned short x) { return x > 100 ? 100 : x; }\n"
                          "int main(void) {\n"
                          "    if (neg(42) != -42) return 1;\n"
                          "    if (cap(250) != 100) return 2;\n"
                          "    if (cap(7) != 7) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}