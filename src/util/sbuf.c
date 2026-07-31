#include "sbuf.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SBUF_INIT_CAP 64

struct Sbuf
{
    char *data;
    size_t len;
    size_t cap;
    Arena *arena;
};

Sbuf *sbuf_new(Arena *arena)
{
    Sbuf *sb = arena_alloc(arena, sizeof(Sbuf), sizeof(void *));
    sb->len = 0;
    sb->cap = SBUF_INIT_CAP;
    sb->data = arena_alloc(arena, sb->cap, 1);
    sb->data[0] = '\0';
    sb->arena = arena;
    return sb;
}

static void sbuf_ensure(Sbuf *sb, size_t need)
{
    if (need > SIZE_MAX - sb->len - 1)
    {
        arena_oom_abort();
    }
    size_t want = sb->len + need + 1;
    if (want > sb->cap)
    {
        while (want > sb->cap)
        {
            if (sb->cap > SIZE_MAX / 2)
            {
                arena_oom_abort();
            }
            sb->cap *= 2;
        }
        char *old = sb->data;
        sb->data = arena_alloc(sb->arena, sb->cap, 1);
        memcpy(sb->data, old, sb->len);
    }
}

void sbuf_append(Sbuf *sb, const char *str)
{
    size_t n = strlen(str);
    sbuf_ensure(sb, n);
    memcpy(sb->data + sb->len, str, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sbuf_appendf(Sbuf *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0)
    {
        return;
    }
    sbuf_ensure(sb, (size_t) n);
    va_start(args, fmt);
    vsnprintf(sb->data + sb->len, (size_t) n + 1, fmt, args);
    va_end(args);
    sb->len += (size_t) n;
}

const char *sbuf_cstr(const Sbuf *sb)
{
    return sb->data;
}

size_t sbuf_len(const Sbuf *sb)
{
    return sb->len;
}
