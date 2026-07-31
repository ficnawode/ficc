#ifndef FICC_QUEUE_H
#define FICC_QUEUE_H

#include "arena.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct Queue Queue;

Queue *queue_new(Arena *arena);
void queue_push(Queue *q, void *item);
void *queue_front(const Queue *q);
void *queue_pop(Queue *q);
size_t queue_size(const Queue *q);
bool queue_is_empty(const Queue *q);

#endif
