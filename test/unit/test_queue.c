#include "harness.h"
#include "util/arena.h"
#include "util/queue.h"

TEST(queue, create_empty)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    EXPECT_TRUE(q != NULL);
    EXPECT_TRUE(queue_is_empty(q));
    EXPECT_EQ(queue_size(q), 0);
    EXPECT_TRUE(queue_pop(q) == NULL);
    arena_free(a);
}

TEST(queue, push_pop)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    int x = 10, y = 20, z = 30;
    queue_push(q, &x);
    queue_push(q, &y);
    queue_push(q, &z);
    EXPECT_EQ(queue_size(q), 3);
    EXPECT_TRUE(queue_pop(q) == &x);
    EXPECT_TRUE(queue_pop(q) == &y);
    EXPECT_TRUE(queue_pop(q) == &z);
    EXPECT_TRUE(queue_is_empty(q));
    arena_free(a);
}

TEST(queue, fifo_order)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    int vals[100];
    for (int i = 0; i < 100; i++)
    {
        vals[i] = i;
        queue_push(q, &vals[i]);
    }
    EXPECT_EQ(queue_size(q), 100);
    for (int i = 0; i < 100; i++)
    {
        int *p = (int *) queue_pop(q);
        EXPECT_TRUE(p != NULL);
        EXPECT_EQ(*p, i);
    }
    arena_free(a);
}

TEST(queue, interleaved_push_pop)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    int a1 = 1, a2 = 2, a3 = 3, a4 = 4;
    queue_push(q, &a1);
    queue_push(q, &a2);
    EXPECT_TRUE(queue_pop(q) == &a1);
    queue_push(q, &a3);
    EXPECT_TRUE(queue_pop(q) == &a2);
    queue_push(q, &a4);
    EXPECT_TRUE(queue_pop(q) == &a3);
    EXPECT_TRUE(queue_pop(q) == &a4);
    EXPECT_TRUE(queue_is_empty(q));
    arena_free(a);
}

TEST(queue, growth)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    int vals[256];
    for (int i = 0; i < 256; i++)
    {
        vals[i] = i;
        queue_push(q, &vals[i]);
    }
    EXPECT_EQ(queue_size(q), 256);
    for (int i = 0; i < 256; i++)
    {
        EXPECT_TRUE(queue_pop(q) == &vals[i]);
    }
    arena_free(a);
}

TEST(queue, front_peeks_without_removing)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    EXPECT_TRUE(queue_front(q) == NULL);
    int x = 10, y = 20;
    queue_push(q, &x);
    queue_push(q, &y);
    EXPECT_TRUE(queue_front(q) == &x);
    EXPECT_TRUE(queue_front(q) == &x);
    EXPECT_EQ(queue_size(q), 2);
    EXPECT_TRUE(queue_pop(q) == &x);
    EXPECT_TRUE(queue_front(q) == &y);
    EXPECT_TRUE(queue_pop(q) == &y);
    EXPECT_TRUE(queue_is_empty(q));
    arena_free(a);
}

TEST(queue, growth_with_items_still_queued)
{
    Arena *a = arena_new();
    Queue *q = queue_new(a);
    int vals[13];
    for (int i = 0; i < 5; i++)
    {
        vals[i] = i;
        queue_push(q, &vals[i]);
    }
    for (int i = 0; i < 3; i++)
    {
        EXPECT_TRUE(queue_pop(q) == &vals[i]);
    }
    for (int i = 5; i <= 10; i++)
    {
        vals[i] = i;
        queue_push(q, &vals[i]);
    }
    vals[11] = 11;
    queue_push(q, &vals[11]); /* grows: 8 -> 16 with head = 3 */
    vals[12] = 12;
    queue_push(q, &vals[12]);
    EXPECT_EQ(queue_size(q), 10);
    for (int i = 3; i <= 12; i++)
    {
        EXPECT_TRUE(queue_pop(q) == &vals[i]);
    }
    EXPECT_TRUE(queue_is_empty(q));
    arena_free(a);
}
