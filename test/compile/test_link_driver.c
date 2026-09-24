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
