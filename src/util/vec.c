#include "vec.h"
#include "assert.h"
#include <stdint.h>

#define VEC_INIT_CAP 8

struct Vec
{
    void **data;
    size_t len;
    size_t cap;
    Arena *arena;
};

Vec *vec_new(Arena *arena)
{
    Vec *v = arena_alloc(arena, sizeof(Vec), sizeof(void *));
    v->len = 0;
    v->cap = VEC_INIT_CAP;
    v->data = arena_alloc(arena, v->cap * sizeof(void *), sizeof(void *));
    v->arena = arena;
    return v;
}

void vec_push(Vec *v, void *item)
{
    if (v->len >= v->cap)
    {
        size_t old_cap = v->cap;
        /* cap * 2 * sizeof(void *) must not wrap */
        if (old_cap > SIZE_MAX / (2 * sizeof(void *)))
        {
            arena_oom_abort();
        }
        v->cap *= 2;
        void **old = v->data;
        v->data = arena_alloc(v->arena, v->cap * sizeof(void *), sizeof(void *));
        for (size_t i = 0; i < old_cap; i++)
        {
            v->data[i] = old[i];
        }
    }
    v->data[v->len++] = item;
}

size_t vec_size(const Vec *v)
{
    return v->len;
}

void *vec_get(const Vec *v, size_t i)
{
    ASSERT(i < v->len);
    return v->data[i];
}

void *vec_last(const Vec *v)
{
    ASSERT(v->len > 0);
    return v->data[v->len - 1];
}

void *vec_pop(Vec *v)
{
    void *item = vec_last(v);
    v->len--;
    return item;
}

void vec_insert(Vec *v, size_t i, void *item)
{
    ASSERT(i <= v->len);
    if (v->len >= v->cap)
    {
        size_t old_cap = v->cap;
        if (old_cap > SIZE_MAX / (2 * sizeof(void *)))
        {
            arena_oom_abort();
        }
        v->cap *= 2;
        void **old = v->data;
        v->data = arena_alloc(v->arena, v->cap * sizeof(void *), sizeof(void *));
        for (size_t j = 0; j < old_cap; j++)
        {
            v->data[j] = old[j];
        }
    }
    for (size_t j = v->len; j > i; j--)
    {
        v->data[j] = v->data[j - 1];
    }
    v->data[i] = item;
    v->len++;
}
