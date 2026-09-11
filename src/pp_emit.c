#include "pp_emit.h"

#include <string.h>

static bool pp_emit_is_trivia(const PpToken *t)
{
    return t->kind == TOK_PP_TRIVIA_WS || t->kind == TOK_PP_TRIVIA_NL ||
           t->kind == TOK_PP_TRIVIA_COMMENT;
}

static bool pp_emit_spell_is(const PpToken *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && strncmp(t->spell, s, n) == 0;
}

static bool pp_emit_kind_word(PpKind kind)
{
    return kind == TOK_PP_IDENT || kind == TOK_PP_NUMBER;
}

/* True when printing prev then cur without a separator would re-lex to a
   different token sequence than the two printed tokens (D17.13). */
static bool pp_emit_needs_space(const PpToken *prev, const PpToken *cur)
{
    if (pp_emit_kind_word(prev->kind) && pp_emit_kind_word(cur->kind))
    {
        return true;
    }
    if (prev->kind == TOK_PP_NUMBER && cur->kind == TOK_PP_PUNCT)
    {
        if (cur->punct == PP_PUNCT_DOT)
        {
            return true; /* a `.` joins a pp-number (`1.`) */
        }
        if ((cur->punct == PP_PUNCT_PLUS || cur->punct == PP_PUNCT_MINUS) && prev->len > 0)
        {
            char last = prev->spell[prev->len - 1];
            if (last == 'e' || last == 'E' || last == 'p' || last == 'P')
            {
                return true; /* an exponent sign joins a pp-number (`1e+`) */
            }
        }
        return false;
    }
    if (prev->kind == TOK_PP_IDENT && (cur->kind == TOK_PP_STRING || cur->kind == TOK_PP_CHAR))
    {
        /* `L "x"` / `u "x"` / `U "x"` / `u8 "x"` would merge into one
           prefixed literal. */
        return pp_emit_spell_is(prev, "L") || pp_emit_spell_is(prev, "u") ||
               pp_emit_spell_is(prev, "U") || pp_emit_spell_is(prev, "u8");
    }
    if (prev->kind == TOK_PP_PUNCT && cur->kind == TOK_PP_PUNCT)
    {
        if (prev->punct == PP_PUNCT_SLASH &&
            (cur->punct == PP_PUNCT_SLASH || cur->punct == PP_PUNCT_STAR))
        {
            return true; /* would start a line or block comment */
        }
        return pp_concat_is_punct(prev->spell, prev->len, cur->spell, cur->len);
    }
    return false;
}

/* Whether a `# NN "file"` marker must precede this line: its first real token
   follows a different file or a non-contiguous line number. */
static bool pp_emit_marker_needed(const PpToken *next, const PpToken *prev)
{
    if (!prev)
    {
        return true;
    }
    if (next->loc.file != prev->loc.file)
    {
        return true;
    }
    return next->loc.line != (u32) (prev->loc.line + 1);
}

static void pp_emit_marker(FILE *f, const PpToken *t)
{
    fprintf(f, "# %u \"%s\"\n", t->loc.line, t->loc.file ? t->loc.file : "");
}

/* Emits a marker at column 0 when the upcoming line needs one. Safe only when
   the first real token of this line is reachable without crossing another
   newline (so the marker sits directly above its content); otherwise the real
   token's own branch emits it. */
static const PpToken *pp_emit_maybe_marker(const Vec *out, size_t i, const PpToken *prev,
                                           bool no_markers, FILE *f, bool *line_started)
{
    if (no_markers || !*line_started)
    {
        return NULL;
    }
    const PpToken *next = NULL;
    for (size_t j = i; j < vec_size(out); j++)
    {
        const PpToken *t = vec_get(out, j);
        if (t->kind == TOK_PP_TRIVIA_NL)
        {
            return NULL;
        }
        if (!pp_emit_is_trivia(t))
        {
            next = t;
            break;
        }
    }
    if (!next || !pp_emit_marker_needed(next, prev))
    {
        return NULL;
    }
    pp_emit_marker(f, next);
    *line_started = false;
    return next;
}

void pp_emit(Pp *pp, FILE *f, PpEmitOptions opts)
{
    const Vec *out = pp->out;
    size_t n = vec_size(out);
    bool at_line_start = true;
    bool separated = true;
    const PpToken *prev = NULL;

    for (size_t i = 0; i < n; i++)
    {
        const PpToken *t = vec_get(out, i);
        switch (t->kind)
        {
            case TOK_PP_TRIVIA_NL:
                fputc('\n', f);
                at_line_start = true;
                separated = true;
                continue;
            case TOK_PP_TRIVIA_WS:
                pp_emit_maybe_marker(out, i, prev, opts.no_markers, f, &at_line_start);
                fprintf(f, "%.*s", (int) t->len, t->spell);
                separated = true;
                continue;
            case TOK_PP_TRIVIA_COMMENT:
                pp_emit_maybe_marker(out, i, prev, opts.no_markers, f, &at_line_start);
                if (opts.keep_comments)
                {
                    fprintf(f, "%.*s", (int) t->len, t->spell);
                    separated = true;
                    at_line_start = t->len > 0 && t->spell[t->len - 1] == '\n';
                }
                continue;
            default:
                pp_emit_maybe_marker(out, i, prev, opts.no_markers, f, &at_line_start);
                if (!separated && prev && pp_emit_needs_space(prev, t))
                {
                    fputc(' ', f);
                }
                fprintf(f, "%.*s", (int) t->len, t->spell);
                separated = false;
                prev = t;
                at_line_start = false;
                continue;
        }
    }
}