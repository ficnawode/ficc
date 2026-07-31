#ifndef FICC_SBUF_H
#define FICC_SBUF_H

#include "arena.h"

typedef struct Sbuf Sbuf;

Sbuf *sbuf_new(Arena *arena);
void sbuf_append(Sbuf *sb, const char *str);
void sbuf_appendf(Sbuf *sb, const char *fmt, ...);
const char *sbuf_cstr(const Sbuf *sb);
size_t sbuf_len(const Sbuf *sb);

#endif
