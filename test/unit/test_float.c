#include "harness.h"
#include "testdriver.h"
#include "type.h"
#include "util/arena.h"

TEST(float, type_layout)
{
    Type *f = type_float();
    Type *d = type_double();
    EXPECT_EQ(f->kind, TYPE_FLOAT);
    EXPECT_EQ(f->width, 32);
    EXPECT_EQ(f->align, 4);
    EXPECT_EQ(f->size, 4);
    EXPECT_EQ(d->kind, TYPE_DOUBLE);
    EXPECT_EQ(d->width, 64);
    EXPECT_EQ(d->align, 8);
    EXPECT_EQ(d->size, 8);
    EXPECT_EQ(type_sizeof(f), 4);
    EXPECT_EQ(type_alignof(d), 8);
}

TEST(float, is_float_and_fp)
{
    EXPECT_TRUE(type_is_float(type_float()));
    EXPECT_TRUE(type_is_float(type_double()));
    EXPECT_TRUE(type_is_fp(type_float()));
    EXPECT_TRUE(type_is_fp(type_double()));
    EXPECT_FALSE(type_is_float(type_int()));
    EXPECT_FALSE(type_is_fp(type_int()));
    EXPECT_FALSE(type_is_fp(type_ptr(type_int())));
    EXPECT_FALSE(type_is_fp(type_void()));
}

TEST(float, floats_are_not_integer)
{
    EXPECT_FALSE(type_is_integer(type_float()));
    EXPECT_FALSE(type_is_integer(type_double()));
    EXPECT_FALSE(type_is_signed_int(type_float()));
    EXPECT_FALSE(type_is_unsigned(type_double()));
    /* `type_rank` is an integer concept: -1 for FP. */
    EXPECT_EQ(type_rank(type_float()), -1);
    EXPECT_EQ(type_rank(type_double()), -1);
}

TEST(float, complete_and_promotion)
{
    /* type_is_complete accepts them; integer promotion leaves them alone. */
    EXPECT_TRUE(type_is_complete(type_float()));
    EXPECT_TRUE(type_is_complete(type_double()));
    EXPECT_TRUE(type_promote(type_float()) == type_float());
    EXPECT_TRUE(type_promote(type_double()) == type_double());
    /* Casting to a qualified float keeps the singleton's identity shape. */
    EXPECT_EQ(type_unqual(type_const(type_double()))->kind, TYPE_DOUBLE);
}

TEST(float, common_type_fp_precedence)
{
    /* §6.3.1.8: FP precedence (double over float) before the integer chain. */
    EXPECT_TRUE(type_common(type_float(), type_float()) == type_float());
    EXPECT_TRUE(type_common(type_double(), type_double()) == type_double());
    EXPECT_TRUE(type_common(type_float(), type_double()) == type_double());
    EXPECT_TRUE(type_common(type_double(), type_float()) == type_double());
    EXPECT_TRUE(type_common(type_float(), type_int()) == type_float());
    EXPECT_TRUE(type_common(type_int(), type_float()) == type_float());
    EXPECT_TRUE(type_common(type_int(), type_double()) == type_double());
    /* Integer pairs still take the integer chain (unchanged). */
    EXPECT_TRUE(type_common(type_int(), type_long()) == type_long());
    EXPECT_TRUE(type_common(type_uint(), type_int()) == type_uint());
}

TEST(float, array_and_record_layout)
{
    /* Arrays/records of float flow through the existing layout rules. */
    Arena *a = arena_new();
    Type *arr = type_array(type_float(), 3);
    EXPECT_EQ(type_sizeof(arr), 12);
    EXPECT_EQ(type_alignof(arr), 4);
    Type *arrd = type_array(type_double(), 3);
    EXPECT_EQ(type_sizeof(arrd), 24);
    EXPECT_EQ(type_alignof(arrd), 8);
    arena_free(a);
}

TEST(float, keywords_parse)
{
    EXPECT_PARSE_SUCCEED("float f;\n"
                         "double d;\n");
    /* Block-scope FP initializers build; file-scope ones are rejected. */
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    float f = 1.5f;\n"
                         "    double d = 1.5;\n"
                         "    return 0;\n"
                         "}");
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    double x = 1e10;\n"
                         "    float y = 0x1.8p3f;\n"
                         "    return 0;\n"
                         "}");
    EXPECT_PARSE_FAIL("double g = 1.5;\n");
    EXPECT_PARSE_FAIL("static double s = 1.5;\n");
}

TEST(float, file_scope_decl_without_init_ok)
{
    EXPECT_BUILD_SUCCEED("double g;\n"
                         "float h;\n"
                         "int main(void) { return 0; }\n");
}

TEST(float, long_double_rejected_for_now)
{
    /* `long double` is recognized but not supported — a loud parse error. */
    EXPECT_PARSE_FAIL("long double x;\n");
    EXPECT_PARSE_FAIL("long double x = 1.5L;\n");
    EXPECT_PARSE_FAIL("double x = 1.5L;\n");
}

TEST(float, bogus_specifiers_rejected)
{
    EXPECT_PARSE_FAIL("signed float x;\n");
    EXPECT_PARSE_FAIL("unsigned double x;\n");
    EXPECT_PARSE_FAIL("long float x;\n");
    EXPECT_PARSE_FAIL("long long double x;\n");
    EXPECT_PARSE_FAIL("float double x;\n");
    EXPECT_PARSE_FAIL("double int x;\n");
    EXPECT_PARSE_FAIL("unsigned long double x;\n");
    /* `_Complex` is not a ficc keyword yet — rejected as a type specifier. */
    EXPECT_PARSE_FAIL("_Complex c;\n");
}

TEST(float, float_storage_through_both_backends)
{
    /* Exchange FP bit patterns through a union: pure storage (no FP
       arithmetic), so it must run identically in the interpreter and the ELF
       backend. 1.1 in double is 0x3FF199999999999A (top word 0x3FF19999 ->
       0x99); 1.1f is 0x3F8CCCCD (low byte 0xCD). */
    EXPECT_INTERP_AND_ELF("union U { double d; long long i; };\n"
                          "int main(void) {\n"
                          "    union U u;\n"
                          "    u.d = 1.1;\n"
                          "    return (int) ((u.i >> 32) & 0xFF);\n"
                          "}",
                          0x99);
    EXPECT_INTERP_AND_ELF("union F { float f; int i; };\n"
                          "int main(void) {\n"
                          "    union F u;\n"
                          "    u.f = 1.1f;\n"
                          "    return u.i & 0xFF;\n"
                          "}",
                          0xCD);
}

TEST(float, int_to_float_init_now_lowers)
{
    /* int→double / double→float conversions build and run in both backends. */
    EXPECT_INTERP_AND_ELF("int main(void) { double d = 5; return (int) d; }", 5);
    EXPECT_INTERP_AND_ELF("int main(void) { float f = 7; return (int) f; }", 7);
    EXPECT_BUILD_SUCCEED("double wid(void) { return 1; }\n"
                         "int main(void) { return 0; }");
}

TEST(float, float_to_bool_still_rejected)
{
    /* Float→`_Bool` needs an x != 0.0 compare, so it stays an error. */
    EXPECT_BUILD_FAIL("_Bool b = 1.5;\nint main(void) { return b; }");
    EXPECT_BUILD_FAIL("_Bool b = (double) 3;\nint main(void) { return b; }");
}

TEST(float, unlowered_fp_contexts_error_loudly)
{
    /* FP math/compare/unary-minus never lowers to an integer op. */
    EXPECT_BUILD_FAIL("int main(void) { double a, b; return a + b; }");
    EXPECT_BUILD_FAIL("int main(void) { double a, b; if (a < b) return 1; return 0; }");
    EXPECT_BUILD_FAIL("double f(double a) { return -a; }");
    EXPECT_BUILD_FAIL("double f(double a, double b) { return a / b; }");
    EXPECT_BUILD_FAIL("int main(void) { double a; return a * 2.0; }");
}

TEST(float, fp_conditions_error_loudly)
{
    EXPECT_BUILD_FAIL("int main(void) { double x; if (x) return 1; return 0; }");
    EXPECT_BUILD_FAIL("int main(void) { double x; while (x) return 1; return 0; }");
    EXPECT_BUILD_FAIL("int main(void) { double x; return x ? 1 : 0; }");
    EXPECT_BUILD_FAIL("int main(void) { double x; return !x; }");
    EXPECT_BUILD_FAIL("int main(void) { double x; return x && x; }");
}

TEST(float, incdec_fp_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) { double x; x++; return 0; }");
    EXPECT_BUILD_FAIL("int main(void) { float x; --x; return 0; }");
}

TEST(float, integer_only_contexts_reject_floats)
{
    /* A float never satisfies an integer constant-expression slot. */
    EXPECT_BUILD_FAIL("int main(void) { switch (1) { case 1.5: break; } return 0; }");
    EXPECT_BUILD_FAIL("_Static_assert(1.5, \"nope\");\n"
                      "int main(void) { return 0; }");
    EXPECT_PARSE_FAIL("int a[1.5];");
    EXPECT_PARSE_FAIL("enum E { A = 1.5 };");
    EXPECT_PARSE_FAIL("_Alignas(1.5) int x;");
}

TEST(float, ternary_fp_mismatch_rejected)
{
    /* A conditional mixing FP classes would select between wrong immediates. */
    EXPECT_BUILD_FAIL("int main(void) { int c; return c ? 1 : 1.5; }");
    EXPECT_BUILD_FAIL("int main(void) { int c; double a; float b; return c ? a : b; }");
}

TEST(float, truncation_toward_zero)
{
    /* §6.3.1.4p1: float→int truncates toward zero; negatives come from casts. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((int)2.9 != 2) return 1;\n"
                          "    if ((int)2.0 != 2) return 2;\n"
                          "    if ((int)0.9 != 0) return 3;\n"
                          "    double n = (double)-2;\n"
                          "    if ((int)n != -2) return 4;\n"
                          "    if ((int)(double)-1 != -1) return 5;\n"
                          "    if ((int)5.0 != 5) return 6;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, truncation_narrow_lanes)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((signed char)300.9 != 44) return 1;\n"
                          "    if ((unsigned char)(double)-1 != 255) return 2;\n"
                          "    if ((short)70000.0 != 4464) return 3;\n"
                          "    if ((unsigned short)70000.0 != 4464) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, int_to_float_rounds_to_nearest)
{
    /* 16777217 = 2^24+1 needs 25 mantissa bits — float rounds it to 2^24. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    float f = 16777217.0;\n"
                          "    if ((int)f != 16777216) return 1;\n"
                          "    float g = 16777219.0;\n"
                          "    if ((int)g != 16777220) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, fconv_between_float_and_double)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    double d = 16777217.0;\n"
                          "    float f = (float)d;\n"
                          "    if ((int)f != 16777216) return 1;\n"
                          "    float f2 = 1.5f;\n"
                          "    double d2 = (double)f2;\n"
                          "    if ((int)d2 != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, float_literal_init_same_kind_passthrough)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    float f = 3.5f;\n"
                          "    if ((int)f != 3) return 1;\n"
                          "    double d = 3.5;\n"
                          "    if ((int)d != 3) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, out_of_range_int_cast_is_branded)
{
    /* Out-of-range/NaN reproduces x86's INT_MIN brand; 1e400 is inf. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((int)3000000000.0 != -2147483648) return 1;\n"
                          "    if ((int)(1e400) != -2147483648) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, u32_edges)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    double d = (double)(unsigned)0xFFFFFFFFU;\n"
                          "    if ((unsigned)d != 0xFFFFFFFFU) return 1;\n"
                          "    double e = (double)(unsigned)0x80000000U;\n"
                          "    if ((unsigned)e != 0x80000000U) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, u64_itof_gedge)
{
    /* ≥2^63 u64s take the clear-top-bit + add-2^63 path in both backends. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned long long u = 0x8000000000000000ULL | 5ULL;\n"
                          "    double d = (double)u;\n"
                          "    if ((unsigned long long)d != 0x8000000000000000ULL) return 1;\n"
                          "    unsigned long long v = 0x8000000000000000ULL;\n"
                          "    double e = (double)v;\n"
                          "    if ((unsigned long long)e != 0x8000000000000000ULL) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, u64_ftoi_gedge)
{
    /* 2^63 triggers the sign-correcting walk; negatives keep their bits. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    double d = 0x1p63;\n"
                          "    if ((unsigned long long)d != 0x8000000000000000ULL) return 1;\n"
                          "    if ((unsigned long long)2.0 != 2ULL) return 2;\n"
                          "    double n = (double)-1;\n"
                          "    if ((unsigned long long)n != 0xFFFFFFFFFFFFFFFFULL) return 3;\n"
                          "    if ((unsigned long long)(double)-5 != 0xFFFFFFFFFFFFFFFBULL) "
                          "return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, signed_64_edges)
{
    /* Powers of two are exact in double, so the roundtrip is an identity. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long long x = -4611686018427387904LL;\n"
                          "    double d = (double)x;\n"
                          "    if ((long long)d != x) return 1;\n"
                          "    long long y = 4611686018427387904LL;\n"
                          "    double e = (double)y;\n"
                          "    if ((long long)e != y) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, fp_args_and_returns_flow_through_both_backends)
{
    /* FP scalars travel through the GP lanes in both backends; the casts agree. */
    EXPECT_INTERP_AND_ELF("double castf(double d) { return (double)(int)d; }\n"
                          "int main(void) {\n"
                          "    double r = castf(2.9);\n"
                          "    if ((int)r != 2) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
    EXPECT_INTERP_AND_ELF("float hf(float f) { return (float)(int)f; }\n"
                          "int main(void) {\n"
                          "    double r = (double)hf(7.9);\n"
                          "    if ((int)r != 7) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
    EXPECT_INTERP_AND_ELF("double ret(void) { return 3.5; }\n"
                          "int main(void) { return (int)ret(); }\n",
                          3);
}

TEST(float, float_subscript_rejected)
{
    /* §6.5.2.1p1: an index must be an integer; an explicit (int) cast is fine. */
    EXPECT_BUILD_SUCCEED("int main(void) { int a[3]; double x; return a[(int)x]; }");
    EXPECT_BUILD_FAIL("int main(void) { int a[3]; double x; return a[x]; }");
    EXPECT_BUILD_FAIL("int main(void) { int a[3]; return a[1.5]; }");
}