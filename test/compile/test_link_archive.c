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

TEST(link_archive, dash_l_resolves_library)
{
    if (!have_ar())
    {
        return;
    }
    char t[128], m[128], to[192], lib[192], bin[192];
    ar_write(t, sizeof(t), "sftriple", "int triple(int x) { return x * 3; }\n");
    ar_write(m, sizeof(m), "sfmain",
             "extern int triple(int);\nint main(void) { return triple(14); }\n");
    ar_path(to, sizeof(to), "sf", "o");
    snprintf(lib, sizeof(lib), "/tmp/libfsearch.a");
    ar_path(bin, sizeof(bin), "sf", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, t, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "ar rcs %s %s >/dev/null 2>&1", lib, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s %s -L/tmp -lfsearch -o %s >/dev/null 2>&1 && %s", FICC_BIN, m,
             bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {t, m, to, lib, bin};
    for (size_t i = 0; i < 5; i++)
    {
        unlink(paths[i]);
    }
}

TEST(link_archive, group_script_links_archive)
{
    if (!have_ar())
    {
        return;
    }
    char t[128], m[128], to[192], lib[192], script[192], mbin[192];
    ar_write(t, sizeof(t), "grtriple", "int triple(int x) { return x * 3; }\n");
    ar_write(m, sizeof(m), "grmain",
             "extern int triple(int);\nint main(void) { return triple(14); }\n");
    ar_path(to, sizeof(to), "gr", "o");
    ar_path(lib, sizeof(lib), "grlib", "a");
    ar_path(script, sizeof(script), "group", "so");
    ar_path(mbin, sizeof(mbin), "gr", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, t, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "ar rcs %s %s >/dev/null 2>&1", lib, to);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    FILE *f = fopen(script, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fprintf(f, "OUTPUT_FORMAT(elf64-x86-64)\nGROUP ( %s )\n", lib);
        fclose(f);
    }
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, m, script, mbin,
             mbin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {t, m, to, lib, script, mbin};
    for (size_t i = 0; i < 6; i++)
    {
        unlink(paths[i]);
    }
}

TEST(link_archive, transitive_member_pull)
{
    if (!have_ar())
    {
        return;
    }
    char a[128], b[128], m[128], ao[192], bo[192], lib[192], bin[192];
    ar_write(a, sizeof(a), "tra",
             "extern int beta(int);\nint alpha(int x) { return beta(x) + 1; }\n");
    ar_write(b, sizeof(b), "trb", "int beta(int x) { return x * 3; }\n");
    ar_write(m, sizeof(m), "trm",
             "extern int alpha(int);\nint main(void) { return alpha(13) + 2; }\n");
    ar_path(ao, sizeof(ao), "tra", "o");
    ar_path(bo, sizeof(bo), "trb", "o");
    ar_path(lib, sizeof(lib), "trans", "a");
    ar_path(bin, sizeof(bin), "trans", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, a, ao);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, b, bo);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "ar rcs %s %s %s >/dev/null 2>&1", lib, ao, bo);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s %s %s -o %s >/dev/null 2>&1 && %s", FICC_BIN, m, lib, bin, bin);
    EXPECT_EQ(tc_run_shell(cmd), 42);

    char *paths[] = {a, b, m, ao, bo, lib, bin};
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
