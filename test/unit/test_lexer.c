#include "harness.h"
#include <string.h>
#include "lexer.h"
#include "util/arena.h"

TEST(lexer, keywords_and_punct)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "int main(void) { return 42; }", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(n, 11);

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
    u64 n;
    Token *t = lex("t", "return 123;", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(t[0].kind, TOK_KW_RETURN);
    EXPECT_EQ(t[1].kind, TOK_INT_LIT);
    EXPECT_EQ(t[1].payload.int_val, 123);
    arena_free(a);
}

TEST(lexer, identifier_name)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "foo_bar;", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(t[0].kind, TOK_IDENT);
    EXPECT_TRUE(strcmp(t[0].payload.str, "foo_bar") == 0);
    arena_free(a);
}

TEST(lexer, location_tracking)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "int\nmain", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(t[0].loc.line, 1);
    EXPECT_EQ(t[0].loc.col, 1);
    EXPECT_EQ(t[1].loc.line, 2);
    EXPECT_EQ(t[1].loc.col, 1);
    arena_free(a);
}

TEST(lexer, tab_advances_col)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "int\tmain", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(t[1].loc.col, 8);  /* tab from col 4 -> col 9 */
    arena_free(a);
}

TEST(lexer, overflow_is_error)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "999999999999999999999999999999", a, &n);
    EXPECT_TRUE(t == NULL);
    arena_free(a);
}

TEST(lexer, unknown_char_is_error)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "int @;", a, &n);
    EXPECT_TRUE(t == NULL);
    arena_free(a);
}

TEST(lexer, empty_source)
{
    Arena *a = arena_new();
    u64 n;
    Token *t = lex("t", "", a, &n);
    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(t[0].kind, TOK_EOF);
    arena_free(a);
}
