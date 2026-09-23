#include "harness.h"
#include "testdriver.h"

/* String-literal array semantics: `sizeof("ab")` is the array length
   including NUL (§6.4.5p5, §6.5.3.4p2), and the fold is available both at
   compile time (array sizes / static inits) and runtime. */

TEST(string_literal, sizeof_is_array_length)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (sizeof(\"abc\") != 4) return 1;\n"
                          "    if (sizeof(\"\") != 1) return 2;\n"
                          "    if (sizeof(\"a\" \"b\") != 3) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(string_literal, sizeof_not_pointer_size)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (sizeof(\"hello\") == sizeof(char *)) return 1;\n"
                          "    if (sizeof(\"x\") != 2) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(string_literal, sizeof_in_static_array_size)
{
    EXPECT_INTERP_AND_ELF("static char buf[sizeof(\"lua\")]; /* 4 bytes */\n"
                          "int main(void) {\n"
                          "    if (sizeof(buf) != 4) return 1;\n"
                          "    buf[3] = 'Z';\n"
                          "    if (buf[3] != 'Z') return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(string_literal, string_init_fills_array)
{
    EXPECT_INTERP_AND_ELF("struct rec {\n"
                          "    char tag[4];\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct rec r = {{\"lua\"}};\n"
                          "    return r.tag[0] == 'l' && r.tag[2] == 'a' ? 0 : 1;\n"
                          "}\n",
                          0);
}
TEST(string_literal, func_identifier)
{
    /* C11 §6.4.2.2: `__func__` is the enclosing function name as a static
       const char array. */
    EXPECT_INTERP_AND_ELF("static int len(void) { return (int) sizeof(__func__); }\n"
                          "int main(void) {\n"
                          "    if (len() != 4) return 1;\n"
                          "    if (__func__[0] != 'm') return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}
