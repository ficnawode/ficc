#include "pp.h"
#include "pp_expr.h"

#include "util/bytebuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef FICC_BUILTIN_INCLUDE
#define FICC_BUILTIN_INCLUDE "include"
#endif

typedef struct Hideset Hideset;
struct Hideset
{
    Macro *macro;
    Hideset *next;
};

typedef struct TokList TokList;
struct TokList
{
    PpToken *tok;
    TokList *next;
};

static bool hs_has(Hideset *hs, const Macro *macro)
{
    for (; hs; hs = hs->next)
    {
        if (hs->macro == macro)
        {
            return true;
        }
    }
    return false;
}

static Hideset *hs_add(Arena *arena, Hideset *hs, Macro *macro)
{
    if (hs_has(hs, macro))
    {
        return hs;
    }
    Hideset *head = arena_alloc(arena, sizeof(Hideset), sizeof(void *));
    head->macro = macro;
    head->next = hs;
    return head;
}

static Hideset *hs_intersect(Arena *arena, Hideset *x, Hideset *y)
{
    Hideset *result = NULL;
    for (; x; x = x->next)
    {
        if (hs_has(y, x->macro))
        {
            result = hs_add(arena, result, x->macro);
        }
    }
    return result;
}

static Hideset *hs_union(Arena *arena, Hideset *x, Hideset *y)
{
    for (; y; y = y->next)
    {
        x = hs_add(arena, x, y->macro);
    }
    return x;
}

static TokList *list_cons(Arena *arena, PpToken *tok, TokList *next)
{
    TokList *node = arena_alloc(arena, sizeof(TokList), sizeof(void *));
    node->tok = tok;
    node->next = next;
    return node;
}

/* Returns a copy of x whose tail is y (y is shared, not copied). */
static TokList *list_concat(Arena *arena, TokList *x, TokList *y)
{
    if (!x)
    {
        return y;
    }
    TokList *head = list_cons(arena, x->tok, NULL);
    TokList *tail = head;
    for (x = x->next; x; x = x->next)
    {
        tail->next = list_cons(arena, x->tok, NULL);
        tail = tail->next;
    }
    tail->next = y;
    return head;
}

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

static bool pp_is_rparen(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_RPAREN;
}

static bool pp_is_comma(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_COMMA;
}

static bool pp_is_hashhash(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_HASHHASH;
}

static bool pp_is_ellipsis(const PpToken *tok)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == PP_PUNCT_ELLIPSIS;
}

static void pp_predefine(Pp *pp, const char *name, const char *spelling)
{
    Vec *body = vec_new(pp->arena);
    PpToken *tok = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *tok = (PpToken) {.kind = TOK_PP_NUMBER, .spell = spelling, .len = (u32) strlen(spelling)};
    vec_push(body, tok);

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    macro->name = name;
    macro->kind = MACRO_OBJ;
    macro->loc = (Loc) {0};
    macro->body = body;
    macro->param_count = 0;
    macro->variadic = false;
    macro->predefined = true;
    strmap_set(pp->macros, name, macro);
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
    pp->exe_path = NULL;
    pp->builtin_dir = NULL;
    pp->cooked_date = NULL;
    pp->cooked_time = NULL;
    pp->pragma_once = hashset_new(arena, hashmap_str_hash, hashmap_str_eq);
    pp->poison = hashset_new(arena, hashmap_str_hash, hashmap_str_eq);
    pp->has_source_date_epoch = false;
    pp->source_date_epoch = 0;
    pp->error_count = 0;
    pp->warning_count = 0;

    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && sde[0])
    {
        u64 value = 0;
        bool ok = true;
        for (const char *p = sde; *p; p++)
        {
            if (*p < '0' || *p > '9')
            {
                ok = false;
                break;
            }
            value = value * 10 + (u64) (*p - '0');
        }
        if (ok)
        {
            pp->has_source_date_epoch = true;
            pp->source_date_epoch = (i64) value;
        }
    }

    pp_predefine(pp, "__STDC__", "1");
    pp_predefine(pp, "__STDC_VERSION__", "201112L");
    pp_predefine(pp, "__STDC_HOSTED__", "1");
    pp_predefine(pp, "__STDC_NO_ATOMICS__", "1");
    pp_predefine(pp, "__STDC_NO_THREADS__", "1");
    pp_predefine(pp, "__STDC_NO_VLA__", "1");
    pp_predefine(pp, "__STDC_NO_COMPLEX__", "1");

    Macro *pragma = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *pragma = (Macro) {.name = "_Pragma", .kind = MACRO_OBJ, .predefined = true, .is_pragma = true};
    strmap_set(pp->macros, "_Pragma", pragma);
    return pp;
}

void pp_free(Pp *pp)
{
    arena_free(pp->arena);
}

static void pp_push_include(Pp *pp, const char *file, Vec *tokens)
{
    PpIncludeFrame *frame = arena_alloc(pp->arena, sizeof(PpIncludeFrame), sizeof(void *));
    frame->file = file;
    frame->tokens = tokens;
    frame->cursor = 0;
    frame->presumed_line = 1;
    frame->presumed_file = file;
    frame->system_header = false;
    vec_push(pp->includes, frame);
}

static void pp_vdiag(Loc loc, const char *level, const char *fmt, va_list args)
{
    fprintf(stderr, "%s:%u:%u: [pp] %s: ", loc.file, loc.line, loc.col, level);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void pp_error(Pp *pp, Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    pp_verror(pp, loc, fmt, args);
    va_end(args);
}

void pp_verror(Pp *pp, Loc loc, const char *fmt, va_list args)
{
    pp_vdiag(loc, "error", fmt, args);
    pp->error_count++;
}

void pp_warn(Pp *pp, Loc loc, const char *fmt, ...)
{
    if (vec_size(pp->includes) > 0)
    {
        PpIncludeFrame *frame = vec_last(pp->includes);
        if (frame->system_header)
        {
            return;
        }
    }
    va_list args;
    va_start(args, fmt);
    pp_vwarn(pp, loc, fmt, args);
    va_end(args);
}

void pp_vwarn(Pp *pp, Loc loc, const char *fmt, va_list args)
{
    pp_vdiag(loc, "warning", fmt, args);
    pp->warning_count++;
}

void pp_note(Loc loc, const char *fmt, ...)
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
        if (x->kind != y->kind)
        {
            return false;
        }
        if (x->kind == TOK_PP_PARAM)
        {
            if (x->param_idx != y->param_idx)
            {
                return false;
            }
            continue;
        }
        if (x->len != y->len || strncmp(x->spell, y->spell, x->len) != 0)
        {
            return false;
        }
    }
    return true;
}

static int pp_param_index(const Vec *params, const PpToken *tok)
{
    for (size_t i = 0; i < vec_size(params); i++)
    {
        const char *name = vec_get(params, i);
        if (strlen(name) == tok->len && strncmp(name, tok->spell, tok->len) == 0)
        {
            return (int) i;
        }
    }
    return -1;
}

static size_t pp_param_list_end(const PpIncludeFrame *frame, size_t open, size_t end)
{
    for (size_t i = open + 1; i < end; i++)
    {
        if (pp_is_rparen(vec_get(frame->tokens, i)))
        {
            return i;
        }
    }
    return end;
}

/* Parses the comma-separated parameter list in (open, close) into params.
   `...` (or GNU `name...`) must be last; the variadic name, if any, aliases
   `__VA_ARGS__` and is not a named parameter. */
static bool pp_parse_params(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t close,
                            Vec *params, bool *variadic, const char **variadic_name)
{
    size_t i = open + 1;
    while (i < close)
    {
        while (i < close && pp_is_trivia(vec_get(frame->tokens, i)))
        {
            i++;
        }
        if (i >= close)
        {
            break;
        }

        PpToken *t = vec_get(frame->tokens, i);
        if (pp_is_ellipsis(t))
        {
            *variadic = true;
            i++;
            while (i < close && pp_is_trivia(vec_get(frame->tokens, i)))
            {
                i++;
            }
            if (i < close)
            {
                pp_error(pp, t->loc, "'...' must be the last macro parameter");
                return false;
            }
            break;
        }
        if (t->kind != TOK_PP_IDENT)
        {
            pp_error(pp, t->loc, "expected macro parameter name");
            return false;
        }

        const char *name = pp_token_name(pp, t);
        i++;
        while (i < close && pp_is_trivia(vec_get(frame->tokens, i)))
        {
            i++;
        }

        if (i < close && pp_is_ellipsis(vec_get(frame->tokens, i)))
        {
            *variadic = true;
            *variadic_name = name;
            i++;
            while (i < close && pp_is_trivia(vec_get(frame->tokens, i)))
            {
                i++;
            }
            if (i < close)
            {
                pp_error(pp, t->loc, "'...' must be the last macro parameter");
                return false;
            }
            break;
        }

        if (pp_param_index(params, t) >= 0)
        {
            pp_error(pp, t->loc, "duplicate macro parameter '%.*s'", (int) t->len, t->spell);
            return false;
        }
        vec_push(params, (void *) name);

        if (i < close)
        {
            if (!pp_is_comma(vec_get(frame->tokens, i)))
            {
                pp_error(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                         "expected ',' or ')' in macro parameter list");
                return false;
            }
            i++;
        }
    }
    return true;
}

/* Rewrites body identifiers that name parameters into TOK_PP_PARAM markers.
   `__VA_ARGS__` and the GNU variadic alias both mark the variadic tail. */
static Vec *pp_mark_params(Pp *pp, Vec *body, Vec *params, bool variadic, const char *variadic_name)
{
    Vec *marked = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *bt = vec_get(body, i);
        int idx = bt->kind == TOK_PP_IDENT ? pp_param_index(params, bt) : -1;
        bool is_va =
            bt->kind == TOK_PP_IDENT && (pp_spelling_is(bt, "__VA_ARGS__") ||
                                         (variadic_name && pp_spelling_is(bt, variadic_name)));
        if (idx < 0 && !is_va)
        {
            vec_push(marked, bt);
            continue;
        }
        if (is_va && !variadic)
        {
            pp_error(pp, bt->loc, "__VA_ARGS__ can only appear in a variadic macro");
            return NULL;
        }

        PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
        *copy = *bt;
        copy->kind = TOK_PP_PARAM;
        copy->param_idx = is_va ? (u32) vec_size(params) : (u32) idx;
        vec_push(marked, copy);
    }
    return marked;
}

/* `__VA_ARGS__` may not appear in an object-like macro (C11 §6.10.3.4). */
static bool pp_check_va_args(Pp *pp, Vec *body)
{
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *t = vec_get(body, i);
        if (t->kind == TOK_PP_IDENT && pp_spelling_is(t, "__VA_ARGS__"))
        {
            pp_error(pp, t->loc, "__VA_ARGS__ can only appear in a variadic macro");
            return false;
        }
    }
    return true;
}

/* Every `#` in a function-like replacement must be followed by a parameter
   (C11 §6.10.3.2). */
static bool pp_check_hashes(Pp *pp, Vec *body)
{
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *t = vec_get(body, i);
        if (t->kind == TOK_PP_PUNCT && t->punct == PP_PUNCT_HASH)
        {
            if (i + 1 >= vec_size(body) || ((PpToken *) vec_get(body, i + 1))->kind != TOK_PP_PARAM)
            {
                pp_error(pp, t->loc, "'#' is not followed by a macro parameter");
                return false;
            }
        }
    }
    return true;
}

/* A `##` operand must not be the first or last token of a replacement list
   (C11 §6.10.3.3). */
static bool pp_check_pastes(Pp *pp, Vec *body)
{
    size_t n = vec_size(body);
    for (size_t i = 0; i < n; i++)
    {
        PpToken *t = vec_get(body, i);
        if (!pp_is_hashhash(t))
        {
            continue;
        }
        if (i == 0)
        {
            pp_error(pp, t->loc, "'##' cannot appear at the start of a macro expansion");
            return false;
        }
        if (i + 1 >= n)
        {
            pp_error(pp, t->loc, "'##' cannot appear at the end of a macro expansion");
            return false;
        }
    }
    return true;
}

/* Predefined names are reserved (§6.10.8): redefinition is a constraint. */
static bool pp_is_reserved_name(const char *name)
{
    if (strcmp(name, "_Pragma") == 0)
    {
        return true;
    }
    if (strcmp(name, "__LINE__") == 0 || strcmp(name, "__FILE__") == 0 ||
        strcmp(name, "__DATE__") == 0 || strcmp(name, "__TIME__") == 0)
    {
        return true;
    }
    if (strcmp(name, "__STDC__") == 0 || strcmp(name, "__STDC_VERSION__") == 0 ||
        strcmp(name, "__STDC_HOSTED__") == 0)
    {
        return true;
    }
    if (strcmp(name, "__STDC_NO_ATOMICS__") == 0 || strcmp(name, "__STDC_NO_THREADS__") == 0 ||
        strcmp(name, "__STDC_NO_VLA__") == 0 || strcmp(name, "__STDC_NO_COMPLEX__") == 0)
    {
        return true;
    }
    return false;
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
    if (pp_is_reserved_name(text))
    {
        pp_error(pp, name->loc, "redefinition of predefined macro '%s'", text);
        return;
    }
    bool function_like = i + 1 < end && pp_is_lparen(vec_get(frame->tokens, i + 1));
    MacroKind kind = function_like ? MACRO_FUNC : MACRO_OBJ;
    Vec *body = NULL;
    u32 param_count = 0;
    bool variadic = false;
    const char *variadic_name = NULL;

    if (function_like)
    {
        size_t close = pp_param_list_end(frame, i + 1, end);
        if (close >= end)
        {
            pp_error(pp, name->loc, "missing ')' in macro parameter list");
            return;
        }
        Vec *params = vec_new(pp->arena);
        if (!pp_parse_params(pp, frame, i + 1, close, params, &variadic, &variadic_name))
        {
            return;
        }
        param_count = (u32) vec_size(params);
        body = pp_mark_params(pp, pp_collect_body(pp, frame, close + 1, end), params, variadic,
                              variadic_name);
        if (!body)
        {
            return;
        }
        if (!pp_check_hashes(pp, body))
        {
            return;
        }
    }
    else
    {
        body = pp_collect_body(pp, frame, i + 1, end);
        if (!pp_check_va_args(pp, body))
        {
            return;
        }
    }

    if (!pp_check_pastes(pp, body))
    {
        return;
    }

    Macro *prev = strmap_get(pp->macros, text);
    if (prev)
    {
        if (prev->kind == kind && prev->param_count == param_count && prev->variadic == variadic &&
            pp_bodies_equal(prev->body, body))
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
    macro->loc = name->loc;
    macro->body = body;
    macro->param_count = param_count;
    macro->variadic = variadic;
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
    const char *text = pp_token_name(pp, name);
    if (pp_is_reserved_name(text))
    {
        pp_warn(pp, name->loc, "undefining '%s'", text);
    }
    hashmap_remove(pp->macros, text);
}

static TokList *pp_expand_list(Pp *pp, TokList *ts);

typedef struct CondFrame CondFrame;
struct CondFrame
{
    bool parent_active;
    bool taken;
    bool ever_taken;
    bool in_else;
};

static bool pp_branch_active(Pp *pp)
{
    if (vec_size(pp->conds) == 0)
    {
        return true;
    }
    return ((CondFrame *) vec_last(pp->conds))->taken;
}

static bool pp_is_skipping(Pp *pp)
{
    return !pp_branch_active(pp);
}

static void pp_push_cond(Pp *pp, bool parent_active, bool taken)
{
    CondFrame *frame = arena_alloc(pp->arena, sizeof(CondFrame), sizeof(void *));
    *frame = (CondFrame) {
        .parent_active = parent_active, .taken = taken, .ever_taken = taken, .in_else = false};
    vec_push(pp->conds, frame);
}

static void pp_ifdef_directive(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc, bool is_ifndef)
{
    size_t i = start;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i >= end)
    {
        pp_error(pp, directive_loc, "#%s requires a macro name", is_ifndef ? "ifndef" : "ifdef");
        return;
    }
    PpToken *name = vec_get(frame->tokens, i);
    if (name->kind != TOK_PP_IDENT)
    {
        pp_error(pp, name->loc, "#%s requires a macro name", is_ifndef ? "ifndef" : "ifdef");
        return;
    }
    i++;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i < end)
    {
        pp_warn(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                "extra tokens at end of #%s directive", is_ifndef ? "ifndef" : "ifdef");
    }

    bool defined = strmap_get(pp->macros, pp_token_name(pp, name)) != NULL;
    bool parent_active = pp_branch_active(pp);
    pp_push_cond(pp, parent_active, parent_active && (defined != is_ifndef));
}

static void pp_else_directive(Pp *pp, Loc directive_loc)
{
    if (vec_size(pp->conds) == 0)
    {
        pp_error(pp, directive_loc, "unexpected #else");
        return;
    }
    CondFrame *top = vec_last(pp->conds);
    if (top->in_else)
    {
        pp_error(pp, directive_loc, "multiple #else directives");
        return;
    }
    top->in_else = true;
    top->taken = top->parent_active && !top->ever_taken;
    top->ever_taken = true;
}

static void pp_endif_directive(Pp *pp, Loc directive_loc)
{
    if (vec_size(pp->conds) == 0)
    {
        pp_error(pp, directive_loc, "unexpected #endif");
        return;
    }
    vec_pop(pp->conds);
}

static bool pp_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return false;
    }
    fclose(f);
    return true;
}

static const char *pp_path_dirname(Pp *pp, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
    {
        return ".";
    }
    size_t len = (size_t) (slash - path);
    if (len == 0)
    {
        return "/";
    }
    char *buf = arena_alloc(pp->arena, len + 1, 1);
    memcpy(buf, path, len);
    buf[len] = '\0';
    return buf;
}

static const char *pp_path_join(Pp *pp, const char *dir, const char *name)
{
    size_t len = strlen(dir) + 1 + strlen(name);
    char *buf = arena_alloc(pp->arena, len + 1, 1);
    memcpy(buf, dir, strlen(dir));
    buf[strlen(dir)] = '/';
    memcpy(buf + strlen(dir) + 1, name, strlen(name) + 1);
    return buf;
}

static char *pp_read_file(const char *path, Arena *arena)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) < 0)
    {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = arena_alloc(arena, (size_t) size + 1, 1);
    if (fread(buf, 1, (size_t) size, f) != (size_t) size)
    {
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static const char *pp_builtin_dir(Pp *pp)
{
    if (!pp->builtin_dir)
    {
        const char *dir = FICC_BUILTIN_INCLUDE;
        if (pp->exe_path)
        {
            const char *cand = pp_path_join(
                pp, pp_path_join(pp, pp_path_dirname(pp, pp->exe_path), ".."), "include");
            if (pp_file_exists(cand))
            {
                dir = cand;
            }
        }
        pp->builtin_dir = dir;
    }
    return pp->builtin_dir;
}

/* Resolves a header name. Quoted form tries the including file's directory
   first, then the angle chain. Returns the path or NULL. */
static const char *pp_include_find(Pp *pp, bool quoted, const char *name,
                                   const char *including_file, bool next_mode)
{
    if (quoted && !next_mode && including_file)
    {
        const char *cand = pp_path_join(pp, pp_path_dirname(pp, including_file), name);
        if (pp_file_exists(cand))
        {
            return cand;
        }
    }

    size_t start = 0;
    if (next_mode && including_file)
    {
        const char *dir = pp_path_dirname(pp, including_file);
        for (size_t k = 0; k < vec_size(pp->include_paths); k++)
        {
            if (strcmp((const char *) vec_get(pp->include_paths, k), dir) == 0)
            {
                start = k + 1;
                break;
            }
        }
    }

    for (size_t i = start; i < vec_size(pp->include_paths); i++)
    {
        const char *cand = pp_path_join(pp, vec_get(pp->include_paths, i), name);
        if (pp_file_exists(cand))
        {
            return cand;
        }
    }

    if (!pp->nostdinc)
    {
        const char *cand = pp_path_join(pp, pp_builtin_dir(pp), name);
        if (pp_file_exists(cand))
        {
            return cand;
        }
    }
    return NULL;
}

static void pp_include_file(Pp *pp, const char *path)
{
    if (vec_size(pp->includes) >= 200)
    {
        pp_error(pp, (Loc) {.file = path, .line = 1, .col = 1},
                 "too many nested #include directives");
        return;
    }
    char *src = pp_read_file(path, pp->arena);
    if (!src)
    {
        pp_error(pp, (Loc) {.file = path, .line = 1, .col = 1}, "cannot read include file");
        return;
    }
    Vec *tokens = pp_lex(path, src, pp->arena);
    if (!tokens)
    {
        pp_error(pp, (Loc) {.file = path, .line = 1, .col = 1}, "cannot preprocess include file");
        return;
    }
    pp_push_include(pp, path, tokens);
}

/* `#include` operand: a literal `"..."`/`<...>` header-name, or macro
   operands that expand to one (`#include H`). */
static const char *pp_string_content(Pp *pp, const PpToken *tok);
static Vec *pp_expand_vec(Pp *pp, Vec *input);
static const char *pp_header_name_from_vec(Pp *pp, Vec *tokens, size_t *pos, bool *quoted);

static void pp_include_directive(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                                 Loc directive_loc, bool next_mode)
{
    size_t i = start;
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i >= end)
    {
        pp_error(pp, directive_loc, "expected a header name after #include");
        return;
    }

    PpToken *first = vec_get(frame->tokens, i);
    bool quoted = false;
    const char *name = "";

    if (first->kind == TOK_PP_PUNCT && first->punct == PP_PUNCT_LT)
    {
        ByteBuf name_buf;
        bytebuf_init(&name_buf, pp->arena);
        size_t j = i + 1;
        for (; j < end; j++)
        {
            PpToken *t = vec_get(frame->tokens, j);
            if (t->kind == TOK_PP_PUNCT && t->punct == PP_PUNCT_GT)
            {
                break;
            }
            if (t->kind == TOK_PP_TRIVIA_COMMENT || t->kind == TOK_PP_TRIVIA_NL)
            {
                pp_error(pp, t->loc, "invalid character inside a #include header name");
                return;
            }
            if (pp_is_trivia(t))
            {
                continue;
            }
            for (u32 k = 0; k < t->len; k++)
            {
                bytebuf_append(&name_buf, t->spell[k]);
            }
        }
        if (j >= end)
        {
            pp_error(pp, first->loc, "missing '>' in #include");
            return;
        }
        bytebuf_append(&name_buf, '\0');
        name = (const char *) bytebuf_data(&name_buf);
        i = j + 1;
    }
    else if (first->kind == TOK_PP_STRING)
    {
        quoted = true;
        name = pp_string_content(pp, first);
        i++;
    }
    else
    {
        Vec *raw = vec_new(pp->arena);
        for (size_t k = i; k < end; k++)
        {
            PpToken *t = vec_get(frame->tokens, k);
            if (!pp_is_trivia(t))
            {
                vec_push(raw, t);
            }
        }
        Vec *expanded = pp_expand_vec(pp, raw);
        if (vec_size(expanded) == 0)
        {
            pp_error(pp, first->loc, "expected a header name after #include");
            return;
        }
        size_t pos = 0;
        PpToken *e0 = vec_get(expanded, 0);
        if (e0->kind == TOK_PP_STRING || (e0->kind == TOK_PP_PUNCT && e0->punct == PP_PUNCT_LT))
        {
            name = pp_header_name_from_vec(pp, expanded, &pos, &quoted);
        }
        if (pos != vec_size(expanded))
        {
            pp_error(pp, first->loc, "#include operand does not expand to a single header name");
            return;
        }
        i = end;
    }

    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    if (i < end)
    {
        pp_warn(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                "extra tokens at end of #include directive");
    }

    const char *path = pp_include_find(pp, quoted, name, frame->file, next_mode);
    if (!path)
    {
        pp_error(pp, directive_loc,
                 next_mode ? "no such #include_next file: '%s'" : "include file not found: '%s'",
                 name);
        return;
    }
    if (hashset_contains(pp->pragma_once, path))
    {
        return;
    }
    pp_include_file(pp, path);
}

/* Warns on a GNU-lite construct when -pedantic is on (D17.9). */
static void pp_gnu_warn(Pp *pp, Loc loc, const char *feature)
{
    if (pp->pedantic)
    {
        pp_warn(pp, loc, "'%s' is a GNU extension", feature);
    }
}

/* `#pragma once`, `#pragma GCC poison A B`, `#pragma GCC system_header`;
   any other pragma is parsed and ignored. */
static void pp_pragma_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                Loc directive_loc)
{
    Vec *tokens = vec_new(pp->arena);
    for (size_t i = start; i < end; i++)
    {
        PpToken *t = vec_get(frame->tokens, i);
        if (!pp_is_trivia(t))
        {
            vec_push(tokens, t);
        }
    }
    size_t n = vec_size(tokens);
    if (n == 0)
    {
        return;
    }
    PpToken *t0 = vec_get(tokens, 0);

    if (n == 1 && t0->kind == TOK_PP_IDENT && pp_spelling_is(t0, "once"))
    {
        pp_gnu_warn(pp, t0->loc, "#pragma once");
        if (vec_size(pp->includes) == 1)
        {
            pp_warn(pp, t0->loc, "'#pragma once' in main file");
        }
        if (frame->file)
        {
            hashset_add(pp->pragma_once, (void *) frame->file);
        }
        return;
    }

    if (n >= 2 && t0->kind == TOK_PP_IDENT && pp_spelling_is(t0, "GCC"))
    {
        PpToken *t1 = vec_get(tokens, 1);
        if (t1->kind == TOK_PP_IDENT && pp_spelling_is(t1, "system_header"))
        {
            pp_gnu_warn(pp, t0->loc, "#pragma GCC system_header");
            frame->system_header = true;
            return;
        }
        if (t1->kind == TOK_PP_IDENT && pp_spelling_is(t1, "poison"))
        {
            pp_gnu_warn(pp, t0->loc, "#pragma GCC poison");
            for (size_t i = 2; i < n; i++)
            {
                PpToken *p = vec_get(tokens, i);
                if (p->kind != TOK_PP_IDENT)
                {
                    pp_error(pp, p->loc, "invalid '#pragma GCC poison' directive");
                    return;
                }
                hashset_add(pp->poison, (void *) pp_token_name(pp, p));
            }
            return;
        }
    }

    (void) directive_loc;
}

static u32 pp_count_newlines(const PpIncludeFrame *frame, size_t begin, size_t end)
{
    u32 count = 0;
    for (size_t i = begin; i < end; i++)
    {
        if (((PpToken *) vec_get(frame->tokens, i))->kind == TOK_PP_TRIVIA_NL)
        {
            count++;
        }
    }
    return count;
}

static bool pp_is_decimal_number(const PpToken *tok)
{
    if (tok->kind != TOK_PP_NUMBER)
    {
        return false;
    }
    for (u32 i = 0; i < tok->len; i++)
    {
        if (tok->spell[i] < '0' || tok->spell[i] > '9')
        {
            return false;
        }
    }
    return true;
}

/* Expands a token vector, dropping trivia from the result. */
static Vec *pp_expand_vec(Pp *pp, Vec *input)
{
    TokList *list = NULL;
    TokList **tail = &list;
    for (size_t i = 0; i < vec_size(input); i++)
    {
        PpToken *t = vec_get(input, i);
        if (!pp_is_trivia(t))
        {
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
        }
    }

    Vec *expanded = vec_new(pp->arena);
    for (TokList *r = pp_expand_list(pp, list); r; r = r->next)
    {
        if (!pp_is_trivia(r->tok))
        {
            vec_push(expanded, r->tok);
        }
    }
    return expanded;
}

/* Expands the real tokens in [start, end) (used for directive operands). */
static Vec *pp_expand_operand(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    Vec *raw = vec_new(pp->arena);
    for (size_t i = start; i < end; i++)
    {
        PpToken *t = vec_get(frame->tokens, i);
        if (!pp_is_trivia(t))
        {
            vec_push(raw, t);
        }
    }
    return pp_expand_vec(pp, raw);
}

static PpToken *pp_cooked_number(Pp *pp, u64 value, Loc loc)
{
    PpToken *t = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *t = (PpToken) {.kind = TOK_PP_NUMBER, .loc = loc, .spell = value ? "1" : "0", .len = 1};
    return t;
}

/* Reads a header-name at *pos from a token vector: a string literal or angle
   form. Returns the name (arena text) and advances *pos past it. */
static const char *pp_header_name_from_vec(Pp *pp, Vec *tokens, size_t *pos, bool *quoted)
{
    size_t n = vec_size(tokens);
    PpToken *t = vec_get(tokens, *pos);
    ByteBuf name_buf;
    bytebuf_init(&name_buf, pp->arena);

    if (t->kind == TOK_PP_STRING)
    {
        *quoted = true;
        for (u32 k = 1; k + 1 < t->len; k++)
        {
            bytebuf_append(&name_buf, t->spell[k]);
        }
        bytebuf_append(&name_buf, '\0');
        (*pos)++;
        return (const char *) bytebuf_data(&name_buf);
    }
    if (t->kind == TOK_PP_PUNCT && t->punct == PP_PUNCT_LT)
    {
        *quoted = false;
        size_t j = *pos + 1;
        for (; j < n; j++)
        {
            PpToken *u = vec_get(tokens, j);
            if (u->kind == TOK_PP_PUNCT && u->punct == PP_PUNCT_GT)
            {
                break;
            }
            for (u32 k = 0; k < u->len; k++)
            {
                bytebuf_append(&name_buf, u->spell[k]);
            }
        }
        if (j >= n)
        {
            pp_error(pp, t->loc, "missing '>' in __has_include");
        }
        else
        {
            *pos = j + 1;
        }
        bytebuf_append(&name_buf, '\0');
        return (const char *) bytebuf_data(&name_buf);
    }

    pp_error(pp, t->loc, "expected a header name in __has_include");
    return "";
}

/* Resolves `defined X` and `__has_include(...)` into cooked literals before
   macro expansion (C11 §6.10.1). */
static Vec *pp_resolve_controls(Pp *pp, Vec *raw, const PpIncludeFrame *frame)
{
    Vec *out = vec_new(pp->arena);
    size_t n = vec_size(raw);
    size_t i = 0;
    while (i < n)
    {
        PpToken *t = vec_get(raw, i);
        if (t->kind == TOK_PP_IDENT && pp_spelling_is(t, "defined"))
        {
            i++;
            bool paren = false;
            if (i < n && pp_is_lparen(vec_get(raw, i)))
            {
                paren = true;
                i++;
            }
            if (i >= n || ((PpToken *) vec_get(raw, i))->kind != TOK_PP_IDENT)
            {
                pp_error(pp, t->loc, "operator 'defined' requires an identifier");
                vec_push(out, pp_cooked_number(pp, 0, t->loc));
                continue;
            }
            PpToken *name = vec_get(raw, i);
            i++;
            bool defined = strmap_get(pp->macros, pp_token_name(pp, name)) != NULL ||
                           pp_spelling_is(name, "__has_include") ||
                           pp_spelling_is(name, "__has_include_next");
            if (paren)
            {
                if (i < n && pp_is_rparen(vec_get(raw, i)))
                {
                    i++;
                }
                else
                {
                    pp_error(pp, name->loc, "expected ')' after 'defined'");
                }
            }
            vec_push(out, pp_cooked_number(pp, defined ? 1 : 0, t->loc));
            continue;
        }
        if (t->kind == TOK_PP_IDENT &&
            (pp_spelling_is(t, "__has_include") || pp_spelling_is(t, "__has_include_next")))
        {
            bool next = pp_spelling_is(t, "__has_include_next");
            i++;
            if (i >= n || !pp_is_lparen(vec_get(raw, i)))
            {
                pp_error(pp, t->loc, "expected '(' after __has_include");
                vec_push(out, pp_cooked_number(pp, 0, t->loc));
                continue;
            }
            i++;

            Vec *operand = vec_new(pp->arena);
            size_t j = i;
            while (j < n && !pp_is_rparen(vec_get(raw, j)))
            {
                vec_push(operand, vec_get(raw, j));
                j++;
            }
            if (j >= n)
            {
                pp_error(pp, t->loc, "missing ')' after __has_include");
                vec_push(out, pp_cooked_number(pp, 0, t->loc));
                continue;
            }
            i = j + 1;

            Vec *expanded = pp_expand_vec(pp, operand);
            bool quoted = false;
            const char *name = "";
            bool valid = false;
            if (vec_size(expanded) > 0)
            {
                size_t pos = 0;
                PpToken *e0 = vec_get(expanded, 0);
                if (e0->kind == TOK_PP_STRING ||
                    (e0->kind == TOK_PP_PUNCT && e0->punct == PP_PUNCT_LT))
                {
                    name = pp_header_name_from_vec(pp, expanded, &pos, &quoted);
                    valid = pos == vec_size(expanded);
                }
            }
            if (!valid)
            {
                pp_error(pp, t->loc, "__has_include requires a single header-name operand");
            }
            bool found = valid && pp_include_find(pp, quoted, name, frame ? frame->file : NULL,
                                                  next) != NULL;
            vec_push(out, pp_cooked_number(pp, found ? 1 : 0, t->loc));
            continue;
        }
        vec_push(out, t);
        i++;
    }
    return out;
}

static void pp_cook_datetime(Pp *pp)
{
    if (pp->cooked_date)
    {
        return;
    }
    static const char *const MONTHS[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    time_t now = pp->has_source_date_epoch ? (time_t) pp->source_date_epoch : time(NULL);
    struct tm *tm = pp->has_source_date_epoch ? gmtime(&now) : localtime(&now);
    char date[64], clock[64];
    snprintf(date, sizeof(date), "%s %2d %d", MONTHS[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    pp->cooked_date = arena_alloc(pp->arena, strlen(date) + 1, 1);
    strcpy((char *) pp->cooked_date, date);
    pp->cooked_time = arena_alloc(pp->arena, strlen(clock) + 1, 1);
    strcpy((char *) pp->cooked_time, clock);
}

static PpToken *pp_cooked_literal(Pp *pp, PpKind kind, const char *spell, u32 len, Loc loc)
{
    PpToken *t = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *t = (PpToken) {.kind = kind, .loc = loc, .spell = spell, .len = len};
    return t;
}

static PpToken *pp_cooked_number_token(Pp *pp, u32 value, Loc loc)
{
    char num[16];
    snprintf(num, sizeof(num), "%u", value);
    char *buf = arena_alloc(pp->arena, strlen(num) + 1, 1);
    memcpy(buf, num, strlen(num) + 1);
    return pp_cooked_literal(pp, TOK_PP_NUMBER, buf, (u32) strlen(num), loc);
}

static PpToken *pp_cooked_string_token(Pp *pp, const char *content, Loc loc)
{
    size_t n = strlen(content);
    char *buf = arena_alloc(pp->arena, n + 3, 1);
    buf[0] = '"';
    memcpy(buf + 1, content, n);
    buf[n + 1] = '"';
    buf[n + 2] = '\0';
    return pp_cooked_literal(pp, TOK_PP_STRING, buf, (u32) (n + 2), loc);
}

/* Materializes the lazy predefined macros at the point of use (§6.10.8). */
static PpToken *pp_cook_predefined(Pp *pp, PpToken *t, u32 line, const char *file)
{
    if (t->kind != TOK_PP_IDENT)
    {
        return t;
    }
    if (pp_spelling_is(t, "__LINE__"))
    {
        return pp_cooked_number_token(pp, line, t->loc);
    }
    if (pp_spelling_is(t, "__FILE__"))
    {
        return pp_cooked_string_token(pp, file ? file : "", t->loc);
    }
    if (pp_spelling_is(t, "__DATE__") || pp_spelling_is(t, "__TIME__"))
    {
        pp_cook_datetime(pp);
        return pp_cooked_string_token(
            pp, pp_spelling_is(t, "__DATE__") ? pp->cooked_date : pp->cooked_time, t->loc);
    }
    return t;
}

/* Evaluates an #if/#elif controlling expression to a branch truth value. */
static bool pp_if_condition(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    Vec *raw = vec_new(pp->arena);
    for (size_t i = start; i < end; i++)
    {
        PpToken *t = vec_get(frame->tokens, i);
        if (!pp_is_trivia(t))
        {
            vec_push(raw, t);
        }
    }
    Vec *resolved = pp_resolve_controls(pp, raw, frame);
    Vec *expanded = pp_expand_vec(pp, resolved);

    Vec *cooked = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(expanded); i++)
    {
        vec_push(cooked, pp_cook_predefined(pp, vec_get(expanded, i), frame->presumed_line,
                                            frame->presumed_file));
    }

    PpExprVal v = pp_eval_expr(pp, cooked);
    return v.value != 0;
}

static const char *pp_render_text(Pp *pp, Vec *tokens)
{
    ByteBuf buf;
    bytebuf_init(&buf, pp->arena);
    bool first = true;
    for (size_t i = 0; i < vec_size(tokens); i++)
    {
        PpToken *t = vec_get(tokens, i);
        if (!first)
        {
            bytebuf_append(&buf, ' ');
        }
        first = false;
        for (u32 j = 0; j < t->len; j++)
        {
            bytebuf_append(&buf, t->spell[j]);
        }
    }
    bytebuf_append(&buf, '\0');
    return (const char *) bytebuf_data(&buf);
}

static const char *pp_string_content(Pp *pp, const PpToken *tok)
{
    char *buf = arena_alloc(pp->arena, tok->len > 1 ? tok->len - 1 : 1, 1);
    u32 n = tok->len > 2 ? tok->len - 2 : 0;
    memcpy(buf, tok->spell + 1, n);
    buf[n] = '\0';
    return buf;
}

/* `#line N ["file"]` (also the GNU `# N "file"` linemarker). */
static void pp_line_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                              Loc directive_loc, bool is_linemarker)
{
    Vec *expanded = pp_expand_operand(pp, frame, start, end);
    if (vec_size(expanded) == 0)
    {
        pp_error(pp, directive_loc, "#line requires a line number");
        return;
    }

    PpToken *num = vec_get(expanded, 0);
    if (!pp_is_decimal_number(num))
    {
        pp_error(pp, num->loc, "invalid line number in #line directive");
        return;
    }
    u32 line = 0;
    for (u32 i = 0; i < num->len; i++)
    {
        line = line * 10 + (u32) (num->spell[i] - '0');
    }
    frame->presumed_line = line;

    size_t i = 1;
    if (i < vec_size(expanded) && ((PpToken *) vec_get(expanded, i))->kind == TOK_PP_STRING)
    {
        frame->presumed_file = pp_string_content(pp, vec_get(expanded, i));
        i++;
    }
    else if (i < vec_size(expanded) && !is_linemarker)
    {
        pp_error(pp, ((PpToken *) vec_get(expanded, i))->loc,
                 "expected a string literal after the line number in #line");
        return;
    }

    if (i < vec_size(expanded) && !is_linemarker)
    {
        pp_warn(pp, ((PpToken *) vec_get(expanded, i))->loc,
                "extra tokens at end of #line directive");
    }
}

static void pp_error_directive(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc)
{
    Vec *expanded = pp_expand_operand(pp, frame, start, end);
    const char *text = pp_render_text(pp, expanded);
    ByteBuf buf;
    bytebuf_init(&buf, pp->arena);
    bytebuf_append_bytes(&buf, (const u8 *) "#error ", 7);
    bytebuf_append_bytes(&buf, (const u8 *) text, strlen(text));
    bytebuf_append(&buf, '\0');
    pp_error(pp, directive_loc, "%s", (const char *) bytebuf_data(&buf));
}

static void pp_warning_directive(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                                 Loc directive_loc)
{
    Vec *expanded = pp_expand_operand(pp, frame, start, end);
    const char *text = pp_render_text(pp, expanded);
    ByteBuf buf;
    bytebuf_init(&buf, pp->arena);
    bytebuf_append_bytes(&buf, (const u8 *) "#warning ", 9);
    bytebuf_append_bytes(&buf, (const u8 *) text, strlen(text));
    bytebuf_append(&buf, '\0');
    pp_warn(pp, directive_loc, "%s", (const char *) bytebuf_data(&buf));
}

/* Returns true when the directive adjusted the presumed line ("#line"). */
static bool pp_directive(Pp *pp, PpIncludeFrame *frame, size_t begin, size_t end)
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
        return false; /* null directive */
    }

    PpToken *name = vec_get(frame->tokens, i);
    size_t name_idx = i;
    i++;
    if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "ifdef"))
    {
        pp_ifdef_directive(pp, frame, i, end, directive_loc, false);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "ifndef"))
    {
        pp_ifdef_directive(pp, frame, i, end, directive_loc, true);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "if"))
    {
        bool parent_active = pp_branch_active(pp);
        bool cond = parent_active ? pp_if_condition(pp, frame, i, end) : false;
        pp_push_cond(pp, parent_active, cond);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "elif"))
    {
        if (vec_size(pp->conds) == 0)
        {
            pp_error(pp, directive_loc, "unexpected #elif");
        }
        else
        {
            CondFrame *top = vec_last(pp->conds);
            if (top->in_else)
            {
                pp_error(pp, directive_loc, "unexpected #elif after #else");
            }
            else if (!top->parent_active || top->ever_taken)
            {
                top->taken = false;
            }
            else
            {
                bool cond = pp_if_condition(pp, frame, i, end);
                top->taken = cond;
                top->ever_taken = top->taken;
            }
        }
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "else"))
    {
        pp_else_directive(pp, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "endif"))
    {
        pp_endif_directive(pp, directive_loc);
    }
    else if (pp_is_skipping(pp))
    {
        return false;
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "include"))
    {
        pp_include_directive(pp, frame, i, end, directive_loc, false);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "include_next"))
    {
        pp_include_directive(pp, frame, i, end, directive_loc, true);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "pragma"))
    {
        pp_pragma_directive(pp, frame, i, end, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "define"))
    {
        pp_define(pp, frame, i, end, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "undef"))
    {
        pp_undef(pp, frame, i, end, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "line"))
    {
        pp_line_directive(pp, frame, i, end, directive_loc, false);
        return true;
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "error"))
    {
        pp_error_directive(pp, frame, i, end, directive_loc);
    }
    else if (name->kind == TOK_PP_IDENT && pp_spelling_is(name, "warning"))
    {
        pp_warning_directive(pp, frame, i, end, directive_loc);
    }
    else if (name->kind == TOK_PP_NUMBER)
    {
        pp_line_directive(pp, frame, name_idx, end, directive_loc, true);
        return true;
    }
    else
    {
        pp_warn(pp, name->loc, "invalid preprocessing directive #%.*s", (int) name->len,
                name->spell);
    }
    return false;
}

static bool pp_arg_is_empty(const Vec *arg)
{
    for (size_t i = 0; i < vec_size(arg); i++)
    {
        if (!pp_is_trivia(vec_get(arg, i)))
        {
            return false;
        }
    }
    return true;
}

static TokList *pp_skip_trivia_list(TokList *list)
{
    while (list && pp_is_trivia(list->tok))
    {
        list = list->next;
    }
    return list;
}

typedef struct MacroArgs MacroArgs;
struct MacroArgs
{
    Vec *args;
    Hideset *hs;
    TokList *after;
};

/* Collects invocation arguments starting at lparen. On a missing ')' it
   reports a diagnostic and returns false. */
static bool pp_collect_args(Pp *pp, PpToken *name, Macro *macro, TokList *lparen, MacroArgs *out)
{
    Vec *args = vec_new(pp->arena);
    Vec *current = vec_new(pp->arena);
    Hideset *hs = name->hide;
    int depth = 1;

    for (TokList *p = lparen->next; p; p = p->next)
    {
        PpToken *tok = p->tok;
        if (pp_is_lparen(tok))
        {
            depth++;
        }
        else if (pp_is_rparen(tok))
        {
            depth--;
            if (depth == 0)
            {
                vec_push(args, current);
                out->args = args;
                out->hs = hs;
                out->after = p->next;
                return true;
            }
        }
        else if (depth == 1 && pp_is_comma(tok))
        {
            vec_push(args, current);
            current = vec_new(pp->arena);
            continue;
        }

        vec_push(current, tok);
        if (!pp_is_trivia(tok))
        {
            hs = hs_intersect(pp->arena, hs, tok->hide);
        }
    }

    pp_error(pp, name->loc, "unterminated argument list invoking macro '%s'", macro->name);
    return false;
}

/* Stringizes a raw argument (§6.10.3.2): strips outer whitespace, collapses
   interior whitespace runs to one space, and quotes `"`/`\`. */
static PpToken *pp_stringize(Pp *pp, Vec *raw, Loc loc)
{
    ByteBuf buf;
    bytebuf_init(&buf, pp->arena);
    bytebuf_append(&buf, '"');

    bool seen_token = false;
    bool pending_space = false;
    for (size_t i = 0; i < vec_size(raw); i++)
    {
        PpToken *t = vec_get(raw, i);
        if (pp_is_trivia(t))
        {
            pending_space = seen_token;
            continue;
        }
        if (pending_space)
        {
            bytebuf_append(&buf, ' ');
            pending_space = false;
        }
        seen_token = true;
        for (u32 j = 0; j < t->len; j++)
        {
            char c = t->spell[j];
            if (c == '"' || c == '\\')
            {
                bytebuf_append(&buf, '\\');
            }
            bytebuf_append(&buf, c);
        }
    }
    bytebuf_append(&buf, '"');
    bytebuf_append(&buf, '\0');

    PpToken *tok = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *tok = (PpToken) {.kind = TOK_PP_STRING,
                      .loc = loc,
                      .spell = (const char *) bytebuf_data(&buf),
                      .len = (u32) (bytebuf_len(&buf) - 1)};
    return tok;
}

/* Concatenates two tokens' spellings and re-lexes the result; it must form
   exactly one valid pp-token (C11 §6.10.3.3). */
static PpToken *pp_paste(Pp *pp, PpToken *left, PpToken *right, Loc loc)
{
    size_t total = (size_t) left->len + right->len;
    char *buf = arena_alloc(pp->arena, total + 1, 1);
    memcpy(buf, left->spell, left->len);
    memcpy(buf + left->len, right->spell, right->len);
    buf[total] = '\0';

    Vec *soup = pp_lex(loc.file ? loc.file : "", buf, pp->arena);
    if (!soup)
    {
        pp_error(pp, loc, "pasting '%.*s' and '%.*s' does not give a valid preprocessing token",
                 (int) left->len, left->spell, (int) right->len, right->spell);
        return left;
    }

    PpToken *result = NULL;
    size_t real = 0;
    for (size_t i = 0; i < vec_size(soup); i++)
    {
        PpToken *t = vec_get(soup, i);
        if (t->kind == TOK_PP_EOF)
        {
            break;
        }
        if (pp_is_trivia(t))
        {
            continue;
        }
        real++;
        if (!result)
        {
            result = t;
        }
    }

    if (real != 1 || !result)
    {
        pp_error(pp, loc, "pasting '%.*s' and '%.*s' does not give a valid preprocessing token",
                 (int) left->len, left->spell, (int) right->len, right->spell);
        return left;
    }
    result->loc = loc;
    result->hide = hs_union(pp->arena, left->hide, right->hide);
    return result;
}

typedef struct
{
    Vec *raw;
    Vec *expanded;
    Hideset *hs;
    Loc loc;
    bool variadic_absent;
} SubstArgs;

static PpToken *pp_hidden_copy(Pp *pp, PpToken *src, const SubstArgs *sa)
{
    PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *copy = *src;
    copy->hide = hs_union(pp->arena, src->hide, sa->hs);
    copy->loc = sa->loc;
    return copy;
}

static void pp_push_arg(Pp *pp, Vec *out, Vec *arg, const SubstArgs *sa)
{
    for (size_t i = 0; i < vec_size(arg); i++)
    {
        PpToken *a = vec_get(arg, i);
        if (!pp_is_trivia(a))
        {
            vec_push(out, pp_hidden_copy(pp, a, sa));
        }
    }
}

static PpToken *pp_comma_token(Pp *pp, Loc loc)
{
    PpToken *t = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *t = (PpToken) {
        .kind = TOK_PP_PUNCT, .loc = loc, .spell = ",", .len = 1, .punct = PP_PUNCT_COMMA};
    return t;
}

/* Collapses the variadic tail (args[param_count..]) into one comma-joined
   argument; named parameters keep their positions. */
static Vec *pp_effective_args(Pp *pp, Vec *args, u32 param_count, Loc loc)
{
    Vec *eff = vec_new(pp->arena);
    for (u32 i = 0; i < param_count; i++)
    {
        vec_push(eff, i < vec_size(args) ? vec_get(args, i) : vec_new(pp->arena));
    }

    Vec *tail = vec_new(pp->arena);
    for (size_t i = param_count; i < vec_size(args); i++)
    {
        if (i > param_count)
        {
            vec_push(tail, pp_comma_token(pp, loc));
        }
        Vec *arg = vec_get(args, i);
        for (size_t j = 0; j < vec_size(arg); j++)
        {
            vec_push(tail, vec_get(arg, j));
        }
    }
    vec_push(eff, tail);
    return eff;
}

/* Substitutes parameters in order; a parameter adjacent to `##` receives its
   raw argument, everything else the prescanned one, and `#` stringizes. */
static Vec *pp_substitute(Pp *pp, Macro *macro, const SubstArgs *sa)
{
    Vec *out = vec_new(pp->arena);
    size_t n = vec_size(macro->body);

    for (size_t i = 0; i < n; i++)
    {
        PpToken *bt = vec_get(macro->body, i);

        /* GNU `, ## __VA_ARGS__`: drop the comma when the tail is absent. */
        if (bt->kind == TOK_PP_PUNCT && bt->punct == PP_PUNCT_COMMA && sa->raw && i + 2 < n &&
            pp_is_hashhash(vec_get(macro->body, i + 1)))
        {
            PpToken *va = vec_get(macro->body, i + 2);
            if (va->kind == TOK_PP_PARAM && va->param_idx == macro->param_count)
            {
                if (!sa->variadic_absent)
                {
                    vec_push(out, pp_hidden_copy(pp, bt, sa));
                    pp_push_arg(pp, out, vec_get(sa->expanded, macro->param_count), sa);
                }
                i += 2;
                continue;
            }
        }

        if (bt->kind == TOK_PP_PUNCT && bt->punct == PP_PUNCT_HASH && sa->raw && i + 1 < n &&
            ((PpToken *) vec_get(macro->body, i + 1))->kind == TOK_PP_PARAM)
        {
            PpToken *next = vec_get(macro->body, i + 1);
            vec_push(out, pp_stringize(pp, vec_get(sa->raw, next->param_idx), sa->loc));
            i++;
            continue;
        }
        if (bt->kind == TOK_PP_PARAM && sa->expanded && bt->param_idx < vec_size(sa->expanded))
        {
            bool under_paste = (i > 0 && pp_is_hashhash(vec_get(macro->body, i - 1))) ||
                               (i + 1 < n && pp_is_hashhash(vec_get(macro->body, i + 1)));
            pp_push_arg(pp, out, vec_get(under_paste ? sa->raw : sa->expanded, bt->param_idx), sa);
            continue;
        }

        vec_push(out, pp_hidden_copy(pp, bt, sa));
    }
    return out;
}

/* Merges `##` runs with placemarker semantics: an empty side disappears. */
static TokList *pp_paste_list(Pp *pp, Vec *tokens, Loc inv_loc)
{
    Vec *out = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(tokens); i++)
    {
        PpToken *t = vec_get(tokens, i);
        if (!pp_is_hashhash(t))
        {
            vec_push(out, t);
            continue;
        }
        if (i + 1 < vec_size(tokens))
        {
            PpToken *right = vec_get(tokens, i + 1);
            if (vec_size(out) > 0)
            {
                PpToken *left = vec_last(out);
                vec_pop(out);
                vec_push(out, pp_paste(pp, left, right, inv_loc));
            }
            else
            {
                vec_push(out, right);
            }
            i++;
        }
        /* trailing `##` with an empty right operand disappears */
    }

    TokList *head = NULL;
    TokList **tail = &head;
    for (size_t i = 0; i < vec_size(out); i++)
    {
        *tail = list_cons(pp->arena, vec_get(out, i), NULL);
        tail = &(*tail)->next;
    }
    return head;
}

/* Substitutes then pastes (§6.10.3.2/§6.10.3.3); every emitted token gets the
   invocation's hide set. */
static TokList *pp_subst(Pp *pp, Macro *macro, const SubstArgs *sa)
{
    return pp_paste_list(pp, pp_substitute(pp, macro, sa), sa->loc);
}

static Vec *pp_prescan_args(Pp *pp, Vec *args);

/* Deletes the L/u8/u/U prefix and quotes from a string-literal token, keeping
   the raw bytes between the quotes and unescaping only `\"` (the rule gcc and
   clang actually implement for `_Pragma`, despite C11 §6.10.9's escape
   decoding). Returns an arena NUL-terminated buffer, or NULL after reporting a
   diagnostic. */
static const char *pp_destringize(Pp *pp, const PpToken *str)
{
    const char *p = str->spell;
    const char *end = str->spell + str->len;
    while (p < end && *p != '"')
    {
        p++;
    }
    if (p >= end - 1)
    {
        pp_error(pp, str->loc, "malformed string literal in _Pragma operand");
        return NULL;
    }
    p++;
    const char *tail = end - 1;
    ByteBuf buf;
    bytebuf_init(&buf, pp->arena);
    while (p < tail)
    {
        if (*p == '\\' && p + 1 < tail && p[1] == '"')
        {
            p += 2;
            bytebuf_append(&buf, '"');
        }
        else
        {
            bytebuf_append(&buf, (u8) *p);
            p++;
        }
    }
    bytebuf_append(&buf, 0);
    return (const char *) buf.data;
}

/* Executes the destringized `_Pragma` operand as a `#pragma` directive line:
   re-lexes the text with the 17b scanner and feeds a synthetic line into the
   same directive dispatcher (reuse, don't fork). */
static void pp_execute_pragma(Pp *pp, const char *text, Loc loc)
{
    if (vec_size(pp->includes) == 0)
    {
        return;
    }
    PpIncludeFrame *cur = vec_last(pp->includes);

    Vec *lexed = pp_lex(loc.file, text, pp->arena);
    if (!lexed)
    {
        return;
    }

    Vec *line = vec_new(pp->arena);
    PpToken *hash = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *hash = (PpToken) {
        .kind = TOK_PP_PUNCT, .punct = PP_PUNCT_HASH, .spell = "#", .len = 1, .loc = loc};
    vec_push(line, hash);
    PpToken *name = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *name = (PpToken) {.kind = TOK_PP_IDENT, .spell = "pragma", .len = 6, .loc = loc};
    vec_push(line, name);
    size_t n = vec_size(lexed);
    for (size_t i = 0; i < n; i++)
    {
        PpToken *tok = vec_get(lexed, i);
        if (tok->kind != TOK_PP_EOF)
        {
            vec_push(line, tok);
        }
    }
    PpToken *eof = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *eof = (PpToken) {.kind = TOK_PP_EOF, .loc = loc};
    vec_push(line, eof);

    PpIncludeFrame syn = {0};
    syn.tokens = line;
    syn.file = cur->file;
    pp_directive(pp, &syn, 0, vec_size(line) - 1);
    if (syn.system_header)
    {
        cur->system_header = true;
    }
}

/* C11 §6.10.9 `_Pragma`: the operand (a string literal, macro-expanded like a
   call argument) is destringized, re-lexed, and executed as a `#pragma`
   directive line. `_Pragma` itself produces no output tokens. */
static TokList *pp_pragma_op(Pp *pp, TokList *ts)
{
    PpToken *prag = ts->tok;

    TokList *lparen = pp_skip_trivia_list(ts->next);
    if (!lparen || !pp_is_lparen(lparen->tok))
    {
        pp_error(pp, prag->loc, "_Pragma expects '(' after it");
        return ts->next;
    }

    TokList *arg = NULL;
    TokList **atail = &arg;
    TokList *it = lparen->next;
    for (; it && !pp_is_rparen(it->tok); it = it->next)
    {
        *atail = list_cons(pp->arena, it->tok, NULL);
        atail = &(*atail)->next;
    }
    if (!it)
    {
        pp_error(pp, prag->loc, "unterminated _Pragma operand");
        return lparen->next;
    }
    TokList *after = it->next;

    PpToken *str = NULL;
    if (arg)
    {
        size_t real = 0;
        for (TokList *r = pp_expand_list(pp, arg); r; r = r->next)
        {
            if (!pp_is_trivia(r->tok))
            {
                str = r->tok;
                real++;
            }
        }
        if (real != 1 || str->kind != TOK_PP_STRING)
        {
            pp_error(pp, prag->loc, "_Pragma operand must be a single string literal");
            str = NULL;
        }
    }
    else
    {
        pp_error(pp, prag->loc, "_Pragma operand must be a string literal");
    }

    if (str)
    {
        const char *text = pp_destringize(pp, str);
        if (text)
        {
            pp_execute_pragma(pp, text, prag->loc);
        }
    }
    return after;
}

/* Rescans ts with hide sets, splicing each macro's replacement in place of its
   invocation (C11 §6.10.3.1 argument prescan, §6.10.3.4 disable-during). */
static TokList *pp_expand_list(Pp *pp, TokList *ts)
{
    TokList *out = NULL;
    TokList **tail = &out;

    while (ts)
    {
        PpToken *t = ts->tok;
        Macro *macro = t->kind == TOK_PP_IDENT ? pp_macro_lookup(pp, t) : NULL;
        if (!macro || hs_has(t->hide, macro))
        {
            if (t->kind == TOK_PP_IDENT && !t->hide &&
                hashset_contains(pp->poison, pp_token_name(pp, t)))
            {
                pp_error(pp, t->loc, "attempt to use poisoned '%.*s'", (int) t->len, t->spell);
            }
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
            ts = ts->next;
            continue;
        }

        if (macro->is_pragma)
        {
            ts = pp_pragma_op(pp, ts);
            continue;
        }

        if (macro->kind == MACRO_OBJ)
        {
            SubstArgs sa = {.hs = hs_add(pp->arena, t->hide, macro), .loc = t->loc};
            ts = list_concat(pp->arena, pp_subst(pp, macro, &sa), ts->next);
            continue;
        }

        TokList *lparen = pp_skip_trivia_list(ts->next);
        if (!lparen || !pp_is_lparen(lparen->tok))
        {
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
            ts = ts->next;
            continue;
        }

        MacroArgs args;
        if (!pp_collect_args(pp, t, macro, lparen, &args))
        {
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
            ts = ts->next;
            continue;
        }

        size_t arg_count = vec_size(args.args);
        bool empty_single = arg_count == 1 && pp_arg_is_empty(vec_get(args.args, 0));
        bool arity_ok =
            macro->variadic ? arg_count >= macro->param_count : arg_count == macro->param_count;
        if (empty_single && macro->param_count == 0)
        {
            arity_ok = true;
        }
        if (!arity_ok)
        {
            pp_error(pp, t->loc, "macro '%s' expects %u arguments, but %zu given", macro->name,
                     macro->param_count, arg_count);
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
            ts = ts->next;
            continue;
        }

        size_t effective_count = (empty_single && macro->param_count == 0) ? 0 : arg_count;
        SubstArgs sa = {.hs = hs_add(pp->arena, args.hs, macro),
                        .loc = t->loc,
                        .variadic_absent =
                            macro->variadic && effective_count <= macro->param_count};

        Vec *expanded = pp_prescan_args(pp, args.args);
        if (macro->variadic)
        {
            sa.raw = pp_effective_args(pp, args.args, macro->param_count, t->loc);
            sa.expanded = pp_effective_args(pp, expanded, macro->param_count, t->loc);
        }
        else
        {
            sa.raw = args.args;
            sa.expanded = expanded;
        }
        ts = list_concat(pp->arena, pp_subst(pp, macro, &sa), args.after);
    }
    return out;
}

static Vec *pp_prescan_args(Pp *pp, Vec *args)
{
    Vec *expanded = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(args); i++)
    {
        Vec *raw = vec_get(args, i);
        TokList *list = NULL;
        TokList **tail = &list;
        for (size_t j = 0; j < vec_size(raw); j++)
        {
            *tail = list_cons(pp->arena, vec_get(raw, j), NULL);
            tail = &(*tail)->next;
        }

        Vec *arg = vec_new(pp->arena);
        for (TokList *r = pp_expand_list(pp, list); r; r = r->next)
        {
            vec_push(arg, r->tok);
        }
        vec_push(expanded, arg);
    }
    return expanded;
}

static void pp_expand_line(Pp *pp, PpIncludeFrame *frame, size_t begin, size_t end)
{
    TokList *list = NULL;
    TokList **tail = &list;
    for (size_t i = begin; i < end; i++)
    {
        *tail = list_cons(pp->arena, vec_get(frame->tokens, i), NULL);
        tail = &(*tail)->next;
    }

    u32 presumed_line = frame->presumed_line;
    const char *presumed_file = frame->presumed_file;
    for (TokList *r = pp_expand_list(pp, list); r; r = r->next)
    {
        PpToken *t = pp_cook_predefined(pp, r->tok, presumed_line, presumed_file);
        t->loc.file = presumed_file;
        t->loc.line = presumed_line;
        if (t->kind == TOK_PP_TRIVIA_NL)
        {
            presumed_line++;
        }
        vec_push(pp->out, t);
    }
    frame->presumed_line = presumed_line;
}

static size_t pp_line_end(const PpIncludeFrame *frame, size_t start)
{
    size_t count = vec_size(frame->tokens);
    for (size_t i = start; i < count; i++)
    {
        PpToken *tok = vec_get(frame->tokens, i);
        if (tok->kind == TOK_PP_EOF)
        {
            return i;
        }
        if (tok->kind == TOK_PP_TRIVIA_NL)
        {
            return i + 1;
        }
    }
    return count;
}

static const PpToken *pp_first_real(const PpIncludeFrame *frame, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++)
    {
        PpToken *tok = vec_get(frame->tokens, i);
        if (!pp_is_trivia(tok))
        {
            return tok;
        }
    }
    return NULL;
}

/* Processes one directive line or, for ordinary text, gathers consecutive
   non-directive lines so macro invocations may span lines, then expands. */
static void pp_line(Pp *pp, PpIncludeFrame *frame)
{
    size_t count = vec_size(frame->tokens);
    size_t begin = frame->cursor;
    size_t end = pp_line_end(frame, begin);
    const PpToken *first = pp_first_real(frame, begin, end);

    if (first && pp_is_hash(first))
    {
        frame->cursor = end;
        bool is_line_directive = pp_directive(pp, frame, begin, end);
        if (!is_line_directive)
        {
            frame->presumed_line += pp_count_newlines(frame, begin, end);
        }
        return;
    }

    while (end < count && ((PpToken *) vec_get(frame->tokens, end))->kind != TOK_PP_EOF)
    {
        size_t next_end = pp_line_end(frame, end);
        const PpToken *next_first = pp_first_real(frame, end, next_end);
        if (next_first && pp_is_hash(next_first))
        {
            break;
        }
        end = next_end;
    }
    frame->cursor = end;
    if (pp_is_skipping(pp))
    {
        frame->presumed_line += pp_count_newlines(frame, begin, end);
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

static bool pp_is_ident_text(const char *s, size_t n)
{
    if (n == 0)
    {
        return false;
    }
    char c0 = s[0];
    if (!(c0 == '_' || (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')))
    {
        return false;
    }
    for (size_t i = 1; i < n; i++)
    {
        char c = s[i];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')))
        {
            return false;
        }
    }
    return true;
}

/* `-D name[=value]`: object-like, value defaults to `1` (gcc), `name=` is
   empty. */
void pp_define_cmdline(Pp *pp, const char *spec)
{
    Loc loc = (Loc) {.file = "<command-line>", .line = 1, .col = 1};
    const char *eq = strchr(spec, '=');
    size_t name_len = eq ? (size_t) (eq - spec) : strlen(spec);
    if (!pp_is_ident_text(spec, name_len))
    {
        pp_error(pp, loc, "invalid macro name '%s'", spec);
        return;
    }
    char *name = arena_alloc(pp->arena, name_len + 1, 1);
    memcpy(name, spec, name_len);
    name[name_len] = '\0';
    if (pp_is_reserved_name(name))
    {
        pp_error(pp, loc, "redefinition of predefined macro '%s'", name);
        return;
    }

    Vec *body = vec_new(pp->arena);
    if (!eq)
    {
        PpToken *one = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
        *one = (PpToken) {.kind = TOK_PP_NUMBER, .loc = loc, .spell = "1", .len = 1};
        vec_push(body, one);
    }
    else if (eq[1] != '\0')
    {
        Vec *soup = pp_lex("<command-line>", eq + 1, pp->arena);
        if (!soup)
        {
            return;
        }
        for (size_t i = 0; i < vec_size(soup); i++)
        {
            PpToken *t = vec_get(soup, i);
            if (!pp_is_trivia(t) && t->kind != TOK_PP_EOF)
            {
                vec_push(body, t);
            }
        }
    }

    Macro *prev = strmap_get(pp->macros, name);
    if (prev)
    {
        if (prev->kind == MACRO_OBJ && pp_bodies_equal(prev->body, body))
        {
            return;
        }
        pp_error(pp, loc, "redefinition of macro '%s'", name);
        pp_note(prev->loc, "previous definition of '%s' is here", name);
        return;
    }

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *macro = (Macro) {.name = name, .kind = MACRO_OBJ, .loc = loc, .body = body};
    strmap_set(pp->macros, name, macro);
}

void pp_undef_cmdline(Pp *pp, const char *name)
{
    hashmap_remove(pp->macros, name);
}

/* `-include file`: resolves like an angle include and processes it to
   completion now, so its macros and output precede the main file. */
void pp_include_cmdline(Pp *pp, const char *file)
{
    Loc loc = (Loc) {.file = file, .line = 1, .col = 1};
    const char *path = pp_file_exists(file) ? file : pp_include_find(pp, false, file, NULL, false);
    if (!path)
    {
        pp_error(pp, loc, "include file not found: '%s'", file);
        return;
    }
    pp_include_file(pp, path);
    pp_run(pp);
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
