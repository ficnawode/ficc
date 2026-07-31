#ifndef FICC_TEST_HARNESS_H
#define FICC_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>

typedef struct TestCase
{
    const char *suite;
    const char *name;
    const char *file;
    void (*fn)(void);
    struct TestCase *next;
} TestCase;

void test_register(const char *s, const char *n, const char *f, void (*fn)(void));
void test_run_all(void);
void test_fail(void);

#define TEST(suite, name)                                                                          \
    static void test_##suite##_##name(void);                                                       \
    static void __attribute__((constructor)) test_##suite##_##name##_register(void)                \
    {                                                                                              \
        test_register(#suite, #name, __FILE__, test_##suite##_##name);                             \
    }                                                                                              \
    static void test_##suite##_##name(void)

#define EXPECT_TRUE(cond)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            fprintf(stderr, "  EXPECT_TRUE failed at %s:%d\n", __FILE__, __LINE__);                \
            test_fail();                                                                           \
        }                                                                                          \
    } while (0)

#define EXPECT_FALSE(cond)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if ((cond))                                                                                \
        {                                                                                          \
            fprintf(stderr, "  EXPECT_FALSE failed at %s:%d\n", __FILE__, __LINE__);               \
            test_fail();                                                                           \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do                                                                                             \
    {                                                                                              \
        if ((a) != (b))                                                                            \
        {                                                                                          \
            fprintf(stderr, "  EXPECT_EQ failed at %s:%d\n", __FILE__, __LINE__);                  \
            test_fail();                                                                           \
        }                                                                                          \
    } while (0)

#endif
