#include "pp.h"

#include <stdarg.h>
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
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_HASH;
}

static bool pp_is_lparen(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_LPAREN;
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

static void pp_vdiag(Loc loc, const char *level, const char *fmt, va_list args)
{
    fprintf(stderr, "%s:%u:%u: [pp] %s: ", loc.file, loc.line, loc.col, level);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

static void pp_error(Pp *pp, Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pp_vdiag(loc, "error", fmt, args);
    va_end(args);
    pp->error_count++;
}

static void pp_warn(Pp *pp, Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pp_vdiag(loc, "warning", fmt, args);
    va_end(args);
    pp->warning_count++;
}

static void pp_note(Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pp_vdiag(loc, "note", fmt, args);
    va_end(args);
}

static const char *pp_token_name(Pp *pp, const PpToken *tok)
{
    char *buf = arena_alloc(pp->arena, tok->len + 1, 1);
    memcpy(buf, tok->spell, tok->len);
    buf[tok->len] = '\0';
    return buf;
}

static Macro *pp_macro_lookup(Pp *pp, const PpToken *tok)
{
    return strmap_get(pp->macros, pp_token_name(pp, tok));
}

/* Copies the real (non-trivia) tokens in [start, end) into a fresh vector. */
static Vec *pp_collect_body(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    Vec *body = vec_new(pp->arena);
    for (size_t i = start; i < end; i++)
    {
        PpToken *tok = vec_get(frame->tokens, i);
        if (!pp_is_trivia(tok))
        {
            vec_push(body, tok);
        }
    }
    return body;
}

static bool pp_bodies_equal(const Vec *a, const Vec *b)
{
    if (vec_size(a) != vec_size(b))
    {
        return false;
    }
    for (size_t i = 0; i < vec_size(a); i++)
    {
        const PpToken *x = vec_get(a, i);
        const PpToken *y = vec_get(b, i);
        if (x->kind != y->kind || x->len != y->len || strncmp(x->spell, y->spell, x->len) != 0)
        {
            return false;
        }
    }
    return true;
}

static void pp_define(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                      Loc directive_loc)
{
    size_t i = start;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i >= end)
    {
        pp_error(pp, directive_loc, "macro name missing");
        return;
    }

    PpToken *name = vec_get(frame->tokens, i);
    if (name->kind != TOK_PP_IDENT)
    {
        pp_error(pp, name->loc, "macro names must be identifiers");
        return;
    }

    const char *text = pp_token_name(pp, name);
    bool function_like = i + 1 < end && pp_is_lparen(vec_get(frame->tokens, i + 1));
    MacroKind kind = function_like ? MACRO_FUNC : MACRO_OBJ;
    Vec *body = pp_collect_body(pp, frame, i + 1, end);
    Macro *prev = strmap_get(pp->macros, text);
    if (prev)
    {
        if (prev->kind == kind && pp_bodies_equal(prev->body, body))
        {
            return;
        }
        pp_error(pp, name->loc, "redefinition of macro '%s'", text);
        pp_note(prev->loc, "previous definition of '%s' is here", text);
        return;
    }

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    macro->name = text;
    macro->kind = kind;
    macro->expanding = false;
    macro->loc = name->loc;
    macro->body = body;
    strmap_set(pp->macros, text, macro);
}

static void pp_undef(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                     Loc directive_loc)
{
    size_t i = start;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i >= end)
    {
        pp_error(pp, directive_loc, "macro name missing");
        return;
    }

    PpToken *name = vec_get(frame->tokens, i);
    if (name->kind != TOK_PP_IDENT)
    {
        pp_error(pp, name->loc, "macro names must be identifiers");
        return;
    }
    hashmap_remove(pp->macros, pp_token_name(pp, name));
}

static void pp_directive(Pp *pp, const PpIncludeFrame *frame, size_t begin, size_t end)
{
    size_t i = begin;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    Loc directive_loc = ((PpToken *) vec_get(frame->tokens, i))->loc;
    i++; /* the introducing # */
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i >= end)
    {
        return; /* null directive */
    }

    PpToken *name = vec_get(frame->tokens, i);
    if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "define"))
    {
        pp_define(pp, frame, i + 1, end, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "undef"))
    {
        pp_undef(pp, frame, i + 1, end, directive_loc);
    }
    else
    {
        pp_warn(pp, name->loc, "invalid preprocessing directive #%.*s", (int) name->len,
                name->spell);
    }
}

/* Expands tok, or emits it unchanged when it is not a live macro name. A
   macro's body is rescanned with the macro marked expanding (disable-during-
   expansion, C11 §6.10.3.4); body tokens inherit the invocation's Loc. */
static void pp_expand_token(Pp *pp, PpToken *tok)
{
    if (tok->kind == TOK_PP_IDENT)
    {
        Macro *macro = pp_macro_lookup(pp, tok);
        if (macro && macro->kind == MACRO_OBJ && !macro->expanding)
        {
            macro->expanding = true;
            for (size_t i = 0; i < vec_size(macro->body); i++)
            {
                PpToken *body = vec_get(macro->body, i);
                PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
                *copy = *body;
                copy->loc = tok->loc;
                pp_expand_token(pp, copy);
            }
            macro->expanding = false;
            return;
        }
    }
    vec_push(pp->out, tok);
}

static void pp_expand_line(Pp *pp, const PpIncludeFrame *frame, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; i++)
    {
        pp_expand_token(pp, vec_get(frame->tokens, i));
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
        if (tok->kind == TOK_PP_TRIVIA_NL)
        {
            break;
        }
        if (!pp_is_trivia(tok) && !first)
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
        pp_expand_line(pp, frame, begin, end);
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
    if (pp->error_count > 0)
    {
        return NULL;
    }
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
