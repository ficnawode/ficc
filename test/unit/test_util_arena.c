#include "harness.h"
#include "util/arena.h"

#include <stdint.h>
#include <string.h>

TEST(arena, create_and_alloc)
{
    Arena *a = arena_new();
    EXPECT_NOTNULL(a);
    void *p = arena_alloc(a, 64, 8);
    EXPECT_NOTNULL(p);
    arena_free(a);
}

TEST(arena, alloc_is_aligned)
{
    Arena *a = arena_new();
    arena_alloc(a, 3, 1);
    void *p = arena_alloc(a, 16, 16);
    EXPECT_EQ((uintptr_t) p % 16, 0);
    arena_free(a);
}

TEST(arena, allocs_do_not_overlap)
{
    Arena *a = arena_new();
    char *p1 = arena_alloc(a, 32, 8);
    char *p2 = arena_alloc(a, 32, 8);
    /* disjoint regions compared as integers (C11 6.5.8p5 — relational
       comparison of distinct allocations is UB) */
    uintptr_t u1 = (uintptr_t) p1;
    uintptr_t u2 = (uintptr_t) p2;
    EXPECT_TRUE(u1 + 32 <= u2 || u2 + 32 <= u1);
    memset(p1, 0xAA, 32);
    memset(p2, 0xBB, 32);
    EXPECT_TRUE(p1[0] == (char) 0xAA);
    EXPECT_TRUE(p2[0] == (char) 0xBB);
    arena_free(a);
}

TEST(arena, chunk_growth)
{
    Arena *a = arena_new();
    char *p1 = arena_alloc(a, 3000, 8);
    char *p2 = arena_alloc(a, 3000, 8);
    EXPECT_NOTNULL(p1);
    EXPECT_NOTNULL(p2);
    memset(p1, 0x11, 3000);
    memset(p2, 0x22, 3000);
    EXPECT_TRUE(p1[2999] == (char) 0x11);
    EXPECT_TRUE(p2[2999] == (char) 0x22);
    arena_free(a);
}

TEST(arena, big_alloc_gets_own_chunk)
{
    Arena *a = arena_new();
    char *big = arena_alloc(a, 10000, 8);
    EXPECT_NOTNULL(big);
    memset(big, 0x33, 10000);
    EXPECT_TRUE(big[0] == (char) 0x33);
    EXPECT_TRUE(big[9999] == (char) 0x33);
    arena_free(a);
}

TEST(arena, usable_after_big_alloc)
{
    Arena *a = arena_new();
    arena_alloc(a, 10000, 8);
    void *small = arena_alloc(a, 8, 8);
    EXPECT_NOTNULL(small);
    arena_free(a);
}

TEST(arena, many_small_allocs)
{
    Arena *a = arena_new();
    enum
    {
        NALLOCS = 1000
    };
    void *ptrs[NALLOCS];
    for (size_t i = 0; i < NALLOCS; i++)
    {
        ptrs[i] = arena_alloc(a, 16, 8);
        EXPECT_NOTNULL(ptrs[i]);
    }
    for (size_t i = 0; i < NALLOCS; i++)
    {
        memset(ptrs[i], (int) i, 16);
    }
    arena_free(a);
}

TEST(arena, multi_chunk_free)
{
    Arena *a = arena_new();
    for (size_t i = 0; i < 10; i++)
    {
        arena_alloc(a, 3000, 8);
    }
    arena_free(a);
}
