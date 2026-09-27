#include "harness.h"
#include "util/arena.h"
#include "util/vec.h"

TEST(vec, push_and_get)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    EXPECT_NOTNULL(v);
    EXPECT_EQ(vec_size(v), 0);
    int x = 42;
    vec_push(v, &x);
    EXPECT_EQ(vec_size(v), 1);
    EXPECT_TRUE(vec_get(v, 0) == &x);
    arena_free(a);
}

TEST(vec, growth)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int vals[64];
    for (int i = 0; i < 64; i++)
    {
        vals[i] = i;
        vec_push(v, &vals[i]);
    }
    EXPECT_EQ(vec_size(v), 64);
    for (int i = 0; i < 64; i++)
    {
        EXPECT_TRUE(vec_get(v, (size_t) i) == &vals[i]);
    }
    arena_free(a);
}

TEST(vec, growth_boundary)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int vals[9];
    for (int i = 0; i < 8; i++)
    {
        vals[i] = i;
        vec_push(v, &vals[i]);
    }
    EXPECT_EQ(vec_size(v), 8);
    vals[8] = 8;
    vec_push(v, &vals[8]);
    EXPECT_EQ(vec_size(v), 9);
    for (int i = 0; i < 9; i++)
    {
        EXPECT_TRUE(vec_get(v, (size_t) i) == &vals[i]);
    }
    arena_free(a);
}

TEST(vec, independent_vecs_share_arena)
{
    Arena *a = arena_new();
    Vec *v1 = vec_new(a);
    Vec *v2 = vec_new(a);
    int vals1[20];
    int vals2[20];
    for (int i = 0; i < 20; i++)
    {
        vals1[i] = i;
        vals2[i] = i * 2;
        vec_push(v1, &vals1[i]);
        vec_push(v2, &vals2[i]);
    }
    EXPECT_EQ(vec_size(v1), 20);
    EXPECT_EQ(vec_size(v2), 20);
    for (int i = 0; i < 20; i++)
    {
        EXPECT_TRUE(vec_get(v1, (size_t) i) == &vals1[i]);
        EXPECT_TRUE(vec_get(v2, (size_t) i) == &vals2[i]);
    }
    arena_free(a);
}

TEST(vec, stack_lifo)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int x = 1, y = 2, z = 3;
    vec_push(v, &x);
    vec_push(v, &y);
    vec_push(v, &z);
    EXPECT_TRUE(vec_last(v) == &z);
    EXPECT_EQ(vec_size(v), 3);
    EXPECT_TRUE(vec_pop(v) == &z);
    EXPECT_TRUE(vec_pop(v) == &y);
    EXPECT_TRUE(vec_pop(v) == &x);
    EXPECT_EQ(vec_size(v), 0);
    arena_free(a);
}

TEST(vec, stack_lifo_after_growth)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int vals[64];
    for (int i = 0; i < 64; i++)
    {
        vals[i] = i;
        vec_push(v, &vals[i]);
    }
    for (int i = 63; i >= 0; i--)
    {
        EXPECT_TRUE(vec_pop(v) == &vals[i]);
    }
    EXPECT_EQ(vec_size(v), 0);
    arena_free(a);
}

TEST(vec, set_overwrites_slot)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int x = 1, y = 2, z = 3;
    vec_push(v, &x);
    vec_push(v, &y);
    vec_push(v, &z);
    vec_set(v, 1, &z);
    EXPECT_TRUE(vec_get(v, 0) == &x);
    EXPECT_TRUE(vec_get(v, 1) == &z);
    EXPECT_TRUE(vec_get(v, 2) == &z);
    arena_free(a);
}

TEST(vec, set_on_last_growth_boundary)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int vals[10];
    for (int i = 0; i < 10; i++)
    {
        vals[i] = i;
        vec_push(v, &vals[i]);
    }
    int repl = 99;
    vec_set(v, 9, &repl);
    EXPECT_TRUE(vec_get(v, 9) == &repl);
    EXPECT_TRUE(vec_get(v, 8) == &vals[8]);
    arena_free(a);
}

TEST(vec, insert_shifts_elements)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int x = 1, y = 2, z = 3, w = 4;
    vec_push(v, &x);
    vec_push(v, &z);
    vec_insert(v, 1, &y);
    EXPECT_EQ(vec_size(v), 3);
    EXPECT_TRUE(vec_get(v, 0) == &x);
    EXPECT_TRUE(vec_get(v, 1) == &y);
    EXPECT_TRUE(vec_get(v, 2) == &z);
    vec_insert(v, 0, &w);
    EXPECT_EQ(vec_size(v), 4);
    EXPECT_TRUE(vec_get(v, 0) == &w);
    EXPECT_TRUE(vec_get(v, 1) == &x);
    vec_insert(v, vec_size(v), &y);
    EXPECT_EQ(vec_size(v), 5);
    EXPECT_TRUE(vec_get(v, 4) == &y);
    arena_free(a);
}

TEST(vec, insert_grows_capacity)
{
    Arena *a = arena_new();
    Vec *v = vec_new(a);
    int vals[9];
    for (int i = 0; i < 8; i++)
    {
        vals[i] = i;
        vec_push(v, &vals[i]);
    }
    vals[8] = 8;
    vec_insert(v, 0, &vals[8]);
    EXPECT_EQ(vec_size(v), 9);
    EXPECT_TRUE(vec_get(v, 0) == &vals[8]);
    EXPECT_TRUE(vec_get(v, 8) == &vals[7]);
    arena_free(a);
}
