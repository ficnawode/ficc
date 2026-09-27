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

/* Only copies x; y is shared. */
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

static TokList **pp_list_append(Pp *pp, TokList **tail, PpToken *t)
{
    *tail = list_cons(pp->arena, t, NULL);
    return &(*tail)->next;
}

static bool pp_is_trivia(const PpToken *tok)
{
    return tok->kind == TOK_PP_TRIVIA_WS || tok->kind == TOK_PP_TRIVIA_NL ||
           tok->kind == TOK_PP_TRIVIA_COMMENT;
}

static bool pp_is_ident(const PpToken *tok, const char *spelling)
{
    if (tok->kind != TOK_PP_IDENT)
    {
        return false;
    }
    size_t len = strlen(spelling);
    return tok->len == len && strncmp(tok->spell, spelling, len) == 0;
}

static bool pp_is_punct(const PpToken *tok, PpPunct punct)
{
    return tok->kind == TOK_PP_PUNCT && tok->punct == punct;
}

static void pp_predefine(Pp *pp, const char *name, const char *spelling)
{
    Vec *body = vec_new(pp->arena);
    PpToken *tok = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *tok = (PpToken) {.kind = TOK_PP_NUMBER, .spell = spelling, .len = (u32) strlen(spelling)};
    vec_push(body, tok);

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *macro = (Macro) {
        .name = name,
        .kind = MACRO_OBJ,
        .body = body,
        .predefined = true,
    };
    strmap_set(pp->macros, name, macro);
}

static void pp_predefine_type(Pp *pp, const char *name, const char *spelling)
{
    Vec *body = vec_new(pp->arena);
    const char *p = spelling;
    while (*p)
    {
        while (*p == ' ')
        {
            p++;
        }
        const char *word = p;
        while (*p && *p != ' ')
        {
            p++;
        }
        if (p == word)
        {
            break;
        }
        PpToken *tok = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
        *tok = (PpToken) {.kind = TOK_PP_IDENT, .spell = word, .len = (u32) (p - word)};
        vec_push(body, tok);
    }

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *macro = (Macro) {
        .name = name,
        .kind = MACRO_OBJ,
        .body = body,
        .predefined = true,
    };
    strmap_set(pp->macros, name, macro);
}

static void pp_load_source_date_epoch(Pp *pp)
{
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (!sde || !sde[0])
    {
        return;
    }
    u64 value = 0;
    for (const char *p = sde; *p; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return;
        }
        value = value * 10 + (u64) (*p - '0');
    }
    pp->has_source_date_epoch = true;
    pp->source_date_epoch = (i64) value;
}

static void pp_predefine_func_macro(Pp *pp, const char *name, const char *params, const char *body);

Pp *pp_new(Arena *arena)
{
    Pp *pp = arena_alloc(arena, sizeof(Pp), sizeof(void *));
    pp->arena = arena;
    pp->out = vec_new(arena);
    pp->includes = vec_new(arena);
    pp->macros = strmap_new(arena);
    pp->conds = vec_new(arena);
    pp->cfg = (PPConfig) {0};
    pp->cfg.include_paths = vec_new(arena);
    pp->cfg.system_include_paths = vec_new(arena);
    pp->physical = (Loc) {.file = NULL, .line = 1, .col = 1};
    pp->presumed = (Loc) {.file = NULL, .line = 1, .col = 1};
    pp->skipping = false;
    pp->exe_path = NULL;
    pp->builtin_dir = NULL;
    pp->cooked_date = NULL;
    pp->cooked_time = NULL;
    pp->pragma_once = hashset_new(arena, hashmap_str_hash, hashmap_str_eq);
    pp->poison = hashset_new(arena, hashmap_str_hash, hashmap_str_eq);
    pp->error_count = 0;
    pp->warning_count = 0;

    pp_load_source_date_epoch(pp);

    pp_predefine(pp, "__STDC__", "1");
    pp_predefine(pp, "__STDC_VERSION__", "201112L");
    pp_predefine(pp, "__STDC_HOSTED__", "1");
    pp_predefine(pp, "__STDC_NO_ATOMICS__", "1");
    pp_predefine(pp, "__STDC_NO_THREADS__", "1");
    pp_predefine(pp, "__STDC_NO_VLA__", "1");
    pp_predefine(pp, "__STDC_NO_COMPLEX__", "1");
    pp_predefine(pp, "__x86_64__", "1");
    pp_predefine(pp, "__x86_64", "1");
    pp_predefine(pp, "__amd64__", "1");
    pp_predefine(pp, "__amd64", "1");
    pp_predefine(pp, "__LP64__", "1");
    pp_predefine(pp, "_LP64", "1");
    /* Host platform macros, as a hosted Linux/x86-64 compiler predefines them.
       Feature code (e.g. SQLite's mmap support) branches on these. */
    pp_predefine(pp, "__linux__", "1");
    pp_predefine(pp, "__linux", "1");
    pp_predefine(pp, "linux", "1");
    pp_predefine(pp, "__gnu_linux__", "1");
    pp_predefine(pp, "__unix__", "1");
    pp_predefine(pp, "__unix", "1");
    pp_predefine(pp, "unix", "1");
    pp_predefine(pp, "__ELF__", "1");
    /* <sys/cdefs.h> keeps `__attribute__` only for GNU/clang/tinycc identity. */
    pp_predefine(pp, "__TINYC__", "927");
    pp_predefine(pp, "__SIZEOF_POINTER__", "8");
    pp_predefine(pp, "__SIZEOF_LONG__", "8");
    pp_predefine_type(pp, "__SIZE_TYPE__", "long unsigned int");
    pp_predefine_type(pp, "__PTRDIFF_TYPE__", "long int");
    pp_predefine_type(pp, "__WCHAR_TYPE__", "int");
    pp_predefine_type(pp, "__WINT_TYPE__", "unsigned int");
    pp_predefine_type(pp, "__INTMAX_TYPE__", "long int");
    pp_predefine_type(pp, "__UINTMAX_TYPE__", "long unsigned int");
    pp_predefine_type(pp, "__INTPTR_TYPE__", "long int");
    pp_predefine_type(pp, "__UINTPTR_TYPE__", "long unsigned int");
    pp_predefine_type(pp, "__CHAR16_TYPE__", "short unsigned int");
    pp_predefine_type(pp, "__CHAR32_TYPE__", "unsigned int");
    pp_predefine_type(pp, "__SSIZE_TYPE__", "long int");
    pp_predefine_func_macro(pp, "__REDIRECT", "name,proto,alias", "name proto");
    pp_predefine_func_macro(pp, "__REDIRECT_NTH", "name,proto,alias", "name proto");
    pp_predefine_func_macro(pp, "__REDIRECT_NTHNL", "name,proto,alias", "name proto");

    Macro *pragma = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *pragma = (Macro) {.name = "_Pragma", .kind = MACRO_OBJ, .predefined = true, .is_pragma = true};
    strmap_set(pp->macros, "_Pragma", pragma);
    return pp;
}

void pp_free(Pp *pp)
{
    arena_free(pp->arena);
}

void pp_apply_config(Pp *pp, const PPConfig *cfg)
{
    pp->cfg.nostdinc = cfg->nostdinc;
    pp->cfg.pedantic = cfg->pedantic;
    for (size_t k = 0; k < vec_size(cfg->include_paths); k++)
    {
        vec_push(pp->cfg.include_paths, vec_get(cfg->include_paths, k));
    }
    for (size_t k = 0; k < vec_size(cfg->system_include_paths); k++)
    {
        vec_push(pp->cfg.system_include_paths, vec_get(cfg->system_include_paths, k));
    }
    for (size_t k = 0; k < vec_size(cfg->cmds); k++)
    {
        PPCommand *cmd = vec_get(cfg->cmds, k);
        switch (cmd->kind)
        {
            case CMD_DEFINE:
                pp_define_cmdline(pp, cmd->arg);
                break;
            case CMD_UNDEF:
                pp_undef_cmdline(pp, cmd->arg);
                break;
            case CMD_INCLUDE:
                pp_include_cmdline(pp, cmd->arg);
                break;
        }
    }
}

static void pp_push_include(Pp *pp, const char *file, Vec *tokens, bool system)
{
    PpIncludeFrame *frame = arena_alloc(pp->arena, sizeof(PpIncludeFrame), sizeof(void *));
    frame->file = file;
    frame->tokens = tokens;
    frame->cursor = 0;
    frame->presumed_line = 1;
    frame->presumed_file = file;
    frame->system_header = system;
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

static const char *pp_token_text(Pp *pp, const PpToken *tok)
{
    char *buf = arena_alloc(pp->arena, tok->len + 1, 1);
    memcpy(buf, tok->spell, tok->len);
    buf[tok->len] = '\0';
    return buf;
}

static const char *pp_string_content(Pp *pp, const PpToken *tok)
{
    char *buf = arena_alloc(pp->arena, tok->len > 1 ? tok->len - 1 : 1, 1);
    u32 n = tok->len > 2 ? tok->len - 2 : 0;
    memcpy(buf, tok->spell + 1, n);
    buf[n] = '\0';
    return buf;
}

static Macro *pp_macro_lookup(Pp *pp, const PpToken *tok)
{
    return strmap_get(pp->macros, pp_token_text(pp, tok));
}

static TokList *pp_expand_list(Pp *pp, TokList *ts);

static Vec *pp_expand_vec(Pp *pp, Vec *input)
{
    TokList *list = NULL;
    TokList **tail = &list;
    for (size_t i = 0; i < vec_size(input); i++)
    {
        PpToken *t = vec_get(input, i);
        if (!pp_is_trivia(t))
        {
            tail = pp_list_append(pp, tail, t);
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

/* The real tokens in [start, end), macro-expanded with trivia dropped. */
static Vec *pp_gather_real(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    Vec *real = vec_new(pp->arena);
    for (size_t i = start; i < end; i++)
    {
        PpToken *tok = vec_get(frame->tokens, i);
        if (!pp_is_trivia(tok))
        {
            vec_push(real, tok);
        }
    }
    return real;
}

static Vec *pp_expand_operand(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    return pp_expand_vec(pp, pp_gather_real(pp, frame, start, end));
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

static size_t pp_skip_trivia(const PpIncludeFrame *frame, size_t i, size_t end)
{
    while (i < end && pp_is_trivia(vec_get(frame->tokens, i)))
    {
        i++;
    }
    return i;
}

static size_t pp_param_list_end(const PpIncludeFrame *frame, size_t open, size_t end)
{
    for (size_t i = open + 1; i < end; i++)
    {
        if (pp_is_punct(vec_get(frame->tokens, i), PP_PUNCT_RPAREN))
        {
            return i;
        }
    }
    return end;
}

static bool pp_require_variadic_last(Pp *pp, const PpIncludeFrame *frame, size_t *i, size_t close,
                                     Loc params_loc)
{
    *i = pp_skip_trivia(frame, *i + 1, close);
    if (*i < close)
    {
        pp_error(pp, params_loc, "'...' must be the last macro parameter");
        return false;
    }
    return true;
}

/* `name...` (GNU) aliases `__VA_ARGS__`; a bare `...` is the variadic tail. */
static bool pp_parse_params(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t close,
                            Vec *params, bool *variadic, const char **variadic_name)
{
    size_t i = pp_skip_trivia(frame, open + 1, close);
    while (i < close)
    {
        PpToken *param = vec_get(frame->tokens, i);
        if (pp_is_punct(param, PP_PUNCT_ELLIPSIS))
        {
            *variadic = true;
            return pp_require_variadic_last(pp, frame, &i, close, param->loc);
        }
        if (param->kind != TOK_PP_IDENT)
        {
            pp_error(pp, param->loc, "expected macro parameter name");
            return false;
        }

        const char *name = pp_token_text(pp, param);
        i = pp_skip_trivia(frame, i + 1, close);
        if (i < close && pp_is_punct(vec_get(frame->tokens, i), PP_PUNCT_ELLIPSIS))
        {
            *variadic = true;
            *variadic_name = name;
            return pp_require_variadic_last(pp, frame, &i, close, param->loc);
        }
        if (pp_param_index(params, param) >= 0)
        {
            pp_error(pp, param->loc, "duplicate macro parameter '%.*s'", (int) param->len,
                     param->spell);
            return false;
        }
        vec_push(params, (void *) name);

        if (i >= close)
        {
            break;
        }
        if (!pp_is_punct(vec_get(frame->tokens, i), PP_PUNCT_COMMA))
        {
            pp_error(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                     "expected ',' or ')' in macro parameter list");
            return false;
        }
        i = pp_skip_trivia(frame, i + 1, close);
    }
    return true;
}

/* Body identifiers naming a parameter become TOK_PP_PARAM markers;
   `__VA_ARGS__` and the GNU alias mark the variadic tail. */
static Vec *pp_mark_params(Pp *pp, Vec *body, Vec *params, bool variadic, const char *variadic_name)
{
    Vec *marked = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *bt = vec_get(body, i);
        int idx = bt->kind == TOK_PP_IDENT ? pp_param_index(params, bt) : -1;
        bool is_va_args =
            bt->kind == TOK_PP_IDENT &&
            (pp_is_ident(bt, "__VA_ARGS__") || (variadic_name && pp_is_ident(bt, variadic_name)));
        if (idx < 0 && !is_va_args)
        {
            vec_push(marked, bt);
            continue;
        }
        if (is_va_args && !variadic)
        {
            pp_error(pp, bt->loc, "__VA_ARGS__ can only appear in a variadic macro");
            return NULL;
        }

        PpToken *copy = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
        *copy = *bt;
        copy->kind = TOK_PP_PARAM;
        copy->param_idx = is_va_args ? (u32) vec_size(params) : (u32) idx;
        vec_push(marked, copy);
    }
    return marked;
}

/* Installs a predefined function-like macro from comma-separated parameter
   names and body text (used for glibc's __REDIRECT family, whose asm-label
   form we replace with a plain declaration). */
static void pp_predefine_func_macro(Pp *pp, const char *name, const char *params, const char *body)
{
    Loc loc = (Loc) {.file = "<built-in>", .line = 1, .col = 1};
    Vec *param_names = vec_new(pp->arena);
    Vec *ptoks = pp_lex("<built-in>", params, pp->arena);
    if (!ptoks)
    {
        return;
    }
    for (size_t i = 0; i < vec_size(ptoks); i++)
    {
        PpToken *t = vec_get(ptoks, i);
        if (t->kind == TOK_PP_IDENT)
        {
            vec_push(param_names, (void *) pp_token_text(pp, t));
        }
    }

    Vec *raw = vec_new(pp->arena);
    Vec *btoks = pp_lex("<built-in>", body, pp->arena);
    if (!btoks)
    {
        return;
    }
    for (size_t i = 0; i < vec_size(btoks); i++)
    {
        PpToken *t = vec_get(btoks, i);
        if (!pp_is_trivia(t) && t->kind != TOK_PP_EOF)
        {
            vec_push(raw, t);
        }
    }
    Vec *marked = pp_mark_params(pp, raw, param_names, false, NULL);
    if (!marked)
    {
        return;
    }

    Macro *m = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    *m = (Macro) {.name = name,
                  .kind = MACRO_FUNC,
                  .loc = loc,
                  .body = marked,
                  .param_count = (u32) vec_size(param_names),
                  .predefined = true};
    strmap_set(pp->macros, name, m);
}

static bool pp_check_va_args(Pp *pp, Vec *body)
{
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *t = vec_get(body, i);
        if (pp_is_ident(t, "__VA_ARGS__"))
        {
            pp_error(pp, t->loc, "__VA_ARGS__ can only appear in a variadic macro");
            return false;
        }
    }
    return true;
}

/* C11 §6.10.3.2: every `#` in the replacement must precede a parameter. */
static bool pp_check_hashes(Pp *pp, Vec *body)
{
    for (size_t i = 0; i < vec_size(body); i++)
    {
        PpToken *t = vec_get(body, i);
        if (pp_is_punct(t, PP_PUNCT_HASH))
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

/* C11 §6.10.3.3: `##` operands must exist on both sides. */
static bool pp_check_pastes(Pp *pp, Vec *body)
{
    size_t n = vec_size(body);
    for (size_t i = 0; i < n; i++)
    {
        PpToken *t = vec_get(body, i);
        if (!pp_is_punct(t, PP_PUNCT_HASHHASH))
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

/* C11 §6.10.8: predefined names may not be redefined. */
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

typedef struct
{
    MacroKind kind;
    Vec *body;
    u32 param_count;
    bool variadic;
} MacroDef;

static bool pp_parse_function_like(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t end,
                                   PpToken *name, MacroDef *def)
{
    size_t close = pp_param_list_end(frame, open + 1, end);
    if (close >= end)
    {
        pp_error(pp, name->loc, "missing ')' in macro parameter list");
        return false;
    }
    Vec *params = vec_new(pp->arena);
    bool variadic = false;
    const char *variadic_name = NULL;
    if (!pp_parse_params(pp, frame, open + 1, close, params, &variadic, &variadic_name))
    {
        return false;
    }
    Vec *body = pp_mark_params(pp, pp_gather_real(pp, frame, close + 1, end), params, variadic,
                               variadic_name);
    if (!body || !pp_check_hashes(pp, body))
    {
        return false;
    }

    def->kind = MACRO_FUNC;
    def->body = body;
    def->param_count = (u32) vec_size(params);
    def->variadic = variadic;
    return true;
}

static bool pp_parse_object_like(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t end,
                                 MacroDef *def)
{
    Vec *body = pp_gather_real(pp, frame, open + 1, end);
    if (!pp_check_va_args(pp, body))
    {
        return false;
    }

    def->kind = MACRO_OBJ;
    def->body = body;
    def->param_count = 0;
    def->variadic = false;
    return true;
}

static bool pp_parse_macro_def(Pp *pp, const PpIncludeFrame *frame, size_t open, size_t end,
                               PpToken *name, MacroDef *def)
{
    bool function_like =
        open + 1 < end && pp_is_punct(vec_get(frame->tokens, open + 1), PP_PUNCT_LPAREN);
    if (function_like)
    {
        return pp_parse_function_like(pp, frame, open, end, name, def);
    }
    return pp_parse_object_like(pp, frame, open, end, def);
}

static bool pp_macro_matches(const Macro *macro, const MacroDef *def)
{
    return macro->kind == def->kind && macro->param_count == def->param_count &&
           macro->variadic == def->variadic && pp_bodies_equal(macro->body, def->body);
}

static void pp_define(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end, Loc directive_loc)
{
    size_t open = pp_skip_trivia(frame, start, end);
    if (open >= end)
    {
        pp_error(pp, directive_loc, "macro name missing");
        return;
    }
    PpToken *name = vec_get(frame->tokens, open);
    if (name->kind != TOK_PP_IDENT)
    {
        pp_error(pp, name->loc, "macro names must be identifiers");
        return;
    }
    const char *text = pp_token_text(pp, name);
    if (pp_is_reserved_name(text))
    {
        pp_error(pp, name->loc, "redefinition of predefined macro '%s'", text);
        return;
    }

    MacroDef def = {0};
    if (!pp_parse_macro_def(pp, frame, open, end, name, &def))
    {
        return;
    }
    if (!pp_check_pastes(pp, def.body))
    {
        return;
    }

    Macro *prev = strmap_get(pp->macros, text);
    if (prev && pp_macro_matches(prev, &def))
    {
        return;
    }
    if (prev)
    {
        pp_error(pp, name->loc, "redefinition of macro '%s'", text);
        pp_note(prev->loc, "previous definition of '%s' is here", text);
        return;
    }

    Macro *macro = arena_alloc(pp->arena, sizeof(Macro), sizeof(void *));
    macro->name = text;
    macro->kind = def.kind;
    macro->loc = name->loc;
    macro->body = def.body;
    macro->param_count = def.param_count;
    macro->variadic = def.variadic;
    macro->predefined = false;
    macro->is_pragma = false;
    strmap_set(pp->macros, text, macro);
}

static void pp_undef(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end, Loc directive_loc)
{
    size_t i = pp_skip_trivia(frame, start, end);
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
    const char *text = pp_token_text(pp, name);
    if (pp_is_reserved_name(text))
    {
        pp_warn(pp, name->loc, "undefining '%s'", text);
    }
    hashmap_remove(pp->macros, text);
}

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

static PpToken *pp_cond_macro(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                              const char *directive, Loc directive_loc)
{
    size_t i = pp_skip_trivia(frame, start, end);
    if (i >= end)
    {
        pp_error(pp, directive_loc, "#%s requires a macro name", directive);
        return NULL;
    }
    PpToken *name = vec_get(frame->tokens, i);
    if (name->kind != TOK_PP_IDENT)
    {
        pp_error(pp, name->loc, "#%s requires a macro name", directive);
        return NULL;
    }
    i = pp_skip_trivia(frame, i + 1, end);
    if (i < end)
    {
        pp_warn(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                "extra tokens at end of #%s directive", directive);
    }
    return name;
}

static void pp_ifdef_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc)
{
    PpToken *name = pp_cond_macro(pp, frame, start, end, "ifdef", directive_loc);
    if (!name)
    {
        return;
    }
    bool defined = strmap_get(pp->macros, pp_token_text(pp, name)) != NULL;
    bool parent_active = pp_branch_active(pp);
    pp_push_cond(pp, parent_active, parent_active && defined);
}

static void pp_ifndef_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                Loc directive_loc)
{
    PpToken *name = pp_cond_macro(pp, frame, start, end, "ifndef", directive_loc);
    if (!name)
    {
        return;
    }
    bool defined = strmap_get(pp->macros, pp_token_text(pp, name)) != NULL;
    bool parent_active = pp_branch_active(pp);
    pp_push_cond(pp, parent_active, parent_active && !defined);
}

static void pp_else_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                              Loc directive_loc)
{
    (void) frame;
    (void) start;
    (void) end;
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

static void pp_endif_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc)
{
    (void) frame;
    (void) start;
    (void) end;
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
    size_t dir_len = strlen(dir);
    char *buf = arena_alloc(pp->arena, dir_len + 1 + strlen(name) + 1, 1);
    memcpy(buf, dir, dir_len);
    buf[dir_len] = '/';
    strcpy(buf + dir_len + 1, name);
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
    size_t size = (size_t) ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = arena_alloc(arena, size + 1, 1);
    if (fread(buf, 1, size, f) != size)
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

typedef struct
{
    const char *path;
    bool system;
} IncludeHit;

/* Quoted headers try the including file's directory first (gcc). */
static IncludeHit pp_include_find(Pp *pp, bool quoted, const char *name, const char *including_file,
                                  bool next_mode)
{
    if (quoted && !next_mode && including_file)
    {
        const char *cand = pp_path_join(pp, pp_path_dirname(pp, including_file), name);
        if (pp_file_exists(cand))
        {
            return (IncludeHit) {.path = cand, .system = false};
        }
    }

    size_t start = 0;
    if (next_mode && including_file)
    {
        const char *dir = pp_path_dirname(pp, including_file);
        for (size_t k = 0; k < vec_size(pp->cfg.include_paths); k++)
        {
            if (strcmp((const char *) vec_get(pp->cfg.include_paths, k), dir) == 0)
            {
                start = k + 1;
                break;
            }
        }
    }

    for (size_t i = start; i < vec_size(pp->cfg.include_paths); i++)
    {
        const char *cand = pp_path_join(pp, vec_get(pp->cfg.include_paths, i), name);
        if (pp_file_exists(cand))
        {
            return (IncludeHit) {.path = cand, .system = false};
        }
    }

    for (size_t i = 0; i < vec_size(pp->cfg.system_include_paths); i++)
    {
        const char *cand = pp_path_join(pp, vec_get(pp->cfg.system_include_paths, i), name);
        if (pp_file_exists(cand))
        {
            return (IncludeHit) {.path = cand, .system = true};
        }
    }

    if (!pp->cfg.nostdinc)
    {
        const char *cand = pp_path_join(pp, pp_builtin_dir(pp), name);
        if (pp_file_exists(cand))
        {
            return (IncludeHit) {.path = cand, .system = true};
        }
    }
    return (IncludeHit) {.path = NULL, .system = false};
}

static void pp_include_file(Pp *pp, const char *path, bool system)
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
    pp_push_include(pp, path, tokens, system);
}

/* Reads `<...>` in a raw line; comments or newlines inside are errors. */
static const char *pp_angle_header_name(Pp *pp, const PpIncludeFrame *frame, size_t open,
                                        size_t end, size_t *next_index)
{
    ByteBuf name;
    bytebuf_init(&name, pp->arena);
    size_t i;
    for (i = open + 1; i < end; i++)
    {
        PpToken *t = vec_get(frame->tokens, i);
        if (pp_is_punct(t, PP_PUNCT_GT))
        {
            break;
        }
        if (t->kind == TOK_PP_TRIVIA_COMMENT || t->kind == TOK_PP_TRIVIA_NL)
        {
            pp_error(pp, t->loc, "invalid character inside a #include header name");
            return NULL;
        }
        if (pp_is_trivia(t))
        {
            continue;
        }
        for (u32 k = 0; k < t->len; k++)
        {
            bytebuf_append(&name, (u8) t->spell[k]);
        }
    }
    if (i >= end)
    {
        pp_error(pp, ((PpToken *) vec_get(frame->tokens, open))->loc, "missing '>' in #include");
        return NULL;
    }
    bytebuf_append(&name, '\0');
    *next_index = i + 1;
    return (const char *) bytebuf_data(&name);
}

/* Reads one header-name at *pos: `"..."` or `<...>`. `what` names the
   directive for diagnostics. */
static const char *pp_read_header_name(Pp *pp, Vec *tokens, size_t *pos, bool *quoted,
                                       const char *what)
{
    PpToken *t = vec_get(tokens, *pos);
    if (t->kind == TOK_PP_STRING)
    {
        *quoted = true;
        (*pos)++;
        return pp_string_content(pp, t);
    }
    if (pp_is_punct(t, PP_PUNCT_LT))
    {
        ByteBuf name;
        bytebuf_init(&name, pp->arena);
        size_t i;
        for (i = *pos + 1; i < vec_size(tokens); i++)
        {
            PpToken *u = vec_get(tokens, i);
            if (pp_is_punct(u, PP_PUNCT_GT))
            {
                break;
            }
            for (u32 k = 0; k < u->len; k++)
            {
                bytebuf_append(&name, (u8) u->spell[k]);
            }
        }
        if (i >= vec_size(tokens))
        {
            pp_error(pp, t->loc, "missing '>' in %s", what);
            return NULL;
        }
        bytebuf_append(&name, '\0');
        *pos = i + 1;
        return (const char *) bytebuf_data(&name);
    }
    return NULL;
}

typedef struct
{
    const char *name;
    bool quoted;
    size_t next;
} IncludeOperand;

/* Requires the expanded operand to be exactly one `"..."`/`<...>` name. */
static const char *pp_expanded_header_name(Pp *pp, Vec *expanded, bool *quoted, Loc loc)
{
    if (vec_size(expanded) == 0)
    {
        pp_error(pp, loc, "expected a header name after #include");
        return NULL;
    }
    PpToken *first = vec_get(expanded, 0);
    if (first->kind != TOK_PP_STRING && !pp_is_punct(first, PP_PUNCT_LT))
    {
        pp_error(pp, loc, "#include operand does not expand to a single header name");
        return NULL;
    }
    size_t pos = 0;
    const char *name = pp_read_header_name(pp, expanded, &pos, quoted, "#include");
    if (!name)
    {
        return NULL;
    }
    if (pos != vec_size(expanded))
    {
        pp_error(pp, loc, "#include operand does not expand to a single header name");
        return NULL;
    }
    return name;
}

/* Parses the header-name on the line: a literal form, or a macro that must
   expand to exactly one. Reports a diagnostic and returns false on error. */
static bool pp_include_operand(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc, IncludeOperand *out)
{
    size_t open = pp_skip_trivia(frame, start, end);
    if (open >= end)
    {
        pp_error(pp, directive_loc, "expected a header name after #include");
        return false;
    }
    PpToken *first = vec_get(frame->tokens, open);
    if (pp_is_punct(first, PP_PUNCT_LT))
    {
        out->quoted = false;
        out->name = pp_angle_header_name(pp, frame, open, end, &out->next);
        return out->name != NULL;
    }
    if (first->kind == TOK_PP_STRING)
    {
        out->quoted = true;
        out->name = pp_string_content(pp, first);
        out->next = open + 1;
        return true;
    }

    out->quoted = false;
    out->name = pp_expanded_header_name(pp, pp_expand_operand(pp, frame, start, end), &out->quoted,
                                        first->loc);
    out->next = end;
    return out->name != NULL;
}

/* Resolves and processes one include; `next_mode` (`#include_next`) searches
   the search path after the current directory. */
static void pp_include_common(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                              Loc directive_loc, bool next_mode)
{
    IncludeOperand op;
    if (!pp_include_operand(pp, frame, start, end, directive_loc, &op))
    {
        return;
    }
    size_t i = pp_skip_trivia(frame, op.next, end);
    if (i < end)
    {
        pp_warn(pp, ((PpToken *) vec_get(frame->tokens, i))->loc,
                "extra tokens at end of #include directive");
    }

    IncludeHit hit = pp_include_find(pp, op.quoted, op.name, frame->file, next_mode);
    if (!hit.path)
    {
        pp_error(pp, directive_loc,
                 next_mode ? "no such #include_next file: '%s'" : "include file not found: '%s'",
                 op.name);
        return;
    }
    if (hashset_contains(pp->pragma_once, hit.path))
    {
        return;
    }
    pp_include_file(pp, hit.path, hit.system);
}

static void pp_include_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                 Loc directive_loc)
{
    pp_include_common(pp, frame, start, end, directive_loc, false);
}

static void pp_include_next_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                      Loc directive_loc)
{
    pp_include_common(pp, frame, start, end, directive_loc, true);
}

static void pp_gnu_warn(Pp *pp, Loc loc, const char *feature)
{
    if (pp->cfg.pedantic)
    {
        pp_warn(pp, loc, "'%s' is a GNU extension", feature);
    }
}

/* Handles `#pragma once`, `#pragma GCC poison`, `#pragma GCC system_header`;
   anything else is parsed and ignored. */
static void pp_pragma_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                Loc directive_loc)
{
    Vec *tokens = pp_gather_real(pp, frame, start, end);
    size_t n = vec_size(tokens);
    if (n == 0)
    {
        return;
    }
    PpToken *t0 = vec_get(tokens, 0);

    if (n == 1 && pp_is_ident(t0, "once"))
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

    if (n >= 2 && pp_is_ident(t0, "GCC"))
    {
        PpToken *t1 = vec_get(tokens, 1);
        if (pp_is_ident(t1, "system_header"))
        {
            pp_gnu_warn(pp, t0->loc, "#pragma GCC system_header");
            frame->system_header = true;
            return;
        }
        if (pp_is_ident(t1, "poison"))
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
                hashset_add(pp->poison, (void *) pp_token_text(pp, p));
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

static PpToken *pp_cooked_bool(Pp *pp, bool value, Loc loc)
{
    PpToken *t = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *t = (PpToken) {.kind = TOK_PP_NUMBER, .loc = loc, .spell = value ? "1" : "0", .len = 1};
    return t;
}

/* Cooks `defined X` and `defined(X)` into 0/1 literals before expansion. */
static void pp_resolve_defined(Pp *pp, Vec *raw, size_t *i, Vec *out, Loc loc)
{
    bool paren = *i < vec_size(raw) && pp_is_punct(vec_get(raw, *i), PP_PUNCT_LPAREN);
    if (paren)
    {
        (*i)++;
    }

    if (*i >= vec_size(raw) || ((PpToken *) vec_get(raw, *i))->kind != TOK_PP_IDENT)
    {
        pp_error(pp, loc, "operator 'defined' requires an identifier");
        vec_push(out, pp_cooked_bool(pp, false, loc));
        return;
    }
    PpToken *name = vec_get(raw, *i);
    (*i)++;

    if (paren)
    {
        if (*i < vec_size(raw) && pp_is_punct(vec_get(raw, *i), PP_PUNCT_RPAREN))
        {
            (*i)++;
        }
        else
        {
            pp_error(pp, name->loc, "expected ')' after 'defined'");
        }
    }

    bool defined = strmap_get(pp->macros, pp_token_text(pp, name)) != NULL ||
                   pp_is_ident(name, "__has_include") || pp_is_ident(name, "__has_include_next");
    vec_push(out, pp_cooked_bool(pp, defined, loc));
}

/* Cooks a `__has_include` operand: a macro-expanded header name whose
   existence is reported back to the #if parser. */
static bool pp_header_found(Pp *pp, Vec *operand, const char *including_file, bool next, Loc loc)
{
    Vec *expanded = pp_expand_vec(pp, operand);
    if (vec_size(expanded) == 0)
    {
        pp_error(pp, loc, "__has_include requires a single header-name operand");
        return false;
    }
    PpToken *e0 = vec_get(expanded, 0);
    if (e0->kind != TOK_PP_STRING && !pp_is_punct(e0, PP_PUNCT_LT))
    {
        pp_error(pp, loc, "__has_include requires a single header-name operand");
        return false;
    }
    size_t pos = 0;
    bool quoted = false;
    const char *name = pp_read_header_name(pp, expanded, &pos, &quoted, "__has_include");
    if (!name)
    {
        return false;
    }
    if (pos != vec_size(expanded))
    {
        pp_error(pp, loc, "__has_include requires a single header-name operand");
        return false;
    }
    return pp_include_find(pp, quoted, name, including_file, next).path != NULL;
}

/* Cooks `__has_include(...)` / `__has_include_next(...)` into 0/1. */
static void pp_resolve_has_include(Pp *pp, Vec *raw, size_t *i, Vec *out, Loc loc, bool next,
                                   const char *including_file)
{
    if (*i >= vec_size(raw) || !pp_is_punct(vec_get(raw, *i), PP_PUNCT_LPAREN))
    {
        pp_error(pp, loc, "expected '(' after __has_include");
        vec_push(out, pp_cooked_bool(pp, false, loc));
        return;
    }
    (*i)++;

    Vec *operand = vec_new(pp->arena);
    for (; *i < vec_size(raw) && !pp_is_punct(vec_get(raw, *i), PP_PUNCT_RPAREN); (*i)++)
    {
        vec_push(operand, vec_get(raw, *i));
    }
    if (*i >= vec_size(raw))
    {
        pp_error(pp, loc, "missing ')' after __has_include");
        vec_push(out, pp_cooked_bool(pp, false, loc));
        return;
    }
    (*i)++;

    bool found = pp_header_found(pp, operand, including_file, next, loc);
    vec_push(out, pp_cooked_bool(pp, found, loc));
}

/* C11 §6.10.1: the operators below are cooked before #if macro expansion. */
static Vec *pp_resolve_controls(Pp *pp, Vec *raw, const PpIncludeFrame *frame)
{
    Vec *out = vec_new(pp->arena);
    size_t i = 0;
    while (i < vec_size(raw))
    {
        PpToken *t = vec_get(raw, i);
        if (pp_is_ident(t, "defined"))
        {
            i++;
            pp_resolve_defined(pp, raw, &i, out, t->loc);
        }
        else if (pp_is_ident(t, "__has_include") || pp_is_ident(t, "__has_include_next"))
        {
            bool next = pp_is_ident(t, "__has_include_next");
            i++;
            pp_resolve_has_include(pp, raw, &i, out, t->loc, next, frame ? frame->file : NULL);
        }
        else
        {
            vec_push(out, t);
            i++;
        }
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

/* C11 §6.10.8: predefined macros materialize at the point of use. */
static PpToken *pp_cook_predefined(Pp *pp, PpToken *t, u32 line, const char *file)
{
    if (t->kind != TOK_PP_IDENT)
    {
        return t;
    }
    if (pp_is_ident(t, "__LINE__"))
    {
        return pp_cooked_number_token(pp, line, t->loc);
    }
    if (pp_is_ident(t, "__FILE__"))
    {
        return pp_cooked_string_token(pp, file ? file : "", t->loc);
    }
    if (pp_is_ident(t, "__DATE__") || pp_is_ident(t, "__TIME__"))
    {
        pp_cook_datetime(pp);
        return pp_cooked_string_token(
            pp, pp_is_ident(t, "__DATE__") ? pp->cooked_date : pp->cooked_time, t->loc);
    }
    return t;
}

static bool pp_if_condition(Pp *pp, const PpIncludeFrame *frame, size_t start, size_t end)
{
    Vec *raw = pp_gather_real(pp, frame, start, end);
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
            bytebuf_append(&buf, (u8) t->spell[j]);
        }
    }
    bytebuf_append(&buf, '\0');
    return (const char *) bytebuf_data(&buf);
}

/* Sets presumed_line from the operand's leading decimal and returns the
   expanded operand for further inspection, or NULL after an error. */
static Vec *pp_apply_presumed_line(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                   Loc directive_loc)
{
    Vec *expanded = pp_expand_operand(pp, frame, start, end);
    if (vec_size(expanded) == 0)
    {
        pp_error(pp, directive_loc, "#line requires a line number");
        return NULL;
    }
    PpToken *num = vec_get(expanded, 0);
    if (!pp_is_decimal_number(num))
    {
        pp_error(pp, num->loc, "invalid line number in #line directive");
        return NULL;
    }
    u32 line = 0;
    for (u32 i = 0; i < num->len; i++)
    {
        line = line * 10 + (u32) (num->spell[i] - '0');
    }
    frame->presumed_line = line;
    return expanded;
}

static void pp_line_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                              Loc directive_loc)
{
    Vec *expanded = pp_apply_presumed_line(pp, frame, start, end, directive_loc);
    if (!expanded)
    {
        return;
    }
    size_t i = 1;
    if (i < vec_size(expanded) && ((PpToken *) vec_get(expanded, i))->kind == TOK_PP_STRING)
    {
        frame->presumed_file = pp_string_content(pp, vec_get(expanded, i));
        i++;
    }
    else if (i < vec_size(expanded))
    {
        pp_error(pp, ((PpToken *) vec_get(expanded, i))->loc,
                 "expected a string literal after the line number in #line");
        return;
    }
    if (i < vec_size(expanded))
    {
        pp_warn(pp, ((PpToken *) vec_get(expanded, i))->loc,
                "extra tokens at end of #line directive");
    }
}

/* The GNU `# N "file"` linemarker: lenient about trailing tokens. */
static void pp_linemarker_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                    Loc directive_loc)
{
    Vec *expanded = pp_apply_presumed_line(pp, frame, start, end, directive_loc);
    if (!expanded)
    {
        return;
    }
    if (vec_size(expanded) > 1 && ((PpToken *) vec_get(expanded, 1))->kind == TOK_PP_STRING)
    {
        frame->presumed_file = pp_string_content(pp, vec_get(expanded, 1));
    }
}

static const char *pp_directive_message(Pp *pp, const PpIncludeFrame *frame, size_t start,
                                        size_t end, const char *prefix)
{
    Vec *expanded = pp_expand_operand(pp, frame, start, end);
    const char *text = pp_render_text(pp, expanded);
    size_t prefix_len = strlen(prefix);
    char *msg = arena_alloc(pp->arena, prefix_len + strlen(text) + 1, 1);
    memcpy(msg, prefix, prefix_len);
    strcpy(msg + prefix_len, text);
    return msg;
}

static void pp_error_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                               Loc directive_loc)
{
    pp_error(pp, directive_loc, "%s", pp_directive_message(pp, frame, start, end, "#error "));
}

static void pp_warning_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                                 Loc directive_loc)
{
    pp_warn(pp, directive_loc, "%s", pp_directive_message(pp, frame, start, end, "#warning "));
}

static void pp_if_directive(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end,
                            Loc directive_loc)
{
    (void) directive_loc;
    bool parent_active = pp_branch_active(pp);
    bool cond = parent_active ? pp_if_condition(pp, frame, start, end) : false;
    pp_push_cond(pp, parent_active, cond);
}

static void pp_elif_directive(Pp *pp, PpIncludeFrame *frame, size_t i, size_t end,
                              Loc directive_loc)
{
    if (vec_size(pp->conds) == 0)
    {
        pp_error(pp, directive_loc, "unexpected #elif");
        return;
    }
    CondFrame *top = vec_last(pp->conds);
    if (top->in_else)
    {
        pp_error(pp, directive_loc, "unexpected #elif after #else");
        return;
    }
    if (!top->parent_active || top->ever_taken)
    {
        top->taken = false;
        return;
    }
    top->taken = pp_if_condition(pp, frame, i, end);
    top->ever_taken = top->taken;
}

/* X-macro: the `#name` directives (C11 §6.10). Columns: kind, spelling,
   handler, and whether it enables conditional inclusion — a conditional must
   still run inside skipped regions so its nesting stays balanced. Append
   only. */
#define DIRECTIVE_KINDS(X)                                                                         \
    X(DIRECTIVE_IFDEF, "ifdef", pp_ifdef_directive, true)                                          \
    X(DIRECTIVE_IFNDEF, "ifndef", pp_ifndef_directive, true)                                       \
    X(DIRECTIVE_IF, "if", pp_if_directive, true)                                                   \
    X(DIRECTIVE_ELIF, "elif", pp_elif_directive, true)                                             \
    X(DIRECTIVE_ELSE, "else", pp_else_directive, true)                                             \
    X(DIRECTIVE_ENDIF, "endif", pp_endif_directive, true)                                          \
    X(DIRECTIVE_INCLUDE, "include", pp_include_directive, false)                                   \
    X(DIRECTIVE_INCLUDE_NEXT, "include_next", pp_include_next_directive, false)                    \
    X(DIRECTIVE_PRAGMA, "pragma", pp_pragma_directive, false)                                      \
    X(DIRECTIVE_DEFINE, "define", pp_define, false)                                                \
    X(DIRECTIVE_UNDEF, "undef", pp_undef, false)                                                   \
    X(DIRECTIVE_LINE, "line", pp_line_directive, false)                                            \
    X(DIRECTIVE_ERROR, "error", pp_error_directive, false)                                         \
    X(DIRECTIVE_WARNING, "warning", pp_warning_directive, false)

typedef void (*DirectiveFn)(Pp *pp, PpIncludeFrame *frame, size_t start, size_t end, Loc loc);

typedef enum
{
#define DIRECTIVE_ENUM_ENTRY(KIND, NAME, RUN, COND) KIND,
    DIRECTIVE_KINDS(DIRECTIVE_ENUM_ENTRY)
#undef DIRECTIVE_ENUM_ENTRY
} DirectiveKind;

typedef struct
{
    DirectiveKind kind;
    const char *name;
    DirectiveFn run;
    bool is_conditional;
} Directive;

static const Directive DIRECTIVES[] = {
#define DIRECTIVE_TABLE_ENTRY(KIND, NAME, RUN, COND) {KIND, NAME, RUN, COND},
    DIRECTIVE_KINDS(DIRECTIVE_TABLE_ENTRY)
#undef DIRECTIVE_TABLE_ENTRY
};

static const Directive *pp_directive_lookup(const PpToken *name)
{
    for (size_t i = 0; i < sizeof(DIRECTIVES) / sizeof(DIRECTIVES[0]); i++)
    {
        if (pp_is_ident(name, DIRECTIVES[i].name))
        {
            return &DIRECTIVES[i];
        }
    }
    return NULL;
}

/* Dispatches one directive line. Returns true when it adjusted the presumed
   line. */
static bool pp_directive(Pp *pp, PpIncludeFrame *frame, size_t begin, size_t end)
{
    size_t i = pp_skip_trivia(frame, begin, end);
    Loc directive_loc = ((PpToken *) vec_get(frame->tokens, i))->loc;
    i++;

    size_t name_index = pp_skip_trivia(frame, i, end);
    if (name_index >= end)
    {
        return false; /* null directive */
    }
    PpToken *name = vec_get(frame->tokens, name_index);
    if (name->kind == TOK_PP_NUMBER)
    {
        pp_linemarker_directive(pp, frame, name_index, end, directive_loc);
        return true;
    }

    const Directive *dir = pp_directive_lookup(name);
    if (!dir)
    {
        if (!pp_is_skipping(pp))
        {
            pp_warn(pp, name->loc, "invalid preprocessing directive #%.*s", (int) name->len,
                    name->spell);
        }
        return false;
    }
    if (!pp_is_skipping(pp) || dir->is_conditional)
    {
        dir->run(pp, frame, name_index + 1, end, directive_loc);
    }
    /* Only `#line` establishes the presumed line itself; the caller must not
       also count this line's newlines. */
    return dir->kind == DIRECTIVE_LINE;
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

static bool pp_collect_args(Pp *pp, PpToken *name, Macro *macro, TokList *lparen, MacroArgs *out)
{
    Vec *args = vec_new(pp->arena);
    Vec *current = vec_new(pp->arena);
    Hideset *hs = name->hide;
    int depth = 1;

    for (TokList *p = lparen->next; p; p = p->next)
    {
        PpToken *tok = p->tok;
        if (pp_is_punct(tok, PP_PUNCT_LPAREN))
        {
            depth++;
        }
        else if (pp_is_punct(tok, PP_PUNCT_RPAREN))
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
        else if (depth == 1 && pp_is_punct(tok, PP_PUNCT_COMMA))
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

/* Stringizes a raw argument (C11 §6.10.3.2): outer whitespace is stripped,
   interior runs collapse to one space, and `"`/`\` are quoted. */
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
            bytebuf_append(&buf, (u8) c);
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

/* Pasting must re-lex into exactly one pp-token (C11 §6.10.3.3). */
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

/* Argument substitution strips outer whitespace but keeps interior trivia, so
   a rescanned outer stringize still sees single spaces (`XSTR(a b)`). */
static void pp_push_arg(Pp *pp, Vec *out, Vec *arg, const SubstArgs *sa)
{
    size_t start = 0;
    while (start < vec_size(arg) && pp_is_trivia(vec_get(arg, start)))
    {
        start++;
    }
    size_t end = vec_size(arg);
    while (end > start && pp_is_trivia(vec_get(arg, end - 1)))
    {
        end--;
    }
    for (size_t i = start; i < end; i++)
    {
        vec_push(out, pp_hidden_copy(pp, vec_get(arg, i), sa));
    }
}

static PpToken *pp_comma_token(Pp *pp, Loc loc)
{
    PpToken *t = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *t = (PpToken) {
        .kind = TOK_PP_PUNCT, .loc = loc, .spell = ",", .len = 1, .punct = PP_PUNCT_COMMA};
    return t;
}

/* Joins the variadic tail into one comma-separated argument; missing named
   args become empty. */
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

/* Substitutes parameters: `#` stringizes, a parameter beside `##` takes its
   raw argument, everything else the prescanned one. */
static Vec *pp_substitute(Pp *pp, Macro *macro, const SubstArgs *sa)
{
    Vec *out = vec_new(pp->arena);
    size_t n = vec_size(macro->body);

    for (size_t i = 0; i < n; i++)
    {
        PpToken *bt = vec_get(macro->body, i);

        /* GNU `, ## __VA_ARGS__`: drop the comma when the tail is absent. */
        if (pp_is_punct(bt, PP_PUNCT_COMMA) && sa->raw && i + 2 < n &&
            pp_is_punct(vec_get(macro->body, i + 1), PP_PUNCT_HASHHASH))
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

        if (pp_is_punct(bt, PP_PUNCT_HASH) && sa->raw && i + 1 < n &&
            ((PpToken *) vec_get(macro->body, i + 1))->kind == TOK_PP_PARAM)
        {
            PpToken *next = vec_get(macro->body, i + 1);
            vec_push(out, pp_stringize(pp, vec_get(sa->raw, next->param_idx), sa->loc));
            i++;
            continue;
        }
        if (bt->kind == TOK_PP_PARAM && sa->expanded && bt->param_idx < vec_size(sa->expanded))
        {
            bool under_paste =
                (i > 0 && pp_is_punct(vec_get(macro->body, i - 1), PP_PUNCT_HASHHASH)) ||
                (i + 1 < n && pp_is_punct(vec_get(macro->body, i + 1), PP_PUNCT_HASHHASH));
            pp_push_arg(pp, out, vec_get(under_paste ? sa->raw : sa->expanded, bt->param_idx), sa);
            continue;
        }

        vec_push(out, pp_hidden_copy(pp, bt, sa));
    }
    return out;
}

/* Merges `##` pairs with placemarker semantics: a missing side disappears,
   and whitespace around the paste is discarded (C11 §6.10.3.3). */
static TokList *pp_paste_list(Pp *pp, Vec *tokens, Loc inv_loc)
{
    Vec *out = vec_new(pp->arena);
    for (size_t i = 0; i < vec_size(tokens); i++)
    {
        PpToken *t = vec_get(tokens, i);
        if (!pp_is_punct(t, PP_PUNCT_HASHHASH))
        {
            vec_push(out, t);
            continue;
        }
        size_t ri = i + 1;
        while (ri < vec_size(tokens) && (pp_is_trivia(vec_get(tokens, ri)) ||
                                         pp_is_punct(vec_get(tokens, ri), PP_PUNCT_HASHHASH)))
        {
            ri++;
        }
        if (ri < vec_size(tokens))
        {
            PpToken *right = vec_get(tokens, ri);
            while (vec_size(out) > 0 && pp_is_trivia(vec_last(out)))
            {
                vec_pop(out);
            }
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
            i = ri;
        }
        /* a trailing `##` with an empty right operand disappears */
    }

    TokList *head = NULL;
    TokList **tail = &head;
    for (size_t i = 0; i < vec_size(out); i++)
    {
        tail = pp_list_append(pp, tail, vec_get(out, i));
    }
    return head;
}

static TokList *pp_subst(Pp *pp, Macro *macro, const SubstArgs *sa)
{
    return pp_paste_list(pp, pp_substitute(pp, macro, sa), sa->loc);
}

/* Deletes the string prefix and quotes, unescaping only `\"` — the rule gcc
   and clang actually implement for `_Pragma` (C11 §6.10.9 decodes all). */
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

static Vec *pp_synthesize_pragma_line(Pp *pp, const char *text, Loc loc)
{
    Vec *lexed = pp_lex(loc.file, text, pp->arena);
    if (!lexed)
    {
        return NULL;
    }

    Vec *line = vec_new(pp->arena);
    PpToken *hash = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *hash = (PpToken) {
        .kind = TOK_PP_PUNCT, .punct = PP_PUNCT_HASH, .spell = "#", .len = 1, .loc = loc};
    vec_push(line, hash);
    PpToken *name = arena_alloc(pp->arena, sizeof(PpToken), sizeof(void *));
    *name = (PpToken) {.kind = TOK_PP_IDENT, .spell = "pragma", .len = 6, .loc = loc};
    vec_push(line, name);
    for (size_t i = 0; i < vec_size(lexed); i++)
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
    return line;
}

/* Feeds the destringized _Pragma operand back through the directive
   dispatcher as a synthetic `#pragma` line. */
static void pp_execute_pragma(Pp *pp, const char *text, Loc loc)
{
    if (vec_size(pp->includes) == 0)
    {
        return;
    }
    PpIncludeFrame *cur = vec_last(pp->includes);

    Vec *line = pp_synthesize_pragma_line(pp, text, loc);
    if (!line)
    {
        return;
    }

    PpIncludeFrame syn = {0};
    syn.tokens = line;
    syn.file = cur->file;
    pp_directive(pp, &syn, 0, vec_size(line) - 1);
    if (syn.system_header)
    {
        cur->system_header = true;
    }
}

/* Returns the single string literal the operand expands to, or NULL after a
   diagnostic. */
static PpToken *pp_pragma_string_arg(Pp *pp, TokList *arg, Loc loc)
{
    if (!arg)
    {
        pp_error(pp, loc, "_Pragma operand must be a string literal");
        return NULL;
    }
    PpToken *str = NULL;
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
        pp_error(pp, loc, "_Pragma operand must be a single string literal");
        return NULL;
    }
    return str;
}

/* C11 §6.10.9: the operand stringifies like a call argument, then executes
   as a #pragma line. `_Pragma` itself emits no tokens. */
static TokList *pp_pragma_op(Pp *pp, TokList *ts)
{
    PpToken *prag = ts->tok;

    TokList *lparen = pp_skip_trivia_list(ts->next);
    if (!lparen || !pp_is_punct(lparen->tok, PP_PUNCT_LPAREN))
    {
        pp_error(pp, prag->loc, "_Pragma expects '(' after it");
        return ts->next;
    }

    TokList *arg = NULL;
    TokList **atail = &arg;
    TokList *it = lparen->next;
    for (; it && !pp_is_punct(it->tok, PP_PUNCT_RPAREN); it = it->next)
    {
        atail = pp_list_append(pp, atail, it->tok);
    }
    if (!it)
    {
        pp_error(pp, prag->loc, "unterminated _Pragma operand");
        return lparen->next;
    }

    PpToken *str = pp_pragma_string_arg(pp, arg, prag->loc);
    if (str)
    {
        const char *text = pp_destringize(pp, str);
        if (text)
        {
            pp_execute_pragma(pp, text, prag->loc);
        }
    }
    return it->next;
}

static Vec *pp_prescan_args(Pp *pp, Vec *args);

/* Reports use of a poisoned identifier that is not being expanded. */
static void pp_check_poison(Pp *pp, const PpToken *t)
{
    if (t->kind == TOK_PP_IDENT && !t->hide && hashset_contains(pp->poison, pp_token_text(pp, t)))
    {
        pp_error(pp, t->loc, "attempt to use poisoned '%.*s'", (int) t->len, t->spell);
    }
}

/* Expands one function-like invocation, returning its replacement spliced
   with the tokens after the call, or NULL to keep the token unchanged. */
static TokList *pp_expand_call(Pp *pp, PpToken *name, Macro *macro, TokList *lparen)
{
    MacroArgs args;
    if (!pp_collect_args(pp, name, macro, lparen, &args))
    {
        return NULL;
    }

    size_t arg_count = vec_size(args.args);
    if (arg_count == 1 && macro->param_count == 0 && pp_arg_is_empty(vec_get(args.args, 0)))
    {
        arg_count = 0; /* `F()` passes one empty argument; treat it as none. */
    }
    bool arity_ok =
        macro->variadic ? arg_count >= macro->param_count : arg_count == macro->param_count;
    if (!arity_ok)
    {
        pp_error(pp, name->loc, "macro '%s' expects %u arguments, but %zu given", macro->name,
                 macro->param_count, arg_count);
        return NULL;
    }

    SubstArgs sa = {.hs = hs_add(pp->arena, args.hs, macro),
                    .loc = name->loc,
                    .variadic_absent = macro->variadic && arg_count <= macro->param_count};
    Vec *expanded = pp_prescan_args(pp, args.args);
    if (macro->variadic)
    {
        sa.raw = pp_effective_args(pp, args.args, macro->param_count, name->loc);
        sa.expanded = pp_effective_args(pp, expanded, macro->param_count, name->loc);
    }
    else
    {
        sa.raw = args.args;
        sa.expanded = expanded;
    }
    return list_concat(pp->arena, pp_subst(pp, macro, &sa), args.after);
}

/* Rescans a token list, splicing in replacements; a macro is disabled while
   its own replacement is rescanned (C11 §6.10.3.4). */
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
            pp_check_poison(pp, t);
            tail = pp_list_append(pp, tail, t);
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
        if (lparen && pp_is_punct(lparen->tok, PP_PUNCT_LPAREN))
        {
            TokList *replacement = pp_expand_call(pp, t, macro, lparen);
            if (replacement)
            {
                ts = replacement;
                continue;
            }
        }
        tail = pp_list_append(pp, tail, t);
        ts = ts->next;
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
            tail = pp_list_append(pp, tail, vec_get(raw, j));
        }

        Vec *arg = vec_new(pp->arena);
        for (TokList *r = pp_expand_list(pp, list); r; r = r->next)
        {
            /* Materialize `__LINE__`/`__FILE__` in the argument before it is
               substituted or stringized (C11 §6.10.3.1). */
            vec_push(arg, pp_cook_predefined(pp, r->tok, r->tok->loc.line, r->tok->loc.file));
        }
        vec_push(expanded, arg);
    }
    return expanded;
}

/* Expands one logical line, stamping presumed locations and materializing
   predefined macros; newline tokens bump the presumed line. */
static void pp_expand_line(Pp *pp, PpIncludeFrame *frame, size_t begin, size_t end)
{
    TokList *list = NULL;
    TokList **tail = &list;
    for (size_t i = begin; i < end; i++)
    {
        tail = pp_list_append(pp, tail, vec_get(frame->tokens, i));
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

/* Processes one directive line, or gathers consecutive non-directive lines
   (so macro invocations may span lines) before expanding them. */
static void pp_line(Pp *pp, PpIncludeFrame *frame)
{
    size_t count = vec_size(frame->tokens);
    size_t begin = frame->cursor;
    size_t end = pp_line_end(frame, begin);
    const PpToken *first = pp_first_real(frame, begin, end);

    if (first && pp_is_punct(first, PP_PUNCT_HASH))
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
        if (next_first && pp_is_punct(next_first, PP_PUNCT_HASH))
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

/* `-D name[=value]` (gcc): object-like, value defaults to `1`; `name=` gives
   an empty body. */
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

/* `-include file`: resolves like an angle include and processes it now, so
   its macros and output precede the main file. */
void pp_include_cmdline(Pp *pp, const char *file)
{
    Loc loc = (Loc) {.file = file, .line = 1, .col = 1};
    IncludeHit hit = pp_include_find(pp, false, file, NULL, false);
    const char *path = pp_file_exists(file) ? file : hit.path;
    if (!path)
    {
        pp_error(pp, loc, "include file not found: '%s'", file);
        return;
    }
    pp_include_file(pp, path, false);
    pp_run(pp);
}

Vec *pp_preprocess(Pp *pp, const char *file, const char *src)
{
    Vec *tokens = pp_lex(file, src, pp->arena);
    if (!tokens)
    {
        return NULL;
    }
    pp_push_include(pp, file, tokens, false);
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
