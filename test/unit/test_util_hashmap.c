#include "harness.h"
#include "util/arena.h"
#include "util/hashmap.h"

TEST(strmap, basic_set_get)
{
    Arena *a = arena_new();
    StrMap *m = strmap_new(a);
    int x = 42;
    strmap_set(m, "foo", &x);
    EXPECT_TRUE(strmap_get(m, "foo") == &x);
    arena_free(a);
}

TEST(strmap, missing_key)
{
    Arena *a = arena_new();
    StrMap *m = strmap_new(a);
    EXPECT_NULL(strmap_get(m, "nope"));
    arena_free(a);
}

TEST(strmap, overwrite)
{
    Arena *a = arena_new();
    StrMap *m = strmap_new(a);
    int x = 1, y = 2;
    strmap_set(m, "k", &x);
    strmap_set(m, "k", &y);
    EXPECT_TRUE(strmap_get(m, "k") == &y);
    arena_free(a);
}

TEST(strmap, many_keys)
{
    Arena *a = arena_new();
    StrMap *m = strmap_new(a);
    int vals[256];
    /* distinct stable key buffers so rehash doesn't see mutated keys */
    char keys[256][8];
    for (int i = 0; i < 256; i++)
    {
        vals[i] = i;
        keys[i][0] = 'k';
        keys[i][1] = '0' + (i % 10);
        keys[i][2] = 'a' + ((i / 10) % 26);
        keys[i][3] = '\0';
        strmap_set(m, keys[i], &vals[i]);
    }
    for (int i = 0; i < 256; i++)
    {
        int *p = (int *) strmap_get(m, keys[i]);
        EXPECT_NOTNULL(p);
        EXPECT_EQ(*p, i);
    }
    arena_free(a);
}

TEST(strmap, collision_handling)
{
    Arena *a = arena_new();
    StrMap *m = strmap_new(a);
    /* Keys that may collide or probe same slots */
    strmap_set(m, "a", (void *) 1);
    strmap_set(m, "b", (void *) 2);
    strmap_set(m, "c", (void *) 3);
    EXPECT_TRUE(strmap_get(m, "a") == (void *) 1);
    EXPECT_TRUE(strmap_get(m, "b") == (void *) 2);
    EXPECT_TRUE(strmap_get(m, "c") == (void *) 3);
    arena_free(a);
}

TEST(u64map, basic_set_get)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    int x = 42;
    u64map_set(m, 7, &x);
    EXPECT_TRUE(u64map_get(m, 7) == &x);
    EXPECT_NULL(u64map_get(m, 99));
    arena_free(a);
}

TEST(u64map, many_keys)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    int vals[512];
    for (int i = 0; i < 512; i++)
    {
        vals[i] = i;
        u64map_set(m, (u64) i, &vals[i]);
    }
    for (int i = 0; i < 512; i++)
    {
        int *p = (int *) u64map_get(m, (u64) i);
        EXPECT_NOTNULL(p);
        EXPECT_EQ(*p, i);
    }
    arena_free(a);
}

TEST(u64map, overwrite)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    int x = 1, y = 2;
    u64map_set(m, 123, &x);
    u64map_set(m, 123, &y);
    EXPECT_TRUE(u64map_get(m, 123) == &y);
    arena_free(a);
}

TEST(u64map, same_slot_collision_chain)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    /* all six hash to slot 9 at cap 16 (distinct full hashes, verified
       by construction) — forces a real linear-probe chain */
    u64 keys[] = {19, 23, 31, 35, 37, 40};
    int vals[6];
    for (int i = 0; i < 6; i++)
    {
        vals[i] = i;
        u64map_set(m, keys[i], &vals[i]);
    }
    for (int i = 0; i < 6; i++)
    {
        EXPECT_TRUE(u64map_get(m, keys[i]) == &vals[i]);
    }
    /* a key hashing to another slot is unaffected */
    int other = 99;
    u64map_set(m, 1000, &other);
    EXPECT_TRUE(u64map_get(m, 1000) == &other);
    arena_free(a);
}

TEST(hashmap, remove_basic)
{
    Arena *a = arena_new();
    HashMap *m = hashmap_new(a, hashmap_str_hash, hashmap_str_eq);
    int x = 42;
    hashmap_set(m, "k", &x);
    EXPECT_TRUE(hashmap_contains(m, "k"));
    EXPECT_TRUE(hashmap_remove(m, "k"));
    EXPECT_FALSE(hashmap_contains(m, "k"));
    EXPECT_NULL(hashmap_get(m, "k"));
    EXPECT_FALSE(hashmap_remove(m, "k"));
    arena_free(a);
}

TEST(hashmap, remove_missing)
{
    Arena *a = arena_new();
    HashMap *m = hashmap_new(a, hashmap_str_hash, hashmap_str_eq);
    EXPECT_FALSE(hashmap_remove(m, "absent"));
    arena_free(a);
}

TEST(u64map, remove_mid_chain)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    /* all hash to slot 9 at cap 16, occupying slots 9..14 in insertion
       order; removing 23 (slot 10) must NOT truncate the probe chain —
       get(31) has to skip the tombstone and keep probing */
    u64 keys[] = {19, 23, 31, 35, 37, 40};
    int vals[6];
    for (int i = 0; i < 6; i++)
    {
        vals[i] = i;
        u64map_set(m, keys[i], &vals[i]);
    }
    EXPECT_TRUE(hashmap_remove((HashMap *) m, (const void *) (uintptr_t) 23));
    EXPECT_NULL(hashmap_get((HashMap *) m, (const void *) (uintptr_t) 23));
    for (int i = 0; i < 6; i++)
    {
        if (keys[i] != 23)
        {
            EXPECT_TRUE(u64map_get(m, keys[i]) == &vals[i]);
        }
    }
    arena_free(a);
}

TEST(u64map, remove_then_reinsert)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    int x = 1, y = 2;
    u64map_set(m, 23, &x);
    EXPECT_TRUE(hashmap_remove((HashMap *) m, (const void *) (uintptr_t) 23));
    u64map_set(m, 23, &y);
    EXPECT_TRUE(u64map_get(m, 23) == &y);
    arena_free(a);
}

TEST(u64map, remove_recycles_tombstone)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    /* remove 23 (slot 10) leaves a tombstone; 55 also hashes to slot 9
       and its insert probes past it, so set() must recycle the tombstone
       rather than stop at the chain end */
    u64 keys[] = {19, 23, 31, 35, 37, 40};
    int vals[6];
    for (int i = 0; i < 6; i++)
    {
        vals[i] = i;
        u64map_set(m, keys[i], &vals[i]);
    }
    EXPECT_TRUE(hashmap_remove((HashMap *) m, (const void *) (uintptr_t) 23));
    int v55 = 55;
    u64map_set(m, 55, &v55);
    EXPECT_TRUE(u64map_get(m, 55) == &v55);
    for (int i = 0; i < 6; i++)
    {
        if (keys[i] != 23)
        {
            EXPECT_TRUE(u64map_get(m, keys[i]) == &vals[i]);
        }
    }
    arena_free(a);
}

TEST(u64map, remove_many_after_growth)
{
    Arena *a = arena_new();
    U64Map *m = u64map_new(a);
    int vals[256];
    for (int i = 0; i < 256; i++)
    {
        vals[i] = i;
        u64map_set(m, (u64) i, &vals[i]);
    }
    for (int i = 0; i < 256; i += 2)
    {
        EXPECT_TRUE(hashmap_remove((HashMap *) m, (const void *) (uintptr_t) i));
    }
    for (int i = 1; i < 256; i += 2)
    {
        EXPECT_TRUE(u64map_get(m, (u64) i) == &vals[i]);
    }
    for (int i = 0; i < 256; i += 2)
    {
        EXPECT_NULL(u64map_get(m, (u64) i));
    }
    arena_free(a);
}

TEST(hashset, add_contains)
{
    Arena *a = arena_new();
    HashSet *s = hashset_new(a, hashmap_str_hash, hashmap_str_eq);
    hashset_add(s, "foo");
    EXPECT_TRUE(hashset_contains(s, "foo"));
    EXPECT_FALSE(hashset_contains(s, "bar"));
    arena_free(a);
}

TEST(hashset, get_returns_canonical)
{
    Arena *a = arena_new();
    HashSet *s = hashset_new(a, hashmap_str_hash, hashmap_str_eq);
    /* interning contract: an equal key probes to the first stored copy,
       and get() hands that exact pointer back */
    char buf1[8] = "foo";
    char buf2[8] = "foo";
    hashset_add(s, buf1);
    EXPECT_TRUE(hashset_contains(s, buf2));
    EXPECT_TRUE(hashset_get(s, buf2) == buf1);
    arena_free(a);
}
