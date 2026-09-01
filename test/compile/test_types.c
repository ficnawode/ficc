#include "harness.h"
#include "testdriver.h"
#include "type.h"

TEST(types, type_widths)
{
    EXPECT_EQ(type_char()->width, 8);
    EXPECT_EQ(type_short()->width, 16);
    EXPECT_EQ(type_int()->width, 32);
    EXPECT_EQ(type_long()->width, 64);
    EXPECT_EQ(type_llong()->width, 64);
    EXPECT_EQ(type_uchar()->width, 8);
    EXPECT_EQ(type_ushort()->width, 16);
    EXPECT_EQ(type_uint()->width, 32);
    EXPECT_EQ(type_ulong()->width, 64);
    EXPECT_EQ(type_ullong()->width, 64);
}

TEST(types, type_align)
{
    EXPECT_EQ(type_char()->align, 1);
    EXPECT_EQ(type_short()->align, 2);
    EXPECT_EQ(type_int()->align, 4);
    EXPECT_EQ(type_long()->align, 8);
}

TEST(types, type_size)
{
    EXPECT_EQ(type_char()->size, 1);
    EXPECT_EQ(type_short()->size, 2);
    EXPECT_EQ(type_int()->size, 4);
    EXPECT_EQ(type_long()->size, 8);
}

TEST(types, type_is_signed)
{
    EXPECT_TRUE(type_is_signed(type_char()));
    EXPECT_TRUE(type_is_signed(type_short()));
    EXPECT_TRUE(type_is_signed(type_int()));
    EXPECT_TRUE(type_is_signed(type_long()));
    EXPECT_TRUE(type_is_signed(type_llong()));
    EXPECT_TRUE(!type_is_signed(type_uchar()));
    EXPECT_TRUE(!type_is_signed(type_uint()));
    EXPECT_TRUE(!type_is_signed(type_ulong()));
}

TEST(types, type_is_integer)
{
    EXPECT_TRUE(type_is_integer(type_char()));
    EXPECT_TRUE(type_is_integer(type_uchar()));
    EXPECT_TRUE(!type_is_integer(type_void()));
}

TEST(types, type_rank)
{
    EXPECT_TRUE(type_rank(type_char()) < type_rank(type_short()));
    EXPECT_TRUE(type_rank(type_short()) < type_rank(type_int()));
    EXPECT_TRUE(type_rank(type_int()) < type_rank(type_long()));
    EXPECT_TRUE(type_rank(type_uchar()) == type_rank(type_char()));
    EXPECT_TRUE(type_rank(type_uint()) == type_rank(type_int()));
}

TEST(types, promote_char)
{
    EXPECT_EQ(type_promote(type_char())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_short())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_uchar())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_ushort())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_long())->kind, TYPE_LONG);
}

TEST(types, common_type_same)
{
    EXPECT_EQ(type_common(type_int(), type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_common(type_long(), type_long())->kind, TYPE_LONG);
}

TEST(types, common_type_mixed_width)
{
    EXPECT_EQ(type_common(type_char(), type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_common(type_int(), type_long())->kind, TYPE_LONG);
    EXPECT_EQ(type_common(type_char(), type_short())->kind, TYPE_INT);
}

TEST(types, common_type_signedness)
{
    EXPECT_EQ(type_common(type_uint(), type_int())->kind, TYPE_UINT);
    EXPECT_EQ(type_common(type_ulong(), type_int())->kind, TYPE_ULONG);
    EXPECT_EQ(type_common(type_ulong(), type_long())->kind, TYPE_ULONG);
}

static const char *char_promo_src = "int f(char a, char b) {\n"
                                    "    return a + b;\n"
                                    "}\n"
                                    "int main(void) {\n"
                                    "    return f(100, 50);\n"
                                    "}\n";

TEST(types, interp_char_promotion)
{
    EXPECT_EQ(tc_run_interp(char_promo_src), 150);
}

static const char *uchar_promo_src = "int f(unsigned char a, unsigned char b) {\n"
                                     "    return a + b;\n"
                                     "}\n"
                                     "int main(void) {\n"
                                     "    return f(200, 50);\n"
                                     "}\n";

TEST(types, interp_uchar_promotion)
{
    EXPECT_EQ(tc_run_interp(uchar_promo_src), 250);
}

TEST(types, interp_short_promotion)
{
    EXPECT_EQ(tc_run_interp("int f(short a, short b) {\n"
                            "    return a + b;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f(1000, 2000);\n"
                            "}\n"),
              3000);
}

static const char *trunc_char_src = "int main(void) {\n"
                                    "    char x = 300;\n"
                                    "    return x;\n"
                                    "}\n";

TEST(types, interp_char_truncation_assign)
{
    EXPECT_EQ(tc_run_interp(trunc_char_src), 44);
}

TEST(types, interp_short_truncation_assign)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    short x = 70000;\n"
                            "    return x;\n"
                            "}\n"),
              4464);
}

TEST(types, interp_uchar_truncation_assign)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    unsigned char x = 300;\n"
                            "    return x;\n"
                            "}\n"),
              44);
}

static const char *ucmp_src = "int main(void) {\n"
                              "    unsigned a = 5;\n"
                              "    unsigned b = 10;\n"
                              "    return a < b;\n"
                              "}\n";

TEST(types, interp_unsigned_comparison)
{
    EXPECT_EQ(tc_run_interp(ucmp_src), 1);
}

TEST(types, interp_unsigned_gt_comparison)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    unsigned a = 20;\n"
                            "    unsigned b = 10;\n"
                            "    return a > b;\n"
                            "}\n"),
              1);
}

TEST(types, interp_signed_lt_negative)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int a = -5;\n"
                            "    int b = 10;\n"
                            "    return a < b;\n"
                            "}\n"),
              1);
}

TEST(types, interp_shift_left)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    return 1 << 3;\n"
                            "}\n"),
              8);
}

static const char *ssh_src = "int main(void) {\n"
                             "    return (-8) >> 1;\n"
                             "}\n";

TEST(types, interp_shift_right_signed)
{
    EXPECT_EQ(tc_run_interp(ssh_src), -4);
}

static const char *ush_src = "int main(void) {\n"
                             "    unsigned a = 8;\n"
                             "    return a >> 1;\n"
                             "}\n";

TEST(types, interp_shift_right_unsigned)
{
    EXPECT_EQ(tc_run_interp(ush_src), 4);
}

static const char *udiv_src = "int main(void) {\n"
                              "    unsigned a = 10;\n"
                              "    unsigned b = 3;\n"
                              "    return a / b;\n"
                              "}\n";

TEST(types, interp_unsigned_div)
{
    EXPECT_EQ(tc_run_interp(udiv_src), 3);
}

TEST(types, interp_unsigned_rem)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    unsigned a = 10;\n"
                            "    unsigned b = 3;\n"
                            "    return a % b;\n"
                            "}\n"),
              1);
}

TEST(types, interp_mixed_signedness_comparison)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int a = -5;\n"
                            "    unsigned b = 10;\n"
                            "    return a < b;\n"
                            "}\n"),
              0);
}

TEST(types, interp_long_add)
{
    EXPECT_EQ(tc_run_interp("long add(long a, long b) {\n"
                            "    return a + b;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return add(1000000, 2000000);\n"
                            "}\n"),
              3000000);
}

TEST(types, interp_char_param_overflow)
{
    EXPECT_EQ(tc_run_interp("int f(char a, char b) {\n"
                            "    return a + b;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f(100, 200);\n"
                            "}\n"),
              44);
}

TEST(types, interp_bitwise_not)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    char a = 0;\n"
                            "    return ~a;\n"
                            "}\n"),
              -1);
}

TEST(types, interp_ternary_mixed_types)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    char a = 10;\n"
                            "    short b = 20;\n"
                            "    return 1 ? a : b;\n"
                            "}\n"),
              10);
}

TEST(types, elf_char_promotion)
{
    EXPECT_EQ(tc_run_elf(char_promo_src), 150);
}

TEST(types, elf_uchar_promotion)
{
    EXPECT_EQ(tc_run_elf(uchar_promo_src), 250);
}

TEST(types, elf_signed_shift)
{
    EXPECT_EQ(tc_run_elf(ssh_src) & 0xFF, 252);
}

TEST(types, elf_unsigned_shift)
{
    EXPECT_EQ(tc_run_elf(ush_src), 4);
}

TEST(types, elf_trunc_char)
{
    EXPECT_EQ(tc_run_elf(trunc_char_src), 44);
}

TEST(types, elf_unsigned_comparison)
{
    EXPECT_EQ(tc_run_elf(ucmp_src), 1);
}

TEST(types, elf_unsigned_div)
{
    EXPECT_EQ(tc_run_elf(udiv_src), 3);
}

TEST(types, elf_long_param)
{
    EXPECT_EQ(tc_run_elf("int use_long(long x) {\n"
                         "    return x == 42L;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return use_long(42L);\n"
                         "}\n"),
              1);
}

TEST(types, elf_long_return)
{
    EXPECT_EQ(tc_run_elf("long ret_long(void) {\n"
                         "    return 99L;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    long r = ret_long();\n"
                         "    return r;\n"
                         "}\n"),
              99);
}
