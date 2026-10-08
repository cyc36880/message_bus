/**
 * @file test_main.c
 * @brief 测试入口：汇总所有套件、打印结果、返回退出码。
 */
#include <stdio.h>

#include "mb_test.h"

int mb_test_checks = 0;
int mb_test_failures = 0;

static const char *s_current_case = "";

void mb_test_fail_(const char *file, int line, const char *message)
{
    mb_test_failures++;
    printf("    [FAIL] %s (%s:%d): %s\n", s_current_case, file, line, message);
}

void mb_test_fail_int_(const char *file, int line, const char *expr,
                       long long actual, long long expected)
{
    mb_test_failures++;
    printf("    [FAIL] %s (%s:%d): %s\n           actual = %lld, expected = %lld\n",
           s_current_case, file, line, expr, actual, expected);
}

void mb_test_fail_str_(const char *file, int line, const char *expr,
                       const char *actual, const char *expected)
{
    mb_test_failures++;
    printf("    [FAIL] %s (%s:%d): %s\n           actual = \"%s\", expected = \"%s\"\n",
           s_current_case, file, line, expr,
           (actual != NULL) ? actual : "(null)",
           (expected != NULL) ? expected : "(null)");
}

static int run_suite(const mb_test_suite_t *suite)
{
    int before = mb_test_failures;
    size_t i;

    printf("  %s\n", suite->name);
    for (i = 0; i < suite->count; ++i) {
        s_current_case = suite->cases[i].name;
        suite->cases[i].fn();
    }
    return mb_test_failures - before;
}

int main(void)
{
    const mb_test_suite_t *suites[5];
    size_t suite_count = 0;
    size_t i;

    suites[suite_count++] = mb_suite_topic();
    suites[suite_count++] = mb_suite_bus();
    suites[suite_count++] = mb_suite_pubsub();
    suites[suite_count++] = mb_suite_async();
    suites[suite_count++] = mb_suite_threads();

    printf("message_bus unit tests\n");

    for (i = 0; i < suite_count; ++i) {
        (void)run_suite(suites[i]);
    }

    printf("\n%d assertions, %d failed\n", mb_test_checks, mb_test_failures);
    if (mb_test_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("FAILED\n");
    return 1;
}
