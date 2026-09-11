#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 17u: the standard shims preprocess + compile smoke programs, and ficc
   processes its own sources (the self-compile acceptance). */

static IrModule *self_compile(const char *path, const char *const *dirs, size_t ndirs)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "  [selfcompile] cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    Arena *a = arena_new();
    char *src = arena_alloc(a, (size_t) size + 1, 1);
    fread(src, 1, (size_t) size, f);
    fclose(f);
    src[size] = '\0';
    IrModule *mod = tc_build_module_with_dirs(src, path, dirs, ndirs, a);
    arena_free(a);
    return mod;
}

TEST(ph17u, self_compiles_sbuf)
{
    const char *dirs[] = {"src"};
    EXPECT_NOTNULL(self_compile("src/util/sbuf.c", dirs, 1));
}

TEST(ph17u, self_compiles_arena_and_vec)
{
    const char *dirs[] = {"src"};
    EXPECT_NOTNULL(self_compile("src/util/arena.c", dirs, 1));
    EXPECT_NOTNULL(self_compile("src/util/vec.c", dirs, 1));
}

TEST(ph17u, shim_stdarg_raw_names)
{
    /* The <stdarg.h> shim (Phase 15 builtins under the portable names). */
    EXPECT_INTERP_AND_ELF("#include <stdarg.h>\n"
                          "int sum(int count, ...) {\n"
                          "    va_list ap;\n"
                          "    va_start(ap, count);\n"
                          "    int t = 0;\n"
                          "    for (int i = 0; i < count; i++) {\n"
                          "        t += va_arg(ap, int);\n"
                          "    }\n"
                          "    va_end(ap);\n"
                          "    return t;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return sum(4, 1, 2, 3, 4);\n"
                          "}\n",
                          10);
}

TEST(ph17u, shim_stdint)
{
    EXPECT_INTERP_AND_ELF("#include <stdint.h>\n"
                          "uint32_t ui;\n"
                          "int64_t si;\n"
                          "int main(void) {\n"
                          "    ui = 4000000000U;\n"
                          "    si = -4000000000LL;\n"
                          "    return (int) (si + ui);\n"
                          "}\n",
                          0);
}

TEST(ph17u, shim_stddef)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "int main(void) {\n"
                          "    size_t n = 4;\n"
                          "    ptrdiff_t d = n - 8;\n"
                          "    return (int) (n + d) + (NULL ? 1 : 0);\n"
                          "}\n",
                          0);
}

TEST(ph17u, shim_stdio_printf_elf)
{
    /* printf resolves to libc through the ELF link (Phase 15 varargs +
       Phase 16 prototypes); interp has no libc, so ELF-only. */
    EXPECT_EQ(tc_run_elf("#include <stdio.h>\n"
                         "int main(void) {\n"
                         "    printf(\"stdio shim ok: %d\\n\", 42);\n"
                         "    return 42;\n"
                         "}\n"),
              42);
}

TEST(ph17u, shim_stdlib_abs_elf)
{
    EXPECT_EQ(tc_run_elf("#include <stdlib.h>\n"
                         "int main(void) {\n"
                         "    return abs(-42);\n"
                         "}\n"),
              42);
}

TEST(ph17u, shim_string_memset_elf)
{
    EXPECT_EQ(tc_run_elf("#include <string.h>\n"
                         "int main(void) {\n"
                         "    char buf[4];\n"
                         "    memset(buf, 0, sizeof(buf));\n"
                         "    buf[0] = 1;\n"
                         "    return buf[3];\n"
                         "}\n"),
              0);
}