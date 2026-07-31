#ifndef FICC_VEC_H
#define FICC_VEC_H

#include "arena.h"
#include <stddef.h>

typedef struct Vec Vec;

Vec *vec_new(Arena *arena);
void vec_push(Vec *v, void *item);
size_t vec_size(const Vec *v);
void *vec_get(const Vec *v, size_t i);
void *vec_last(const Vec *v);
void *vec_pop(Vec *v);

#endif
