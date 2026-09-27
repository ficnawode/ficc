#ifndef FICC_ARENA_H
#define FICC_ARENA_H

#include <stddef.h>

typedef struct Arena Arena;

Arena *arena_new(void);
void *arena_alloc(Arena *a, size_t size, size_t align);
void arena_reset(Arena *a);
void arena_free(Arena *a);
void arena_oom_abort(void);
size_t arena_bytes(const Arena *a);

#endif
