#ifndef FICC_TEST_TESTDRIVER_H
#define FICC_TEST_TESTDRIVER_H

#include "ast.h"
#include "harness.h"
#include "ir.h"
#include "util/arena.h"
#include "util/types.h"

IrModule *tc_build_module(const char *src, Arena *arena);

/* lex + parse only (front-end check; no semantic or IR stages). */
ASTNode *tc_parse(const char *src, Arena *arena);

i64 tc_run_interp(const char *src);

int tc_run_elf(const char *src);

/* Like tc_run_elf, but links the ficc-compiled unit against an extra
   translation unit, written from `extra_src` and compiled by the host gcc.
   Used for extern-linkage tests where the referenced symbols live in a
   second object file. */
int tc_run_elf_with_extra_tu(const char *src, const char *extra_src);

int tc_run_shell(const char *cmd);

#define EXPECT_BUILD_FAIL(src)                                                                     \
    do                                                                                             \
    {                                                                                              \
        Arena *tc_arena = arena_new();                                                             \
        EXPECT_TRUE(tc_build_module((src), tc_arena) == NULL);                                     \
        arena_free(tc_arena);                                                                      \
    } while (0)

#define EXPECT_BUILD_SUCCEED(src)                                                                  \
    do                                                                                             \
    {                                                                                              \
        Arena *tc_arena = arena_new();                                                             \
        EXPECT_TRUE(tc_build_module((src), tc_arena) != NULL);                                     \
        arena_free(tc_arena);                                                                      \
    } while (0)

#define EXPECT_PARSE_FAIL(src)                                                                     \
    do                                                                                             \
    {                                                                                              \
        Arena *tc_arena = arena_new();                                                             \
        EXPECT_TRUE(tc_parse((src), tc_arena) == NULL);                                            \
        arena_free(tc_arena);                                                                      \
    } while (0)

#define EXPECT_PARSE_SUCCEED(src)                                                                  \
    do                                                                                             \
    {                                                                                              \
        Arena *tc_arena = arena_new();                                                             \
        EXPECT_TRUE(tc_parse((src), tc_arena) != NULL);                                            \
        arena_free(tc_arena);                                                                      \
    } while (0)

/* Run the program through both the interpreter and the x86 ELF backend and
   require both to produce `expected`. Failing either reports which backend
   diverged. */
#define EXPECT_INTERP_AND_ELF(src, expected)                                                       \
    do                                                                                             \
    {                                                                                              \
        if (tc_run_interp((src)) != (expected))                                                    \
        {                                                                                          \
            fprintf(stderr, "  EXPECT_INTERP_AND_ELF (interp) failed at %s:%d\n", __FILE__,        \
                    __LINE__);                                                                     \
            test_fail();                                                                           \
        }                                                                                          \
        if (tc_run_elf((src)) != (expected))                                                       \
        {                                                                                          \
            fprintf(stderr, "  EXPECT_INTERP_AND_ELF (elf) failed at %s:%d\n", __FILE__,           \
                    __LINE__);                                                                     \
            test_fail();                                                                           \
        }                                                                                          \
    } while (0)

#endif
