#include "harness.h"
#include "lexer.h"
#include "util/arena.h"

#include <string.h>

TEST(lexer, keywords_and_punct)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "int main(void) { return 42; }", a);
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
    LexResult res = lex("t", "return 123;", a);
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
    LexResult res = lex("t", "foo_bar;", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_STR_EQ(t[0].payload.str, "foo_bar");
    arena_free(a);
}

TEST(lexer, location_tracking)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "int\nmain", a);
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
    LexResult res = lex("t", "int\tmain", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(t[1].loc.col, 8); /* tab from col 4 -> col 9 */
    arena_free(a);
}

TEST(lexer, overflow_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "999999999999999999999999999999", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, unknown_char_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "int @;", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, empty_source)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 1);
    EXPECT_EQ(t[0].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, operators)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "+ - * / % = == != < > <= >= && || ! & | ^ << >> ~ : ,", a);
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

TEST(lexer, block_comment)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "int /* comment */ x;", a);
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
    LexResult res = lex("t", "int /* never ends", a);
    Token *t = res.tokens;
    EXPECT_NULL(t);
    arena_free(a);
}

TEST(lexer, if_else_keywords)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "if (x) else", a);
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
    LexResult res = lex("t", "while for do break continue goto", a);
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
    LexResult res = lex("t", "switch case default", a);
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
    LexResult res = lex("t", "typedef int Foo;", a);
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
    LexResult res = lex("t", "a ? b : c", a);
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
    LexResult res = lex("t", "\"hello\"", a);
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
    LexResult res = lex("t", "\"\"", a);
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
    LexResult res = lex("t", "\"a\\nb\\tc\\\\d\\\"e\\0f\"", a);
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
    LexResult res = lex("t", "\"hello", a);
    EXPECT_NULL(res.tokens);
    EXPECT_EQ(res.count, 0);
    arena_free(a);
}

TEST(lexer, sizeof_keyword)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "sizeof", a);
    Token *t = res.tokens;
    EXPECT_NOTNULL(t);
    EXPECT_EQ(res.count, 2);
    EXPECT_EQ(t[0].kind, TOK_KW_SIZEOF);
    EXPECT_EQ(t[1].kind, TOK_EOF);
    arena_free(a);
}

TEST(lexer, char_literal_value)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "'a'", a);
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
        lex("t", "'\\n' '\\0' '\\t' '\\a' '\\'' '\\\\' '\"' '\\?' '\\v' '\\f' '\\r' '\\b'", a);
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
    LexResult res = lex("t", "'\\0' '\\01' '\\007' '\\101' '\\377'", a);
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
    LexResult res = lex("t", "'\\x41' '\\x0a' '\\x7f'", a);
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
    LexResult res = lex("t", "\"\\0123\"", a);
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
    LexResult res = lex("t", "\"\\01\\x41\"", a);
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
    LexResult res = lex("t", "''", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_multi_char_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "'ab'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_hex_no_digits_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "'\\x'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_out_of_range_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "'\\400'", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}

TEST(lexer, char_literal_unterminated_is_error)
{
    Arena *a = arena_new();
    LexResult res = lex("t", "'a", a);
    EXPECT_NULL(res.tokens);
    arena_free(a);
}
