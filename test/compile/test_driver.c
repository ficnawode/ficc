#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int drv_seq;

/* Replaces the extension of `src` (….<ext>) with `new_ext` (as the driver's
   replace_ext does for the derived object default), into `out`. */
static void drv_replace_ext(const char *src, const char *new_ext, char *out, size_t out_len)
{
    const char *dot = strrchr(src, '.');
    size_t base_len = dot ? (size_t) (dot - src) : strlen(src);
    snprintf(out, out_len, "%.*s%s", (int) base_len, src, new_ext);
}

static void drv_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_18e_%u_%s.%s", drv_seq++, tag, ext);
}

/* Writes src to a fresh temp .c and returns its path in `out`. */
static void drv_write_src(char *out, size_t sz, const char *tag, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_18e_%u_%s.c", drv_seq++, tag);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void drv_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

/* Link a ficc-produced object with gcc and run it; expect `expected` as exit
   status (sign-extended from int). */
static void drv_run_obj(const char *obj, int expected)
{
    char bin[256];
    drv_path(bin, sizeof(bin), "bin", "bin");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj, bin, bin);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, expected);
    char *paths[] = {bin};
    drv_cleanup(paths, 1);
}

TEST(driver, o_explicit_object_path)
{
    char src[128], obj[192];
    drv_write_src(src, sizeof(src), "explicit",
                  "int main(void) { return 6 + 36; }\n");
    drv_path(obj, sizeof(obj), "explicit_obj", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);

    FILE *f = fopen(obj, "rb");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fclose(f);
    }

    drv_run_obj(obj, 42);

    char *paths[] = {src, obj};
    drv_cleanup(paths, 2);
}

TEST(driver, o_joined_equals_form)
{
    char src[128], obj[192];
    drv_write_src(src, sizeof(src), "joined",
                  "int main(void) { return 0x2a; }\n");
    drv_path(obj, sizeof(obj), "joined_obj", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o=%s >/dev/null 2>&1", FICC_BIN, src, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);

    drv_run_obj(obj, 42);

    char *paths[] = {src, obj};
    drv_cleanup(paths, 2);
}

TEST(driver, no_o_derives_default_object)
{
    char src[128], defo[192];
    drv_write_src(src, sizeof(src), "derived",
                  "int main(void) { return 7 * 6; }\n");
    drv_replace_ext(src, ".o", defo, sizeof(defo));

    /* With no -o, the object lands next to the source as <base>.o. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s >/dev/null 2>&1", FICC_BIN, src);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);

    FILE *f = fopen(defo, "rb");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fclose(f);
    }

    drv_run_obj(defo, 42);

    char *paths[] = {src, defo};
    drv_cleanup(paths, 2);
}

TEST(driver, multi_input_c_emits_many_objects)
{
    char a_src[128], b_src[128], a_o[192], b_o[192];
    drv_write_src(a_src, sizeof(a_src), "multi_a",
                  "int add(int a, int b) { return a + b; }\n");
    drv_write_src(b_src, sizeof(b_src), "multi_b",
                  "int add(int, int);\nint main(void) { return add(20, 22); }\n");
    drv_replace_ext(a_src, ".o", a_o, sizeof(a_o));
    drv_replace_ext(b_src, ".o", b_o, sizeof(b_o));

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s %s >/dev/null 2>&1", FICC_BIN, a_src, b_src);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);

    FILE *fa = fopen(a_o, "rb");
    FILE *fb = fopen(b_o, "rb");
    EXPECT_NOTNULL(fa);
    EXPECT_NOTNULL(fb);
    if (fa)
    {
        fclose(fa);
    }
    if (fb)
    {
        fclose(fb);
    }

    /* Link both objects: cross-file references must resolve for -c parity. */
    char bin[256];
    drv_path(bin, sizeof(bin), "multi_bin", "bin");
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s %s -o %s >/dev/null 2>&1 && %s", a_o, b_o, bin, bin);
    rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 42);

    char *paths[] = {a_src, b_src, a_o, b_o, bin};
    drv_cleanup(paths, 5);
}

TEST(driver, o_rejected_with_multiple_inputs)
{
    char a_src[128], b_src[128], obj[192];
    drv_write_src(a_src, sizeof(a_src), "rej_a", "int a(void) { return 1; }\n");
    drv_write_src(b_src, sizeof(b_src), "rej_b", "int b(void) { return 2; }\n");
    drv_path(obj, sizeof(obj), "rej", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s %s -o %s >/dev/null 2>&1", FICC_BIN, a_src, b_src, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_TRUE(rc != 0);

    FILE *f = fopen(obj, "rb");
    EXPECT_NULL(f);
    if (f)
    {
        fclose(f);
    }

    char *paths[] = {a_src, b_src, obj};
    drv_cleanup(paths, 3);
}