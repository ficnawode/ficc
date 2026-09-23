#include "harness.h"
#include "lexer.h"
#include "pp.h"
#include "util/arena.h"

#include <string.h>

/* Runs the real frontend (pp -> lex_finalize) so these rows exercise the
   production token path. */
static LexResult lex_text(const char *src, Arena *a)
{
    Pp *pp = pp_new(a);
    Vec *soup = pp_preprocess(pp, "t", src);
    if (!soup)
    {
        return (LexResult) {0};
    }
    return lex_finalize(soup, a);
}

TEST(lexer, keywords_and_punct)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int main(void) { return 42; }", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 11);

    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[1].kind, TOK_IDENT);
    EXPECT_EQ(t[2].kind, TOK_LPAREN);
    EXPECT_EQ(t[3].kind, TOK_KW_VOID);
    EXPECT_EQ(t[4].kind, TOK_RPAREN);
    EXPECT_EQ(t[5].kind, TOK_LBRACE);
    EXPECT_EQ(t[6].kind, TOK_KW_RETURN);
    EXPECT_EQ(t[7].kind, TOK_INT_LIT);
    EXPECT_EQ(t[8].kind, TOK_SEMI);
    EXPECT_EQ(t[9].kind, TOK_RBRACE);
    EXPECT_EQ(t[10].kind, TOK_EOF);

    arena_free(a);
}

TEST(lexer, literal_value)
{
    Arena *a = arena_new();
    LexResult res = lex_text("return 123;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[0].kind, TOK_KW_RETURN);
    EXPECT_EQ(t[1].kind, TOK_INT_LIT);
    EXPECT_EQ(t[1].payload.int_val, 123);
    arena_free(a);
}

TEST(lexer, identifier_name)
{
    Arena *a = arena_new();
    LexResult res = lex_text("foo_bar;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_STR_EQ(t[0].payload.str, "foo_bar");
    arena_free(a);
}

TEST(lexer, location_tracking)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int\nmain", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[0].loc.line, 1);
    EXPECT_EQ(t[0].loc.col, 1);
    EXPECT_EQ(t[1].loc.line, 2);
    EXPECT_EQ(t[1].loc.col, 1);
    arena_free(a);
}

TEST(lexer, tab_advances_col)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int\tmain", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[1].loc.col, 8); /* tab from col 4 -> col 9 */
    arena_free(a);
}

TEST(lexer, overflow_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("999999999999999999999999999999", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, unknown_char_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int @;", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, empty_source)
{
    Arena *a = arena_new();
    LexResult res = lex_text("", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 1);
    EXPECT_EQ(t[0].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, operators)
{
    Arena *a = arena_new();
    LexResult res = lex_text("+ - * / % = == != < > <= >= && || ! & | ^ << >> ~ : ,", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 24);
    EXPECT_EQ(t[0].kind, TOK_PLUS);
    EXPECT_EQ(t[1].kind, TOK_MINUS);
    EXPECT_EQ(t[2].kind, TOK_STAR);
    EXPECT_EQ(t[3].kind, TOK_SLASH);
    EXPECT_EQ(t[4].kind, TOK_PERCENT);
    EXPECT_EQ(t[5].kind, TOK_ASSIGN);
    EXPECT_EQ(t[6].kind, TOK_EQ);
    EXPECT_EQ(t[7].kind, TOK_NE);
    EXPECT_EQ(t[8].kind, TOK_LT);
    EXPECT_EQ(t[9].kind, TOK_GT);
    EXPECT_EQ(t[10].kind, TOK_LE);
    EXPECT_EQ(t[11].kind, TOK_GE);
    EXPECT_EQ(t[12].kind, TOK_LOG_AND);
    EXPECT_EQ(t[13].kind, TOK_LOG_OR);
    EXPECT_EQ(t[14].kind, TOK_NOT);
    EXPECT_EQ(t[15].kind, TOK_BW_AND);
    EXPECT_EQ(t[16].kind, TOK_BW_OR);
    EXPECT_EQ(t[17].kind, TOK_BW_XOR);
    EXPECT_EQ(t[18].kind, TOK_SHL);
    EXPECT_EQ(t[19].kind, TOK_SHR);
    EXPECT_EQ(t[20].kind, TOK_TILDE);
    EXPECT_EQ(t[21].kind, TOK_COLON);
    EXPECT_EQ(t[22].kind, TOK_COMMA);
    EXPECT_EQ(t[23].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, ellipsis)
{
    Arena *a = arena_new();
    LexResult res = lex_text("...", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_ELLIPSIS);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, dot_is_not_ellipsis)
{
    /* A lone `.` and a `..` pair stay TOK_DOT (member access / GNU ranges
       stay lexable); only three consecutive dots are TOK_ELLIPSIS. */
    Arena *a = arena_new();
    LexResult res = lex_text("..", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 3);
    EXPECT_EQ(t[0].kind, TOK_DOT);
    EXPECT_EQ(t[1].kind, TOK_DOT);
    EXPECT_EQ(t[2].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, member_dot_unchanged)
{
    Arena *a = arena_new();
    LexResult res = lex_text(".", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_DOT);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, block_comment)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int /* comment */ x;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[1].kind, TOK_IDENT);
    EXPECT_EQ(t[2].kind, TOK_SEMI);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, unterminated_comment)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int /* never ends", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, line_comment)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int // comment\n x;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[1].kind, TOK_IDENT);
    EXPECT_EQ(t[2].kind, TOK_SEMI);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, line_comment_to_eof)
{
    /* A `//` comment runs to the end of input (no trailing newline). */
    Arena *a = arena_new();
    LexResult res = lex_text("int x // trailing", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 3);
    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[1].kind, TOK_IDENT);
    EXPECT_EQ(t[2].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, line_comment_does_not_swallow_newline)
{
    /* The comment ends at (and does not consume) the newline, so a token on
       the next line is located on line 2. */
    Arena *a = arena_new();
    LexResult res = lex_text("// c\nx", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[0].loc.line, 2);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, slash_is_not_comment)
{
    /* A lone `/` stays a division operator. */
    Arena *a = arena_new();
    LexResult res = lex_text("a / b", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[1].kind, TOK_SLASH);
    EXPECT_EQ(t[2].kind, TOK_IDENT);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, line_and_block_comment_mixed)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int /* block */ // line\n x;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[1].kind, TOK_IDENT);
    EXPECT_EQ(t[2].kind, TOK_SEMI);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, if_else_keywords)
{
    Arena *a = arena_new();
    LexResult res = lex_text("if (x) else", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 6);
    EXPECT_EQ(t[0].kind, TOK_KW_IF);
    EXPECT_EQ(t[1].kind, TOK_LPAREN);
    EXPECT_EQ(t[2].kind, TOK_IDENT);
    EXPECT_EQ(t[3].kind, TOK_RPAREN);
    EXPECT_EQ(t[4].kind, TOK_KW_ELSE);
    EXPECT_EQ(t[5].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, loop_keywords)
{
    Arena *a = arena_new();
    LexResult res = lex_text("while for do break continue goto", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 7);
    EXPECT_EQ(t[0].kind, TOK_KW_WHILE);
    EXPECT_EQ(t[1].kind, TOK_KW_FOR);
    EXPECT_EQ(t[2].kind, TOK_KW_DO);
    EXPECT_EQ(t[3].kind, TOK_KW_BREAK);
    EXPECT_EQ(t[4].kind, TOK_KW_CONTINUE);
    EXPECT_EQ(t[5].kind, TOK_KW_GOTO);
    EXPECT_EQ(t[6].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, switch_keywords)
{
    Arena *a = arena_new();
    LexResult res = lex_text("switch case default", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_KW_SWITCH);
    EXPECT_EQ(t[1].kind, TOK_KW_CASE);
    EXPECT_EQ(t[2].kind, TOK_KW_DEFAULT);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, typedef_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("typedef int Foo;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 5);
    EXPECT_EQ(t[0].kind, TOK_KW_TYPEDEF);
    EXPECT_EQ(t[1].kind, TOK_KW_INT);
    EXPECT_EQ(t[2].kind, TOK_IDENT);
    EXPECT_EQ(t[3].kind, TOK_SEMI);
    EXPECT_EQ(t[4].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, ternary_tokens)
{
    Arena *a = arena_new();
    LexResult res = lex_text("a ? b : c", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 6);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[1].kind, TOK_QUESTION);
    EXPECT_EQ(t[2].kind, TOK_IDENT);
    EXPECT_EQ(t[3].kind, TOK_COLON);
    EXPECT_EQ(t[4].kind, TOK_IDENT);
    EXPECT_EQ(t[5].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, string_literal_simple)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"hello\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 5);
    EXPECT_TRUE(memcmp(t[0].payload.str, "hello", 5) == 0);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, string_literal_empty)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 0);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, string_literal_escapes)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"a\\nb\\tc\\\\d\\\"e\\0f\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 11);
    EXPECT_EQ(t[0].payload.str[0], 'a');
    EXPECT_EQ(t[0].payload.str[1], '\n');
    EXPECT_EQ(t[0].payload.str[2], 'b');
    EXPECT_EQ(t[0].payload.str[3], '\t');
    EXPECT_EQ(t[0].payload.str[4], 'c');
    EXPECT_EQ(t[0].payload.str[5], '\\');
    EXPECT_EQ(t[0].payload.str[6], 'd');
    EXPECT_EQ(t[0].payload.str[7], '"');
    EXPECT_EQ(t[0].payload.str[8], 'e');
    EXPECT_EQ(t[0].payload.str[9], '\0');
    EXPECT_EQ(t[0].payload.str[10], 'f');
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, string_literal_unterminated)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"hello", a);
    EXPECT_NULL(res.tokens);
    EXPECT_EQ(res.count, 0);
    arena_free(a);
}

TEST(lexer, sizeof_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("sizeof", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_SIZEOF);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, alignof_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("_Alignof", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_ALIGNOF);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, generic_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("_Generic", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_GENERIC);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, static_assert_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("_Static_assert", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_STATIC_ASSERT);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, bool_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("_Bool", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_BOOL);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, bool_keyword_is_exact)
{
    /* `bool` (without the underscore prefix) is not yet a keyword — it arrives
       via <stdbool.h> in Phase 17. */
    Arena *a = arena_new();
    LexResult res = lex_text("bool", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    arena_free(a);
}

TEST(lexer, alignas_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("_Alignas", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_ALIGNAS);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_value)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'a'", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[0].payload.int_val, 'a');
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_simple_escapes)
{
    Arena *a = arena_new();
    LexResult res =
        lex_text("'\\n' '\\0' '\\t' '\\a' '\\'' '\\\\' '\"' '\\?' '\\v' '\\f' '\\r' '\\b'", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 13);
    EXPECT_EQ(t[0].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[0].payload.int_val, '\n');
    EXPECT_EQ(t[1].payload.int_val, '\0');
    EXPECT_EQ(t[2].payload.int_val, '\t');
    EXPECT_EQ(t[3].payload.int_val, '\a');
    EXPECT_EQ(t[4].payload.int_val, '\'');
    EXPECT_EQ(t[5].payload.int_val, '\\');
    EXPECT_EQ(t[6].payload.int_val, '"');
    EXPECT_EQ(t[7].payload.int_val, '?');
    EXPECT_EQ(t[8].payload.int_val, '\v');
    EXPECT_EQ(t[9].payload.int_val, '\f');
    EXPECT_EQ(t[10].payload.int_val, '\r');
    EXPECT_EQ(t[11].payload.int_val, '\b');
    EXPECT_EQ(t[12].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_octal_escapes)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'\\0' '\\01' '\\007' '\\101' '\\377'", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 6);
    EXPECT_EQ(t[0].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[0].payload.int_val, 0);
    EXPECT_EQ(t[1].payload.int_val, 1);
    EXPECT_EQ(t[2].payload.int_val, 7);
    EXPECT_EQ(t[3].payload.int_val, 65);  /* \101 octal == 'A' */
    EXPECT_EQ(t[4].payload.int_val, 255); /* \377 octal == 0xFF */
    EXPECT_EQ(t[5].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_hex_escapes)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'\\x41' '\\x0a' '\\x7f'", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[0].payload.int_val, 0x41);
    EXPECT_EQ(t[1].payload.int_val, 0x0a);
    EXPECT_EQ(t[2].payload.int_val, 0x7f);
    EXPECT_EQ(t[3].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, octal_escape_digit_boundary)
{
    /* Octal escapes consume at most three octal digits; a following digit in
       a *string* is a separate byte ("\0123" == {10, '3'}). In a char
       constant the same sequence is a multi-char constant (rejected). */
    Arena *a = arena_new();
    LexResult res = lex_text("\"\\0123\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 2);
    EXPECT_EQ(t[0].payload.str[0], 10); /* \012 octal == newline */
    EXPECT_EQ(t[0].payload.str[1], '3');
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, string_literal_octal_and_hex_escapes)
{
    /* The shared escape decoder gives strings the full §6.4.4.4 table:
       "\01" is octal 1 (not NUL then '1'), "\x41" is 'A' (not 'x','4','1'). */
    Arena *a = arena_new();
    LexResult res = lex_text("\"\\01\\x41\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 2);
    EXPECT_EQ(t[0].payload.str[0], 1);
    EXPECT_EQ(t[0].payload.str[1], 'A');
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_empty_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("''", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_multi_char_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'ab'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_hex_no_digits_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'\\x'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_out_of_range_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'\\400'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_unterminated_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'a", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, increment_decrement_tokens)
{
    Arena *a = arena_new();
    LexResult res = lex_text("x++ --y a+-+b ++++c", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 13);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[1].kind, TOK_PLUS_PLUS);
    EXPECT_EQ(t[2].kind, TOK_MINUS_MINUS);
    EXPECT_EQ(t[3].kind, TOK_IDENT);
    EXPECT_EQ(t[4].kind, TOK_IDENT);
    EXPECT_EQ(t[5].kind, TOK_PLUS);
    EXPECT_EQ(t[6].kind, TOK_MINUS);
    EXPECT_EQ(t[7].kind, TOK_PLUS);
    EXPECT_EQ(t[8].kind, TOK_IDENT);
    EXPECT_EQ(t[9].kind, TOK_PLUS_PLUS);
    EXPECT_EQ(t[10].kind, TOK_PLUS_PLUS);
    EXPECT_EQ(t[11].kind, TOK_IDENT);
    EXPECT_EQ(t[12].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, arrow_and_decrement_max_munch)
{
    /* `p->x` lexes the arrow; `p-->y` is maximal-munched as `p -- > y` (a
       comparison of a decremented pointer), not `p -- -> y`. */
    Arena *a = arena_new();
    LexResult res = lex_text("p->x p-->y", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 8);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[1].kind, TOK_ARROW);
    EXPECT_EQ(t[2].kind, TOK_IDENT);
    EXPECT_EQ(t[3].kind, TOK_IDENT);
    EXPECT_EQ(t[4].kind, TOK_MINUS_MINUS);
    EXPECT_EQ(t[5].kind, TOK_GT);
    EXPECT_EQ(t[6].kind, TOK_IDENT);
    EXPECT_EQ(t[7].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, compound_assignment_tokens)
{
    Arena *a = arena_new();
    LexResult res = lex_text("+= -= *= /= %= <<= >>= &= |= ^= <= >= << >>", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 15);
    EXPECT_EQ(t[0].kind, TOK_PLUS_ASSIGN);
    EXPECT_EQ(t[1].kind, TOK_MINUS_ASSIGN);
    EXPECT_EQ(t[2].kind, TOK_STAR_ASSIGN);
    EXPECT_EQ(t[3].kind, TOK_SLASH_ASSIGN);
    EXPECT_EQ(t[4].kind, TOK_PERCENT_ASSIGN);
    EXPECT_EQ(t[5].kind, TOK_SHL_ASSIGN);
    EXPECT_EQ(t[6].kind, TOK_SHR_ASSIGN);
    EXPECT_EQ(t[7].kind, TOK_BW_AND_ASSIGN);
    EXPECT_EQ(t[8].kind, TOK_BW_OR_ASSIGN);
    EXPECT_EQ(t[9].kind, TOK_BW_XOR_ASSIGN);
    EXPECT_EQ(t[10].kind, TOK_LE);
    EXPECT_EQ(t[11].kind, TOK_GE);
    EXPECT_EQ(t[12].kind, TOK_SHL);
    EXPECT_EQ(t[13].kind, TOK_SHR);
    EXPECT_EQ(t[14].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, compound_assignment_not_single_equals)
{
    /* `a>>=3` and `a<<=3` must not fall back to `>`/`<` + `=`. */
    Arena *a = arena_new();
    LexResult res = lex_text("a>>=3 b<<=3", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 7);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_EQ(t[1].kind, TOK_SHR_ASSIGN);
    EXPECT_EQ(t[2].kind, TOK_INT_LIT);
    EXPECT_EQ(t[3].kind, TOK_IDENT);
    EXPECT_EQ(t[4].kind, TOK_SHL_ASSIGN);
    EXPECT_EQ(t[5].kind, TOK_INT_LIT);
    EXPECT_EQ(t[6].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, signed_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex_text("signed long long unsigned", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 5);
    EXPECT_EQ(t[0].kind, TOK_KW_SIGNED);
    EXPECT_EQ(t[1].kind, TOK_KW_LONG);
    EXPECT_EQ(t[2].kind, TOK_KW_LONG);
    EXPECT_EQ(t[3].kind, TOK_KW_UNSIGNED);
    EXPECT_EQ(t[4].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, plain_declaration)
{
    Arena *a = arena_new();
    LexResult res = lex_text("int x = 1;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 6);
    EXPECT_EQ(t[0].kind, TOK_KW_INT);
    EXPECT_EQ(t[5].kind, TOK_EOF);
    arena_free(a);
}

TEST(finalize, string_concat)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"a\" \"b\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 2);
    EXPECT_EQ(t[0].payload.str[0], 'a');
    EXPECT_EQ(t[0].payload.str[1], 'b');
    arena_free(a);
}

TEST(finalize, string_concat_across_comment)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"a\" /* mid */ \"b\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].str_len, 2);
    EXPECT_EQ(t[0].payload.str[0], 'a');
    EXPECT_EQ(t[0].payload.str[1], 'b');
    arena_free(a);
}

TEST(finalize, string_not_concat_across_other_token)
{
    Arena *a = arena_new();
    LexResult res = lex_text("\"a\" + \"b\"", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 4);
    EXPECT_EQ(t[0].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[1].kind, TOK_PLUS);
    EXPECT_EQ(t[2].kind, TOK_STRING_LIT);
    EXPECT_EQ(t[0].str_len, 1);
    EXPECT_EQ(t[2].str_len, 1);
    arena_free(a);
}

TEST(finalize, chars_are_not_concatenated)
{
    Arena *a = arena_new();
    LexResult res = lex_text("'a' 'b'", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 3);
    EXPECT_EQ(t[0].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[0].payload.int_val, 'a');
    EXPECT_EQ(t[1].kind, TOK_CHAR_LIT);
    EXPECT_EQ(t[1].payload.int_val, 'b');
    arena_free(a);
}

TEST(finalize, wide_string_rejected)
{
    Arena *a = arena_new();
    EXPECT_NULL(lex_text("L\"a\"", a).tokens);
    EXPECT_NULL(lex_text("u8\"a\"", a).tokens);
    EXPECT_NULL(lex_text("u\"a\"", a).tokens);
    EXPECT_NULL(lex_text("U\"a\"", a).tokens);
    arena_free(a);
}

TEST(finalize, wide_char_rejected)
{
    Arena *a = arena_new();
    EXPECT_NULL(lex_text("L'a'", a).tokens);
    EXPECT_NULL(lex_text("u'a'", a).tokens);
    EXPECT_NULL(lex_text("U'a'", a).tokens);
    arena_free(a);
}

TEST(finalize, float_literals_lex)
{
    Arena *a = arena_new();
    LexResult res = lex_text("1.5 .5 1e5 0x1p3 2f", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 6);
    EXPECT_EQ(t[0].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[0].float_kind, FK_DOUBLE);
    EXPECT_EQ(t[1].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[1].payload.float_pat, 0x3FE0000000000000ULL); /* .5 */
    EXPECT_EQ(t[2].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[2].payload.float_pat, 0x40F86A0000000000ULL); /* 1e5 */
    EXPECT_EQ(t[3].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[3].payload.float_pat, 0x4020000000000000ULL); /* 0x1p3 == 8.0 */
    EXPECT_EQ(t[3].int_suffix.is_hex, false);
    EXPECT_EQ(t[4].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[4].float_kind, FK_FLOAT);
    EXPECT_EQ(t[5].kind, TOK_EOF);
    arena_free(a);
}

TEST(finalize, float_suffix_kinds)
{
    Arena *a = arena_new();
    LexResult res = lex_text("1.5f 2.5F 1.5L 1.5l", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 5);
    EXPECT_EQ(t[0].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[0].float_kind, FK_FLOAT);
    EXPECT_EQ(t[1].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[1].float_kind, FK_FLOAT);
    EXPECT_EQ(t[2].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[2].float_kind, FK_LONG);
    EXPECT_EQ(t[3].kind, TOK_FLOAT_LIT);
    EXPECT_EQ(t[3].float_kind, FK_LONG);
    EXPECT_EQ(t[4].kind, TOK_EOF);
    /* An 80-bit literal must carry the exact host value (1.5 is exact). */
    EXPECT_TRUE((double) t[2].payload.ld_val == 1.5);
    arena_free(a);
}

/* The whole spelling must parse (C11 §6.4.4.2): malformed suffixes reject. */
TEST(finalize, float_long_malformed_rejected)
{
    Arena *a = arena_new();
    EXPECT_NULL(lex_text("1.5j", a).tokens);
    EXPECT_NULL(lex_text(".5Lz", a).tokens);
    EXPECT_NULL(lex_text("0x1.5L", a).tokens); /* hex long double needs p */
    arena_free(a);
}

TEST(finalize, float_malformed_rejected)
{
    Arena *a = arena_new();
    /* `0x1.5` (no p exponent) and `1e` reject instead of half-parsing. */
    EXPECT_NULL(lex_text("0x1.5", a).tokens);
    EXPECT_NULL(lex_text("1e", a).tokens);
    EXPECT_NULL(lex_text(".5j", a).tokens);
    arena_free(a);
}

TEST(finalize, hex_int_is_not_float)
{
    Arena *a = arena_new();
    LexResult res = lex_text("0xFF 0x1e", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 3);
    EXPECT_EQ(t[0].kind, TOK_INT_LIT);
    EXPECT_EQ(t[0].payload.int_val, 255);
    EXPECT_TRUE(t[0].int_suffix.is_hex);
    EXPECT_EQ(t[1].payload.int_val, 30);
    arena_free(a);
}

TEST(finalize, octal_int_literals)
{
    Arena *a = arena_new();
    LexResult res = lex_text("0644 010 0777 0", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 5);
    EXPECT_EQ(t[0].kind, TOK_INT_LIT);
    EXPECT_EQ(t[0].payload.int_val, 420); /* 0644 octal */
    EXPECT_EQ(t[1].payload.int_val, 8);   /* 010 octal */
    EXPECT_EQ(t[2].payload.int_val, 511); /* 0777 octal */
    EXPECT_EQ(t[3].payload.int_val, 0);
    EXPECT_TRUE(t[0].int_suffix.is_hex); /* non-decimal: unsigned-capable */
    arena_free(a);
}

TEST(finalize, integer_suffixes)
{
    Arena *a = arena_new();
    LexResult res = lex_text("1u 2L 3LL 4UL 5llu", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[0].int_suffix.is_unsigned, true);
    EXPECT_EQ(t[0].int_suffix.length, SUFFIX_NONE);
    EXPECT_EQ(t[1].int_suffix.length, SUFFIX_L);
    EXPECT_EQ(t[2].int_suffix.length, SUFFIX_LL);
    EXPECT_EQ(t[3].int_suffix.is_unsigned, true);
    EXPECT_EQ(t[3].int_suffix.length, SUFFIX_L);
    EXPECT_EQ(t[4].int_suffix.is_unsigned, true);
    EXPECT_EQ(t[4].int_suffix.length, SUFFIX_LL);
    arena_free(a);
}

TEST(finalize, digraph_brackets)
{
    Arena *a = arena_new();
    LexResult res = lex_text("<% %> <: :>", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 5);
    EXPECT_EQ(t[0].kind, TOK_LBRACE);
    EXPECT_EQ(t[1].kind, TOK_RBRACE);
    EXPECT_EQ(t[2].kind, TOK_LBRACKET);
    EXPECT_EQ(t[3].kind, TOK_RBRACKET);
    arena_free(a);
}

TEST(finalize, stray_hash_is_error)
{
    Arena *a = arena_new();
    EXPECT_NULL(lex_text("a # b", a).tokens);
    EXPECT_NULL(lex_text("a ## b", a).tokens);
    arena_free(a);
}
