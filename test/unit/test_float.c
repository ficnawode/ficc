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
    /* Block-scope FP initializers build. */
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
    /* File-scope and static FP globals serialize through the init machinery. */
    EXPECT_BUILD_SUCCEED("double g = 1.5;\n");
    EXPECT_BUILD_SUCCEED("static double s = 1.5;\n");
}

TEST(float, file_scope_decl_without_init_ok)
{
    EXPECT_BUILD_SUCCEED("double g;\n"
                         "float h;\n"
                         "int main(void) { return 0; }\n");
}

TEST(float, long_double_type_layout)
{
    /* 80-bit x87 double-extended stored in a 16-byte slot (SysV). */
    Type *ld = type_long_double();
    EXPECT_EQ(ld->kind, TYPE_LONG_DOUBLE);
    EXPECT_EQ(ld->width, 128);
    EXPECT_EQ(ld->align, 16);
    EXPECT_EQ(ld->size, 16);
    EXPECT_TRUE(type_is_complete(ld));
    EXPECT_TRUE(type_is_fp(ld));
    EXPECT_FALSE(type_is_float(ld)); /* x87, not an SSE lane */
    EXPECT_FALSE(type_is_integer(ld));
    EXPECT_TRUE(type_promote(ld) == ld);
}

TEST(float, long_double_literals_and_decls)
{
    /* `1.5L` is a long-double literal; `long double` parses as a type. */
    EXPECT_BUILD_SUCCEED("long double x;\n"
                         "int main(void) { return 0; }\n");
    EXPECT_BUILD_SUCCEED("long double x = 1.5L;\n"
                         "int main(void) { return 0; }\n");
    EXPECT_BUILD_SUCCEED("double d = 1.5L;\n"
                         "float f = 1.5L;\n"
                         "int main(void) { return 0; }\n");
    EXPECT_BUILD_SUCCEED("const long double g = 2.5L;\n"
                         "static long double s = 1.5L;\n"
                         "int main(void) { return 0; }\n");
    /* The usual arithmetic conversions put long double at the top. */
    EXPECT_TRUE(type_common(type_long_double(), type_double()) == type_long_double());
    EXPECT_TRUE(type_common(type_float(), type_long_double()) == type_long_double());
    EXPECT_TRUE(type_common(type_int(), type_long_double()) == type_long_double());
    EXPECT_TRUE(type_common(type_long_double(), type_long_double()) == type_long_double());
    EXPECT_TRUE(type_common(type_double(), type_double()) == type_double());
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
    EXPECT_BUILD_FAIL("int main(void) { int a[1.5]; return 0; }");
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

/* SysV FP ABI and file-scope FP globals */

TEST(float, fp_args_mixed_and_overflow_both_backends)
{
    /* Independent GP/SSE counters; >8 floats spill to the stack in argument order. */
    EXPECT_INTERP_AND_ELF("double add(double a, double b) { return a + b; }\n"
                          "int main(void) { return (int)add(1.5, 2.5); }\n",
                          4);
    EXPECT_INTERP_AND_ELF("double mix(int a, double b, int c, double d)\n"
                          "{ return a + b + c + d; }\n"
                          "int main(void) { double r = mix(1, 2.0, 3, 4.0);\n"
                          "    if (r != 10.0) return 1; return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("double sum10(double a,double b,double c,double d,double e,double f,\n"
                          "double g,double h,double i,double j){ return a+b+c+d+e+f+g+h+i+j; }\n"
                          "int main(void){ double r = sum10(1,2,3,4,5,6,7,8,9,10);\n"
                          "    if (r != 55.0) return 1; return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("int probe(int a,double b,int c,double d,int e,double f,int g,\n"
                          "double h,int i,double j){ return (int)(a+b+c+d+e+f+g+h+i+j); }\n"
                          "int main(void){ if (probe(1,1.0,1,1.0,1,1.0,1,1.0,1,1.0) != 10)\n"
                          "    return 1; if (probe(1,2,3,4,5,6,7,8,9,10) != 55) return 2;\n"
                          "    return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("float fmul(float a, float b) { return a * b; }\n"
                          "int main(void){ float r = fmul(1.5f, 2.5f);\n"
                          "    if (r != 3.75f) return 1; return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("double ret(double x) { return x; }\n"
                          "int main(void){ return (int)(ret(42.0)); }\n",
                          42);
}

TEST(float, fp_function_pointers)
{
    EXPECT_INTERP_AND_ELF("double add(double a, double b) { return a + b; }\n"
                          "double sub(double a, double b) { return a - b; }\n"
                          "int main(void){ double (*f)(double,double) = add;\n"
                          "    if (f(1.5, 2.5) != 4.0) return 1;\n"
                          "    f = sub;\n"
                          "    if (f(2.5, 1.5) != 1.0) return 2;\n"
                          "    return 42; }\n",
                          42);
}

TEST(float, variadic_fp_sums)
{
    EXPECT_INTERP_AND_ELF("double sum(int n, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    double s = 0.0;\n"
                          "    for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){ double r = sum(4, 1.5, 2.5, 3.5, 4.5);\n"
                          "    if (r != 12.0) return 1; return 42; }\n",
                          42);
    /* Nine trailing doubles cross the SSE save area. */
    EXPECT_INTERP_AND_ELF(
        "double sum(int n, ...)\n"
        "{\n"
        "    __builtin_va_list ap;\n"
        "    __builtin_va_start(ap, n);\n"
        "    double s = 0.0;\n"
        "    for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, double);\n"
        "    __builtin_va_end(ap);\n"
        "    return s;\n"
        "}\n"
        "int main(void){ double r = sum(9, 1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0);\n"
        "    if ((int)(r * 10) != 450) return 1; return 42; }\n",
        42);
}

TEST(float, variadic_fp_float_promotes_and_narrows)
{
    /* A float tail arg promotes to double at the call and va_arg(ap, float) narrows. */
    EXPECT_INTERP_AND_ELF("float sumf(int n, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    float s = 0.0f;\n"
                          "    for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, float);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){ float r = sumf(3, 0.5f, 1.5f, 2.5f);\n"
                          "    if (r != 4.5f) return 1; return 42; }\n",
                          42);
    /* 9 floats → 9 doubles: the last one overflows the SSE save area, narrowing still applies. */
    EXPECT_INTERP_AND_ELF("float sumf(int n, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    float s = 0.0f;\n"
                          "    for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, float);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){ float r = sumf(9, 0.5f,1.5f,2.5f,3.5f,4.5f,5.5f,\n"
                          "                                 6.5f,7.5f,8.5f);\n"
                          "    if (r != 40.5f) return 1; return 42; }\n",
                          42);
}

TEST(float, variadic_fp_named_params_shift_fp_offset)
{
    /* Fixed FP params shift fp_offset to 48 + 16×named, not 48. */
    EXPECT_INTERP_AND_ELF("double vaf(double first, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, first);\n"
                          "    double s = first;\n"
                          "    s += __builtin_va_arg(ap, double);\n"
                          "    s += __builtin_va_arg(ap, double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){ if (vaf(1.0, 2.0, 3.0) != 6.0) return 1;\n"
                          "    if (vaf(10.0, 20.0, 30.0) != 60.0) return 2; return 42; }\n",
                          42);
    /* Two fixed FP params: the variadic tail starts at slot 80. */
    EXPECT_INTERP_AND_ELF("double vad(double a, double b, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, b);\n"
                          "    return a + b + __builtin_va_arg(ap, double);\n"
                          "}\n"
                          "int main(void){ if (vad(1.0, 2.0, 3.0) != 6.0) return 1; return 42; }\n",
                          42);
}

TEST(float, variadic_fp_mixed_and_helper_reads)
{
    /* Mixed int/double reads in argument order; a va_list walked inside a helper. */
    EXPECT_INTERP_AND_ELF("double read2(__builtin_va_list ap)\n"
                          "{\n"
                          "    double a = __builtin_va_arg(ap, double);\n"
                          "    double b = __builtin_va_arg(ap, double);\n"
                          "    return a * 100 + b;\n"
                          "}\n"
                          "double via_helper(double first, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, first);\n"
                          "    double s = read2(ap);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){ if (via_helper(1.5, 2.0, 3.0) != 203.0) return 1;\n"
                          "    return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("int named_overflow(int a,int b,int c,int d,int e,int f,int g,\n"
                          "double x, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, x);\n"
                          "    double tail = __builtin_va_arg(ap, double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return (int)(a + b + c + d + e + f + g - tail);\n"
                          "}\n"
                          "int main(void){ if (named_overflow(1,1,1,1,1,1,1,100.0,200.0) !=\n"
                          "    7 - 200) return 1; return 42; }\n",
                          42);
}

TEST(float, file_scope_fp_globals)
{
    /* .data/.rodata/.bss globals, folded const exprs, and the surviving -0.0 sign. */
    EXPECT_INTERP_AND_ELF("double g = 1.5;\n"
                          "double gsum = 1.5 + 2.5;\n"
                          "const double gc = 6.25;\n"
                          "double gzero;\n"
                          "double gnzero = -0.0;\n"
                          "int main(void){\n"
                          "    if (g != 1.5) return 1;\n"
                          "    if (gsum != 4.0) return 2;\n"
                          "    if (gc != 6.25) return 3;\n"
                          "    if (gzero != 0.0) return 4;\n"
                          "    if (gnzero != 0.0 || !(1.0 / gnzero < 0.0)) return 5;\n"
                          "    return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("float f = 0.25f;\n"
                          "float gdiv = 1.0f / 3.0f;\n"
                          "double garr[3] = {1.5, 2.5, 3.5};\n"
                          "float farr[4] = {0.5f, 1.5f, 2.5f, 3.5f};\n"
                          "struct P { double x; int i; double y; } gr = {1.5, 7, 2.5};\n"
                          "int main(void){\n"
                          "    if (f != 0.25f) return 1;\n"
                          "    if (gdiv * 3.0f != 1.0f) return 2;\n"
                          "    if (garr[1] != 2.5) return 3;\n"
                          "    if (farr[3] != 3.5f) return 4;\n"
                          "    if (gr.x != 1.5 || gr.i != 7 || gr.y != 2.5) return 5;\n"
                          "    if (gr.x + gr.y != 4.0) return 6;\n"
                          "    return 42; }\n",
                          42);
    EXPECT_INTERP_AND_ELF("int main(void)\n"
                          "{\n"
                          "    static double blk = 1.75;\n"
                          "    static float fb = 0.5f;\n"
                          "    if (blk != 1.75 || fb != 0.5f) return 1;\n"
                          "    return 42; }\n",
                          42);
}

TEST(float, file_scope_fp_nonconstant_rejected)
{
    EXPECT_BUILD_FAIL("double g = 1.5;\n"
                      "static double x = g;\n"
                      "int main(void) { return 0; }\n");
    EXPECT_BUILD_FAIL("static double y = x;\n"
                      "double x;\n"
                      "int main(void) { return 0; }\n");
}

/* long double: type, literals, constants, value model (storage-only) */

TEST(float, long_double_sizeof_alignof_folded)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (sizeof(long double) != 16) return 1;\n"
                          "    if (_Alignof(long double) != 16) return 2;\n"
                          "    struct S { char c; long double d; int i; };\n"
                          "    if (sizeof(struct S) != 48) return 3;\n"
                          "    if (_Alignof(struct S) != 16) return 4;\n"
                          "    unsigned long off = (unsigned long) &((struct S *) 0)->d;\n"
                          "    if (off != 16) return 5;\n"
                          "    off = (unsigned long) &((struct S *) 0)->i;\n"
                          "    if (off != 32) return 6;\n"
                          "    long double a[2];\n"
                          "    if (sizeof(a) != 32) return 7;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_global_init_bytes)
{
    /* 1.5L = fraction 0xC000000000000000 @ exp 0x3FFF; padding stays zero. */
    EXPECT_INTERP_AND_ELF("long double g = 1.5L;\n"
                          "long double gsum = 1.5L + 2.5L;\n" /* 4.0L @ 0x4001 */
                          "const long double gc = -2.5L;\n"
                          "int main(void) {\n"
                          "    unsigned long long lo = *(unsigned long long *) &g;\n"
                          "    unsigned long long hi = *(unsigned long long *) ((char *) &g + 8);\n"
                          "    if (lo != 0xC000000000000000ULL) return 1;\n"
                          "    if (hi != 0x3FFFULL) return 2;\n"
                          "    unsigned char *p = (unsigned char *) &g;\n"
                          "    if (p[9] != 0x3F || p[10] != 0 || p[15] != 0) return 3;\n"
                          "    lo = *(unsigned long long *) &gsum;\n"
                          "    hi = *(unsigned long long *) ((char *) &gsum + 8);\n"
                          "    if (lo != 0x8000000000000000ULL) return 4;\n"
                          "    if (hi != 0x4001ULL) return 5;\n"
                          "    hi = *(unsigned long long *) ((char *) &gc + 8);\n"
                          "    if (hi != 0xC000ULL) return 6;\n" /* sign bit in byte 9 */
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_block_scope_copy)
{
    /* Address-taken locals store/load the full 16-byte value. */
    EXPECT_INTERP_AND_ELF("long double g = 1.5L;\n"
                          "int main(void) {\n"
                          "    long double x = 0.0L;\n"
                          "    x = g;\n"
                          "    unsigned long long lo = *(unsigned long long *) &x;\n"
                          "    if (lo != 0xC000000000000000ULL) return 1;\n"
                          "    long double y = g;\n" /* SSA value, no address taken */
                          "    if (*(unsigned long long *) &y != 0xC000000000000000ULL) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_width16_phi_store)
{
    /* A ternary over two width-16 globals feeds a global store through a phi. */
    EXPECT_INTERP_AND_ELF(
        "long double ga = 1.5L;\n"
        "long double gb = 2.5L;\n"
        "long double gout;\n"
        "int main(void) {\n"
        "    int c = *(unsigned char *) ((char *) &ga + 9) == 0x3F;\n"
        "    gout = c ? ga : gb;\n"
        "    unsigned long long lo = *(unsigned long long *) &gout;\n"
        "    if (lo != 0xC000000000000000ULL) return 1;\n"
        "    unsigned long long hi = *(unsigned long long *) ((char *) &gout + 8);\n"
        "    if (hi != 0x3FFFULL) return 2;\n"
        "    gout = c ? gb : ga;\n"
        "    if (*(unsigned long long *) &gout != 0xA000000000000000ULL) return 3;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_struct_field_storage)
{
    EXPECT_INTERP_AND_ELF("struct S { char c; long double d; int i; };\n"
                          "struct S s = {1, 2.5L, 3};\n"
                          "int main(void) {\n"
                          "    unsigned long long lo =\n"
                          "        *(unsigned long long *) ((char *) &s + 16);\n"
                          "    if (lo != 0xA000000000000000ULL) return 1;\n" /* 2.5L */
                          "    if (*(int *) ((char *) &s + 32) != 3) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_static_block_and_literal_typed_right)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    static long double bs = 4.5L;\n"
        "    long double lit = 1.5L;\n"
        "    unsigned long long lo = *(unsigned long long *) &bs;\n"
        "    unsigned long long hi = *(unsigned long long *) ((char *) &bs + 8);\n"
        "    if (lo != 0x9000000000000000ULL) return 1;\n" /* 4.5L */
        "    if (hi != 0x4001ULL) return 2;\n"
        "    if (*(unsigned long long *) &lit != 0xC000000000000000ULL) return 3;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_cast_matrix_builds)
{
    /* i/d/ld casts type correctly and build; their runtime lowering is idle. */
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    long double a = (long double) 3;\n"
                         "    long double b = 1.5;\n"
                         "    double c = (double) a;\n"
                         "    float d = (float) a;\n"
                         "    int e = (int) a;\n"
                         "    a = (long double) 1.5f;\n"
                         "    return 0;\n"
                         "}\n");
    EXPECT_BUILD_SUCCEED("long double g = (long double) 7;\n"
                         "long double h = 1.5f;\n"
                         "int main(void) { return 0; }\n");
}

/* 19f: x87 arithmetic / compares / converts (both backends) */

TEST(float, long_double_arith_both_backends)
{
    /* add/sub/mul/div on the x87 stack, compared via width-16 FCMP. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double a = 1.5L;\n"
                          "    long double b = 2.5L;\n"
                          "    if (a + b != 4.0L) return 1;\n"
                          "    if (b - a != 1.0L) return 2;\n"
                          "    if (a * b != 3.75L) return 3;\n"
                          "    if (b / a != 1.6666666666666666666L) return 4;\n"
                          "    long double c = (a + b) * b - a; /* 8.5 */\n"
                          "    if (c != 8.5L) return 5;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_arith_bit_exact)
{
    /* Independent gcc oracle: the 80-bit patterns these chains produce. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double s = 0.0L;\n"
                          "    s = 0.1L + 0.2L;\n"
                          "    if (*(unsigned long long *) &s != 0x999999999999999aULL) return 1;\n"
                          "    if (*(unsigned short *) ((char *) &s + 8) != 0x3FFD) return 2;\n"
                          "    s = 3.0L / 7.0L;\n"
                          "    if (*(unsigned long long *) &s != 0xdb6db6db6db6db6eULL) return 3;\n"
                          "    if (*(unsigned short *) ((char *) &s + 8) != 0x3FFD) return 4;\n"
                          "    s = 1.0L / 3.0L;\n"
                          "    if (*(unsigned long long *) &s != 0xaaaaaaaaaaaaaaabULL) return 5;\n"
                          "    int i;\n"
                          "    s = 0.0L;\n"
                          "    for (i = 0; i < 10; i++) { s = s + 0.1L; }\n"
                          "    if (*(unsigned long long *) &s != 0x8000000000000001ULL) return 6;\n"
                          "    if (*(unsigned short *) ((char *) &s + 8) != 0x3FFF) return 7;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_precision_beyond_double)
{
    /* More than 53 significant bits distinguish ld from double. */
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    long double a = 0x1.0000000000000002p0L;\n"
        "    if (*(unsigned long long *) &a != 0x8000000000000001ULL) return 1;\n"
        "    if (*(unsigned short *) ((char *) &a + 8) != 0x3FFF) return 2;\n"
        "    if (a == (long double) (double) a) return 3;       /* ld keeps the bit */\n"
        "    if (0.1L == (long double) 0.1) return 4;\n"
        "    /* accumulate in ld vs double: the results diverge */\n"
        "    long double s = 0.0L;\n"
        "    double d = 0.0;\n"
        "    int i;\n"
        "    for (i = 0; i < 10; i++) { s = s + 0.1L; d = d + 0.1; }\n"
        "    if (s == (long double) d) return 5;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_compare_matrix)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double a = 2.0L;\n"
                          "    long double b = 3.0L;\n"
                          "    if (!(a < b)) return 1;\n"
                          "    if (!(a <= b)) return 2;\n"
                          "    if (!(a <= 2.0L)) return 3;\n"
                          "    if (!(b > a)) return 4;\n"
                          "    if (!(b >= a)) return 5;\n"
                          "    if (!(b >= 3.0L)) return 6;\n"
                          "    if (!(a == 2.0L)) return 7;\n"
                          "    if (!(a != b)) return 8;\n"
                          "    if (a == b) return 9;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_nan_compare_semantics)
{
    /* NaN is unequal to everything; every ordered predicate is false. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double z = 0.0L;\n"
                          "    long double nan = z / z;\n"
                          "    if (nan == nan) return 1;\n"
                          "    if (!(nan != nan)) return 2;\n"
                          "    if (nan < 1.0L) return 3;\n"
                          "    if (nan > 1.0L) return 4;\n"
                          "    if (nan <= 1.0L) return 5;\n"
                          "    if (nan >= 1.0L) return 6;\n"
                          "    if (!(nan != 1.0L)) return 7;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_boolify_and_conditions)
{
    /* -0.0L falsy, NaN truthy, through FCMP_NE/FCMP_EQ. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double nz = -0.0L;\n"
                          "    if (nz) return 1;\n"
                          "    if (!(!nz)) return 2;\n"
                          "    if (!(1.0L)) return 3;\n"
                          "    long double z = 0.0L;\n"
                          "    long double nan = z / z;\n"
                          "    if (!nan) return 4;\n"
                          "    if (!(nan || 0.0L)) return 5;\n"
                          "    if (nan && 0.0L) return 6;\n"
                          "    _Bool b = nz;\n"
                          "    if (b) return 7;\n"
                          "    b = nan;\n"
                          "    if (!b) return 8;\n"
                          "    if ((nz ? 7 : 9) != 9) return 9;\n"
                          "    if ((1.0L ? 7 : 9) != 7) return 10;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_unary_and_compound)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double a = 1.5L;\n"
                          "    if (-a != -1.5L) return 1;\n"
                          "    if (*(unsigned short *) ((char *) &a + 8) != 0x3FFF) return 2;\n"
                          "    a += 2.5L;\n"
                          "    if (a != 4.0L) return 3;\n"
                          "    a *= 2.0L;\n"
                          "    if (a != 8.0L) return 4;\n"
                          "    a -= 1.0L;\n"
                          "    if (a != 7.0L) return 5;\n"
                          "    a /= 2.0L;\n"
                          "    if (a != 3.5L) return 6;\n"
                          "    a++;\n"
                          "    if (a != 4.5L) return 7;\n"
                          "    --a;\n"
                          "    if (a != 3.5L) return 8;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_cast_matrix_runtime)
{
    /* The i/d/f ↔ ld cast matrix now lowers on x87. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double a = (long double) 3;\n"
                          "    if (a != 3.0L) return 1;\n"
                          "    long double b = (long double) -7LL;\n"
                          "    if (b != -7.0L) return 2;\n"
                          "    long double c = (long double) 1.5f;\n"
                          "    if (*(unsigned long long *) &c != 0xC000000000000000ULL) return 3;\n"
                          "    long double d = (long double) 1.5;\n"
                          "    if (*(unsigned long long *) &d != 0xC000000000000000ULL) return 4;\n"
                          "    if ((double) 4.25L != 4.25) return 5;\n"
                          "    if ((float) 4.25L != 4.25f) return 6;\n"
                          "    if ((int) 4.25L != 4) return 7;\n"
                          "    if ((long long) -4.5L != -4) return 8;\n"
                          "    if ((unsigned) 4.25L != 4U) return 9;\n"
                          "    if ((unsigned long long) 4.25L != 4ULL) return 10;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_int64_edges)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    long double lo = (long double) (-9223372036854775807LL - 1);\n"
        "    if (*(unsigned long long *) &lo != 0x8000000000000000ULL) return 1;\n"
        "    if (*(unsigned short *) ((char *) &lo + 8) != 0xC03E) return 2;\n"
        "    long double hi = (long double) 9223372036854775807LL;\n"
        "    if (*(unsigned long long *) &hi != 0xFFFFFFFFFFFFFFFEULL) return 3;\n"
        "    if (*(unsigned short *) ((char *) &hi + 8) != 0x403D) return 4;\n"
        "    if (hi != 9223372036854775807.0L) return 5;\n"
        "    if (lo != -9223372036854775807.0L - 1.0L) return 6;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_u64_edge)
{
    /* u64 ≥ 2^63 takes the add-back-2^63 path. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double a = (long double) 9223372036854775808ULL;   /* 2^63 */\n"
                          "    if (*(unsigned long long *) &a != 0x8000000000000000ULL) return 1;\n"
                          "    if (*(unsigned short *) ((char *) &a + 8) != 0x403E) return 2;\n"
                          "    long double b = (long double) 0xFFFFFFFFFFFFFFFFULL;\n"
                          "    if (*(unsigned long long *) &b != 0xFFFFFFFFFFFFFFFFULL) return 3;\n"
                          "    if (*(unsigned short *) ((char *) &b + 8) != 0x403E) return 4;\n"
                          "    long double c = (long double) 0x8000000000000005ULL;\n"
                          "    if (*(unsigned long long *) &c != 0x8000000000000005ULL) return 5;\n"
                          "    if ((unsigned long long) b != 0xFFFFFFFFFFFFFFFFULL) return 6;\n"
                          "    if ((unsigned long long) a != 0x8000000000000000ULL) return 7;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_ftoi_brands)
{
    /* NaN / out-of-range brand INT_MIN / 0x80000000. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double z = 0.0L;\n"
                          "    long double nan = z / z;\n"
                          "    if ((int) nan != -2147483647 - 1) return 1;\n"
                          "    if ((long long) nan != (-9223372036854775807LL - 1)) return 2;\n"
                          "    if ((unsigned) nan != 0U) return 3;\n"
                          "    if ((unsigned long long) nan != 0x8000000000000000ULL) return 4;\n"
                          "    if ((int) 1e30L != -2147483647 - 1) return 5;\n"
                          "    if ((unsigned) 1e30L != 0U) return 6;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_mixed_precision_expressions)
{
    /* §6.3.1.8: a long double operand pulls the whole expression to ld. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long double m = 1.5L + 2.5 + 0.5f;   /* 4.5 */\n"
                          "    if (m != 4.5L) return 1;\n"
                          "    if (*(unsigned long long *) &m != 0x9000000000000000ULL) return 2;\n"
                          "    if (3.0L * 7 != 21.0L) return 3;\n"
                          "    if (21.0L / 4 != 5.25L) return 4;\n"
                          "    if ((2.5f + 1) * 2.0 + 0.0L != 7.0L) return 5;\n"
                          "    long double a = 1.0L;\n"
                          "    double b = 2.0;\n"
                          "    float c = 3.0f;\n"
                          "    if (a + b + c != 6.0L) return 6;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(float, long_double_subnormal_and_hex_literals)
{
    /* Hex floats are correctly rounded; 2^-16445 is the smallest positive
       extended subnormal (exponent field 0, significand 1). */
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    long double sub = 0x1p-16445L;\n"
        "    if (*(unsigned long long *) &sub != 0x0000000000000001ULL) return 1;\n"
        "    if (*(unsigned short *) ((char *) &sub + 8) != 0x0000) return 2;\n"
        "    if (sub == 0.0L) return 3;\n"
        "    long double sub2 = 0x1p-16444L;\n"
        "    if (*(unsigned long long *) &sub2 != 0x0000000000000002ULL) return 4;\n"
        "    if (sub2 <= sub) return 5;\n"
        "    /* 65 significant bits correctly round to 2.0 in 80-bit */\n"
        "    long double two = 0x1.ffffffffffffffffp0L;\n"
        "    if (two != 2.0L) return 6;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_abi_args_and_returns)
{
    /* Stack-passed ld args and %st0 returns, incl. mixed int/double/float calls. */
    EXPECT_INTERP_AND_ELF(
        "long double mul(long double a, long double b)\n"
        "{ return a * b + 1.0L; }\n"
        "double grad(int i, double d, long double ld, float f, long double l2)\n"
        "{ return (double) (i + d + ld + (long double) f + l2); }\n"
        "int main(void){\n"
        "    if (mul(1.5L, 2.5L) != 4.75L) return 1;\n"
        "    if (mul(-3.0L, 4.0L) != -11.0L) return 2;\n"
        "    if (grad(2, 1.5, 2.5L, 1.25f, 5.0L) != 12.25) return 3;\n"
        "    if (mul(mul(2.0L, 3.0L), 2.0L) != 15.0L) return 4;\n"
        "    long double r = mul(100.0L, 0.5L);\n"
        "    if (r != 51.0L) return 5;\n"
        "    long double eight = mul(1.0L, 7.0L);\n"
        "    if (*(unsigned long long *) &eight != 0x8000000000000000ULL) return 6;\n"
        "    if (*(unsigned short *) ((char *) &eight + 8) != 0x4002) return 7;\n"
        "    return 42; }\n",
        42);
    /* Six ints fill GP, then a stack ld between two stack ints. */
    EXPECT_INTERP_AND_ELF(
        "long double seven(int a, int b, int c, int d, int e, int f, long double g, int h,\n"
        "                   int i)\n"
        "{ return a + b + c + d + e + f + g + h + i; }\n"
        "int main(void){\n"
        "    if (seven(1, 2, 3, 4, 5, 6, 7.5L, 8, 9) != 45.5L) return 1;\n"
        "    if (seven(0, 0, 0, 0, 0, 0, -1.25L, 0, 0) != -1.25L) return 2;\n"
        "    return 42; }\n",
        42);
    /* A %st0 return read before the caller's next x87 op. */
    EXPECT_INTERP_AND_ELF("long double f(long double a) { return a + 1.0L; }\n"
                          "long double g(long double a) { return a * 2.0L; }\n"
                          "int main(void){\n"
                          "    long double x = f(g(1.5L));\n"
                          "    if (x != 4.0L) return 1;\n"
                          "    long double y = g(f(0.5L)) + f(g(2.0L));\n"
                          "    if (y != 8.0L) return 2;\n"
                          "    return 42; }\n",
                          42);
}

TEST(float, long_double_varargs_overflow_only)
{
    /* va_arg(ap, long double) reads only the overflow area. */
    EXPECT_INTERP_AND_ELF(
        "long double vsum(int n, ...)\n"
        "{\n"
        "    __builtin_va_list ap;\n"
        "    __builtin_va_start(ap, n);\n"
        "    long double s = 0.0L;\n"
        "    int i;\n"
        "    for (i = 0; i < n; i++) s += __builtin_va_arg(ap, long double);\n"
        "    __builtin_va_end(ap);\n"
        "    return s;\n"
        "}\n"
        "int main(void){\n"
        "    if (vsum(3, 1.0L, 2.0L, 3.0L) != 6.0L) return 1;\n"
        "    if (vsum(1, -0.5L) != -0.5L) return 2;\n"
        "    if (vsum(2, 10.0L, 20.0L) != 30.0L) return 3;\n"
        "    /* 9 tail ld args: 16-byte slots, 144 overflow bytes. */\n"
        "    if (vsum(9, 1.0L,2.0L,3.0L,4.0L,5.0L,6.0L,7.0L,8.0L,9.0L) != 45.0L)\n"
        "        return 4;\n"
        "    return 42; }\n",
        42);
    /* Named ld params shift the overflow pointer by their 16-byte layout. */
    EXPECT_INTERP_AND_ELF("long double vsum(int n, long double named, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, named);\n"
                          "    long double s = named;\n"
                          "    int i;\n"
                          "    for (i = 0; i < n; i++) s += __builtin_va_arg(ap, long double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){\n"
                          "    if (vsum(3, 0.5L, 1.0L, 2.0L, 3.0L) != 6.5L) return 1;\n"
                          "    if (vsum(1, 100.0L, 1.0L) != 101.0L) return 2;\n"
                          "    return 42; }\n",
                          42);
    /* Mixed tail reads; the ld walk aligns up mid-sequence. */
    EXPECT_INTERP_AND_ELF("long double vmix(int n, ...)\n"
                          "{\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    long double s = 0.0L;\n"
                          "    int i;\n"
                          "    for (i = 0; i < n; i++)\n"
                          "    {\n"
                          "        if (i == 0) s += __builtin_va_arg(ap, int);\n"
                          "        else if (i == 1) s += __builtin_va_arg(ap, double);\n"
                          "        else s += __builtin_va_arg(ap, long double);\n"
                          "    }\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void){\n"
                          "    if (vmix(3, 7, 1.5, 2.5L) != 11.0L) return 1;\n"
                          "    if (vmix(3, 1, 2.0, 0.5L) != 3.5L) return 2;\n"
                          "    return 42; }\n",
                          42);
}

TEST(float, float_h_limits)
{
    /* <float.h> limits round-trip through the lexer at each suffix's width. */
    EXPECT_INTERP_AND_ELF(
        "#include <float.h>\n"
        "int main(void) {\n"
        "    if (FLT_RADIX != 2) return 1;\n"
        "    if (FLT_EVAL_METHOD != 0) return 2;\n"
        "    if (FLT_MANT_DIG != 24 || DBL_MANT_DIG != 53 || LDBL_MANT_DIG != 64) return 3;\n"
        "    if (FLT_DIG != 6 || DBL_DIG != 15 || LDBL_DIG != 18) return 4;\n"
        "    float epsf = FLT_EPSILON;\n"
        "    double epsd = DBL_EPSILON;\n"
        "    long double epsl = LDBL_EPSILON;\n"
        "    if (1.0f + epsf == 1.0f) return 5;\n"
        "    if (1.0 + epsd == 1.0) return 6;\n"
        "    if (1.0L + epsl == 1.0L) return 7;\n"
        "    if (*(unsigned *) &epsf != 0x34000000U) return 8; /* 2^-23 */\n"
        "    float bigf = FLT_MAX; if (!(bigf > 1e30f)) return 9;\n"
        "    double bigd = DBL_MAX; if (!(bigd > 1e300)) return 10;\n"
        "    long double bigl = LDBL_MAX; if (!(bigl > 1e4900L)) return 11;\n"
        "    long double minl = LDBL_MIN;\n"
        "    if (!(minl > 0.0L)) return 12;\n"
        "    if (*(unsigned long long *) &minl == 0ULL) return 13;\n"
        "    if (FLT_DECIMAL_DIG != 9 || DBL_DECIMAL_DIG != 17 || LDBL_DECIMAL_DIG != 21)\n"
        "        return 14;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(float, long_double_narrow_does_not_leak_x87_stack)
{
    /* Each `long double -> double/float` conversion must pop its x87 operand;
       a leak overflows the 8-entry stack and yields NaN after a few steps. */
    EXPECT_INTERP_AND_ELF("int main(void){ long double x = 1.5L; double s = 0.0; int i;"
                          " for (i = 0; i < 100; i++) s += (double) x;"
                          " return (int) s; }",
                          150);
    EXPECT_INTERP_AND_ELF("int main(void){ long double x = 1.5L; float f = 0.0f; int i;"
                          " for (i = 0; i < 100; i++) f += (float) x;"
                          " return (int) f; }",
                          150);
}
