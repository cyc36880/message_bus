/**
 * @file test_threads.c
 * @brief 多线程安全：并发发布、并发订阅/退订。
 *
 * 说明：库本身不提供线程 API（同步投递模型下不需要），因此这里直接用
 * 平台线程（Windows / POSIX）驱动测试。本文件依赖 GCC/Clang 的原子内建函数。
 */
#include "mb_test.h"
#include "message_bus/message_bus.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define PUB_THREADS 4
#define MSGS_PER_THREAD 500
#define CHURN_ITERATIONS 300

/* -------------------------------------------------------------------------
 * 最小线程封装
 * ---------------------------------------------------------------------- */

typedef struct {
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t handle;
#endif
} thread_t;

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} thread_job_t;

#if defined(_WIN32)
static DWORD WINAPI thread_trampoline(LPVOID param)
{
    thread_job_t *job = (thread_job_t *)param;

    job->fn(job->arg);
    return 0;
}

static bool thread_start(thread_t *thread, thread_job_t *job)
{
    thread->handle = CreateThread(NULL, 0, thread_trampoline, job, 0, NULL);
    return thread->handle != NULL;
}

static void thread_join(thread_t *thread)
{
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
}
#else
static void *thread_trampoline(void *param)
{
    thread_job_t *job = (thread_job_t *)param;

    job->fn(job->arg);
    return NULL;
}

static bool thread_start(thread_t *thread, thread_job_t *job)
{
    return pthread_create(&thread->handle, NULL, thread_trampoline, job) == 0;
}

static void thread_join(thread_t *thread)
{
    pthread_join(thread->handle, NULL);
}
#endif

/* 回调在发布者线程里被同步调用，因此计数器必须是原子的 */
static int g_delivered;

static void count_atomic(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (void)user_data;
    __atomic_add_fetch(&g_delivered, 1, __ATOMIC_SEQ_CST);
}

/* 单线程用例使用的普通计数器 */
static void count_local(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (*(int *)user_data)++;
}

/* -------------------------------------------------------------------------
 * 用例
 * ---------------------------------------------------------------------- */

static void publisher_job(void *arg)
{
    mb_node_t *node = (mb_node_t *)arg;
    int i;

    for (i = 0; i < MSGS_PER_THREAD; ++i) {
        (void)mb_node_publish(node, "load/value", &i, sizeof(i), NULL);
    }
}

MB_TEST(concurrent_publishers)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    mb_node_t *publishers[PUB_THREADS] = { NULL };
    thread_t threads[PUB_THREADS];
    thread_job_t jobs[PUB_THREADS];
    char name[16];
    int i;

    MB_CHECK_INT(mb_bus_create("concurrent", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "load/#", count_atomic, NULL, NULL), MB_OK);

    g_delivered = 0;

    for (i = 0; i < PUB_THREADS; ++i) {
        snprintf(name, sizeof(name), "pub%d", i);
        MB_CHECK_INT(mb_node_create(bus, name, &publishers[i]), MB_OK);
        jobs[i].fn = publisher_job;
        jobs[i].arg = publishers[i];
        MB_CHECK(thread_start(&threads[i], &jobs[i]));
    }
    for (i = 0; i < PUB_THREADS; ++i) {
        thread_join(&threads[i]);
    }

    /* 每条消息对唯一订阅者恰好投递一次，一条不多一条不少 */
    MB_CHECK_INT(__atomic_load_n(&g_delivered, __ATOMIC_SEQ_CST), PUB_THREADS * MSGS_PER_THREAD);

    mb_bus_destroy(bus);
}

static void churn_job(void *arg)
{
    mb_node_t *node = (mb_node_t *)arg;
    int i;

    for (i = 0; i < CHURN_ITERATIONS; ++i) {
        mb_subscription_t *sub = NULL;

        if (mb_node_subscribe(node, "load/#", count_atomic, NULL, &sub) == MB_OK) {
            (void)mb_node_unsubscribe(&sub);
        }
    }
}

static void steady_publisher_job(void *arg)
{
    mb_node_t *node = (mb_node_t *)arg;
    int i;

    for (i = 0; i < CHURN_ITERATIONS * 2; ++i) {
        (void)mb_node_publish(node, "load/value", "v", 1, NULL);
    }
}

MB_TEST(concurrent_subscribe_unsubscribe)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *churner = NULL;
    thread_t churn_thread;
    thread_t pub_thread;
    thread_job_t churn_job_desc;
    thread_job_t pub_job_desc;

    MB_CHECK_INT(mb_bus_create("churn", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "churner", &churner), MB_OK);

    g_delivered = 0;

    churn_job_desc.fn = churn_job;
    churn_job_desc.arg = churner;
    pub_job_desc.fn = steady_publisher_job;
    pub_job_desc.arg = pub;

    MB_CHECK(thread_start(&churn_thread, &churn_job_desc));
    MB_CHECK(thread_start(&pub_thread, &pub_job_desc));

    thread_join(&churn_thread);
    thread_join(&pub_thread);

    /* 关键断言是「没有崩溃 / 没有 use-after-free」：订阅表最终必须为空 */
    MB_CHECK_INT(mb_bus_subscription_count(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(buses_are_independent)
{
    mb_bus_t *bus_a = NULL;
    mb_bus_t *bus_b = NULL;
    mb_node_t *a = NULL;
    mb_node_t *b = NULL;
    int count_a = 0;
    int count_b = 0;

    MB_CHECK_INT(mb_bus_create("a", &bus_a), MB_OK);
    MB_CHECK_INT(mb_bus_create("b", &bus_b), MB_OK);
    MB_CHECK_INT(mb_node_create(bus_a, "n", &a), MB_OK);
    MB_CHECK_INT(mb_node_create(bus_b, "n", &b), MB_OK);

    MB_CHECK_INT(mb_node_subscribe(b, "same/#", count_local, &count_b, NULL), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(a, "same/#", count_local, &count_a, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(a, "same/x", "1", 1, NULL), MB_OK);
    MB_CHECK_INT(count_a, 1);
    MB_CHECK_INT(count_b, 0); /* 另一条总线不会被串扰 */

    MB_CHECK_INT(mb_node_publish(b, "same/x", "2", 1, NULL), MB_OK);
    MB_CHECK_INT(count_a, 1);
    MB_CHECK_INT(count_b, 1);

    mb_bus_destroy(bus_a);
    mb_bus_destroy(bus_b);
}

static const mb_test_case_t cases[] = {
    MB_CASE(concurrent_publishers),
    MB_CASE(concurrent_subscribe_unsubscribe),
    MB_CASE(buses_are_independent),
};

const mb_test_suite_t *mb_suite_threads(void)
{
    static const mb_test_suite_t suite = { "threads (thread safety)", cases, sizeof(cases) / sizeof(cases[0]) };

    return &suite;
}
