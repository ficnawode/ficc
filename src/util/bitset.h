#ifndef FICC_BITSET_H
#define FICC_BITSET_H

#include "arena.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct Bitset Bitset;

Bitset *bitset_new(Arena *arena, size_t nbits);
void bitset_set(Bitset *bs, size_t i);
void bitset_clear(Bitset *bs, size_t i);
bool bitset_test(const Bitset *bs, size_t i);
size_t bitset_count(const Bitset *bs);
void bitset_and(Bitset *dst, const Bitset *src);
void bitset_or(Bitset *dst, const Bitset *src);

/* Word-level access: `bitset_nwords` u64 words, vreg 0 in the low bit. */
size_t bitset_nwords(const Bitset *bs);
u64 *bitset_words(Bitset *bs);

#endif
