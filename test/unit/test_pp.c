#include "harness.h"
#include "pp.h"
#include "util/arena.h"

static Pp *pp_run_text(Arena *a, const char *src)
{
    Pp *pp = pp_new(a);
    pp_preprocess(pp, "<test>", src);
    return pp;
}

static PpToken *out_tok(Pp *pp, size_t i)
{
    return vec_get(pp->out, i);
}

static bool spell_is(const PpToken *t, const char *spelling)
{
    return t->len == strlen(spelling) && strncmp(t->spell, spelling, t->len) == 0;
}

static void expect_out(Pp *pp, size_t i, PpKind kind, const char *spelling)
{
    PpToken *t = out_tok(pp, i);
    EXPECT_EQ(t->kind, kind);
    EXPECT_TRUE(spell_is(t, spelling));
}

TEST(pp, ordinary_line_passthrough)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "a b\nc\n");
    EXPECT_EQ(vec_size(pp->out), 6);
    expect_out(pp, 0, TOK_PP_IDENT, "a");
    expect_out(pp, 1, TOK_PP_TRIVIA_WS, " ");
    expect_out(pp, 2, TOK_PP_IDENT, "b");
    expect_out(pp, 3, TOK_PP_TRIVIA_NL, "\n");
    expect_out(pp, 4, TOK_PP_IDENT, "c");
    expect_out(pp, 5, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, hash_mid_line_is_token)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "a # b\n");
    EXPECT_EQ(vec_size(pp->out), 6);
    expect_out(pp, 0, TOK_PP_IDENT, "a");
    expect_out(pp, 1, TOK_PP_TRIVIA_WS, " ");
    expect_out(pp, 2, TOK_PP_PUNCT, "#");
    expect_out(pp, 3, TOK_PP_TRIVIA_WS, " ");
    expect_out(pp, 4, TOK_PP_IDENT, "b");
    expect_out(pp, 5, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, directive_at_line_start_after_whitespace)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "  #foo bar\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, directive_after_multiline_comment)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "/*\n*/ #foo bar\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, mid_line_multiline_comment_does_not_start_directive)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "x /*\n*/ #foo\n");
    EXPECT_EQ(vec_size(pp->out), 7);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 1, TOK_PP_TRIVIA_WS, " ");
    EXPECT_EQ(out_tok(pp, 2)->kind, TOK_PP_TRIVIA_COMMENT);
    EXPECT_TRUE(out_tok(pp, 2)->has_newline);
    expect_out(pp, 3, TOK_PP_TRIVIA_WS, " ");
    expect_out(pp, 4, TOK_PP_PUNCT, "#");
    expect_out(pp, 5, TOK_PP_IDENT, "foo");
    expect_out(pp, 6, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, null_directive_is_no_op)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#\nint y;\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    expect_out(pp, 1, TOK_PP_TRIVIA_WS, " ");
    expect_out(pp, 2, TOK_PP_IDENT, "y");
    expect_out(pp, 3, TOK_PP_PUNCT, ";");
    expect_out(pp, 4, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, directive_with_only_trivia_is_null)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "# /* c */ \nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, unknown_directive_warns_and_is_null)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#bogus\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, stray_hash_warns_and_is_null)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#+\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, digraph_hash_introduces_directive)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "%:foo\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, empty_input_yields_empty_soup)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "");
    EXPECT_EQ(vec_size(pp->out), 0);
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, lex_error_propagates)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "abc\\"));
    arena_free(a);
}

TEST(pp, object_macro_substitution)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 42\nX\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "42");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, nested_object_macros)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define A B\n#define B 1\nA\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, macro_body_keeps_all_tokens)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define ADD 1 + 2\nADD\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, "+");
    expect_out(pp, 2, TOK_PP_NUMBER, "2");
    expect_out(pp, 3, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, empty_macro_expands_to_nothing)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define E\nx E y\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 3, TOK_PP_IDENT, "y");
    arena_free(a);
}

TEST(pp, recursive_macro_terminates)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define A A A\nA\n");
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_IDENT, "A");
    expect_out(pp, 1, TOK_PP_IDENT, "A");
    expect_out(pp, 2, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, self_referential_macro_yields_one)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X X\nX\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "X");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, indirect_recursion_is_hidden)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define A B\n#define B A\nA\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "A");
    arena_free(a);
}

TEST(pp, redefinition_identical_is_ok)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1 + 2\n#define X 1+2\nX\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, redefinition_mismatch_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define X 1\n#define X 2\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, undef_restores_identifier)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\n#undef X\nX\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "X");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, undef_undefined_is_noop)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#undef Y\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, define_missing_name_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, define_non_identifier_name_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define 1 x\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, undef_missing_name_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#undef\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_like_macro_is_inert)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(x) x\nF\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "F");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, object_vs_function_like_mismatch_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F 1\n#define F(x) x\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_like_then_object_mismatch_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(x) x\n#define F 1\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, object_like_paren_body_has_space)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F (x)\nF\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_PUNCT, "(");
    expect_out(pp, 1, TOK_PP_IDENT, "x");
    expect_out(pp, 2, TOK_PP_PUNCT, ")");
    expect_out(pp, 3, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}
