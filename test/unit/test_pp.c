#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "harness.h"
#include "pp.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static Pp *pp_run_text(Arena *a, const char *src)
{
    Pp *pp = pp_new(a);
    pp_preprocess(pp, "<test>", src);
    return pp;
}

static unsigned int pp_inc_seq;

static const char *pp_test_name(Arena *a, const char *tag)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "ficcinc_%u_%s", pp_inc_seq++, tag);
    char *copy = arena_alloc(a, strlen(buf) + 1, 1);
    strcpy(copy, buf);
    return copy;
}

static void pp_test_write(const char *name, const char *content)
{
    char path[160];
    snprintf(path, sizeof(path), "/tmp/%s", name);
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

static const char *pp_test_path(Arena *a, const char *name)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "/tmp/%s", name);
    char *copy = arena_alloc(a, strlen(buf) + 1, 1);
    strcpy(copy, buf);
    return copy;
}

static void pp_test_write_at(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

static const char *pp_test_mkdir(Arena *a, const char *tag)
{
    char buf[120];
    snprintf(buf, sizeof(buf), "/tmp/ppnext_%u_%s", pp_inc_seq++, tag);
    mkdir(buf, 0700);
    char *copy = arena_alloc(a, strlen(buf) + 1, 1);
    strcpy(copy, buf);
    return copy;
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

static bool soup_has_token(Pp *pp, PpKind kind, const char *spelling)
{
    for (size_t i = 0; i < vec_size(pp->out); i++)
    {
        PpToken *t = out_tok(pp, i);
        if (t->kind == kind && spell_is(t, spelling))
        {
            return true;
        }
    }
    return false;
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

TEST(pp, function_macro_basic)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\nF(1)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, function_macro_two_params)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a,b) a b\nF(1,2)\n");
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, function_macro_not_invoked_is_ident)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\nF\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "F");
    arena_free(a);
}

TEST(pp, function_macro_prescans_argument)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define A B\n#define B(x) x\nA(1)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, function_macro_self_nested_argument)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\nF(F(2))\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, function_macro_blue_paint_argument)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\n#define G F\nF(G)(2)\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "F");
    expect_out(pp, 1, TOK_PP_PUNCT, "(");
    expect_out(pp, 2, TOK_PP_NUMBER, "2");
    expect_out(pp, 3, TOK_PP_PUNCT, ")");
    arena_free(a);
}

TEST(pp, function_macro_blue_paint_object_alias)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\n#define G F\nG(G)(1)\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "F");
    expect_out(pp, 1, TOK_PP_PUNCT, "(");
    expect_out(pp, 2, TOK_PP_NUMBER, "1");
    expect_out(pp, 3, TOK_PP_PUNCT, ")");
    expect_out(pp, 4, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, function_macro_argument_producing_macro_invokes)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define ID(x) x\n#define A B\n#define B(x) x\nID(A)(1)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, function_macro_hide_set_across_body)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\n#define G x G\nF(G)\n");
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 1, TOK_PP_IDENT, "G");
    arena_free(a);
}

TEST(pp, function_macro_comma_inside_parens)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a,b) a b\nF((1,2), 3)\n");
    EXPECT_EQ(vec_size(pp->out), 7);
    expect_out(pp, 0, TOK_PP_PUNCT, "(");
    expect_out(pp, 1, TOK_PP_NUMBER, "1");
    expect_out(pp, 2, TOK_PP_PUNCT, ",");
    expect_out(pp, 3, TOK_PP_NUMBER, "2");
    expect_out(pp, 4, TOK_PP_PUNCT, ")");
    expect_out(pp, 5, TOK_PP_NUMBER, "3");
    arena_free(a);
}

TEST(pp, function_macro_empty_argument)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\nF()\n");
    EXPECT_EQ(vec_size(pp->out), 1);
    expect_out(pp, 0, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, function_macro_zero_params)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F() 99\nF()\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "99");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, function_macro_multiline_arguments)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a) a\nF(\n42\n)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "42");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, function_macro_nested_reuse)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define f(a) a\n#define g(a,b) a b\ng(f(1),f(2))\n");
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, function_macro_arity_too_few_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a,b) a b\nF(1)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_arity_too_many_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a) a\nF(1,2)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_unterminated_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a) a\nF(1\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_duplicate_param_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a,a) a\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_ellipsis_not_last_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a, ..., b) a\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_missing_close_paren_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_variadic_parses)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a\nF(1,2,3)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, va_args_without_variadic_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a) __VA_ARGS__\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, function_macro_redefinition_param_rename_is_ok)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a,b) a b\n#define F(x,y) x y\nF(1,2)\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, stringize_basic)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\nSTR(abc)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_STRING, "\"abc\"");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, stringize_collapses_whitespace)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\nSTR(a  b)\n");
    expect_out(pp, 0, TOK_PP_STRING, "\"a b\"");
    arena_free(a);
}

TEST(pp, stringize_comment_becomes_space)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\nSTR(a/*c*/b)\n");
    expect_out(pp, 0, TOK_PP_STRING, "\"a b\"");
    arena_free(a);
}

TEST(pp, stringize_escapes_quotes_and_backslashes)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\nSTR(\"q\\z\")\n");
    PpToken *t = out_tok(pp, 0);
    EXPECT_EQ(t->kind, TOK_PP_STRING);
    EXPECT_EQ(t->len, 10);
    EXPECT_EQ(t->spell[1], '\\');
    EXPECT_EQ(t->spell[2], '"');
    EXPECT_EQ(t->spell[4], '\\');
    EXPECT_EQ(t->spell[5], '\\');
    EXPECT_EQ(t->spell[8], '"');
    arena_free(a);
}

TEST(pp, stringize_empty_argument)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\nSTR()\n");
    expect_out(pp, 0, TOK_PP_STRING, "\"\"");
    arena_free(a);
}

TEST(pp, stringize_double_level)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\n#define XSTR(x) STR(x)\nXSTR(BAR)\n");
    expect_out(pp, 0, TOK_PP_STRING, "\"BAR\"");
    arena_free(a);
}

TEST(pp, stringize_not_followed_by_param_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(x) a # b\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, paste_ident_ident)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define CAT(a,b) a##b\nCAT(foo,bar)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "foobar");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, paste_chain)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define C(a,b,c) a##b##c\nC(1,2,3)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "123");
    arena_free(a);
}

TEST(pp, paste_forms_new_macro_name)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define FOO1 42\n#define CAT(a,b) a##b\nCAT(FOO,1)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "42");
    arena_free(a);
}

TEST(pp, paste_multi_token_arg)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define CAT(a,b) a##b\nCAT(x+y,z)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    expect_out(pp, 1, TOK_PP_PUNCT, "+");
    expect_out(pp, 2, TOK_PP_IDENT, "yz");
    expect_out(pp, 3, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, paste_empty_right_placemarker)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define C(a,b) a##b\nC(q,)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "q");
    arena_free(a);
}

TEST(pp, paste_empty_left_placemarker)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define C(a,b) a##b\nC(,y)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "y");
    arena_free(a);
}

TEST(pp, paste_raw_operand_not_expanded)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define E\n#define CAT(a,b) a##b\nCAT(x,E)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "xE");
    arena_free(a);
}

TEST(pp, paste_suffix_building)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define MK(n) n##u\nMK(5)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "5u");
    arena_free(a);
}

TEST(pp, paste_invalid_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define CAT(a,b) a##b\nCAT(a,+)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, paste_at_start_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(x) ## x\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, paste_at_end_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(x) x ##\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, variadic_splices_tail)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(...) __VA_ARGS__\nF(1,2)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    expect_out(pp, 2, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, variadic_named_then_unused_tail)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a\nF(1,2,3)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, variadic_stringize_lines_up_commas)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define S(...) #__VA_ARGS__\nS(1,2)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_STRING, "\"1,2\"");
    arena_free(a);
}

TEST(pp, variadic_gnu_comma_drop)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a, ##__VA_ARGS__\nF(1)\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    arena_free(a);
}

TEST(pp, variadic_gnu_comma_kept_with_tail)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a, ##__VA_ARGS__\nF(1,2)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    expect_out(pp, 2, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, variadic_gnu_comma_kept_for_present_empty)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a, ##__VA_ARGS__\nF(1,)\n");
    EXPECT_EQ(vec_size(pp->out), 3);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    arena_free(a);
}

TEST(pp, variadic_standard_paste_uses_raw_tail)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a##__VA_ARGS__\nF(1,2)\nF(1)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "12");
    expect_out(pp, 1, TOK_PP_TRIVIA_NL, "\n");
    expect_out(pp, 2, TOK_PP_NUMBER, "1");
    expect_out(pp, 3, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, variadic_tail_is_expanded)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define M 42\n#define F(a, ...) a,__VA_ARGS__\nF(1,M)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    expect_out(pp, 2, TOK_PP_NUMBER, "42");
    arena_free(a);
}

TEST(pp, named_variadic_alias)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(args...) args\nF(1,2)\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    expect_out(pp, 2, TOK_PP_NUMBER, "2");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, va_args_in_object_like_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define X __VA_ARGS__\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, variadic_too_few_arguments_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a,b,...) x\nF(1)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, line_directive_sets_presumed_loc)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#line 100\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 100);
    EXPECT_EQ(out_tok(pp, 0)->loc.col, 1);
    arena_free(a);
}

TEST(pp, line_directive_with_filename)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#line 100 \"foo.c\"\nx\n");
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 100);
    EXPECT_STR_EQ(out_tok(pp, 0)->loc.file, "foo.c");
    arena_free(a);
}

TEST(pp, line_directive_macro_operand)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define N 50\n#line N\nx\n");
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 50);
    arena_free(a);
}

TEST(pp, line_directive_advances_per_newline)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#line 10\nx\ny\n");
    EXPECT_EQ(vec_size(pp->out), 4);
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 10);
    EXPECT_EQ(out_tok(pp, 2)->loc.line, 11);
    arena_free(a);
}

TEST(pp, linemarker_accepted)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "# 300 \"bar.c\" 2\nx\n");
    EXPECT_EQ(vec_size(pp->out), 2);
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 300);
    EXPECT_STR_EQ(out_tok(pp, 0)->loc.file, "bar.c");
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);
}

TEST(pp, line_directive_missing_number_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#line\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, error_directive_is_fatal_but_keeps_scanning)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#error boom\n#warning oops\nx\n"));
    EXPECT_EQ(pp->error_count, 1);
    EXPECT_EQ(pp->warning_count, 1);
    arena_free(a);
}

TEST(pp, warning_directive_is_nonfatal)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#warning careful\nx\n");
    EXPECT_EQ(pp->warning_count, 1);
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    arena_free(a);
}

TEST(pp, error_directive_expands_message)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define MSG boom\n#error MSG\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, ifdef_taken_when_defined)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\n#ifdef X\nint a;\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    expect_out(pp, 4, TOK_PP_TRIVIA_NL, "\n");
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, ifdef_skipped_when_undefined)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifdef Y\nint a;\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 0);
    arena_free(a);
}

TEST(pp, ifndef_taken_when_undefined)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifndef Y\nint a;\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    arena_free(a);
}

TEST(pp, else_taken_when_ifdef_false)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifdef Y\nint a;\n#else\nint b;\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    expect_out(pp, 2, TOK_PP_IDENT, "b");
    arena_free(a);
}

TEST(pp, nested_conditionals)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\n#ifdef X\n#ifdef Y\nint both;\n#else\nint only_x;\n"
                            "#endif\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    expect_out(pp, 2, TOK_PP_IDENT, "only_x");
    arena_free(a);
}

TEST(pp, skipped_define_has_no_effect)
{
    Arena *a = arena_new();
    Pp *pp =
        pp_run_text(a, "#ifdef NOPE\n#define B 2\n#endif\n#ifdef B\nint has;\n#else\nint none;\n"
                       "#endif\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 2, TOK_PP_IDENT, "none");
    arena_free(a);
}

TEST(pp, skipped_region_does_not_error_on_garbage)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifdef NOPE\n@@@ :::\n#endif\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_IDENT, "int");
    arena_free(a);
}

TEST(pp, skipped_region_expands_nothing)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define A 1\n#ifdef NOPE\nA\n#endif\n");
    EXPECT_EQ(vec_size(pp->out), 0);
    arena_free(a);
}

TEST(pp, skipped_region_preserves_line_numbers)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifdef NOPE\nskipme\n#endif\nint a;\n");
    EXPECT_EQ(vec_size(pp->out), 5);
    EXPECT_EQ(out_tok(pp, 0)->loc.line, 4);
    arena_free(a);
}

TEST(pp, endif_without_if_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#endif\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, else_without_if_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#else\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, ifdef_missing_name_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#ifdef\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, include_quoted_resolves_relative)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "h.h");
    pp_test_write(hdr, "#define FROM_HEADER 42\n");
    const char *main = pp_test_name(a, "main.c");

    char src[200];
    snprintf(src, sizeof(src), "#include \"%s\"\nint v = FROM_HEADER;\n", hdr);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "42"));
    arena_free(a);
}

TEST(pp, include_angle_with_I)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "h.h");
    pp_test_write(hdr, "#define FROM_I 77\n");

    char src[200];
    snprintf(src, sizeof(src), "#include <%s>\nint v = FROM_I;\n", hdr);
    Pp *pp = pp_new(a);
    vec_push(pp->include_paths, (void *) "/tmp");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "77"));
    arena_free(a);
}

TEST(pp, include_builtin_shim_resolves)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#include <stdbool.h>\nbool b = true;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "_Bool"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    arena_free(a);
}

TEST(pp, include_missing_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#include <ficcinc_zz_missing.h>\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, include_guards_include_once)
{
    Arena *a = arena_new();
    const char *ba = pp_test_name(a, "a.h");
    const char *bb = pp_test_name(a, "b.h");
    pp_test_write(bb, "#define FROM_B 7\n");
    char asrc[200];
    snprintf(asrc, sizeof(asrc),
             "#ifndef %s\n#define %s\n#include \"%s\"\n#define FROM_A 42\n#endif\n", ba, ba, bb);
    pp_test_write(ba, asrc);
    const char *main = pp_test_name(a, "main.c");

    char src[200];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\nint v = FROM_A + FROM_B;\n", ba,
             ba);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "42"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "7"));
    arena_free(a);
}

TEST(pp, include_depth_cap_is_error)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "rec.h");
    char rec[200];
    snprintf(rec, sizeof(rec), "#include \"%s\"\n", hdr);
    pp_test_write(hdr, rec);
    const char *main = pp_test_name(a, "main.c");

    char src[200];
    snprintf(src, sizeof(src), "#include \"%s\"\n", hdr);
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, include_in_skipped_branch_not_opened)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(
        pp, "<test>", "#ifdef NOPE\n#include <ficcinc_zz_skipped_missing.h>\n#endif\nint ok;\n"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

static void expect_if_taken_has(Pp *pp, const char *ident)
{
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, ident));
}

TEST(pp, if_true_and_false)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 1\nint a;\n#endif\n#if 0\nint gone;\n#endif\nint b;\n");
    expect_if_taken_has(pp, "a");
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "b"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "gone"));
    arena_free(a);
}

TEST(pp, if_arithmetic_and_precedence)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 5\n#if X * 2 == 10 && 1 + 2 * 3 == 7\nint a;\n#endif\n");
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, if_short_circuit_avoids_errors)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 0 && 1/0\nint gone;\n#endif\n#if 1 ? 2 : 1/0\nint a;\n#endif\n");
    expect_if_taken_has(pp, "a");
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "gone"));
    arena_free(a);
}

TEST(pp, if_unsigned_comparison)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if -1 < 0u\nint gone;\n#else\nint kept;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "kept"));
    arena_free(a);
}

TEST(pp, if_defined_and_elif)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\n#if defined(X) && !defined(Y)\nint a;\n#elif 1\nint b;\n"
                            "#else\nint c;\n#endif\n");
    expect_if_taken_has(pp, "a");
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "b"));
    arena_free(a);
}

TEST(pp, if_elif_chain)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 0\nint a;\n#elif 1\nint b;\n#else\nint c;\n#endif\n");
    expect_if_taken_has(pp, "b");
    arena_free(a);
}

TEST(pp, if_char_and_hex_constants)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 'a' == 97 && 0x10 == 16\nint a;\n#endif\n");
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, if_zero_hides_everything)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 0\n#define Z 1\nZ\n#error boom\n#endif\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "Z"));
    arena_free(a);
}

TEST(pp, if_div_by_zero_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#if 1/0\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, if_defined_from_macro_expansion_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define D defined\n#if D(X)\n"));
    EXPECT_TRUE(pp->error_count >= 1);
    arena_free(a);
}

TEST(pp, has_include_builtin_shim_is_present)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if __has_include(<stdbool.h>)\nint a;\n#endif\n");
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, has_include_missing_is_absent)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if __has_include(<ficcinc_zz_none.h>)\nint gone;\n#endif\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "gone"));
    arena_free(a);
}

TEST(pp, has_include_macro_operand)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define H <string.h>\n#if defined(__has_include) && __has_include(H)\n"
                            "int a;\n#endif\n");
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, has_include_next_is_absent)
{
    Arena *a = arena_new();
    Pp *pp =
        pp_run_text(a, "#if __has_include_next(<ficcinc_zz_none.h>)\nint gone;\n#endif\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    arena_free(a);
}

TEST(pp, include_macro_operand_expands)
{
    Arena *a = arena_new();
    const char *angle = pp_test_name(a, "macro.h");
    pp_test_write(angle, "#define FROM_ANGLE 33\n");
    char asrc[220];
    snprintf(asrc, sizeof(asrc), "#define H <%s>\n#include H\nint v = FROM_ANGLE;\n", angle);
    Pp *pp = pp_new(a);
    vec_push(pp->include_paths, (void *) "/tmp");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", asrc));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "33"));
    arena_free(a);
}

TEST(pp, include_quoted_macro_operand)
{
    Arena *a = arena_new();
    const char *qh = pp_test_name(a, "q.h");
    pp_test_write(qh, "#define FROM_Q 44\n");
    const char *main = pp_test_name(a, "main.c");
    char qsrc[220];
    snprintf(qsrc, sizeof(qsrc), "#define Q \"%s\"\n#include Q\nint v = FROM_Q;\n", qh);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), qsrc));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "44"));
    arena_free(a);
}

TEST(pp, include_operand_not_single_header_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define BAD foo bar\n#include BAD\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, include_next_finds_later_dir)
{
    Arena *a = arena_new();
    const char *d1 = pp_test_mkdir(a, "d1");
    const char *d2 = pp_test_mkdir(a, "d2");
    char path1[160], path2[160];
    snprintf(path1, sizeof(path1), "%s/a.h", d1);
    snprintf(path2, sizeof(path2), "%s/a.h", d2);
    pp_test_write_at(path1, "#define FIRST 1\n#include_next <a.h>\n");
    pp_test_write_at(path2, "#define SECOND 2\n");

    Pp *pp = pp_new(a);
    vec_push(pp->include_paths, (void *) d1);
    vec_push(pp->include_paths, (void *) d2);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "#include <a.h>\nint v = FIRST + SECOND;\n"));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "2"));
    arena_free(a);
}

TEST(pp, predefined_line_and_file)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "__LINE__ __FILE__ __LINE__\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_STRING, "\"<test>\""));
    arena_free(a);
}

TEST(pp, predefined_recook_per_use_and_after_line)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define L __LINE__\nL\nL\n#line 100\nL\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "2"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "3"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "100"));
    arena_free(a);
}

TEST(pp, predefined_stdc_family)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "__STDC__ __STDC_VERSION__ __STDC_HOSTED__ __STDC_NO_VLA__\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "201112L"));
    arena_free(a);
}

TEST(pp, predefined_usable_in_if)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if __STDC__ == 1 && __LINE__ == 1\nint a;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "a"));
    arena_free(a);
}

TEST(pp, predefined_redefinition_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define __STDC__ 1\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, predefined_line_redefinition_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define __LINE__ 5\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, predefined_date_time_respect_source_date_epoch)
{
    setenv("SOURCE_DATE_EPOCH", "0", 1);
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "__DATE__ __TIME__\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_STRING, "\"Jan  1 1970\""));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_STRING, "\"00:00:00\""));
    arena_free(a);
}

TEST(pp, pragma_once_prevents_double_include)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "once.h");
    pp_test_write(hdr, "#pragma once\n#define ONCE_X 5\n");
    const char *main = pp_test_name(a, "main.c");
    char src[240];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\nint v = ONCE_X;\n", hdr, hdr);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "5"));
    arena_free(a);
}

TEST(pp, pragma_unknown_is_ignored)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#pragma Whatever foo bar\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    arena_free(a);
}

TEST(pp, pragma_poison_use_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#pragma GCC poison printf\nprintf(\"x\");\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, pragma_poison_not_fired_from_expansion)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define CAT(a,b) a##b\n#pragma GCC poison foo\nint x = CAT(fo,o);\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "foo"));
    arena_free(a);
}

TEST(pp, pragma_system_header_silences_warning)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#pragma GCC system_header\n#warning muted\nint ok;\n");
    EXPECT_EQ(pp->warning_count, 0);
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    arena_free(a);
}
