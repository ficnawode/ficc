#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "harness.h"
#include "pp.h"
#include "pp_emit.h"
#include "testdriver.h"
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
    vec_push(pp->cfg.include_paths, (void *) "/tmp");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "77"));
    arena_free(a);
}

TEST(pp, gcc_builtin_type_macros_expand)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "unsigned long x = __SIZE_TYPE__;\nlong y = __PTRDIFF_TYPE__;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "long"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "unsigned"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "__SIZE_TYPE__"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "__PTRDIFF_TYPE__"));
    arena_free(a);
}

TEST(pp, redirect_macro_expands_to_plain_decl)
{
    /* glibc's __REDIRECT uses an asm label; the ficc predefine drops it to a
       plain declaration (the alias symbol is not needed on x86-64). */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "int __REDIRECT(foo, (int x), foo64);\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "foo"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "foo64"));
    arena_free(a);
}

TEST(pp, include_angle_with_isystem)
{
    Arena *a = arena_new();
    const char *dir = pp_test_mkdir(a, "isys");
    char hpath[200];
    snprintf(hpath, sizeof(hpath), "%s/h.h", dir);
    pp_test_write_at(hpath, "#define FROM_ISYS 88\n");

    Pp *pp = pp_new(a);
    vec_push(pp->cfg.system_include_paths, (void *) dir);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "#include <h.h>\nint v = FROM_ISYS;\n"));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "88"));
    arena_free(a);
}

TEST(pp, isystem_searched_after_I)
{
    Arena *a = arena_new();
    const char *user = pp_test_mkdir(a, "iuser");
    const char *sys = pp_test_mkdir(a, "isys2");
    char upath[200];
    snprintf(upath, sizeof(upath), "%s/h.h", user);
    pp_test_write_at(upath, "#define PICK 11\n");
    char spath[200];
    snprintf(spath, sizeof(spath), "%s/h.h", sys);
    pp_test_write_at(spath, "#define PICK 22\n");

    Pp *pp = pp_new(a);
    vec_push(pp->cfg.include_paths, (void *) user);
    vec_push(pp->cfg.system_include_paths, (void *) sys);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "#include <h.h>\nint v = PICK;\n"));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "11"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_NUMBER, "22"));
    arena_free(a);
}

TEST(pp, isystem_header_warnings_suppressed)
{
    Arena *a = arena_new();
    const char *sys = pp_test_mkdir(a, "isyswarn");
    char spath[200];
    snprintf(spath, sizeof(spath), "%s/h.h", sys);
    pp_test_write_at(spath, "#warning suppressed_here\n");

    Pp *pp = pp_new(a);
    vec_push(pp->cfg.system_include_paths, (void *) sys);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "#include <h.h>\n"));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(pp->warning_count, 0);
    arena_free(a);

    Arena *a2 = arena_new();
    const char *user = pp_test_mkdir(a2, "iuserwarn");
    char upath[200];
    snprintf(upath, sizeof(upath), "%s/h.h", user);
    pp_test_write_at(upath, "#warning visible_here\n");
    Pp *pp2 = pp_new(a2);
    vec_push(pp2->cfg.include_paths, (void *) user);
    EXPECT_NOTNULL(pp_preprocess(pp2, "<test>", "#include <h.h>\n"));
    EXPECT_EQ(pp2->error_count, 0);
    EXPECT_EQ(pp2->warning_count, 1);
    arena_free(a2);
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
    vec_push(pp->cfg.include_paths, (void *) "/tmp");
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
    vec_push(pp->cfg.include_paths, (void *) d1);
    vec_push(pp->cfg.include_paths, (void *) d2);
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

TEST(pp, pragma_operator_once)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "ponce.h");
    pp_test_write(hdr, "_Pragma(\"once\")\n#define PONCE_Y 5\n");
    const char *main = pp_test_name(a, "main.c");
    char src[240];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\nint v = PONCE_Y;\n", hdr, hdr);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "5"));
    arena_free(a);
}

TEST(pp, pragma_operator_once_in_main_file_warns)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "_Pragma(\"once\")\nint ok;\n");
    EXPECT_EQ(pp->warning_count, 1);
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, pragma_operator_once_via_macro)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "poncem.h");
    pp_test_write(hdr, "#define DO _Pragma(\"once\")\nDO\n#define PONCE_M 7\n");
    const char *main = pp_test_name(a, "main.c");
    char src[240];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\nint v = PONCE_M;\n", hdr, hdr);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "7"));
    arena_free(a);
}
TEST(pp, pragma_operator_escapes_not_decoded)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "_Pragma(\"GCC poison \\x70p\")\nint a = pp;\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, pragma_operator_escaped_quote_operand)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "_Pragma(\"GCC poison \\\"a\\\"\")\nint ok;\n");
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, pragma_operator_non_string_operand_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "_Pragma(foo)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

static char *pp_emit_str(Pp *pp, PpEmitOptions opts)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    pp_emit(pp, f, opts);
    fclose(f);
    return buf;
}

static char *pp_file_read(Arena *a, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = arena_alloc(a, (size_t) size + 1, 1);
    fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[size] = '\0';
    return buf;
}

/* Two soups are the same if every token shares kind, spelling and presumed
   loc; the -E round-trip restores exactly these. */
static void expect_soups_equal(Pp *a, Pp *b)
{
    EXPECT_EQ(vec_size(a->out), vec_size(b->out));
    for (size_t i = 0; i < vec_size(a->out) && i < vec_size(b->out); i++)
    {
        PpToken *x = vec_get(a->out, i);
        PpToken *y = vec_get(b->out, i);
        EXPECT_EQ(x->kind, y->kind);
        EXPECT_EQ(x->len, y->len);
        EXPECT_TRUE(x->len == y->len && strncmp(x->spell, y->spell, x->len) == 0);
        EXPECT_EQ(x->loc.line, y->loc.line);
        EXPECT_TRUE((x->loc.file == NULL) == (y->loc.file == NULL));
        if (x->loc.file && y->loc.file)
        {
            EXPECT_STR_EQ(x->loc.file, y->loc.file);
        }
    }
}

static void pp_emit_fixture(const char *base)
{
    Arena *a = arena_new();
    char path[160];
    snprintf(path, sizeof(path), "test/pp/%s.in", base);
    char *src = pp_file_read(a, path);
    EXPECT_NOTNULL(src);
    if (!src)
    {
        arena_free(a);
        return;
    }
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, path, src));
    if (pp->error_count > 0)
    {
        arena_free(a);
        return;
    }

    struct
    {
        const char *suffix;
        PpEmitOptions opts;
    } modes[] = {
        {"", {false, false}},
        {"-C", {true, false}},
        {"-P", {false, true}},
    };
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    {
        char exp[180];
        snprintf(exp, sizeof(exp), "test/pp/%s.expect%s", base, modes[m].suffix);
        char *want = pp_file_read(a, exp);
        if (!want)
        {
            continue;
        }
        char *got = pp_emit_str(pp, modes[m].opts);
        EXPECT_STR_EQ(want, got);
    }
    arena_free(a);
}

TEST(pp, emit_fixture_basic)
{
    pp_emit_fixture("basic");
}

TEST(pp, emit_fixture_sep)
{
    pp_emit_fixture("sep");
}

TEST(pp, emit_trivia_fidelity)
{
    Arena *a = arena_new();
    const char *src = "/* lead */\n"
                      "#define ADD(a, b) ((a) + (b))\n"
                      "\n"
                      "int x = ADD(1, 2);\n"
                      "\n"
                      "#define STR(x) #x\n"
                      "char *s = STR(a   b);\n";
    Pp *pp = pp_run_text(a, src);
    EXPECT_EQ(pp->error_count, 0);

    /* Blank lines and macro-expanded spacing survive; comments drop unless -C. */
    char *plain = pp_emit_str(pp, (PpEmitOptions) {.no_markers = true});
    EXPECT_STR_EQ("\n\nint x = ((1)+(2));\n\nchar *s = \"a b\";\n", plain);
    EXPECT_TRUE(strstr(plain, "/* lead */") == NULL);

    char *kept = pp_emit_str(pp, (PpEmitOptions) {.keep_comments = true, .no_markers = true});
    EXPECT_TRUE(strstr(kept, "/* lead */") != NULL);
    EXPECT_TRUE(strstr(kept, "char *s = \"a b\";") != NULL);
    arena_free(a);
}

/* Preprocess a program, emit it with line markers, and feed that back through
   the whole pipeline; the interpreter's exit value must not change. Pure
   in-process (no files, no external compiler) — the delta check the golden
   suite exists to protect. */
static void expect_roundtrip_exit(const char *src)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", src));
    char *emitted = pp_emit_str(pp, (PpEmitOptions) {0});
    arena_free(a);

    i64 before = tc_run_interp(src);
    i64 after = tc_run_interp(emitted);
    if (before != after)
    {
        fprintf(stderr, "  [pp roundtrip] exit %lld -> %lld for:\n%s\n", (long long) before,
                (long long) after, src);
        test_fail();
    }
}

TEST(pp, e_roundtrip_preserves_exit_status)
{
    expect_roundtrip_exit("int main(void) { return 42; }\n");
    expect_roundtrip_exit("#define ADD(a,b) ((a)+(b))\n#define TWICE(x) ADD(x,x)\n"
                          "int main(void) { return TWICE(21); }\n");
    expect_roundtrip_exit("#define N 3\n#if N > 2\nint main(void) { return 42; }\n"
                          "#else\nint main(void) { return 0; }\n#endif\n");
    expect_roundtrip_exit("#define CAT(a,b) a##b\n"
                          "int main(void) { int xy = 42; return CAT(x,y); }\n");
    expect_roundtrip_exit("#define STR(x) #x\n#define XSTR(x) STR(x)\n"
                          "int main(void) { return XSTR(42)[0] == 52 ? 42 : 0; }\n");
    expect_roundtrip_exit("#define OPS(X) X(10) X(20) X(12)\n"
                          "int main(void) { int t = 0;\n"
                          "#define ACC(v) t = t + (v);\n"
                          "OPS(ACC)\n"
                          "return t; }\n");
}

TEST(pp, emit_keep_comments)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "/* hi */\nint ok;\n");
    char *got = pp_emit_str(pp, (PpEmitOptions) {.keep_comments = true, .no_markers = true});
    EXPECT_STR_EQ("/* hi */\nint ok;\n", got);
    arena_free(a);
}

TEST(pp, emit_drops_comments)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "/* hi */\nint ok;\n");
    char *got = pp_emit_str(pp, (PpEmitOptions) {.keep_comments = false, .no_markers = true});
    EXPECT_STR_EQ("\nint ok;\n", got);
    arena_free(a);
}

TEST(pp, emit_no_markers)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\nint ok = X;\nint ok2;\n");
    char *got = pp_emit_str(pp, (PpEmitOptions) {.no_markers = true});
    EXPECT_STR_EQ("int ok = 1;\nint ok2;\n", got);
    arena_free(a);
}

TEST(pp, emit_markers)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define X 1\nint ok = X;\n#line 100 \"renamed.c\"\nint later;\n");
    char *got = pp_emit_str(pp, (PpEmitOptions) {0});
    EXPECT_STR_EQ("#line 2 \"<test>\"\nint ok = 1;\n#line 100 \"renamed.c\"\nint later;\n", got);
    arena_free(a);
}

TEST(pp, emit_separates_fused_pairs)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define DASH -\n#define LSH <\n#define DOT .\n"
                            "int a = 1 DASH DASH 2;\nint b = 1 LSH LSH 2;\nint c = 3 DOT 5;\n");
    char *got = pp_emit_str(pp, (PpEmitOptions) {.no_markers = true});
    EXPECT_STR_EQ("int a = 1 - - 2;\nint b = 1 < < 2;\nint c = 3 . 5;\n", got);
    arena_free(a);
}

TEST(pp, emit_roundtrip_identical_soup)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "rt.h");
    pp_test_write(hdr, "#define RTT_X 42\nint r = RTT_X;\n");
    const char *main = pp_test_name(a, "main.c");
    char src[240];
    snprintf(src, sizeof(src),
             "#include \"%s\"\n#line 9 \"mapped.c\"\nint w = RTT_X;\nint ln = __LINE__;\n", hdr);
    Pp *first = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(first, pp_test_path(a, main), src));
    char *text = pp_emit_str(first, (PpEmitOptions) {0});
    EXPECT_EQ(first->error_count, 0);

    Pp *second = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(second, pp_test_path(a, main), text));
    expect_soups_equal(first, second);
    arena_free(a);
}

TEST(pp, cmdline_define_value)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "FEATURE=17");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "int x = FEATURE;\n"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "17"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, cmdline_define_defaults_to_one)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "FLAG");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "int x = FLAG;\n"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    arena_free(a);
}

TEST(pp, cmdline_define_empty_value)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "EMPTY=");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "a EMPTY b;\n"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "EMPTY"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, cmdline_undef_and_ordering)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "V=1");
    pp_undef_cmdline(pp, "V");
    pp_define_cmdline(pp, "V=2");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "int x = V;\n"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "2"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    arena_free(a);
}

TEST(pp, cmdline_undef_leaves_ident)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "KEEP=1");
    pp_undef_cmdline(pp, "KEEP");
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "int KEEP;\n"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "KEEP"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, cmdline_define_reserved_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp_define_cmdline(pp, "__LINE__=5");
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, cmdline_include_forces_header)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "forced.h");
    pp_test_write(hdr, "#define FROM_HDR 99\n");
    Pp *pp = pp_new(a);
    pp_include_cmdline(pp, pp_test_path(a, hdr));
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "int y = FROM_HDR;\n"));
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "99"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, pedantic_warns_on_gnu_pragma)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    pp->cfg.pedantic = true;
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>", "#pragma once\nint ok;\n"));
    EXPECT_TRUE(pp->warning_count >= 1);
    arena_free(a);
}

/* Phase 17x: hostile-input hardening corpus (in-process, D17.14)
   Every row below is a corner that a naive preprocessor gets wrong: the
   rescan/disable-during boundary, empty-argument placemarkers, paste
   validity, intmax arithmetic in `#if`, and the diagnostic pragmas. */

/* Nested stringize/paste lattices. */

TEST(pp, hostile_stringize_of_paste)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define STR(x) #x\n#define XSTR(x) STR(x)\n#define CAT(a,b) a##b\n"
                            "XSTR(CAT(1,2))\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 2);
    expect_out(pp, 0, TOK_PP_STRING, "\"12\"");
    arena_free(a);
}

TEST(pp, hostile_paste_of_stringize_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>",
                              "#define CAT(a,b) a##b\n#define STR(x) #x\nCAT(STR(1),STR(2))\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_nested_arg_prescan_then_paste_rescan)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define foo1 42\n#define CAT(a,b) a##b\n#define XCAT(a,b) CAT(a,b)\n"
                            "XCAT(foo,1)\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "42"));
    arena_free(a);
}

TEST(pp, hostile_stringize_chain_five_deep)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define S(x) #x\n#define S2(x) S(x)\n#define S3(x) S2(x)\n"
                            "#define S4(x) S3(x)\n#define S5(x) S4(x)\nS5(a b)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_STRING, "\"a b\"");
    arena_free(a);
}

TEST(pp, hostile_stringize_prescans_keep_arg_spacing)
{
    /* Hash-quote wraps the stringizing away from the argument: the argument
       is prescanned (macro-replaced) first, but the interior whitespace must
       survive so the outer stringize still sees one space between tokens. */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define S(x) #x\n#define S2(x) S(x)\nS2(a   b)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_STRING, "\"a b\"");
    arena_free(a);
}

/* Empty-argument and GNU comma-deletion corners. */

TEST(pp, hostile_two_empty_arguments)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a,b) a b\nF(,)\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 1);
    expect_out(pp, 0, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

TEST(pp, hostile_comma_drop_empty_middle_kept)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define F(a, ...) a, ##__VA_ARGS__\nF(1,,2)\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 5);
    expect_out(pp, 0, TOK_PP_NUMBER, "1");
    expect_out(pp, 1, TOK_PP_PUNCT, ",");
    expect_out(pp, 2, TOK_PP_PUNCT, ",");
    expect_out(pp, 3, TOK_PP_NUMBER, "2");
    arena_free(a);
}

TEST(pp, hostile_raw_empty_macro_stringizes_to_its_name)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define E\n#define S(x) #x\nS(E)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_STRING, "\"E\"");
    arena_free(a);
}

TEST(pp, hostile_expanded_empty_macro_stringizes_empty)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define E\n#define S(x) #x\n#define XS(x) S(x)\nXS(E)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_STRING, "\"\"");
    arena_free(a);
}

TEST(pp, hostile_stringize_strips_argument_whitespace)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define S(x) #x\nS(  a   b  )\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_STRING, "\"a b\"");
    arena_free(a);
}

TEST(pp, hostile_paste_both_empty_is_nothing)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define CAT(a,b) a##b\nCAT(,)\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_EQ(vec_size(pp->out), 1);
    expect_out(pp, 0, TOK_PP_TRIVIA_NL, "\n");
    arena_free(a);
}

/* Paste validity failures (re-lexed spelling must be one pp-token). */

TEST(pp, hostile_paste_semicolons_invalid)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define CAT(a,b) a##b\nCAT(;,;)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_paste_dots_invalid)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define CAT(a,b) a##b\nCAT(.,.)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_paste_numbers_concatenate)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define CAT(a,b) a##b\nCAT(12,34)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_NUMBER, "1234");
    arena_free(a);
}

TEST(pp, hostile_paste_middle_placemarker_vanishes)
{
    /* An empty argument between two `##` is a placemarker: the chain collapses
       to a single paste of the non-empty neighbours (`x ## ## z` -> `xz`). */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define C(a,b,c) a##b##c\nC(x,,z)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_IDENT, "xz");
    arena_free(a);
}

TEST(pp, hostile_paste_edge_placemarkers_vanish)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define C(a,b,c) a##b##c\nC(,x,)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_IDENT, "x");
    arena_free(a);
}

TEST(pp, hostile_paste_number_then_operator_invalid)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define CAT(a,b) a##b\nCAT(1,+)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_paste_ident_dot_invalid)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define CAT(a,b) a##b\nCAT(a,.)\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

/* `#if` arithmetic: intmax_t typing, wraparound, and diagnostics. */

TEST(pp, hostile_if_hex_unsigned_exceeds_int)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 0x80000000u > 0x7fffffff\nint a;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, hostile_if_wraps_to_all_ones)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if 0xFFFFFFFFFFFFFFFF == -1\nint a;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, hostile_if_signed_shift_is_wide)
{
    /* pp arithmetic is intmax_t, so `1 << 31` is positive and `1 << 63`
       wraps to INT64_MIN (both differ from 32-bit-int intuition). */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if (1 << 31) > 0 && (1 << 63) < 0\nint a;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

TEST(pp, hostile_if_shift_count_exceeds_width_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#if 1 << 64\nint a;\n#endif\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_if_invalid_octal_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#if 09 == 9\nint a;\n#endif\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_if_hex_float_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#if 0x1p4 > 0\nint a;\n#endif\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_if_undefined_ident_is_zero)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if UNKNOWN && !OTHER\nint gone;\n#else\nint kept;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "kept"));
    EXPECT_FALSE(soup_has_token(pp, TOK_PP_IDENT, "gone"));
    arena_free(a);
}

TEST(pp, hostile_if_modulo_and_shift)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#if (7 % 3) == 1 && (1 << 4) == 16\nint a;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_if_taken_has(pp, "a");
    arena_free(a);
}

/* Include-guard and `#pragma once` double-inclusion (nested). */

TEST(pp, hostile_nested_include_guards)
{
    Arena *a = arena_new();
    const char *inner = pp_test_name(a, "inner.h");
    const char *outer = pp_test_name(a, "outer.h");
    char inner_src[220];
    snprintf(inner_src, sizeof(inner_src), "#ifndef %s\n#define %s\n#define INNER 3\n#endif\n",
             inner, inner);
    pp_test_write(inner, inner_src);
    char outer_src[300];
    snprintf(outer_src, sizeof(outer_src),
             "#ifndef %s\n#define %s\n#include \"%s\"\n#include \"%s\"\n#endif\n", outer, outer,
             inner, inner);
    pp_test_write(outer, outer_src);
    const char *main = pp_test_name(a, "main.c");

    char src[400];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\nint v = INNER;\n", outer, outer);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "3"));
    arena_free(a);
}

TEST(pp, hostile_pragma_once_beats_guardless_double_include)
{
    Arena *a = arena_new();
    const char *hdr = pp_test_name(a, "guardless.h");
    pp_test_write(hdr, "#pragma once\n#define G 5\n");
    const char *main = pp_test_name(a, "main.c");
    char src[300];
    snprintf(src, sizeof(src), "#include \"%s\"\n#include \"%s\"\n#include \"%s\"\nint v = G;\n",
             hdr, hdr, hdr);
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, pp_test_path(a, main), src));
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "5"));
    arena_free(a);
}

/* `#line` + marker round-trips. */

TEST(pp, hostile_line_markers_roundtrip_through_emit)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "int a;\n#line 50 \"x.c\" 2\nint b;\n#line 7 \"y.c\"\nint c;\n");
    EXPECT_EQ(pp->error_count, 0);
    char *text = pp_emit_str(pp, (PpEmitOptions) {0});
    Pp *again = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(again, "<test>", text));
    expect_soups_equal(pp, again);
    arena_free(a);
}

/* Macro redefinition conflicts. */

TEST(pp, hostile_redefinition_whitespace_only_is_ok)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#define M(a,b) a ## b\n#define M(a, b) a##b\nM(x,y)\n");
    EXPECT_EQ(pp->error_count, 0);
    expect_out(pp, 0, TOK_PP_IDENT, "xy");
    arena_free(a);
}

TEST(pp, hostile_redefinition_arity_change_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define F(a) a\n#define F(a,b) a\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_redefinition_object_body_change_is_error)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(pp_preprocess(pp, "<test>", "#define N 1\n#define N 2\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

/* `_Pragma` weaves. */

TEST(pp, hostile_pragma_weave_through_macro)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(
        pp_preprocess(pp, "<test>", "#define DO(x) _Pragma(#x)\nDO(GCC poison q)\nint z = q;\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_two_pragmas_in_one_expansion)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NOTNULL(pp_preprocess(pp, "<test>",
                                 "#define DO(x) _Pragma(#x)\nDO(GCC system_header) DO(once)\n"
                                 "int ok;\n"));
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

/* Poison / system_header interplay. */

TEST(pp, hostile_poison_not_fired_from_skipped_branch)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#pragma GCC poison foo\n#if 0\nfoo\n#endif\nint ok;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_IDENT, "ok"));
    arena_free(a);
}

TEST(pp, hostile_poison_stays_after_undef)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(
        pp_preprocess(pp, "<test>", "#pragma GCC poison foo\n#define foo 1\n#undef foo\nfoo\n"));
    EXPECT_EQ(pp->error_count, 1);
    arena_free(a);
}

TEST(pp, hostile_poison_multiple_idents_reported)
{
    Arena *a = arena_new();
    Pp *pp = pp_new(a);
    EXPECT_NULL(
        pp_preprocess(pp, "<test>", "#pragma GCC poison foo bar baz\nint a = foo + bar;\n"));
    EXPECT_EQ(pp->error_count, 2);
    arena_free(a);
}

TEST(pp, hostile_poison_identifier_used_as_macro_name_is_allowed)
{
    /* A `#define` whose *name* is poisoned is a definition, not a use. */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#pragma GCC poison foo\n#define foo 1\nint a = foo;\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    arena_free(a);
}

TEST(pp, hostile_system_header_silences_warning_but_not_poison)
{
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#pragma GCC system_header\n#warning muted\nint ok;\n");
    EXPECT_EQ(pp->warning_count, 0);
    EXPECT_EQ(pp->error_count, 0);
    arena_free(a);
}

TEST(pp, target_architecture_macros)
{
    /* glibc's bits/wordsize.h needs __x86_64__ to pick 64-bit __WORDSIZE. */
    Arena *a = arena_new();
    Pp *pp = pp_run_text(a, "#ifdef __x86_64__\nint x = 1;\n#endif\n");
    EXPECT_EQ(pp->error_count, 0);
    EXPECT_TRUE(soup_has_token(pp, TOK_PP_NUMBER, "1"));
    arena_free(a);
}
