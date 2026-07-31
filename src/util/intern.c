#include "intern.h"
#include "hashmap.h"
#include <string.h>

struct InternPool
{
    HashSet *set;
    Arena *arena;
};

InternPool *intern_pool_new(Arena *arena)
{
    InternPool *pool = arena_alloc(arena, sizeof(InternPool), sizeof(void *));
    pool->set = hashset_new(arena, hashmap_str_hash, hashmap_str_eq);
    pool->arena = arena;
    return pool;
}

const char *intern(InternPool *pool, const char *s)
{
    const char *existing = hashset_get(pool->set, s);
    if (existing)
    {
        return existing;
    }
    size_t n = strlen(s) + 1;
    char *copy = arena_alloc(pool->arena, n, 1);
    memcpy(copy, s, n);
    hashset_add(pool->set, copy);
    return copy;
}
