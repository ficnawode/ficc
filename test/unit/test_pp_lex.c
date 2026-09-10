#include "harness.h"
#include "pp_lex.h"
#include "util/arena.h"

static char *prepare(Arena *a, const char *src)
{
    return pp_prepare("<test>", src, a);
}

static PpToken *tok(Vec *toks, size_t i)
{
    return vec_get(toks, i);
}

static bool spell_is(const PpToken *t, const char *spelling)
{
    return t->len == strlen(spelling) && strncmp(t->spell, spelling, t->len) == 0;
}

static void expect_token(Vec *toks, size_t i, PpKind kind, const char *spelling)
{
    PpToken *t = tok(toks, i);
    EXPECT_EQ(t->kind, kind);
    EXPECT_TRUE(spell_is(t, spelling));
}

static void expect_trigraph(Arena *a, char marker, const char *expected)
{
    char src[4] = {'?', '?', marker, '\0'};
    EXPECT_STR_EQ(pp_prepare("<test>", src, a), expected);
}

TEST(pp_prepare, trigraphs)
{
    Arena *a = arena_new();
    expect_trigraph(a, '=', "#");
    expect_trigraph(a, '\'', "^");
    expect_trigraph(a, '(', "[");
    expect_trigraph(a, ')', "]");
    expect_trigraph(a, '!', "|");
    expect_trigraph(a, '<', "{");
    expect_trigraph(a, '>', "}");
    expect_trigraph(a, '-', "~");
    arena_free(a);
}

TEST(pp_prepare, trigraph_backslash_needs_following_char)
{
    Arena *a = arena_new();
    char src[5] = {'?', '?', '/', 'x', '\0'};
    EXPECT_STR_EQ(prepare(a, src), "\\x");
    arena_free(a);
}

TEST(pp_prepare, non_trigraph_question_marks)
{
    Arena *a = arena_new();
    char spaced[7] = {'a', ' ', '?', '?', ' ', 'b', '\0'};
    EXPECT_STR_EQ(prepare(a, spaced), "a ?? b");
    char overlap[5] = {'?', '?', '?', '=', '\0'};
    EXPECT_STR_EQ(prepare(a, overlap), "?#");
    arena_free(a);
}

TEST(pp_prepare, splices_backslash_newline)
{
    Arena *a = arena_new();
    EXPECT_STR_EQ(prepare(a, "ab\\\ncd"), "abcd");
    EXPECT_STR_EQ(prepare(a, "a\\\n\\\nb"), "ab");
    arena_free(a);
}

TEST(pp_prepare, splices_trigraph_newline)
{
    Arena *a = arena_new();
    char src[9] = {'a', 'b', '?', '?', '/', '\n', 'c', 'd', '\0'};
    EXPECT_STR_EQ(prepare(a, src), "abcd");
    arena_free(a);
}

TEST(pp_prepare, splices_inside_comment)
{
    Arena *a = arena_new();
    EXPECT_STR_EQ(prepare(a, "/* co\\\nnt */ x"), "/* cont */ x");
    arena_free(a);
}

TEST(pp_prepare, trailing_backslash_is_error)
{
    Arena *a = arena_new();
    EXPECT_NULL(prepare(a, "abc\\"));
    char trigraph[7] = {'a', 'b', 'c', '?', '?', '/', '\0'};
    EXPECT_NULL(prepare(a, trigraph));
    arena_free(a);
}

TEST(pp_prepare, backslash_not_before_newline_is_kept)
{
    Arena *a = arena_new();
    EXPECT_STR_EQ(prepare(a, "a\\b"), "a\\b");
    arena_free(a);
}

TEST(pp_prepare, empty_and_plain_source)
{
    Arena *a = arena_new();
    EXPECT_STR_EQ(prepare(a, ""), "");
    EXPECT_STR_EQ(prepare(a, "int main(void) { return 0; }"), "int main(void) { return 0; }");
    arena_free(a);
}

TEST(pp_lex, horizontal_ws_run)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", " \t\v\f\r ", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 2);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_TRIVIA_WS);
    EXPECT_EQ(tok(toks, 0)->len, 6);
    EXPECT_FALSE(tok(toks, 0)->has_newline);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_EOF);
    arena_free(a);
}

TEST(pp_lex, one_newline_token_per_line)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "a\nb\n", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 5);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_IDENT);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_TRIVIA_NL);
    EXPECT_EQ(tok(toks, 1)->len, 1);
    EXPECT_TRUE(tok(toks, 1)->has_newline);
    EXPECT_EQ(tok(toks, 2)->kind, TOK_PP_IDENT);
    EXPECT_EQ(tok(toks, 3)->kind, TOK_PP_TRIVIA_NL);
    EXPECT_EQ(tok(toks, 4)->kind, TOK_PP_EOF);
    arena_free(a);
}

TEST(pp_lex, block_comment_on_one_line)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "/* hi */x", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 3);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_TRIVIA_COMMENT);
    EXPECT_TRUE(spell_is(tok(toks, 0), "/* hi */"));
    EXPECT_FALSE(tok(toks, 0)->has_newline);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_IDENT);
    arena_free(a);
}

TEST(pp_lex, block_comment_sets_has_newline)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "/*a\nb*/x", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_TRIVIA_COMMENT);
    EXPECT_TRUE(tok(toks, 0)->has_newline);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_IDENT);
    EXPECT_EQ(tok(toks, 1)->loc.line, 2);
    arena_free(a);
}

TEST(pp_lex, line_comment_stops_before_newline)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "// hi\nx", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 4);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_TRIVIA_COMMENT);
    EXPECT_TRUE(spell_is(tok(toks, 0), "// hi"));
    EXPECT_FALSE(tok(toks, 0)->has_newline);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_TRIVIA_NL);
    EXPECT_EQ(tok(toks, 2)->kind, TOK_PP_IDENT);
    arena_free(a);
}

TEST(pp_lex, unterminated_block_comment_is_error)
{
    Arena *a = arena_new();
    EXPECT_NULL(pp_lex("<test>", "/* abc", a));
    EXPECT_NULL(pp_lex("<test>", "/*", a));
    arena_free(a);
}

TEST(pp_lex, digraph_spellings)
{
    Arena *a = arena_new();
    static const struct
    {
        const char *spelling;
        PpPunct punct;
    } CASES[] = {
        {"<:", PP_PUNCT_LBRACKET}, {":>", PP_PUNCT_RBRACKET}, {"<%", PP_PUNCT_LBRACE},
        {"%>", PP_PUNCT_RBRACE},   {"%:", PP_PUNCT_HASH},     {"%:%:", PP_PUNCT_HASHHASH},
    };
    Vec *toks = pp_lex("<test>", "<: :> <% %> %: %:%:", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 12);
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
    {
        PpToken *t = tok(toks, i * 2);
        EXPECT_EQ(t->kind, TOK_PP_PUNCT);
        EXPECT_TRUE(spell_is(t, CASES[i].spelling));
        EXPECT_EQ(t->punct, CASES[i].punct);
    }
    arena_free(a);
}

TEST(pp_lex, hash_and_paste_are_punct)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "# ##", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_PUNCT);
    EXPECT_TRUE(spell_is(tok(toks, 0), "#"));
    EXPECT_EQ(tok(toks, 0)->punct, PP_PUNCT_HASH);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_TRIVIA_WS);
    EXPECT_EQ(tok(toks, 2)->kind, TOK_PP_PUNCT);
    EXPECT_TRUE(spell_is(tok(toks, 2), "##"));
    EXPECT_EQ(tok(toks, 2)->punct, PP_PUNCT_HASHHASH);
    arena_free(a);
}

TEST(pp_lex, punct_matched_longest_first)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "<<= << <", a);
    EXPECT_NOTNULL(toks);
    EXPECT_TRUE(spell_is(tok(toks, 0), "<<="));
    EXPECT_EQ(tok(toks, 0)->punct, PP_PUNCT_SHL_ASSIGN);
    EXPECT_TRUE(spell_is(tok(toks, 2), "<<"));
    EXPECT_EQ(tok(toks, 2)->punct, PP_PUNCT_SHL);
    EXPECT_TRUE(spell_is(tok(toks, 4), "<"));
    EXPECT_EQ(tok(toks, 4)->punct, PP_PUNCT_LT);
    arena_free(a);
}

TEST(pp_lex, unknown_bytes_are_other)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "@`$", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 4);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_OTHER);
    EXPECT_EQ(tok(toks, 0)->len, 1);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_OTHER);
    EXPECT_EQ(tok(toks, 2)->kind, TOK_PP_OTHER);
    EXPECT_EQ(tok(toks, 3)->kind, TOK_PP_EOF);
    arena_free(a);
}

TEST(pp_lex, locations_across_newlines)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "ab\n  cd", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(tok(toks, 0)->loc.line, 1);
    EXPECT_EQ(tok(toks, 0)->loc.col, 1);
    EXPECT_EQ(tok(toks, 3)->kind, TOK_PP_IDENT);
    EXPECT_EQ(tok(toks, 3)->loc.line, 2);
    EXPECT_EQ(tok(toks, 3)->loc.col, 3);
    arena_free(a);
}

TEST(pp_lex, tab_advances_to_tab_stop)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "\tx", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_TRIVIA_WS);
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_IDENT);
    EXPECT_EQ(tok(toks, 1)->loc.col, 8);
    arena_free(a);
}

TEST(pp_lex, splices_before_scanning)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "a\\\nb", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 2);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_IDENT);
    EXPECT_TRUE(spell_is(tok(toks, 0), "ab"));
    EXPECT_EQ(tok(toks, 0)->loc.line, 1);
    arena_free(a);
}

TEST(pp_lex, trigraph_before_scanning)
{
    Arena *a = arena_new();
    char src[5] = {'?', '?', '=', 'x', '\0'};
    Vec *toks = pp_lex("<test>", src, a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(tok(toks, 0)->kind, TOK_PP_PUNCT);
    EXPECT_TRUE(spell_is(tok(toks, 0), "#"));
    EXPECT_EQ(tok(toks, 1)->kind, TOK_PP_IDENT);
    arena_free(a);
}

TEST(pp_lex, prepare_error_propagates)
{
    Arena *a = arena_new();
    EXPECT_NULL(pp_lex("<test>", "abc\\", a));
    arena_free(a);
}

TEST(pp_kind_name, names_each_kind)
{
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_EOF), "TOK_PP_EOF");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_TRIVIA_WS), "TOK_PP_TRIVIA_WS");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_TRIVIA_NL), "TOK_PP_TRIVIA_NL");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_TRIVIA_COMMENT), "TOK_PP_TRIVIA_COMMENT");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_IDENT), "TOK_PP_IDENT");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_NUMBER), "TOK_PP_NUMBER");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_STRING), "TOK_PP_STRING");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_CHAR), "TOK_PP_CHAR");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_PUNCT), "TOK_PP_PUNCT");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_OTHER), "TOK_PP_OTHER");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_HEADER_NAME), "TOK_PP_HEADER_NAME");
    EXPECT_STR_EQ(pp_kind_name(TOK_PP_PARAM), "TOK_PP_PARAM");
}

TEST(pp_lex, plain_string_literal)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "\"abc\"", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 2);
    expect_token(toks, 0, TOK_PP_STRING, "\"abc\"");
    arena_free(a);
}

TEST(pp_lex, prefixed_string_literals)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "L\"a\" u8\"b\" u\"c\" U\"d\"", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 8);
    expect_token(toks, 0, TOK_PP_STRING, "L\"a\"");
    expect_token(toks, 2, TOK_PP_STRING, "u8\"b\"");
    expect_token(toks, 4, TOK_PP_STRING, "u\"c\"");
    expect_token(toks, 6, TOK_PP_STRING, "U\"d\"");
    arena_free(a);
}

TEST(pp_lex, string_escapes_kept_raw)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "\"\\x41\\n\" \"\\u1234\"", a);
    EXPECT_NOTNULL(toks);
    expect_token(toks, 0, TOK_PP_STRING, "\"\\x41\\n\"");
    expect_token(toks, 2, TOK_PP_STRING, "\"\\u1234\"");
    arena_free(a);
}

TEST(pp_lex, string_escaped_quote_does_not_end_literal)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "\"a\\\"b\"", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 2);
    expect_token(toks, 0, TOK_PP_STRING, "\"a\\\"b\"");
    arena_free(a);
}

TEST(pp_lex, unterminated_string_is_error)
{
    Arena *a = arena_new();
    EXPECT_NULL(pp_lex("<test>", "\"abc", a));
    EXPECT_NULL(pp_lex("<test>", "\"abc\nx\"", a));
    arena_free(a);
}

TEST(pp_lex, char_literals_and_escapes)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "'a' '\\n'", a);
    EXPECT_NOTNULL(toks);
    expect_token(toks, 0, TOK_PP_CHAR, "'a'");
    expect_token(toks, 2, TOK_PP_CHAR, "'\\n'");
    arena_free(a);
}

TEST(pp_lex, multi_char_constant_is_one_token)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "'ab'", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 2);
    expect_token(toks, 0, TOK_PP_CHAR, "'ab'");
    arena_free(a);
}

TEST(pp_lex, prefixed_char_literals)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "L'a' u'b' U'c'", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 6);
    expect_token(toks, 0, TOK_PP_CHAR, "L'a'");
    expect_token(toks, 2, TOK_PP_CHAR, "u'b'");
    expect_token(toks, 4, TOK_PP_CHAR, "U'c'");
    arena_free(a);
}

TEST(pp_lex, unterminated_char_is_error)
{
    Arena *a = arena_new();
    EXPECT_NULL(pp_lex("<test>", "'a", a));
    EXPECT_NULL(pp_lex("<test>", "'a\nb'", a));
    arena_free(a);
}

TEST(pp_lex, char_adjacent_to_ident)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "x'a'", a);
    EXPECT_NOTNULL(toks);
    EXPECT_EQ(vec_size(toks), 3);
    expect_token(toks, 0, TOK_PP_IDENT, "x");
    expect_token(toks, 1, TOK_PP_CHAR, "'a'");
    arena_free(a);
}

TEST(pp_lex, prefix_without_quote_is_ident)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "Labc u8 lu", a);
    EXPECT_NOTNULL(toks);
    expect_token(toks, 0, TOK_PP_IDENT, "Labc");
    expect_token(toks, 2, TOK_PP_IDENT, "u8");
    expect_token(toks, 4, TOK_PP_IDENT, "lu");
    arena_free(a);
}

TEST(pp_lex, pp_numbers)
{
    Arena *a = arena_new();
    static const char *const NUMBERS[] = {"0x1p-3", ".5",   "1e+5", "5.",     "123abc",
                                          "1e-",    "1..2", "0xFF", "1.5e-3", "42"};
    for (size_t i = 0; i < sizeof(NUMBERS) / sizeof(NUMBERS[0]); i++)
    {
        Vec *toks = pp_lex("<test>", NUMBERS[i], a);
        EXPECT_NOTNULL(toks);
        EXPECT_EQ(vec_size(toks), 2);
        expect_token(toks, 0, TOK_PP_NUMBER, NUMBERS[i]);
    }
    arena_free(a);
}

TEST(pp_lex, dot_is_punct_but_dot_digit_is_number)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", ". ... .5", a);
    EXPECT_NOTNULL(toks);
    expect_token(toks, 0, TOK_PP_PUNCT, ".");
    expect_token(toks, 2, TOK_PP_PUNCT, "...");
    expect_token(toks, 4, TOK_PP_NUMBER, ".5");
    arena_free(a);
}

TEST(pp_lex, angle_quotes_are_not_header_names)
{
    Arena *a = arena_new();
    Vec *toks = pp_lex("<test>", "<foo.h>", a);
    EXPECT_NOTNULL(toks);
    expect_token(toks, 0, TOK_PP_PUNCT, "<");
    expect_token(toks, 1, TOK_PP_IDENT, "foo");
    expect_token(toks, 2, TOK_PP_PUNCT, ".");
    expect_token(toks, 3, TOK_PP_IDENT, "h");
    expect_token(toks, 4, TOK_PP_PUNCT, ">");
    arena_free(a);
}
