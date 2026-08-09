#ifndef GUARD_TESTS_HARNESS_H
#define GUARD_TESTS_HARNESS_H

#include <stdio.h>

static int test_failures;
static const char *test_case_name = "";

#define TEST_CASE(name) (test_case_name = (name))

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            printf("FAIL %s:%d [%s] ", __FILE__, __LINE__, test_case_name); \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            test_failures++;                                                \
        }                                                                   \
    } while (0)

static int test_report(const char *suite)
{
    printf("%s: %s (%d failures)\n", suite, test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}

#endif // GUARD_TESTS_HARNESS_H
