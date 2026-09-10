#include "pp.h"

#include <stdio.h>
#include <string.h>

static bool pp_is_trivia(const PpToken *tok)
{
    return tok->kind == TOK_PP_TRIVIA_WS || tok->kind == TOK_PP_TRIVIA_NL ||
           tok->kind == TOK_PP_TRIVIA_COMMENT;
}

static bool pp_spelling_is(const PpToken *tok, const char *spelling)
{
    size_t len = strlen(spelling);
    return tok->len == len && strncmp(tok->spell, spelling, len) == 0;
}

/* Directives are introduced by `#` and its digraph `%:` (C11 §6.4.6/§6.10). */
static bool pp_is_hash(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && (pp_spelling_is(tok, "#") || pp_spelling_is(tok, "%:"));
}

Pp *pp_new(Arena *arena)
{
    Pp *pp = arena_alloc(arena, sizeof(Pp), sizeof(void *));
    pp->arena = arena;
    pp->out = vec_new(arena);
    pp->includes = vec_new(arena);
    pp->macros = strmap_new(arena);
    pp->conds = vec_new(arena);
    pp->include_paths = vec_new(arena);
    pp->physical = (Loc) {.file = NULL, .line = 1, .col = 1};
    pp->presumed = (Loc) {.file = NULL, .line = 1, .col = 1};
    pp->skipping = false;
    pp->pedantic = false;
    pp->nostdinc = false;
    pp->has_source_date_epoch = false;
    pp->source_date_epoch = 0;
    pp->error_count = 0;
    pp->warning_count = 0;
    return pp;
}

void pp_free(Pp *pp)
{
    (void) pp;
}

static void pp_push_include(Pp *pp, const char *file, Vec *tokens)
{
    PpIncludeFrame *frame = arena_alloc(pp->arena, sizeof(PpIncludeFrame), sizeof(void *));
    frame->file = file;
    frame->tokens = tokens;
    frame->cursor = 0;
    vec_push(pp->includes, frame);
}

static void pp_emit_range(Pp *pp, const PpIncludeFrame *frame, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; i++)
    {
        vec_push(pp->out, vec_get(frame->tokens, i));
    }
}

static void pp_warn_directive(Pp *pp, const PpToken *name)
{
    fprintf(stderr, "%s:%u:%u: [pp] warning: invalid preprocessing directive #%.*s\n",
            name->loc.file, name->loc.line, name->loc.col, (int) name->len, name->spell);
    pp->warning_count++;
}

/* Consumes a directive line. The null directive is a no-op; every other
   directive name is not implemented yet, so it warns and acts as null. */
static void pp_directive(Pp *pp, const PpIncludeFrame *frame, size_t begin, size_t end)
{
    size_t i = begin;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    i++; /* the introducing # */
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i < end)
    {
        pp_warn_directive(pp, vec_get(frame->tokens, i));
    }
}

static void pp_line(Pp *pp, PpIncludeFrame *frame)
{
    size_t begin = frame->cursor;
    size_t end = begin;
    size_t count = vec_size(frame->tokens);
    const PpToken *first = NULL;

    while (end < count)
    {
        PpToken *tok = vec_get(frame->tokens, end);
        if (tok->kind == TOK_PP_EOF)
        {
            break;
        }
        end++;
        if (pp_is_trivia(tok))
        {
            if (tok->has_newline)
            {
                break;
            }
        }
        else if (!first)
        {
            first = tok;
        }
    }
    frame->cursor = end;

    if (first && pp_is_hash(first))
    {
        pp_directive(pp, frame, begin, end);
    }
    else
    {
        pp_emit_range(pp, frame, begin, end);
    }
}

static void pp_run(Pp *pp)
{
    while (vec_size(pp->includes) > 0)
    {
        PpIncludeFrame *frame = vec_last(pp->includes);
        if (frame->cursor >= vec_size(frame->tokens))
        {
            vec_pop(pp->includes);
            continue;
        }
        PpToken *tok = vec_get(frame->tokens, frame->cursor);
        if (tok->kind == TOK_PP_EOF)
        {
            vec_pop(pp->includes);
            continue;
        }
        pp_line(pp, frame);
    }
}

Vec *pp_preprocess(Pp *pp, const char *file, const char *src)
{
    Vec *tokens = pp_lex(file, src, pp->arena);
    if (!tokens)
    {
        return NULL;
    }
    pp_push_include(pp, file, tokens);
    pp_run(pp);
    return pp->out;
}

void pp_dump(const Vec *soup)
{
    for (size_t i = 0; i < vec_size(soup); i++)
    {
        const PpToken *tok = vec_get(soup, i);
        printf("%s:%u:%u %s", tok->loc.file, tok->loc.line, tok->loc.col, pp_kind_name(tok->kind));
        if (tok->has_newline)
        {
            printf(" nl");
        }
        if (tok->len > 0)
        {
            printf(" %.*s", (int) tok->len, tok->spell);
        }
        printf("\n");
    }
}
