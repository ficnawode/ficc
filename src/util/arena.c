#include "arena.h"
#include "assert.h"
#include <stdio.h>
#include <stdlib.h>

#define CHUNK_SIZE 4096

typedef struct Chunk Chunk;
struct Chunk
{
    char *buf;
    size_t cap;
    size_t used;
    Chunk *next;
};

struct Arena
{
    Chunk *head;
};

void arena_oom_abort(void)
{
    fprintf(stderr, "ficc: out of memory\n");
    abort();
}

static Chunk *chunk_new(size_t cap)
{
    Chunk *c = malloc(sizeof(Chunk));
    if (!c)
    {
        arena_oom_abort();
    }
    c->buf = malloc(cap);
    if (!c->buf)
    {
        arena_oom_abort();
    }
    c->cap = cap;
    c->used = 0;
    c->next = NULL;
    return c;
}

Arena *arena_new(void)
{
    Arena *a = malloc(sizeof(Arena));
    if (!a)
    {
        arena_oom_abort();
    }
    a->head = chunk_new(CHUNK_SIZE);
    return a;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    ASSERT(align >= 1 && align <= 16 && (align & (align - 1)) == 0);
    Chunk *c = a->head;
    size_t mask = align - 1;
    size_t pos = (c->used + mask) & ~mask;
    if (size > c->cap || pos > c->cap - size)
    {
        c = chunk_new(size);
        c->next = a->head;
        a->head = c;
        pos = 0;
    }
    c->used = pos + size;
    return c->buf + pos;
}

size_t arena_bytes(const Arena *a)
{
    size_t total = 0;
    for (const Chunk *c = a->head; c; c = c->next)
    {
        total += c->cap;
    }
    return total;
}

void arena_reset(Arena *a)
{
    Chunk *c = a->head;
    while (c)
    {
        Chunk *next = c->next;
        free(c->buf);
        free(c);
        c = next;
    }
    a->head = chunk_new(CHUNK_SIZE);
}

void arena_free(Arena *a)
{
    Chunk *c = a->head;
    while (c)
    {
        Chunk *next = c->next;
        free(c->buf);
        free(c);
        c = next;
    }
    free(a);
}
