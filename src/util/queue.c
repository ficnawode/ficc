#include "queue.h"
#include <stdint.h>

#define QUEUE_INIT_CAP 8

struct Queue
{
    void **data;
    size_t cap;
    size_t head;
    size_t tail;
    size_t len;
    Arena *arena;
};

Queue *queue_new(Arena *arena)
{
    Queue *q = arena_alloc(arena, sizeof(Queue), sizeof(void *));
    q->cap = QUEUE_INIT_CAP;
    q->data = arena_alloc(arena, q->cap * sizeof(void *), sizeof(void *));
    q->head = 0;
    q->tail = 0;
    q->len = 0;
    q->arena = arena;
    return q;
}

static void queue_grow(Queue *q)
{
    size_t old_cap = q->cap;
    /* cap * 2 * sizeof(void *) must not wrap */
    if (old_cap > SIZE_MAX / (2 * sizeof(void *)))
    {
        arena_oom_abort();
    }
    q->cap *= 2;
    void **old = q->data;
    q->data = arena_alloc(q->arena, q->cap * sizeof(void *), sizeof(void *));
    for (size_t i = 0; i < q->len; i++)
    {
        q->data[i] = old[(q->head + i) % old_cap];
    }
    q->head = 0;
    q->tail = q->len;
}

void queue_push(Queue *q, void *item)
{
    if (q->len >= q->cap)
    {
        queue_grow(q);
    }
    q->data[q->tail] = item;
    q->tail = (q->tail + 1) % q->cap;
    q->len++;
}

void *queue_front(const Queue *q)
{
    if (q->len == 0)
    {
        return NULL;
    }
    return q->data[q->head];
}

void *queue_pop(Queue *q)
{
    void *item = queue_front(q);
    if (item == NULL)
    {
        return NULL;
    }
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return item;
}

size_t queue_size(const Queue *q)
{
    return q->len;
}

bool queue_is_empty(const Queue *q)
{
    return q->len == 0;
}
