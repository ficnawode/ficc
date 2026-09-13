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

TEST(float, float_to_bool)
{
    /* §6.3.1.2: _Bool holds (x != 0.0) — FCMP_NE, so -0.0 is false and
       anything with magnitude is true. */
    EXPECT_INTERP_AND_ELF("int main(void) { _Bool b = 1.5; return (int) b; }", 1);
    EXPECT_INTERP_AND_ELF("int main(void) { _Bool b = (double) 3; return (int) b; }", 1);
    EXPECT_INTERP_AND_ELF("int main(void) { double x = 0.0; _Bool b = x; return (int) b; }", 0);
    EXPECT_INTERP_AND_ELF("int main(void) { double x = -0.0; _Bool b = x; return (int) b; }", 0);
    EXPECT_INTERP_AND_ELF("int main(void) { double x = 1e-300; _Bool b = x; return (int) b; }", 1);
}

TEST(float, fp_arithmetic_both_backends)
{
    /* FP binary arithmetic lands in both backends; exact values stay exact. */
    EXPECT_INTERP_AND_ELF("int main(void){ double a=1.5, b=2.5;"
                          " if (a+b != 4.0) return 1;"
                          " if (a*b != 3.75) return 2;"
                          " if (b-a != 1.0) return 3;"
                          " if (b/a != 1.6666666666666667) return 4;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ float a=1.5f, b=2.5f;"
                          " if (a+b != 4.0f) return 1;"
                          " if (a*b != 3.75f) return 2;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ float a=1.5f, b=2.5f;"
                          " if (b-a != 1.0f) return 1;"
                          " if (!(b/a > 1.6666665f && b/a < 1.6666668f)) return 2;"
                          " return 42; }",
                          42);
    /* A literal overload lands on the FP opcodes (the immediates carry bits). */
    EXPECT_INTERP_AND_ELF("int main(void){ return (int)(1.5 + 2.5); }", 4);
    EXPECT_INTERP_AND_ELF("int main(void){ return (int)(1.5f * 2.0f); }", 3);
    /* int × float promotes to float via the usual arithmetic conversions. */
    EXPECT_INTERP_AND_ELF("int main(void){ double a = 1.5; return (int)(a * 2); }", 3);
    EXPECT_INTERP_AND_ELF("int main(void){ float a = 1.5f; return (int)(3 * a); }", 4);
}

TEST(float, fp_compound_assign)
{
    EXPECT_INTERP_AND_ELF("int main(void){ double a=1.5; a += 2.5; return (int)a; }", 4);
    EXPECT_INTERP_AND_ELF("int main(void){ double a=5.0; a /= 2.0; return (int)a; }", 2);
    EXPECT_INTERP_AND_ELF("int main(void){ float f=1.5f; f *= 2.0f; return (int)f; }", 3);
    EXPECT_INTERP_AND_ELF("int main(void){ double a=1.5; a -= 0.5; return (int)(a*10.0); }", 10);
}

TEST(float, fp_reround_each_step)
{
    /* 200 × 0.1f summed in float drifts away from the double-accumulated value;
       the interpreter must re-round to float at every add or `interp` and the
       ELF (which computes in single precision) would diverge. */
    EXPECT_INTERP_AND_ELF("int main(void){ float s = 0.0f; int i;"
                          " for (i = 0; i < 200; i++) s += 0.1f;"
                          " if ((int)(s * 10.0f) != 200) return 1;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ float p = 1.0f; int i;"
                          " for (i = 0; i < 10; i++) p = p * 1.1f;"
                          " if ((int)(p * 1000.0f) != 2593) return 1;"
                          " return 42; }",
                          42);
}

TEST(float, fp_unary_minus)
{
    EXPECT_INTERP_AND_ELF("int main(void){ double a = 1.5;"
                          " if ((int)(-a) != -1) return 1;"
                          " if ((int)(-a * 10.0) != -15) return 2;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ if ((int)-2.25 != -2) return 1; return 42; }", 42);
    EXPECT_INTERP_AND_ELF("float fn(float x){ return -x; }"
                          " int main(void){ if ((int)(fn(3.5f)) != -3) return 1; return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ float f = -1.5f;"
                          " if (f != -1.5f) return 1; return 42; }",
                          42);
}

TEST(float, fp_conditions_allowed)
{
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.5; if (x) return 1; "
                          " return 0; }",
                          1);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 0.0; if (x) return 1; return 2; }", 2);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0; if (x) return 1; return 2; }", 2);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.0; while (x < 3.0) x += 0.5; "
                          " return (int)x; }",
                          3);
    /* Ternary conditions boolify like if/while. */
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.5; return x ? 7 : 9; }", 7);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0; return x ? 7 : 9; }", 9);
}

TEST(float, fp_logical_ops)
{
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.5, y = 0.0;"
                          " if (x && y) return 1;"
                          " if (!(x || y)) return 2;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0;"
                          " if (x || 1.0) return 1;"
                          " return 0; }",
                          1);
}

TEST(float, fp_compare_matrix)
{
    EXPECT_INTERP_AND_ELF("int main(void){\n"
                          "  double x = 1.5, y = 2.5;\n"
                          "  if (!(x == x)) return 1;\n"
                          "  if (x == y) return 2;\n"
                          "  if (!(x != y)) return 3;\n"
                          "  if (x != x) return 4;\n"
                          "  if (!(x < y)) return 5;\n"
                          "  if (y < x) return 6;\n"
                          "  if (!(y > x)) return 7;\n"
                          "  if (x > y) return 8;\n"
                          "  if (!(x <= y)) return 9;\n"
                          "  if (y <= x) return 10;\n"
                          "  if (x <= 1.5 - 1.0) return 11;\n"
                          "  if (!(x >= x)) return 12;\n"
                          "  if (y >= 3.0) return 13;\n"
                          "  return 42;\n"
                          "}\n",
                          42);
}

TEST(float, fp_compare_nan_semantics)
{
    /* C11: `x == NaN` is 0, `x != NaN` is 1, ordered predicates are all 0. */
    EXPECT_INTERP_AND_ELF("int main(void){\n"
                          "  double n = 0.0 / 0.0;\n"
                          "  if (n == n) return 1;\n"
                          "  if (!(n != n)) return 2;\n"
                          "  if (n < n) return 3;\n"
                          "  if (n > n) return 4;\n"
                          "  if (n <= n) return 5;\n"
                          "  if (n >= n) return 6;\n"
                          "  if (n < 1.0) return 7;\n"
                          "  if (n > -1.0) return 8;\n"
                          "  return 42;\n"
                          "}\n",
                          42);
}

TEST(float, fp_neg_zero)
{
    /* -0.0 compares equal to 0.0 and is falsy; the sign survives FNEG. */
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0;"
                          " if (x != 0.0) return 1;"
                          " if (x) return 2;"
                          " double y = -x;"
                          " if (y != 0.0) return 3;"
                          " return 42; }",
                          42);
}

TEST(float, boolify_is_not_bit_testing)
{
    /* -0.0 has nonzero bits yet compares equal to 0.0: conditions must not
       test the raw bytes. */
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0;"
                          " if (x) return 1;"
                          " if (!x) return 2;"
                          " return 0; }",
                          2);
    /* NaN is nonzero (truthy) even though it is not `<`/`>`/`==` anything. */
    EXPECT_INTERP_AND_ELF("int main(void){ double n = 0.0 / 0.0;"
                          " if (!n) return 1;"
                          " return 2; }",
                          2);
}

TEST(float, fp_log_not)
{
    EXPECT_INTERP_AND_ELF("int main(void){ return !0.0 == 1 ? 42 : 1; }", 42);
    EXPECT_INTERP_AND_ELF("int main(void){ return !1.5 == 0 ? 42 : 1; }", 42);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = -0.0; return !x == 1 ? 42 : 1; }", 42);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1e-300; return !x == 0 ? 42 : 1; }", 42);
}

TEST(float, fp_incdec)
{
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.5; x++; return (int)x; }", 2);
    EXPECT_INTERP_AND_ELF("int main(void){ double x = 1.5; x--; return (int)x; }", 0);
    EXPECT_INTERP_AND_ELF("int main(void){ float f = 1.5f; ++f; return (int)f; }", 2);
    EXPECT_INTERP_AND_ELF("int main(void){ float f = 1.5f; if (f++ != 1.5f) return 1;"
                          " if (f != 2.5f) return 2; return 42; }",
                          42);
}

TEST(float, fp_mixed_ternary)
{
    /* A conditional mixing an int and a float converts the int to the FP
       common type (§6.5.15); float/double pick double. */
    EXPECT_INTERP_AND_ELF("int main(void){ int c = 0;"
                          " if ((c ? 1 : 1.5) != 1.5) return 1;"
                          " if ((c ? 1.5 : 2) != 2.0) return 2;"
                          " if ((c ? 1 : 1.5f) != 1.5) return 3;"
                          " c = 1;"
                          " if ((c ? 1 : 1.5) != 1.0) return 4;"
                          " if ((c ? 2 : 2.5f) != 2.0) return 5;"
                          " return 42; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ double a = 1.5, b = 2.5;"
                          " if ((a < b ? a : b) != 1.5) return 1;"
                          " if ((a > b ? a : b) != 2.5) return 2;"
                          " return 42; }",
                          42);
}

TEST(float, fp_ternary_branches_promote)
{
    /* The selected branch converts to the common type before the phi, so a
       double-typed conditional never reads an integer immediate as FP bits. */
    EXPECT_INTERP_AND_ELF("int main(void){ int c = 1; double d = c ? 300000000 : 1.5;"
                          " return (int)d == 300000000 ? 42 : 1; }",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void){ int c = 0; double d = c ? 300000000 : 1.5;"
                          " return (int)(d * 10.0) == 15 ? 42 : 1; }",
                          42);
}

TEST(float, fp_arrays_stay_32bit)
{
    /* movss must stay 32-bit: 8-byte moves on a float slot would read the
       neighbor's bytes in arrays. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    float a[4];\n"
                          "    a[0] = 1.5f; a[1] = 2.5f; a[2] = 3.5f; a[3] = 4.5f;\n"
                          "    if (a[0] + a[3] != 6.0f) return 1;\n"
                          "    if (a[1] * a[2] != 8.75f) return 2;\n"
                          "    double d[3];\n"
                          "    d[0] = 0.5; d[1] = 1.5; d[2] = 2.5;\n"
                          "    if (d[0] + d[1] + d[2] != 4.5) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, fp_integer_lane_ops_rejected)
{
    /* % / shifts / bitwise are integer-only: a float operand is a violation,
       both plain and compound. */
    EXPECT_BUILD_FAIL("int main(void) { double a, b; return (int) (a % b); }");
    EXPECT_BUILD_FAIL("int main(void) { double a; return (int) (a & 1); }");
    EXPECT_BUILD_FAIL("int main(void) { double a; return (int) (a << 1); }");
    EXPECT_BUILD_FAIL("int main(void) { double a; return (int) ~a; }");
    EXPECT_BUILD_FAIL("int main(void) { double a; a %= 1.0; return 0; }");
    EXPECT_BUILD_FAIL("int main(void) { double a; a <<= 1; return 0; }");
    EXPECT_BUILD_FAIL("int main(void) { double a; float b; return (int) (a & b); }");
    /* A float on either side of a shift/remainder/bitwise op is rejected. */
    EXPECT_BUILD_FAIL("int main(void) { int x = 4; return (int) (x << 1.5); }");
    EXPECT_BUILD_FAIL("int main(void) { return (int) (1.5 << 2); }");
    EXPECT_BUILD_FAIL("int main(void) { double d; return (int) (d % 2); }");
    EXPECT_BUILD_FAIL("int main(void) { double d; return (int) (d & 1.5); }");
}

TEST(float, fp_integer_only_contexts_reject_floats)
{
    /* A float never satisfies an integer constant-expression slot. */
    EXPECT_BUILD_FAIL("int main(void) { switch (1) { case 1.5: break; } return 0; }");
    EXPECT_BUILD_FAIL("_Static_assert(1.5, \"nope\");\n"
                      "int main(void) { return 0; }");
    EXPECT_PARSE_FAIL("int a[1.5];");
    EXPECT_PARSE_FAIL("enum E { A = 1.5 };");
    EXPECT_PARSE_FAIL("_Alignas(1.5) int x;");
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