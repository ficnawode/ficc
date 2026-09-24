#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Phase 23 E: selective `__attribute__` semantics. Constructors run before
   `main`, so those cases assert on the compiled ELF only (the interpreter does
   not model .init_array). */

TEST(attributes, constructor_runs_before_main)
{
    EXPECT_EQ(tc_run_elf("static int g;\n"
                         "static void __attribute__((constructor)) boot(void) { g = 42; }\n"
                         "int main(void) { return g; }\n"),
              42);
}

TEST(attributes, constructor_priority_argument_ignored)
{
    EXPECT_EQ(tc_run_elf("static int g;\n"
                         "static void __attribute__((constructor(101))) boot(void) { g = 7; }\n"
                         "int main(void) { return g; }\n"),
              7);
}

TEST(attributes, multiple_constructors_in_declaration_order)
{
    EXPECT_EQ(tc_run_elf("static int g;\n"
                         "static void __attribute__((constructor)) a(void) { g = g * 10 + 1; }\n"
                         "static void __attribute__((constructor)) b(void) { g = g * 10 + 2; }\n"
                         "int main(void) { return g; }\n"),
              12);
}

TEST(attributes, unknown_attributes_ignored)
{
    EXPECT_EQ(
        tc_run_elf("static int f(void)\n"
                   "    __attribute__((unused, format(printf, 1, 2), nonnull(1))) { return 42; }\n"
                   "int main(void) { return f(); }\n"),
        42);
}

TEST(attributes, attribute_before_type)
{
    EXPECT_EQ(tc_run_elf("__attribute__((unused)) static int f(void) { return 42; }\n"
                         "int main(void) { return f(); }\n"),
              42);
}

TEST(attributes, packed_struct_layout)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct P { char c; int i; short s; } __attribute__((packed));\n"
                          "int main(void) {\n"
                          "    if (sizeof(struct P) != 7) return 1;\n"
                          "    if (offsetof(struct P, i) != 1) return 2;\n"
                          "    if (offsetof(struct P, s) != 5) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(attributes, packed_member)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct M { char c; int i __attribute__((packed)); };\n"
                          "int main(void) {\n"
                          "    if (offsetof(struct M, i) != 1) return 1;\n"
                          "    if (sizeof(struct M) != 5) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(attributes, aligned_record)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct A { char c; } __attribute__((aligned(16)));\n"
                          "int main(void) {\n"
                          "    if (sizeof(struct A) != 16) return 1;\n"
                          "    if (_Alignof(struct A) != 16) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(attributes, aligned_variable_accepted)
{
    EXPECT_INTERP_AND_ELF("static int g __attribute__((aligned(16))) = 42;\n"
                          "int main(void) { return g; }\n",
                          42);
}

/* Runs ficc with -fpedantic and returns the captured diagnostics in `out`. */
static void attr_run_pedantic(const char *src, char *out, size_t out_sz)
{
    static unsigned int seq;
    char path[128];
    snprintf(path, sizeof(path), "/tmp/ficc_attr_%u.c", seq++);
    FILE *f = fopen(path, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
    snprintf(out, out_sz, "/tmp/ficc_attr_%u.err", seq++);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s -fpedantic -c %s -o /tmp/ficc_attr.o >%s 2>&1", FICC_BIN, path,
             out);
    tc_run_shell(cmd);
    unlink(path);
    unlink("/tmp/ficc_attr.o");
}

TEST(attributes, unknown_attribute_pedantic_warning)
{
    char err[128];
    attr_run_pedantic("static int f(void) __attribute__((no_such_attr)) { return 1; }\n"
                      "int main(void) { return f(); }\n",
                      err, sizeof(err));
    FILE *f = fopen(err, "r");
    EXPECT_NOTNULL(f);
    char buf[512] = {0};
    if (f)
    {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    unlink(err);
    EXPECT_NOTNULL(strstr(buf, "attribute 'no_such_attr' ignored"));
}
