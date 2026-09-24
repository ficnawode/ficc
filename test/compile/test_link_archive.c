#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int ar_seq;

static void ar_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_24f_%u_%s.%s", ar_seq++, tag, ext);
}

static void ar_write(char *out, size_t sz, const char *tag, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_24f_%u_%s.c", ar_seq++, tag);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static bool have_ar(void)
{
    return tc_run_shell("command -v ar >/dev/null 2>&1") == 0;
}

TEST(link_archive, lazy_member_pull)
{
    if (!have_ar())
    {
        return;
    }
    char t[128], u[128], m[128], to[192], uo[192], lib[192], bin[192];
    ar_write(t, sizeof(t), "triple", "int triple(int x) { return x * 3; }\n");
    ar_write(u, sizeof(u), "unused", "int unused_fn(int x) { return x + 1; }\n");
    ar_write(m, sizeof(m), "main",
             "extern int triple(int);\nint main(void) { return triple(14); }\n");
    ar_path(to, sizeof(to), "triple", "o");
    ar_path(uo, sizeof(uo), "unused", "o");
    ar_path(lib, sizeof(lib), "libfoo", "a");
    ar_path(bin, sizeof(bin), "lazy", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, t, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, u, uo);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "ar rcs %s %s %s >/dev/null 2>&1", lib, to, uo);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, m, lib, bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {t, u, m, to, uo, lib, bin};
    for (size_t i = 0; i < 7; i++)
    {
        unlink(paths[i]);
    }
}

TEST(link_archive, search_with_l_and_script)
{
    if (!have_ar())
    {
        return;
    }
    char t[128], m[128], to[192], lib[192], script[192], bin[192], mbin[192];
    ar_write(t, sizeof(t), "sftriple", "int triple(int x) { return x * 3; }\n");
    ar_write(m, sizeof(m), "sfmain",
             "extern int triple(int);\nint main(void) { return triple(14); }\n");
    ar_path(to, sizeof(to), "sf", "o");
    snprintf(lib, sizeof(lib), "/tmp/libfsearch.a");
    ar_path(script, sizeof(script), "group", "so");
    ar_path(bin, sizeof(bin), "sf", "bin");
    ar_path(mbin, sizeof(mbin), "sfm", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, t, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "ar rcs %s %s >/dev/null 2>&1", lib, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* -L<dir> -l<name> resolves libfsearch.a in /tmp. */
    snprintf(cmd, sizeof(cmd), "%s %s -L/tmp -lfsearch -o %s >/dev/null 2>&1 && %s", FICC_BIN, m,
             bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    /* A GROUP() script naming the archive directly also links. */
    FILE *f = fopen(script, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fprintf(f, "/* test ld script */\nOUTPUT_FORMAT(elf64-x86-64)\nGROUP ( %s )\n", lib);
        fclose(f);
    }
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, m, script, mbin,
             mbin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {t, m, to, lib, script, bin, mbin};
    for (size_t i = 0; i < 7; i++)
    {
        unlink(paths[i]);
    }
}

TEST(link_archive, unknown_library_errors)
{
    char m[128], bin[192];
    ar_write(m, sizeof(m), "ukmain", "int main(void) { return 0; }\n");
    ar_path(bin, sizeof(bin), "uk", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s -lficc_no_such_lib -o %s >/dev/null 2>&1", FICC_BIN, m, bin);
    EXPECT_TRUE(tc_run_shell(cmd) != 0);
    EXPECT_NULL(fopen(bin, "rb"));

    char *paths[] = {m, bin};
    for (size_t i = 0; i < 2; i++)
    {
        unlink(paths[i]);
    }
}
