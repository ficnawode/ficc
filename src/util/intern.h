#ifndef FICC_INTERN_H
#define FICC_INTERN_H

#include "arena.h"

typedef struct InternPool InternPool;

InternPool *intern_pool_new(Arena *arena);
const char *intern(InternPool *pool, const char *s);

#endif
