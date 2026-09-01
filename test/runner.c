#include "harness.h"

#include <stdlib.h>

static TestCase *head;
static int global_failures;
static int total_tests;
static int current_failures;

void test_register(const char *s, const char *n, const char *f, void (*fn)(void))
{
    TestCase *tc = malloc(sizeof(TestCase));
    tc->suite = s;
    tc->name = n;
    tc->file = f;
    tc->fn = fn;
    tc->next = head;
    head = tc;
}

void test_fail(void)
{
    current_failures++;
}

void test_run_all(void)
{
    TestCase *prev = NULL;
    TestCase *cur = head;
    while (cur)
    {
        TestCase *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    head = prev;

    for (TestCase *tc = head; tc; tc = tc->next)
    {
        current_failures = 0;
        total_tests++;
        printf("RUN  %s.%s ... ", tc->suite, tc->name);
        tc->fn();
        if (current_failures == 0)
        {
            printf("[PASS]\n");
        }
        else
        {
            printf("[FAIL] (%d failures)\n", current_failures);
            global_failures += current_failures;
        }
    }

    printf("\n%d tests run, %d failures\n", total_tests, global_failures);
}

int main(void)
{
    test_run_all();
    return global_failures > 0 ? 1 : 0;
}
