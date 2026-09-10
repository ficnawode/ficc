#include "pp_lex.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char pp_trigraph(char c)
{
    switch (c)
    {
        case '=':
            return '#';
        case '/':
            return '\\';
        case '\'':
            return '^';
        case '(':
            return '[';
        case ')':
            return ']';
        case '!':
            return '|';
        case '<':
            return '{';
        case '>':
            return '}';
        case '-':
            return '~';
        default:
            return '\0';
    }
}

/* Returns the width of a trigraph at src[i] and writes its replacement to
 *out; 0 when src[i] does not begin one. */
static u32 pp_trigraph_width(const char *src, size_t i, size_t n, char *out)
{
    if (src[i] != '?' || i + 2 >= n || src[i + 1] != '?')
    {
        return 0;
    }
    char replaced = pp_trigraph(src[i + 2]);
    if (replaced == '\0')
    {
        return 0;
    }
    *out = replaced;
    return 3;
}

static void pp_prepare_error(const char *file, Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s:%u:%u: [pp] error: ", file, loc.line, loc.col);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

char *pp_prepare(const char *file, const char *src, Arena *arena)
{
    size_t n = strlen(src);
    char *out = arena_alloc(arena, n + 1, 1);
    size_t i = 0;
    size_t o = 0;
    u32 line = 1;
    u32 col = 1;

    while (i < n)
    {
        Loc loc = {.file = file, .line = line, .col = col};
        char c = src[i];
        u32 width = pp_trigraph_width(src, i, n, &c);
        if (width == 0)
        {
            width = 1;
        }

        if (c == '\\' && i + width >= n)
        {
            pp_prepare_error(file, loc, "backslash at end of file");
            return NULL;
        }
        if (c == '\\' && src[i + width] == '\n')
        {
            i += width + 1;
            line++;
            col = 1;
            continue;
        }

        out[o++] = c;
        if (c == '\n')
        {
            line++;
            col = 1;
        }
        else
        {
            col += width;
        }
        i += width;
    }
    out[o] = '\0';
    return out;
}
