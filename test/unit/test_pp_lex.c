#include "harness.h"
#include "pp_lex.h"
#include "util/arena.h"

static char *prepare(Arena *a, const char *src)
{
    return pp_prepare("<test>", src, a);
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
