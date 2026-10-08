/**
 * @file mb_test.h
 * @brief 极简单元测试框架（约 100 行），不引入任何第三方依赖。
 *
 * 用法：
 * @code
 *   static void test_something(void) {
 *       MB_CHECK(1 + 1 == 2);
 *       MB_CHECK_EQ_INT(mb_topic_level_count("a/b"), 2);
 *   }
 *   static const mb_test_case_t cases[] = { MB_CASE(test_something) };
 *   const mb_test_suite_t *mb_suite_xxx(void) { ... }
 * @endcode
 *
 * 每个测试文件导出一个 mb_suite_* 函数，test_main.c 负责汇总执行，
 * CTest 通过进程返回值判断成败。
 */
#ifndef MB_TEST_H
#define MB_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef void (*mb_test_fn_t)(void);

typedef struct mb_test_case {
    const char *name;
    mb_test_fn_t fn;
} mb_test_case_t;

typedef struct mb_test_suite {
    const char *name;
    const mb_test_case_t *cases;
    size_t count;
} mb_test_suite_t;

extern int mb_test_checks;
extern int mb_test_failures;

void mb_test_fail_(const char *file, int line, const char *message);
void mb_test_fail_int_(const char *file, int line, const char *expr,
                       long long actual, long long expected);
void mb_test_fail_str_(const char *file, int line, const char *expr,
                       const char *actual, const char *expected);

/** 定义一个测试用例（函数体紧随其后）。 */
#define MB_TEST(name) static void name(void)

/** 在用例表中引用一个用例。 */
#define MB_CASE(name) { #name, name }

#define MB_CHECK(expr)                                                      \
    do {                                                                    \
        mb_test_checks++;                                                   \
        if (!(expr)) {                                                      \
            mb_test_fail_(__FILE__, __LINE__, "assertion failed: " #expr);  \
        }                                                                   \
    } while (0)

#define MB_CHECK_INT(actual, expected)                                      \
    do {                                                                    \
        long long mb_a_ = (long long)(actual);                              \
        long long mb_e_ = (long long)(expected);                            \
        mb_test_checks++;                                                   \
        if (mb_a_ != mb_e_) {                                               \
            mb_test_fail_int_(__FILE__, __LINE__, #actual, mb_a_, mb_e_);   \
        }                                                                   \
    } while (0)

#define MB_CHECK_STR(actual, expected)                                      \
    do {                                                                    \
        const char *mb_a_ = (actual);                                       \
        const char *mb_e_ = (expected);                                     \
        mb_test_checks++;                                                   \
        if (mb_a_ == NULL || mb_e_ == NULL || strcmp(mb_a_, mb_e_) != 0) {  \
            mb_test_fail_str_(__FILE__, __LINE__, #actual, mb_a_, mb_e_);   \
        }                                                                   \
    } while (0)

/* 各测试文件导出的套件 */
const mb_test_suite_t *mb_suite_topic(void);
const mb_test_suite_t *mb_suite_bus(void);
const mb_test_suite_t *mb_suite_pubsub(void);
const mb_test_suite_t *mb_suite_threads(void);
const mb_test_suite_t *mb_suite_async(void);

#endif /* MB_TEST_H */
