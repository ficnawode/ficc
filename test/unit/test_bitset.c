#include "harness.h"
#include "util/arena.h"
#include "util/bitset.h"

TEST(bitset, create_all_clear)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 64);
    for (size_t i = 0; i < 64; i++)
    {
        EXPECT_TRUE(bitset_test(bs, i) == false);
    }
    EXPECT_EQ(bitset_count(bs), 0);
    arena_free(a);
}

TEST(bitset, set_and_test)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 128);
    bitset_set(bs, 0);
    bitset_set(bs, 7);
    bitset_set(bs, 63);
    bitset_set(bs, 64);
    bitset_set(bs, 127);
    EXPECT_TRUE(bitset_test(bs, 0));
    EXPECT_TRUE(bitset_test(bs, 7));
    EXPECT_TRUE(bitset_test(bs, 63));
    EXPECT_TRUE(bitset_test(bs, 64));
    EXPECT_TRUE(bitset_test(bs, 127));
    EXPECT_TRUE(bitset_test(bs, 1) == false);
    EXPECT_TRUE(bitset_test(bs, 62) == false);
    EXPECT_TRUE(bitset_test(bs, 65) == false);
    EXPECT_EQ(bitset_count(bs), 5);
    arena_free(a);
}

TEST(bitset, clear)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 32);
    bitset_set(bs, 5);
    EXPECT_TRUE(bitset_test(bs, 5));
    bitset_clear(bs, 5);
    EXPECT_TRUE(bitset_test(bs, 5) == false);
    arena_free(a);
}

TEST(bitset, out_of_range)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 10);
    bitset_set(bs, 100);
    EXPECT_TRUE(bitset_test(bs, 100) == false);
    arena_free(a);
}

TEST(bitset, count_large)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 256);
    for (size_t i = 0; i < 256; i += 2)
    {
        bitset_set(bs, i);
    }
    EXPECT_EQ(bitset_count(bs), 128);
    arena_free(a);
}

TEST(bitset, or)
{
    Arena *a = arena_new();
    Bitset *a1 = bitset_new(a, 64);
    Bitset *a2 = bitset_new(a, 64);
    bitset_set(a1, 0);
    bitset_set(a1, 1);
    bitset_set(a2, 1);
    bitset_set(a2, 2);
    bitset_or(a1, a2);
    EXPECT_TRUE(bitset_test(a1, 0));
    EXPECT_TRUE(bitset_test(a1, 1));
    EXPECT_TRUE(bitset_test(a1, 2));
    EXPECT_EQ(bitset_count(a1), 3);
    arena_free(a);
}

TEST(bitset, and)
{
    Arena *a = arena_new();
    Bitset *a1 = bitset_new(a, 64);
    Bitset *a2 = bitset_new(a, 64);
    bitset_set(a1, 0);
    bitset_set(a1, 1);
    bitset_set(a2, 1);
    bitset_set(a2, 2);
    bitset_and(a1, a2);
    EXPECT_TRUE(bitset_test(a1, 0) == false);
    EXPECT_TRUE(bitset_test(a1, 1));
    EXPECT_TRUE(bitset_test(a1, 2) == false);
    EXPECT_EQ(bitset_count(a1), 1);
    arena_free(a);
}

TEST(bitset, and_mismatched_size)
{
    Arena *a = arena_new();
    Bitset *dst = bitset_new(a, 128);
    Bitset *src = bitset_new(a, 64);
    bitset_set(dst, 0);
    bitset_set(dst, 70);
    bitset_set(src, 0);
    bitset_and(dst, src);
    EXPECT_TRUE(bitset_test(dst, 0));
    EXPECT_TRUE(bitset_test(dst, 70) == false);
    EXPECT_EQ(bitset_count(dst), 1);
    arena_free(a);
}

TEST(bitset, count_non_multiple_of_64)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 10);
    bitset_set(bs, 0);
    bitset_set(bs, 5);
    bitset_set(bs, 9);
    EXPECT_EQ(bitset_count(bs), 3);
    arena_free(a);
}

TEST(bitset, or_mismatched_size)
{
    Arena *a = arena_new();
    Bitset *dst = bitset_new(a, 128);
    Bitset *src = bitset_new(a, 64);
    bitset_set(dst, 70);
    bitset_set(src, 0);
    bitset_or(dst, src);
    EXPECT_TRUE(bitset_test(dst, 0));
    /* dst's excess words survive: src's missing bits are implicit zeros */
    EXPECT_TRUE(bitset_test(dst, 70));
    arena_free(a);
}

TEST(bitset, src_larger_than_dst)
{
    Arena *a = arena_new();
    Bitset *dst = bitset_new(a, 64);
    Bitset *src = bitset_new(a, 128);
    bitset_set(dst, 5);
    bitset_set(src, 3);  /* shared word */
    bitset_set(src, 70); /* beyond dst's width */
    bitset_and(dst, src);
    /* src bit 70 is beyond dst's width: implicit zero, so and clears bit 5 */
    EXPECT_TRUE(bitset_test(dst, 5) == false);
    /* or touches the shared range only: bit 3 appears, bit 70 does not */
    bitset_or(dst, src);
    EXPECT_TRUE(bitset_test(dst, 3));
    EXPECT_TRUE(bitset_test(dst, 70) == false);
    arena_free(a);
}

TEST(bitset, clear_out_of_range)
{
    Arena *a = arena_new();
    Bitset *bs = bitset_new(a, 10);
    bitset_clear(bs, 100); /* no-op, must not crash */
    EXPECT_EQ(bitset_count(bs), 0);
    arena_free(a);
}
