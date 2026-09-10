#include "pp.h"

#include "util/bytebuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
    arena_free(pp->arena);
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

/* Parses the comma-separated parameter list in (open, close) into params. */
static bool pp_parse_params(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t close,
                            Vec *params, bool *variadic)
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
        if (pp_param_index(params, t) >= 0)
        {
            pp_error(pp, t->loc, "duplicate macro parameter '%.*s'", (int) t->len, t->spell);
            return false;
        }
        vec_push(params, (void *) pp_token_name(pp, t));
        i++;

        while (i < close && pp_is_trivia(vec_get(frame->tokens, i)))
        {
            i++;
        }
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

/* Rewrites body identifiers that name parameters into TOK_PP_PARAM markers. */
static Vec *pp_mark_params(Pp *pp, Vec *body, Vec *params, bool variadic)
{
    Vec *marked = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *bt = vec_get(body, i);
        int idx = bt->kind == TOK_PP_IDENT ? pp_param_index(params, bt) : -1;
        bool is_va = bt->kind == TOK_PP_IDENT && pp_spelling_is(bt, "__VA_ARGS__");
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
    Vec *body = NULL;
    u32 param_count = 0;
    bool variadic = false;

    if (function_like)
    {
        size_t close = pp_param_list_end(frame, i + 1, end);
        if (close >= end)
        {
            pp_error(pp, name->loc, "missing ')' in macro parameter list");
            return;
        }
        Vec *params = vec_new(pp->arena);
        if (!pp_parse_params(pp, frame, i + 1, close, params, &variadic))
        {
            return;
        }
        if (variadic && vec_size(params) == 0)
        {
            pp_error(pp, name->loc, "'...' requires at least one named parameter");
            return;
        }
        param_count = (u32) vec_size(params);
        body = pp_mark_params(pp, pp_collect_body(pp, frame, close + 1, end), params, variadic);
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

/* Substitutes parameters in order; a parameter adjacent to `##` receives its
   raw argument, everything else the prescanned one, and `#` stringizes. */
static Vec *pp_substitute(Pp *pp, Macro *macro, Vec *raw_args, Vec *args, Hideset *new_hs,
                          Loc inv_loc)
{
    Vec *out = vec_new(pp->arena);
    size_t n = vec_size(macro->body);

    for (size_t i = 0; i < n; i++)
    {
        PpToken *bt = vec_get(macro->body, i);
        if (bt->kind == TOK_PP_PUNCT && bt->punct == PP_PUNCT_HASH && raw_args && i + 1 < n &&
            ((PpToken *) vec_get(macro->body, i + 1))->kind == TOK_PP_PARAM)
        {
            PpToken *next = vec_get(macro->body, i + 1);
            vec_push(out, pp_stringize(pp, vec_get(raw_args, next->param_idx), inv_loc));
            i++;
            continue;
        }
        if (bt->kind == TOK_PP_PARAM && args && bt->param_idx < vec_size(args))
        {
            bool under_paste = (i > 0 && pp_is_hashhash(vec_get(macro->body, i - 1))) ||
                               (i + 1 < n && pp_is_hashhash(vec_get(macro->body, i + 1)));
            Vec *arg =
                under_paste ? vec_get(raw_args, bt->param_idx) : vec_get(args, bt->param_idx);
            for (size_t j = 0; j < vec_size(arg); j++)
            {
                PpToken *a = vec_get(arg, j);
                if (pp_is_trivia(a))
                {
                    continue;
                }
                PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
                *copy = *a;
                copy->hide = hs_union(pp->arena, a->hide, new_hs);
                copy->loc = inv_loc;
                vec_push(out, copy);
            }
            continue;
        }

        PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
        *copy = *bt;
        copy->hide = hs_union(pp->arena, bt->hide, new_hs);
        copy->loc = inv_loc;
        vec_push(out, copy);
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
static TokList *pp_subst(Pp *pp, Macro *macro, Vec *raw_args, Vec *args, Hideset *new_hs,
                         Loc inv_loc)
{
    return pp_paste_list(pp, pp_substitute(pp, macro, raw_args, args, new_hs, inv_loc), inv_loc);
}

static Vec *pp_prescan_args(Pp *pp, Vec *args);

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
            *tail = list_cons(pp->arena, t, NULL);
            tail = &(*tail)->next;
            ts = ts->next;
            continue;
        }

        if (macro->kind == MACRO_OBJ)
        {
            Hideset *new_hs = hs_add(pp->arena, t->hide, macro);
            ts = list_concat(pp->arena, pp_subst(pp, macro, NULL, NULL, new_hs, t->loc), ts->next);
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

        Vec *expanded = pp_prescan_args(pp, args.args);
        Hideset *new_hs = hs_add(pp->arena, args.hs, macro);
        ts = list_concat(pp->arena, pp_subst(pp, macro, args.args, expanded, new_hs, t->loc),
                         args.after);
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

static void pp_expand_line(Pp *pp, const PpIncludeFrame *frame, size_t begin, size_t end)
{
    TokList *list = NULL;
    TokList **tail = &list;
    for (size_t i = begin; i < end; i++)
    {
        *tail = list_cons(pp->arena, vec_get(frame->tokens, i), NULL);
        tail = &(*tail)->next;
    }

    for (TokList *r = pp_expand_list(pp, list); r; r = r->next)
    {
        vec_push(pp->out, r->tok);
    }
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
        pp_directive(pp, frame, begin, end);
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
    pp_expand_line(pp, frame, begin, end);
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
