#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int ld_seq;

static void ld_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_24d_%u_%s.%s", ld_seq++, tag, ext);
}

static void ld_write(char *out, size_t sz, const char *tag, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_24d_%u_%s.c", ld_seq++, tag);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void ld_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

TEST(link_driver, filc_oracle_simple)
{
    EXPECT_INTERP_AND_ELF_FILC("int main(void) { return 7 * 6; }\n", 42);
}

TEST(link_driver, filc_oracle_globals)
{
    EXPECT_INTERP_AND_ELF_FILC("static int g = 10;\n"
                               "int main(void) { int a = 5; return g * a; }\n",
                               50);
}

TEST(link_driver, multi_c_compile_and_link)
{
    char a[128], b[128], bin[192];
    ld_write(a, sizeof(a), "ml_a", "int add(int x, int y) { return x + y; }\n");
    ld_write(b, sizeof(b), "ml_b", "int add(int, int);\nint main(void) { return add(20, 22); }\n");
    ld_path(bin, sizeof(bin), "ml", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, a, b, bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {a, b, bin};
    ld_cleanup(paths, 3);
}

TEST(link_driver, object_only_link)
{
    char a[128], b[128], ao[192], bo[192], bin[192];
    ld_write(a, sizeof(a), "ol_a", "int sub(int x, int y) { return x - y; }\n");
    ld_write(b, sizeof(b), "ol_b", "int sub(int, int);\nint main(void) { return sub(50, 8); }\n");
    ld_path(ao, sizeof(ao), "ol_a", "o");
    ld_path(bo, sizeof(bo), "ol_b", "o");
    ld_path(bin, sizeof(bin), "ol", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, a, ao);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, b, bo);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, ao, bo, bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {a, b, ao, bo, bin};
    ld_cleanup(paths, 5);
}

TEST(link_driver, hosted_printf)
{
    char src[128], bin[192], out[192];
    ld_write(src, sizeof(src), "hosted",
             "#include <stdio.h>\nint main(void) { printf(\"ok %d\\n\", 40 + 2); return 5; }\n");
    ld_path(bin, sizeof(bin), "hosted", "bin");
    ld_path(out, sizeof(out), "hosted", "out");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s >/dev/null 2>&1 && %s > %s 2>&1", FICC_BIN, src, bin,
             bin, out);
    EXPECT_EQ(tc_run_shell(cmd), 5);

    FILE *f = fopen(out, "rb");
    EXPECT_NOTNULL(f);
    if (f)
    {
        char line[64] = {0};
        size_t n = fread(line, 1, sizeof(line) - 1, f);
        fclose(f);
        EXPECT_TRUE(n > 0);
        EXPECT_STR_EQ(line, "ok 42\n");
    }

    char *paths[] = {src, bin, out};
    ld_cleanup(paths, 3);
}

static int rdyn_build_and_run(bool export_dynamic)
{
    char plugin[128], so[128], main_c[128], bin[192];
    ld_write(plugin, sizeof(plugin), "rdyn_plugin",
             "extern int host_value(void);\n"
             "int plugin_value(void) { return host_value() + 1; }\n");
    ld_path(so, sizeof(so), "rdyn", "so");
    ld_path(main_c, sizeof(main_c), "rdyn_main", "c");
    ld_path(bin, sizeof(bin), "rdyn", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -o %s %s >/dev/null 2>&1", so, plugin);
    if (tc_run_shell(cmd) != 0)
    {
        return -1;
    }

    char src[512];
    snprintf(src, sizeof(src),
             "#include <dlfcn.h>\n"
             "int host_value(void) { return 41; }\n"
             "int main(void)\n"
             "{\n"
             "    void *h = dlopen(\"%s\", RTLD_NOW);\n"
             "    if (!h) return 1;\n"
             "    int (*fn)(void) = (int (*)(void)) dlsym(h, \"plugin_value\");\n"
             "    if (!fn) return 2;\n"
             "    return fn();\n"
             "}\n",
             so);
    FILE *f = fopen(main_c, "w");
    EXPECT_NOTNULL(f);
    if (!f)
    {
        return -1;
    }
    fputs(src, f);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "%s %s %s-ldl -o %s >/dev/null 2>&1 && %s", FICC_BIN, main_c,
             export_dynamic ? "-rdynamic " : "", bin, bin);
    int rc = tc_run_shell(cmd);

    char *paths[] = {plugin, so, main_c, bin};
    ld_cleanup(paths, 4);
    return rc;
}

static bool have_gcc(void)
{
    return tc_run_shell("command -v gcc >/dev/null 2>&1") == 0;
}

TEST(link_driver, export_dynamic_plugin)
{
    if (!have_gcc())
    {
        return;
    }
    EXPECT_EQ(rdyn_build_and_run(true), 42);
}

TEST(link_driver, export_dynamic_required)
{
    if (!have_gcc())
    {
        return;
    }
    EXPECT_EQ(rdyn_build_and_run(false), 1);
}

TEST(link_driver, dso_without_soname_is_needed)
{
    if (!have_gcc())
    {
        return;
    }
    char lib_c[128], so[128], main_c[128], bin[192];
    ld_write(lib_c, sizeof(lib_c), "noso", "int noso_val(void) { return 42; }\n");
    unsigned int id = ld_seq++;
    snprintf(so, sizeof(so), "/tmp/libficc_24d_%u.so", id);
    ld_write(main_c, sizeof(main_c), "noso_main",
             "extern int noso_val(void);\nint main(void) { return noso_val(); }\n");
    ld_path(bin, sizeof(bin), "noso", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -o %s %s >/dev/null 2>&1", so, lib_c);
    if (tc_run_shell(cmd) != 0)
    {
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s %s -L/tmp -lficc_24d_%u -o %s >/dev/null 2>&1", FICC_BIN, main_c,
             id, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "LD_LIBRARY_PATH=/tmp %s", bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {lib_c, so, main_c, bin};
    ld_cleanup(paths, 4);
}
