#include "hashmap.h"
#include <stdint.h>
#include <string.h>

#define HASHMAP_INIT_CAP 16
#define LOAD_FACTOR_NUM 3
#define LOAD_FACTOR_DEN 4

typedef enum
{
    ENTRY_EMPTY,
    ENTRY_OCCUPIED,
    ENTRY_TOMBSTONE
} EntryState;

typedef struct
{
    u64 hash;
    const void *key;
    void *value;
    EntryState state;
} HashMapEntry;

struct HashMap
{
    HashMapEntry *entries;
    size_t cap;
    size_t len;
    size_t tombstones;
    Arena *arena;
    u64 (*hash)(const void *key);
    bool (*eq)(const void *a, const void *b);
};

/* FNV-1a 64-bit: cheap for short C strings. */
u64 hashmap_str_hash(const void *key)
{
    const char *s = key;
    u64 h = 14695981039346656037ULL;
    while (*s)
    {
        h ^= (u64) (unsigned char) *s;
        h *= 1099511628211ULL;
        s++;
    }
    return h;
}

bool hashmap_str_eq(const void *a, const void *b)
{
    return strcmp((const char *) a, (const char *) b) == 0;
}

/* MurmurHash3 finalizer: avalanche separates nearby integer keys. */
static u64 hash_u64(const void *key)
{
    u64 k = (u64) (uintptr_t) key;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static bool eq_u64(const void *a, const void *b)
{
    return a == b;
}

static HashMap *hashmap_new_with_cap(Arena *arena, size_t cap, u64 (*hash)(const void *key),
                                     bool (*eq)(const void *a, const void *b))
{
    HashMap *m = arena_alloc(arena, sizeof(HashMap), _Alignof(HashMap));
    m->cap = cap;
    m->len = 0;
    m->tombstones = 0;
    m->entries = arena_alloc(arena, cap * sizeof(HashMapEntry), _Alignof(HashMapEntry));
    for (size_t i = 0; i < cap; i++)
    {
        m->entries[i].state = ENTRY_EMPTY;
    }
    m->arena = arena;
    m->hash = hash;
    m->eq = eq;
    return m;
}

HashMap *hashmap_new(Arena *arena, u64 (*hash)(const void *key),
                     bool (*eq)(const void *a, const void *b))
{
    return hashmap_new_with_cap(arena, HASHMAP_INIT_CAP, hash, eq);
}

static void hashmap_rehash(HashMap *m)
{
    size_t old_cap = m->cap;
    HashMapEntry *old = m->entries;
    if (old_cap > SIZE_MAX / (2 * sizeof(HashMapEntry)))
    {
        arena_oom_abort();
    }
    m->cap *= 2;
    m->entries = arena_alloc(m->arena, m->cap * sizeof(HashMapEntry), _Alignof(HashMapEntry));
    for (size_t i = 0; i < m->cap; i++)
    {
        m->entries[i].state = ENTRY_EMPTY;
    }
    m->len = 0;
    m->tombstones = 0;
    for (size_t i = 0; i < old_cap; i++)
    {
        if (old[i].state == ENTRY_OCCUPIED)
        {
            hashmap_set(m, old[i].key, old[i].value);
        }
    }
}

void hashmap_set(HashMap *m, const void *key, void *value)
{
    if ((m->len + m->tombstones) * LOAD_FACTOR_DEN >= m->cap * LOAD_FACTOR_NUM)
    {
        hashmap_rehash(m);
    }
    u64 h = m->hash(key);
    size_t mask = m->cap - 1;
    size_t idx = (size_t) (h & mask);
    size_t first_tomb = SIZE_MAX;
    for (size_t i = 0; i < m->cap; i++)
    {
        HashMapEntry *e = &m->entries[idx];
        if (e->state == ENTRY_EMPTY)
        {
            if (first_tomb != SIZE_MAX)
            {
                idx = first_tomb;
                e = &m->entries[idx];
                m->tombstones--;
            }
            e->hash = h;
            e->key = key;
            e->value = value;
            e->state = ENTRY_OCCUPIED;
            m->len++;
            return;
        }
        if (e->state == ENTRY_OCCUPIED && e->hash == h && m->eq(e->key, key))
        {
            e->value = value;
            return;
        }
        if (e->state == ENTRY_TOMBSTONE && first_tomb == SIZE_MAX)
        {
            first_tomb = idx;
        }
        idx = (idx + 1) & mask;
    }
}

void *hashmap_get(const HashMap *m, const void *key)
{
    u64 h = m->hash(key);
    size_t mask = m->cap - 1;
    size_t idx = (size_t) (h & mask);
    for (size_t i = 0; i < m->cap; i++)
    {
        HashMapEntry *e = &m->entries[idx];
        if (e->state == ENTRY_EMPTY)
        {
            return NULL;
        }
        if (e->state == ENTRY_OCCUPIED && e->hash == h && m->eq(e->key, key))
        {
            return e->value;
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}

bool hashmap_remove(HashMap *m, const void *key)
{
    u64 h = m->hash(key);
    size_t mask = m->cap - 1;
    size_t idx = (size_t) (h & mask);
    for (size_t i = 0; i < m->cap; i++)
    {
        HashMapEntry *e = &m->entries[idx];
        if (e->state == ENTRY_EMPTY)
        {
            return false;
        }
        if (e->state == ENTRY_OCCUPIED && e->hash == h && m->eq(e->key, key))
        {
            e->state = ENTRY_TOMBSTONE;
            m->len--;
            m->tombstones++;
            return true;
        }
        idx = (idx + 1) & mask;
    }
    return false;
}

bool hashmap_contains(const HashMap *m, const void *key)
{
    return hashmap_get(m, key) != NULL;
}

StrMap *strmap_new(Arena *arena)
{
    return hashmap_new_with_cap(arena, HASHMAP_INIT_CAP, hashmap_str_hash, hashmap_str_eq);
}

void strmap_set(StrMap *m, const char *key, void *value)
{
    hashmap_set(m, key, value);
}

void *strmap_get(const StrMap *m, const char *key)
{
    return hashmap_get(m, key);
}

U64Map *u64map_new(Arena *arena)
{
    return hashmap_new_with_cap(arena, HASHMAP_INIT_CAP, hash_u64, eq_u64);
}

void u64map_set(U64Map *m, u64 key, void *value)
{
    hashmap_set(m, (const void *) (uintptr_t) key, value);
}

void *u64map_get(const U64Map *m, u64 key)
{
    return hashmap_get(m, (const void *) (uintptr_t) key);
}

HashSet *hashset_new(Arena *arena, u64 (*hash)(const void *key),
                     bool (*eq)(const void *a, const void *b))
{
    return hashmap_new(arena, hash, eq);
}

void hashset_add(HashSet *s, void *key)
{
    hashmap_set(s, key, key);
}

bool hashset_contains(const HashSet *s, const void *key)
{
    return hashmap_get(s, key) != NULL;
}

const void *hashset_get(const HashSet *s, const void *key)
{
    return hashmap_get(s, key);
}
