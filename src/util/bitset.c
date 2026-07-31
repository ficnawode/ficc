#include "bitset.h"

#define WORD_BITS 64

typedef u64 Word;

struct Bitset
{
    Word *words;
    size_t nwords;
    size_t nbits;
    Arena *arena;
};

Bitset *bitset_new(Arena *arena, size_t nbits)
{
    if (nbits > SIZE_MAX - (WORD_BITS - 1) || nbits / WORD_BITS > SIZE_MAX / sizeof(Word))
    {
        arena_oom_abort();
    }
    Bitset *bs = arena_alloc(arena, sizeof(Bitset), sizeof(void *));
    bs->nbits = nbits;
    bs->nwords = (nbits + WORD_BITS - 1) / WORD_BITS;
    bs->words = arena_alloc(arena, bs->nwords * sizeof(Word), sizeof(Word));
    for (size_t i = 0; i < bs->nwords; i++)
    {
        bs->words[i] = 0;
    }
    bs->arena = arena;
    return bs;
}

void bitset_set(Bitset *bs, size_t i)
{
    if (i >= bs->nbits)
    {
        return;
    }
    bs->words[i / WORD_BITS] |= (1ULL << (i % WORD_BITS));
}

void bitset_clear(Bitset *bs, size_t i)
{
    if (i >= bs->nbits)
    {
        return;
    }
    bs->words[i / WORD_BITS] &= ~(1ULL << (i % WORD_BITS));
}

bool bitset_test(const Bitset *bs, size_t i)
{
    if (i >= bs->nbits)
    {
        return false;
    }
    return (bs->words[i / WORD_BITS] >> (i % WORD_BITS)) & 1ULL;
}

size_t bitset_count(const Bitset *bs)
{
    size_t c = 0;
    for (size_t i = 0; i < bs->nwords; i++)
    {
        Word w = bs->words[i];
        if (i == bs->nwords - 1 && bs->nbits % WORD_BITS != 0)
        {
            w &= ((1ULL << (bs->nbits % WORD_BITS)) - 1);
        }
        while (w)
        {
            c += w & 1ULL;
            w >>= 1;
        }
    }
    return c;
}

void bitset_and(Bitset *dst, const Bitset *src)
{
    size_t n = dst->nwords < src->nwords ? dst->nwords : src->nwords;
    for (size_t i = 0; i < n; i++)
    {
        dst->words[i] &= src->words[i];
    }
    /* zero remaining words in dst: implicit zeros in src */
    for (size_t i = n; i < dst->nwords; i++)
    {
        dst->words[i] = 0;
    }
}

void bitset_or(Bitset *dst, const Bitset *src)
{
    size_t n = dst->nwords < src->nwords ? dst->nwords : src->nwords;
    for (size_t i = 0; i < n; i++)
    {
        dst->words[i] |= src->words[i];
    }
}
