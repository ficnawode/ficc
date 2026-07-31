#ifndef FICC_HASHMAP_H
#define FICC_HASHMAP_H

#include "arena.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct HashMap HashMap;

HashMap *hashmap_new(Arena *arena, u64 (*hash)(const void *key),
                     bool (*eq)(const void *a, const void *b));
void hashmap_set(HashMap *m, const void *key, void *value);
void *hashmap_get(const HashMap *m, const void *key);
bool hashmap_remove(HashMap *m, const void *key);
bool hashmap_contains(const HashMap *m, const void *key);
u64 hashmap_str_hash(const void *key);
bool hashmap_str_eq(const void *a, const void *b);

/* StrMap: const char* -> void* */
typedef HashMap StrMap;
StrMap *strmap_new(Arena *arena);
void strmap_set(StrMap *m, const char *key, void *value);
void *strmap_get(const StrMap *m, const char *key);

/* U64Map: u64 -> void* */
typedef HashMap U64Map;
U64Map *u64map_new(Arena *arena);
void u64map_set(U64Map *m, u64 key, void *value);
void *u64map_get(const U64Map *m, u64 key);

/* HashSet: HashMap with value == key — membership only */
typedef HashMap HashSet;
HashSet *hashset_new(Arena *arena, u64 (*hash)(const void *key),
                     bool (*eq)(const void *a, const void *b));
void hashset_add(HashSet *s, void *key);
bool hashset_contains(const HashSet *s, const void *key);
const void *hashset_get(const HashSet *s, const void *key);

#endif
