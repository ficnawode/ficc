#include "harness.h"
#include "util/arena.h"
#include "util/sbuf.h"

#include <string.h>

TEST(sbuf, empty)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    EXPECT_NOTNULL(sb);
    EXPECT_EQ(sbuf_len(sb), 0);
    EXPECT_STR_EQ(sbuf_cstr(sb), "");
    arena_free(a);
}

TEST(sbuf, append)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    sbuf_append(sb, "hello");
    EXPECT_EQ(sbuf_len(sb), 5);
    EXPECT_STR_EQ(sbuf_cstr(sb), "hello");
    sbuf_append(sb, " world");
    EXPECT_EQ(sbuf_len(sb), 11);
    EXPECT_STR_EQ(sbuf_cstr(sb), "hello world");
    arena_free(a);
}

TEST(sbuf, appendf_basic)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    sbuf_appendf(sb, "x=%d y=%s", 42, "abc");
    EXPECT_STR_EQ(sbuf_cstr(sb), "x=42 y=abc");
    arena_free(a);
}

TEST(sbuf, appendf_growth)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    /* initial cap is 64; append enough to force multiple growths */
    for (int i = 0; i < 100; i++)
    {
        sbuf_appendf(sb, "%d ", i);
    }
    EXPECT_TRUE(sbuf_len(sb) > 64);
    /* last few chars should be "99 " */
    const char *s = sbuf_cstr(sb);
    size_t n = sbuf_len(sb);
    EXPECT_TRUE(n >= 3);
    EXPECT_TRUE(s[n - 1] == ' ');
    EXPECT_TRUE(s[n - 2] == '9');
    EXPECT_TRUE(s[n - 3] == '9');
    arena_free(a);
}

TEST(sbuf, append_and_appendf_mix)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    sbuf_append(sb, "A");
    sbuf_appendf(sb, "%d", 1);
    sbuf_append(sb, "B");
    sbuf_appendf(sb, "%d", 2);
    EXPECT_STR_EQ(sbuf_cstr(sb), "A1B2");
    arena_free(a);
}

TEST(sbuf, exact_cap_boundary)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    /* initial cap is 64; landing exactly on it must fit, and the
       NUL terminator must stay in bounds */
    char buf[64];
    memset(buf, 'a', 63);
    buf[63] = '\0';
    sbuf_append(sb, buf);
    sbuf_append(sb, "x");
    EXPECT_EQ(sbuf_len(sb), 64);
    EXPECT_TRUE(sbuf_cstr(sb)[64] == '\0');
    arena_free(a);
}

TEST(sbuf, single_append_larger_than_cap)
{
    Arena *a = arena_new();
    Sbuf *sb = sbuf_new(a);
    /* one call bigger than cap forces multiple doublings in one grow */
    char big[300];
    memset(big, 'b', 299);
    big[299] = '\0';
    sbuf_append(sb, big);
    EXPECT_EQ(sbuf_len(sb), 299);
    EXPECT_STR_EQ(sbuf_cstr(sb), big);
    arena_free(a);
}
