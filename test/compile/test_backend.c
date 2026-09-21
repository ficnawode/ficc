#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <unistd.h>

/* Compiles each value-path fixture on both backends and requires the same
   exit code from the linked program at -O0 and -O1. */

static unsigned int backend_seq;

static void backend_path(char *buf, size_t sz, const char *backend, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_backend_%u_%s.%s", backend_seq++, backend, ext);
}

static void backend_write_src(char *out, size_t sz, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_backend_%u_src.c", backend_seq++);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void backend_run_one(const char *src_path, const char *backend, const char *level,
                            int expected)
{
    char obj[256], bin[256], cmd[2048];
    backend_path(obj, sizeof(obj), backend, "o");
    backend_path(bin, sizeof(bin), backend, "bin");

    snprintf(cmd, sizeof(cmd), "%s -backend %s %s -c %s -o %s >/dev/null 2>&1", FICC_BIN, backend,
             level, src_path, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);
    if (rc != 0)
    {
        return;
    }
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj, bin, bin);
    rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, expected);

    unlink(obj);
    unlink(bin);
}

static void backend_run_level(const char *src, const char *level, int expected)
{
    char src_path[256];
    backend_write_src(src_path, sizeof(src_path), src);
    backend_run_one(src_path, "stack", level, expected);
    backend_run_one(src_path, "linear", level, expected);
    unlink(src_path);
}

static void backend_run(const char *src, int expected)
{
    backend_run_level(src, "-O0", expected);
    backend_run_level(src, "-O1", expected);
}

/* The in-process oracle: same fixture through both backends via the testdriver. */
static void backend_run_inproc(const char *src, int expected)
{
    tc_set_codegen_backend(CG_STACK);
    EXPECT_EQ(tc_run_elf(src), expected);
    tc_set_codegen_backend(CG_LINEAR);
    EXPECT_EQ(tc_run_elf(src), expected);
    tc_set_codegen_backend(CG_STACK);
}

TEST(backend, return_literal)
{
    backend_run("int main(void) { return 40 + 2; }\n", 42);
}

TEST(backend, local_arithmetic)
{
    backend_run("int main(void) { int a = 20; int b = 3; return a * b + a / b + a % b; }\n", 68);
}

TEST(backend, division_and_remainder)
{
    backend_run("int main(void) { int a = 100; int b = 7; return a / b * 7 + a % b; }\n", 100);
}

TEST(backend, unsigned_division)
{
    backend_run("int main(void) { unsigned a = 4000000000u; unsigned b = 7u;\n"
                "                return (int) (a / b); }\n",
                219);
}

TEST(backend, shifts_signed_and_unsigned)
{
    backend_run("int main(void) { int a = -16; int b = a >> 2;\n"
                "                int c = (int) ((unsigned) a >> 1); return b + c; }\n",
                244);
}

TEST(backend, char_truncation_preserves_sign)
{
    backend_run("int main(void) { char c = (char) 200; return c; }\n", 200);
}

TEST(backend, bitops)
{
    backend_run("int main(void) { int x = 42; int y = x & 0xFF; y = y | 0x100;\n"
                "                y = y ^ 0x100; return y; }\n",
                42);
}

TEST(backend, comparisons)
{
    backend_run("int main(void) { unsigned a = 100; unsigned b = 200;\n"
                "                return (a < b) + (a > b) * 10 + 42; }\n",
                43);
}

TEST(backend, long_arithmetic)
{
    backend_run("int main(void) { long a = 1234567890123L; long b = 7; return (int) (a % b); }\n",
                1);
}

TEST(backend, negative_long_immediate)
{
    backend_run("int main(void) { long x = -1L; return (int) x; }\n", 255);
}

TEST(backend, long_immediate_shift_keeps_sign_bits)
{
    backend_run("int main(void) { unsigned long x = 0xFFFFFFFFFFFFFFFFUL;\n"
                "                return (int) (x >> 60); }\n",
                15);
}

TEST(backend, narrow_unsigned_and_signed_loads)
{
    backend_run("int main(void) { unsigned char c = 200; int x = c + 56;\n"
                "                signed char d = -2; return x + d * 3 + 8; }\n",
                2);
}

TEST(backend, array_alloca_gep_load_store)
{
    backend_run("int main(void) { int a[4]; a[0] = 1; a[1] = 2; a[2] = 3; a[3] = 36;\n"
                "                return a[0] + a[1] + a[2] + a[3]; }\n",
                42);
}

TEST(backend, global_load_store)
{
    backend_run("int g;\nint main(void) { g = 21; int x = g; return x * 2; }\n", 42);
}

TEST(backend, pointer_arithmetic)
{
    backend_run("int main(void) { int a[3]; a[0] = 10; a[1] = 20; a[2] = 12;\n"
                "                int *p = a; return p[0] + p[1] + p[2]; }\n",
                42);
}

TEST(backend, parameters_compile)
{
    backend_run("int f(int x, int y) { return x * 3 + y; }\n"
                "int main(void) { return 42; }\n",
                42);
}

TEST(backend, register_pressure_spills)
{
    backend_run("int g0; int g1; int g2; int g3; int g4; int g5; int g6; int g7; int g8;\n"
                "int g9;\n"
                "int main(void) { g0 = 0; g1 = 1; g2 = 2; g3 = 3; g4 = 4; g5 = 5; g6 = 6;\n"
                "                g7 = 7; g8 = 8; g9 = 9;\n"
                "                return g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9; }\n",
                45);
}

TEST(backend, in_process_oracle)
{
    backend_run_inproc("int main(void) { int a = 6; int b = 7; int c[1]; c[0] = a * b;\n"
                       "                       return c[0]; }\n",
                       42);
}

TEST(backend, control_flow_if_else_chain)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = -2; i < 12; i = i + 1) {\n"
                "    if (i < 0) { s = s + 1; }\n"
                "    else if (i == 0) { s = s + 10; }\n"
                "    else if (i < 10) { s = s + 100; }\n"
                "    else { s = s + 1000; } }\n"
                "  return s % 251; }\n",
                151);
}

TEST(backend, control_flow_while_break_continue)
{
    backend_run("int main(void) { int i = 0; int s = 0;\n"
                "  while (i < 100) {\n"
                "    i = i + 1;\n"
                "    if (i % 2 == 0) { continue; }\n"
                "    if (i > 50) { break; }\n"
                "    s = s + i; }\n"
                "  return s % 251; }\n",
                123);
}

TEST(backend, control_flow_do_while)
{
    backend_run("int main(void) { int i = 0; int s = 0;\n"
                "  do { s = s + i; i = i + 1; } while (i < 10);\n"
                "  return s; }\n",
                45);
}

TEST(backend, control_flow_nested_loops)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = 0; i < 6; i = i + 1)\n"
                "    for (int j = 0; j < 7; j = j + 1)\n"
                "      s = s + i * j;\n"
                "  return s; }\n",
                59);
}

TEST(backend, control_flow_dense_switch)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = 0; i < 6; i = i + 1) {\n"
                "    switch (i) {\n"
                "      case 0: s = s + 10; break;\n"
                "      case 1: s = s + 20; break;\n"
                "      case 2: s = s + 30; break;\n"
                "      case 3: s = s + 40; break;\n"
                "      case 4: s = s + 50; break;\n"
                "      default: s = s + 7; break; } }\n"
                "  return s % 251; }\n",
                157);
}

TEST(backend, control_flow_switch_fallthrough)
{
    backend_run("int main(void) { int x = 5; int s = 0;\n"
                "  switch (x) {\n"
                "    case 0: s = s + 1;\n"
                "    case 1: s = s + 2; break;\n"
                "    case 5: s = s + 10;\n"
                "    case 6: s = s + 20; break;\n"
                "    default: s = s + 100; break; }\n"
                "  return s; }\n",
                30);
}

TEST(backend, control_flow_switch_negative)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = -3; i <= 3; i = i + 1) {\n"
                "    switch (i) {\n"
                "      case -3: s = s + 1; break;\n"
                "      case -1: s = s + 10; break;\n"
                "      case 0: s = s + 100; break;\n"
                "      case 2: s = s + 1000; break;\n"
                "      default: s = s + 10000; break; } }\n"
                "  return s % 251; }\n",
                238);
}

TEST(backend, control_flow_sparse_switch_chain)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = 0; i < 1000; i = i + 1) {\n"
                "    switch (i) {\n"
                "      case 3: s = s + 1; break;\n"
                "      case 500: s = s + 2; break;\n"
                "      case 999: s = s + 4; break;\n"
                "      default: break; } }\n"
                "  return s; }\n",
                7);
}

TEST(backend, control_flow_goto_loop)
{
    backend_run("int main(void) { int i = 0; int s = 0;\n"
                "again:\n"
                "  if (i >= 10) { goto done; }\n"
                "  s = s + i * i; i = i + 1;\n"
                "  goto again;\n"
                "done:\n"
                "  return s % 251; }\n",
                34);
}

TEST(backend, control_flow_logical_short_circuit)
{
    backend_run("int main(void) { int s = 0;\n"
                "  for (int i = 0; i < 20; i = i + 1) {\n"
                "    if (i > 2 && i < 15) {\n"
                "      if (i % 3 == 0 || i % 5 == 0) { s = s + i; } }\n"
                "    if (!(i & 1)) { s = s + 100; } }\n"
                "  return s % 251; }\n",
                41);
}

TEST(backend, control_flow_in_process_oracle)
{
    backend_run_inproc("int main(void) { int s = 0;\n"
                       "  for (int i = 0; i < 10; i = i + 1) {\n"
                       "    if (i == 5) { continue; }\n"
                       "    switch (i % 3) { case 0: s = s + 1; break;\n"
                       "                     case 1: s = s + 2; break;\n"
                       "                     default: s = s + 3; break; } }\n"
                       "  return s; }\n",
                       16);
}

TEST(backend, call_direct_and_live_across)
{
    backend_run("int add(int a, int b) { return a + b; }\n"
                "int main(void) { int x = 10; int y = add(20, 30); return x + y; }\n",
                60);
}

TEST(backend, call_nested)
{
    backend_run("int add(int a, int b) { return a + b; }\n"
                "int main(void) { return add(add(1, 2), add(3, 36)); }\n",
                42);
}

TEST(backend, call_stack_arguments)
{
    backend_run("int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {\n"
                "    return a + b + c + d + e + f + g + h; }\n"
                "int main(void) { return sum8(1, 2, 3, 4, 5, 6, 7, 8); }\n",
                36);
}

TEST(backend, call_indirect)
{
    backend_run("int inc(int x) { return x + 1; }\n"
                "int apply(int (*fp)(int), int x) { return fp(x); }\n"
                "int main(void) { return apply(inc, 41); }\n",
                42);
}

TEST(backend, call_recursion)
{
    backend_run("int fact(int n) { return n < 2 ? 1 : n * fact(n - 1); }\n"
                "int main(void) { return fact(5) % 251; }\n",
                120);
}

TEST(backend, call_variadic_sum)
{
    backend_run("int sumv(int n, ...) {\n"
                "    __builtin_va_list ap;\n"
                "    __builtin_va_start(ap, n);\n"
                "    int s = 0;\n"
                "    for (int i = 0; i < n; i = i + 1) { s = s + __builtin_va_arg(ap, int); }\n"
                "    __builtin_va_end(ap);\n"
                "    return s; }\n"
                "int main(void) { return sumv(5, 1, 2, 3, 4, 32); }\n",
                42);
}

TEST(backend, call_in_process_oracle)
{
    backend_run_inproc("int add(int a, int b) { return a + b; }\n"
                       "int main(void) { int x = 20; int y = add(x, 22); return y; }\n",
                       42);
}

TEST(backend, struct_arg_and_return)
{
    backend_run("struct Pair { int a; int b; };\n"
                "struct Pair bump(struct Pair p) { p.a = p.a + 1; p.b = p.b + 1; return p; }\n"
                "int main(void) { struct Pair x; x.a = 10; x.b = 20;\n"
                "                struct Pair y = bump(x);\n"
                "                return x.a + x.b + y.a + y.b; }\n",
                62);
}

TEST(backend, struct_arg_mixed_with_scalars)
{
    backend_run("struct P { int a; int b; };\n"
                "struct P mk(int a, int b) { struct P p; p.a = a; p.b = b; return p; }\n"
                "int g(struct P p, int c) { return p.a + p.b + c; }\n"
                "int main(void) { struct P p = mk(10, 20); return g(p, 12); }\n",
                42);
}

TEST(backend, struct_arg_register_lanes)
{
    backend_run("struct P { int a; int b; };\n"
                "int sum(struct P p) { return p.a + p.b; }\n"
                "int main(void) { struct P p; p.a = 40; p.b = 2; return sum(p); }\n",
                42);
}

TEST(backend, struct_arg_memory_class)
{
    backend_run("struct Big { char a[24]; };\n"
                "int f(struct Big b) { return b.a[0] + b.a[23]; }\n"
                "int main(void) { struct Big b; b.a[0] = 20; b.a[23] = 22; return f(b); }\n",
                42);
}

TEST(backend, struct_arg_int_and_sse_lanes)
{
    backend_run("struct CD { int tag; double d; };\n"
                "int f(struct CD v) { return v.tag; }\n"
                "int main(void) { struct CD v; v.tag = 42; v.d = 99.0; return f(v); }\n",
                42);
}

TEST(backend, struct_arg_overflows_to_stack)
{
    backend_run("struct P { int a; int b; };\n"
                "int h(int a, int b, int c, int d, int e, struct P p) {\n"
                "    return a + b + c + d + e + p.a + p.b; }\n"
                "int main(void) { struct P p; p.a = 10; p.b = 20;\n"
                "                return h(1, 2, 3, 4, 2, p); }\n",
                42);
}

TEST(backend, struct_arg_and_return_with_scalar)
{
    backend_run("struct P { int a; int b; };\n"
                "struct P addp(struct P x, int k) { x.a = x.a + k; x.b = x.b + k; return x; }\n"
                "int main(void) { struct P p; p.a = 10; p.b = 20;\n"
                "                struct P q = addp(p, 6); return q.a + q.b; }\n",
                42);
}

/* Caller-side SysV aggregate placement must match gcc, which is the ABI oracle. */
static void backend_run_gcc_tu(const char *ficc_src, const char *gcc_src, int expected)
{
    char fsrc[256], csrc[256], obj[256], gobj[256], bin[256], cmd[4096];
    backend_write_src(fsrc, sizeof(fsrc), ficc_src);
    backend_path(csrc, sizeof(csrc), "gcc", "c");
    backend_path(obj, sizeof(obj), "linear", "o");
    backend_path(gobj, sizeof(gobj), "gcc", "o");
    backend_path(bin, sizeof(bin), "mix", "bin");

    FILE *f = fopen(csrc, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(gcc_src, f);
        fclose(f);
    }
    snprintf(cmd, sizeof(cmd), "%s -backend linear -O0 %s -c -o %s >/dev/null 2>&1", FICC_BIN, fsrc,
             obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);
    if (rc != 0)
    {
        return;
    }
    snprintf(cmd, sizeof(cmd), "gcc -c %s -o %s >/dev/null 2>&1", csrc, gobj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s %s -o %s >/dev/null 2>&1 && %s", obj, gobj, bin,
             bin);
    EXPECT_EQ(tc_run_shell(cmd), expected);

    unlink(fsrc);
    unlink(csrc);
    unlink(obj);
    unlink(gobj);
    unlink(bin);
}

TEST(backend, gcc_interop_scalar_args)
{
    backend_run_gcc_tu("long gcc_add(long a, long b);\n"
                       "int main(void) { return (int) gcc_add(40, 2); }\n",
                       "long gcc_add(long a, long b) { return a + b; }\n", 42);
}

TEST(backend, gcc_interop_register_aggregate)
{
    backend_run_gcc_tu("struct P { long a; long b; };\n"
                       "long gcc_sum(struct P p);\n"
                       "int main(void) { struct P p; p.a = 40; p.b = 2;\n"
                       "                return (int) gcc_sum(p); }\n",
                       "struct P { long a; long b; };\n"
                       "long gcc_sum(struct P p) { return p.a + p.b; }\n",
                       42);
}

TEST(backend, gcc_interop_memory_aggregate)
{
    backend_run_gcc_tu("struct Big { char a[24]; };\n"
                       "long gcc_big(struct Big b);\n"
                       "int main(void) { struct Big b; b.a[0] = 20; b.a[23] = 22;\n"
                       "                return (int) gcc_big(b); }\n",
                       "struct Big { char a[24]; };\n"
                       "long gcc_big(struct Big b) { return b.a[0] + b.a[23]; }\n",
                       42);
}

TEST(backend, gcc_interop_float_and_double_args)
{
    backend_run_gcc_tu("int gcc_fp(double x, float y);\n"
                       "int main(void) { return gcc_fp(21.0, 2.0f); }\n",
                       "int gcc_fp(double x, float y) { return (int) (x * 2.0 + y); }\n", 44);
}

TEST(backend, gcc_interop_variadic_double_args)
{
    backend_run_gcc_tu("int gcc_fsum(int n, ...);\n"
                       "int main(void) { return gcc_fsum(2, 40.0, 2.0); }\n",
                       "#include <stdarg.h>\n"
                       "int gcc_fsum(int n, ...) {\n"
                       "    va_list ap; va_start(ap, n);\n"
                       "    double s = 0.0;\n"
                       "    for (int i = 0; i < n; i = i + 1) { s = s + va_arg(ap, double); }\n"
                       "    va_end(ap); return (int) s; }\n",
                       42);
}

TEST(backend, gcc_interop_printf)
{
    backend_run_gcc_tu("int printf(const char *fmt, ...);\n"
                       "int main(void) { printf(\"%d\\n\", 42); return 0; }\n",
                       "/* libc provides printf; this TU only anchors the link. */\n", 0);
}
