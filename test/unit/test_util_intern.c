#include "harness.h"
#include "util/arena.h"
#include "util/intern.h"

TEST(intern, same_string_same_pointer)
{
    Arena *a = arena_new();
    InternPool *pool = intern_pool_new(a);
    const char *s1 = intern(pool, "hello");
    const char *s2 = intern(pool, "hello");
    EXPECT_TRUE(s1 == s2);
    EXPECT_STR_EQ(s1, "hello");
    arena_free(a);
}

TEST(intern, different_strings_different_pointers)
{
    Arena *a = arena_new();
    InternPool *pool = intern_pool_new(a);
    const char *s1 = intern(pool, "foo");
    const char *s2 = intern(pool, "bar");
    EXPECT_TRUE(s1 != s2);
    EXPECT_STR_EQ(s1, "foo");
    EXPECT_STR_EQ(s2, "bar");
    arena_free(a);
}

TEST(intern, many_strings)
{
    Arena *a = arena_new();
    InternPool *pool = intern_pool_new(a);
    const char *ptrs[64];
    char buf[16];
    for (int i = 0; i < 64; i++)
    {
        buf[0] = 'a' + (i % 26);
        buf[1] = '0' + (i % 10);
        buf[2] = '\0';
        ptrs[i] = intern(pool, buf);
        EXPECT_STR_EQ(ptrs[i], buf);
    }
    /* intern again, should get same pointers */
    for (int i = 0; i < 64; i++)
    {
        buf[0] = 'a' + (i % 26);
        buf[1] = '0' + (i % 10);
        buf[2] = '\0';
        const char *p = intern(pool, buf);
        EXPECT_TRUE(p == ptrs[i]);
    }
    arena_free(a);
}

TEST(intern, empty_string)
{
    Arena *a = arena_new();
    InternPool *pool = intern_pool_new(a);
    const char *s1 = intern(pool, "");
    const char *s2 = intern(pool, "");
    EXPECT_TRUE(s1 == s2);
    EXPECT_STR_EQ(s1, "");
    arena_free(a);
}
