/**
 * @file test_async.c
 * @brief 异步投递：两级队列、非阻塞 pump、满队列的等待/覆盖策略。
 *
 * 与 test_threads.c 一样，库本身不提供线程 API，这里直接用平台线程驱动。
 */
#include <stdint.h>

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

/* 关掉异步功能时（MB_CONFIG_ASYNC_MAX_TOPICS == 0）全部 _async API 都退化成
 * 返回 MB_ERR_UNSUPPORTED 的桩，这组用例没有意义 —— 整体编译掉，
 * 由文件末尾的 mb_suite_async() 导出一个空套件。 */
#if MB_CONFIG_ASYNC_MAX_TOPICS > 0

/* -------------------------------------------------------------------------
 * 队列深度不是一个固定数字
 *
 * 多条用例都要问「同一个主题里能不能同时积压两条」以及「总的能积压几条」，
 * 答案跟 MB_CONFIG_ASYNC_QUEUE_DEPTH 走。深度 1 是**受支持的配置**
 * （mb_config.h 只要求 >= 1，见那里的 #error），所以这些地方不能写死 2。
 *
 * 写成运行期常量而不是 #if，是为了让两个分支都保持可编译、可断言语义：
 * 深度 1 时「再发一条」不是被跳过，而是被断言为返回 MB_ERR_TIMEOUT ——
 * 那正是满队列账目该有的行为，顺带也验了它。
 * ---------------------------------------------------------------------- */

/** 同一主题内能否同时积压两条（需要深度 >= 2）。 */
#define MB_ASYNC_TWO_PER_TOPIC (MB_CONFIG_ASYNC_QUEUE_DEPTH >= 2)

/** 同一主题在深度之外能多积压几条：深度 >= 2 时是 1，深度 1 时是 0。 */
#define MB_ASYNC_EXTRA_PER_TOPIC (MB_ASYNC_TWO_PER_TOPIC ? 1 : 0)

/* -------------------------------------------------------------------------
 * 最小线程封装（与 test_threads.c 相同；两个文件各自独立编译）
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

static unsigned long current_thread_id(void)
{
    return (unsigned long)GetCurrentThreadId();
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

static unsigned long current_thread_id(void)
{
    return (unsigned long)(uintptr_t)pthread_self();
}
#endif

/* -------------------------------------------------------------------------
 * 测试用记录结构
 * ---------------------------------------------------------------------- */

#define MAX_RECORDS 64

typedef struct {
    int count;
    char topic[MAX_RECORDS][48];
    char payload[MAX_RECORDS][32];
    char source[MAX_RECORDS][24];
    uint32_t ids[MAX_RECORDS];
    bool retained[MAX_RECORDS];
} log_t;

static void log_reset(log_t *log)
{
    memset(log, 0, sizeof(*log));
}

static void log_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    log_t *log = (log_t *)user_data;
    int i = log->count;
    size_t n;

    (void)sub;
    if (i >= MAX_RECORDS) {
        log->count++;
        return;
    }

    snprintf(log->topic[i], sizeof(log->topic[i]), "%s", msg->topic);
    snprintf(log->source[i], sizeof(log->source[i]), "%s",
             (msg->source != NULL) ? msg->source : "");

    n = msg->payload_len;
    if (n >= sizeof(log->payload[i])) {
        n = sizeof(log->payload[i]) - 1;
    }
    if (n > 0 && msg->payload != NULL) {
        memcpy(log->payload[i], msg->payload, n);
    }
    log->payload[i][n] = '\0';

    log->ids[i] = msg->id;
    log->retained[i] = ((msg->flags & MB_MSG_FLAG_RETAINED) != 0);
    log->count++;
}

/** 建总线 + 一个订阅者，订阅 filter。 */
static mb_bus_t *make_bus_with_sink(const char *bus_name, const char *filter,
                                    log_t *log, mb_node_t **out_sink)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;

    MB_CHECK_INT(mb_bus_create(bus_name, &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, filter, log_cb, log, NULL), MB_OK);
    if (out_sink != NULL) {
        *out_sink = sink;
    }
    return bus;
}

/* -------------------------------------------------------------------------
 * 用例：基本投递
 * ---------------------------------------------------------------------- */

MB_TEST(delivery_waits_for_pump)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async1", "a/b", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "hello", 5, NULL, MB_WAIT_NONE), MB_OK);

    /* 关键：publish_async 返回时回调**一次都没跑过** —— 这就是异步的全部意义 */
    MB_CHECK_INT(log.count, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 1);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.payload[0], "hello");
    MB_CHECK_STR(log.topic[0], "a/b");
    MB_CHECK_STR(log.source[0], "pub");
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK(mb_bus_async_topic_count(bus) == 0); /* 空条目被回收 */

    mb_bus_destroy(bus);
}

static unsigned long g_cb_thread;
static unsigned long g_pump_thread;
static unsigned long g_publish_thread;

static void record_thread_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    g_cb_thread = current_thread_id();
    (*(int *)user_data)++;
}

MB_TEST(callback_runs_on_the_pump_thread)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    int count = 0;

    MB_CHECK_INT(mb_bus_create("async2", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "a/b", record_thread_cb, &count, NULL), MB_OK);

    g_cb_thread = 0;
    g_publish_thread = current_thread_id();

    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    MB_CHECK_INT(count, 0); /* 还没跑 */

    g_pump_thread = current_thread_id();
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(count, 1);
    /* 回调跑在 pump 的线程上，而不是发布者的线程上 */
    MB_CHECK(g_cb_thread == g_pump_thread);
    MB_CHECK(g_cb_thread != g_publish_thread || g_pump_thread == g_publish_thread);

    mb_bus_destroy(bus);
}

MB_TEST(pump_on_empty_queue_returns_immediately)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    log_t log;
    uint32_t start;

    log_reset(&log);
    bus = make_bus_with_sink("async3", "#", &log, &sink);

    /* 空队列：pump 不阻塞，直接返回 MB_OK（反复调用也安全） */
    start = mb_os_time_ms();
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK(mb_os_time_ms() - start < 100);

    MB_CHECK_INT(log.count, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：两级队列结构
 * ---------------------------------------------------------------------- */

MB_TEST(topics_queue_independently)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async4", "a/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    /* 往 "a/b" 连发两条，往 "a/c" 发一条。
     * 深度 1 时同主题塞不下第二条，那一发应被正确拒绝 —— 下面断言它的返回码。 */
    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "1", 1, NULL, MB_WAIT_NONE), MB_OK);
    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "2", 1, NULL, MB_WAIT_NONE),
                 MB_ASYNC_TWO_PER_TOPIC ? MB_OK : MB_ERR_TIMEOUT);
    MB_CHECK_INT(mb_node_publish_async(pub, "a/c", "7", 1, NULL, MB_WAIT_NONE), MB_OK);

    /* 两级结构：2 个主题条目，每个条目内各自排队。
     * 深度 1 时 a/b 只进得去一条，"2" 被拒了，所以总量少一条。 */
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 2);
    MB_CHECK_INT(mb_bus_async_pending(bus), 2 + MB_ASYNC_EXTRA_PER_TOPIC);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 2 + MB_ASYNC_EXTRA_PER_TOPIC);

    /* FIFO 保证是**每个主题内部**的，不是全局的：pump 在各主题条目之间轮转，
     * 免得一个刷屏的主题把别的主题饿死。因此这里按主题分别断言顺序。 */
    {
        int b_seen = 0;
        int c_seen = 0;
        uint32_t b_ids[2] = { 0, 0 };
        uint32_t c_id = 0;
        int i;

        for (i = 0; i < log.count; ++i) {
            if (strcmp(log.topic[i], "a/b") == 0) {
                MB_CHECK_STR(log.payload[i], (b_seen == 0) ? "1" : "2");
                b_ids[b_seen] = log.ids[i];
                b_seen++;
            } else if (strcmp(log.topic[i], "a/c") == 0) {
                MB_CHECK_STR(log.payload[i], "7");
                c_id = log.ids[i];
                c_seen++;
            }
        }
        MB_CHECK_INT(b_seen, 1 + MB_ASYNC_EXTRA_PER_TOPIC);
        MB_CHECK_INT(c_seen, 1);

        /* id 在**入队时**确定，不是投递时：投递顺序被轮转打乱了，
         * 但 a/b 先入队的那条 id 仍小于后入队的 a/c。
         * 「两条 a/b 之间递增」要深度 >= 2 才有第二条可断言。 */
        MB_CHECK(b_ids[0] < c_id);
        if (MB_ASYNC_TWO_PER_TOPIC) {
            MB_CHECK(b_ids[0] < b_ids[1]);
            MB_CHECK(b_ids[1] < c_id);
        }
    }

    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(audience_capacity_is_per_topic_not_per_message)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    char topic[16];
    int i;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async5", "t/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    /* 占满全部主题条目，每个只放一条 */
    for (i = 0; i < MB_CONFIG_ASYNC_MAX_TOPICS; ++i) {
        snprintf(topic, sizeof(topic), "t/%d", i);
        MB_CHECK_INT(mb_node_publish_async(pub, topic, "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }
    MB_CHECK_INT(mb_bus_async_topic_count(bus), MB_CONFIG_ASYNC_MAX_TOPICS);

    /* 一个全新的主题：没有条目名额了，不等待就是超时 */
    MB_CHECK_INT(mb_node_publish_async(pub, "t/new", "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_TIMEOUT);

    /* 但**已有**主题不受影响 —— 条目内部还有空位，容量是按主题算的。
     * 深度 1 时条目本身就是满的（只放了 1 条），这一发会被正确拒绝。 */
    MB_CHECK_INT(mb_node_publish_async(pub, "t/0", "y", 1, NULL, MB_WAIT_NONE),
                 MB_ASYNC_TWO_PER_TOPIC ? MB_OK : MB_ERR_TIMEOUT);
    MB_CHECK_INT(mb_bus_async_pending(bus),
                 MB_CONFIG_ASYNC_MAX_TOPICS + MB_ASYNC_EXTRA_PER_TOPIC);

    /* 腾空后条目名额全部归还，可以重新建主题 */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_MAX_TOPICS + MB_ASYNC_EXTRA_PER_TOPIC);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);
    MB_CHECK_INT(mb_node_publish_async(pub, "t/fresh", "z", 1, NULL, MB_WAIT_NONE), MB_OK);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：深拷贝
 * ---------------------------------------------------------------------- */

MB_TEST(payload_and_topic_are_deep_copied)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    char topic[32];
    char payload[32];
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async6", "copy/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    strcpy(topic, "copy/x");
    strcpy(payload, "hello");
    MB_CHECK_INT(mb_node_publish_async(pub, topic, payload, 5, NULL, MB_WAIT_NONE), MB_OK);

    /* 调用方回来就把自己的缓冲区覆盖掉：队列里必须是独立的一份 */
    strcpy(topic, "WRECKED");
    strcpy(payload, "XXXXX");

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.topic[0], "copy/x");
    MB_CHECK_STR(log.payload[0], "hello");

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：队列满
 * ---------------------------------------------------------------------- */

/* 下面三个用例描述的是「队列满且**不允许**覆盖」时的行为：超时丢弃、或者
 * 阻塞等 pump 腾位置。把编译期默认值 MB_CONFIG_ASYNC_OVERWRITE_OLDEST 打开
 * 之后，满队列永远不会阻塞也永远不会超时，这些断言自然不再成立 ——
 * 所以用编译期开关把它们整体摘掉，两种配置下套件都应该是绿的。
 * 「覆盖」这条路径本身由 overwrite_oldest_discards_the_head 覆盖。 */
#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0

MB_TEST(full_queue_times_out_without_waiting)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_stats_t stats;
    log_t log;
    int i;

    log_reset(&log);
    bus = make_bus_with_sink("async7", "q/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "q/a", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }
    /* 第 N+1 条：MB_WAIT_NONE 表示不等待，直接超时丢弃 */
    MB_CHECK_INT(mb_node_publish_async(pub, "q/a", "boom", 4, NULL, MB_WAIT_NONE),
                 MB_ERR_TIMEOUT);
    MB_CHECK_INT(mb_bus_async_pending(bus), MB_CONFIG_ASYNC_QUEUE_DEPTH);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_dropped, 1);
    MB_CHECK_INT(stats.async_enqueued, MB_CONFIG_ASYNC_QUEUE_DEPTH);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_QUEUE_DEPTH);

    mb_bus_destroy(bus);
}

/* 阻塞发布：等 pump 腾出空位后应当成功 */
typedef struct {
    mb_node_t *node;
    const char *topic;
    const char *payload;
    uint32_t timeout_ms;
    mb_err_t result;
    uint32_t elapsed_ms;
} publish_job_t;

static void blocked_publish_job(void *arg)
{
    publish_job_t *job = (publish_job_t *)arg;
    uint32_t start = mb_os_time_ms();

    job->result = mb_node_publish_async(job->node, job->topic, job->payload,
                                        strlen(job->payload), NULL, job->timeout_ms);
    job->elapsed_ms = mb_os_time_ms() - start;
}

MB_TEST(full_queue_blocks_until_pump_frees_space)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    publish_job_t job;
    log_t log;
    int i;

    log_reset(&log);
    bus = make_bus_with_sink("async8", "q/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "q/a", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }

    job.node = pub;
    job.topic = "q/a";
    job.payload = "5";
    job.timeout_ms = MB_WAIT_FOREVER;
    job.result = MB_ERR_TIMEOUT;
    desc.fn = blocked_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    /* 让它确实卡在信号量上（若实现不是阻塞的，这里会立刻返回） */
    mb_os_sleep_ms(80);
    MB_CHECK_INT(job.result, MB_ERR_TIMEOUT); /* 仍在等待：字段还没被写 */
    MB_CHECK_INT(mb_bus_async_pending(bus), MB_CONFIG_ASYNC_QUEUE_DEPTH);

    /* pump 取走消息 → 归还空位 → 发布者被唤醒。
     * 注意 pump 是「一直取到队列空」的循环：它完全可能在被唤醒的发布者入队后
     * 再转一圈，把新消息一并投递掉，所以这里不能断言此刻的投递条数。 */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    MB_CHECK_INT(job.result, MB_OK); /* 等到了空位，没有超时 */

    /* 再 pump 一次兜底，然后才断言最终状态 */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_QUEUE_DEPTH + 1);
    MB_CHECK_STR(log.payload[log.count - 1], "5"); /* 唯一的主题，FIFO 到队尾 */

    mb_bus_destroy(bus);
}

MB_TEST(full_queue_wait_honours_timeout)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    publish_job_t job;
    mb_bus_stats_t stats;
    log_t log;
    int i;

    log_reset(&log);
    bus = make_bus_with_sink("async9", "q/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "q/a", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }

    job.node = pub;
    job.topic = "q/a";
    job.payload = "late";
    job.timeout_ms = 80;
    job.result = MB_OK;
    desc.fn = blocked_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    /* 主线程故意一直不 pump */
    thread_join(&thread);

    MB_CHECK_INT(job.result, MB_ERR_TIMEOUT);
    MB_CHECK(job.elapsed_ms >= 40); /* 确实等过，不是立刻返回 */
    MB_CHECK_INT(mb_bus_async_pending(bus), MB_CONFIG_ASYNC_QUEUE_DEPTH);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_dropped, 1);

    mb_bus_destroy(bus);
}

#endif /* MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0 */

MB_TEST(overwrite_oldest_discards_the_head)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_publish_opts_t opts;
    mb_bus_stats_t stats;
    log_t log;
    int i;

    log_reset(&log);
    bus = make_bus_with_sink("async10", "q/#", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    opts.flags = 0;
    opts.qos = 0;

    /* 放满 0..N-1 */
    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        char payload[8];

        snprintf(payload, sizeof(payload), "%d", i);
        MB_CHECK_INT(mb_node_publish_async(pub, "q/a", payload, strlen(payload), &opts,
                                           MB_WAIT_NONE), MB_OK);
    }

    /* 第 N+1 条带头覆盖：不阻塞、不丢弃新消息，而是扔掉最旧的那条 */
    opts.flags = MB_PUB_FLAG_ASYNC_OVERWRITE;
    MB_CHECK_INT(mb_node_publish_async(pub, "q/a", "99", 2, &opts, MB_WAIT_NONE), MB_OK);

    MB_CHECK_INT(mb_bus_async_pending(bus), MB_CONFIG_ASYNC_QUEUE_DEPTH);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_overwritten, 1);
    MB_CHECK_INT(stats.async_dropped, 0);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_QUEUE_DEPTH);
    /* "0" 被覆盖掉了，队列里剩下的是 1..N-1 再加 "99"。
     * 深度 1 时只剩新消息这一条，没有「1」可断言 —— 那时 payload[0] 就是 "99"，
     * 下面那行会覆盖到它。 */
    if (MB_ASYNC_TWO_PER_TOPIC) {
        MB_CHECK_STR(log.payload[0], "1");
    }
    MB_CHECK_STR(log.payload[MB_CONFIG_ASYNC_QUEUE_DEPTH - 1], "99");

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：retained
 * ---------------------------------------------------------------------- */

MB_TEST(retained_is_updated_at_enqueue_time)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_publish_opts_t opts;
    mb_bus_stats_t stats;
    mb_bus_config_t config;
    log_t log;

    log_reset(&log);
    /* 关掉节点上下线事件：它们也往 retained 表里塞 $mb/nodes/... 条目，
     * 会让下面 retained_stored 的计数不再干净 */
    MB_CHECK_INT(mb_bus_default_config(&config), MB_OK);
    config.publish_node_events = false;
    MB_CHECK_INT(mb_bus_create_ex("async11", &config, &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    opts.flags = MB_PUB_FLAG_RETAIN;
    opts.qos = 0;
    MB_CHECK_INT(mb_node_publish_async(pub, "r/x", "42", 2, &opts, MB_WAIT_NONE), MB_OK);

    /* 还没 pump，retained 就已经生效：新订阅者立刻能拿到 */
    MB_CHECK_INT(mb_node_create(bus, "late", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "r/x", log_cb, &log, NULL), MB_OK);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.payload[0], "42");
    MB_CHECK(log.retained[0]);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.retained_stored, 1);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：订阅语义与同步完全一致
 * ---------------------------------------------------------------------- */

MB_TEST(subscription_semantics_match_sync)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sensor = NULL;
    mb_node_t *other = NULL;
    mb_node_t *sink = NULL;
    mb_subscribe_opts_t sub_opts;
    log_t log;

    log_reset(&log);
    MB_CHECK_INT(mb_bus_create("async12", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sensor0", &sensor), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "other", &other), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);

    /* 通配符 + 来源过滤，与同步订阅是同一套参数 */
    sub_opts.filter = "sensor/+/value";
    sub_opts.source_filter = "sensor0";
    sub_opts.flags = 0;
    sub_opts.user_data = &log;
    MB_CHECK_INT(mb_node_subscribe_ex(sink, &sub_opts, log_cb, NULL), MB_OK);

    /* 三条消息**逐条发布 + 逐条 pump**：这样无论队列深度是多少都成立，
     * 而且每条过滤规则被单独验证，失败时一眼能看出是哪一条漏了。
     * （一次性发三条的话，深度 1 时后两条会因为同主题条目已满而被拒。） */

    /* 主题与来源都匹配 → 投递 */
    MB_CHECK_INT(mb_node_publish_async(sensor, "sensor/1/value", "23", 2, NULL, MB_WAIT_NONE),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.payload[0], "23");
    MB_CHECK_STR(log.source[0], "sensor0");

    /* 主题匹配、来源不匹配 → 被 source_filter 挡掉 */
    MB_CHECK_INT(mb_node_publish_async(other, "sensor/1/value", "no", 2, NULL, MB_WAIT_NONE),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);

    /* 来源匹配、主题不匹配 → 被 filter 挡掉 */
    MB_CHECK_INT(mb_node_publish_async(sensor, "sensor/1/other", "no", 2, NULL, MB_WAIT_NONE),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);

    mb_bus_destroy(bus);
}

MB_TEST(bus_level_publish_has_no_source)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async13", "sys/#", &log, &sink);

    MB_CHECK_INT(mb_bus_publish_async(bus, "sys/x", "v", 1, NULL, MB_WAIT_NONE), MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.source[0], ""); /* source 为 NULL */

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：pump 的独占性
 * ---------------------------------------------------------------------- */

static mb_bus_t *g_nested_bus;
static mb_err_t g_nested_result;

static void pump_from_callback_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (*(int *)user_data)++;
    /* 正在 pump 的过程中再 pump 一次：必须被拒绝，否则回调会被并发进入 */
    g_nested_result = mb_bus_pump(g_nested_bus);
}

MB_TEST(pump_from_callback_is_rejected)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    int count = 0;

    MB_CHECK_INT(mb_bus_create("async14", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "a/b", pump_from_callback_cb, &count, NULL), MB_OK);

    g_nested_bus = bus;
    g_nested_result = MB_OK;

    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(count, 1);
    MB_CHECK_INT(g_nested_result, MB_ERR_BUSY);

    /* 兜底标志必须被清掉：正常路径仍然可用 */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：回调里做「会无限等」的异步发布
 *
 * 能归还主题条目空位的只有 mb_bus_pump() 自己。回调就是在 pump 里跑的，
 * 所以从回调里发一条**无上限等待**的异步消息，等的是一个永远不会来的空位 ——
 * 那是彻底挂死，连超时都报不出来。库必须提前拒绝。
 *
 * 这条用例同时钉住反面：有限超时**不能**被拦。往另一个还有空位的主题发布
 * 根本不会阻塞，一刀切会把这种完全正常的写法一起挡掉。
 * ---------------------------------------------------------------------- */

static mb_node_t *g_cb_pub;
static mb_err_t g_cb_forever;
static mb_err_t g_cb_none;
static mb_err_t g_cb_finite;
static int g_cb_calls;

static void blocking_publish_from_callback_cb(mb_subscription_t *sub,
                                              const mb_message_t *msg,
                                              void *user_data)
{
    (void)sub;
    (void)msg;
    (void)user_data;
    g_cb_calls++;

    g_cb_forever = mb_node_publish_async(g_cb_pub, "cb/forever", "x", 1, NULL, MB_WAIT_FOREVER);
    g_cb_none = mb_node_publish_async(g_cb_pub, "cb/none", "x", 1, NULL, MB_WAIT_NONE);
    g_cb_finite = mb_node_publish_async(g_cb_pub, "cb/finite", "x", 1, NULL, 5);
}

MB_TEST(blocking_publish_from_callback_is_rejected)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_config_t config;
    mb_bus_stats_t stats;

    /* 关掉节点上线事件：否则两次 mb_node_create() 会各发一条没人订阅的
     * 系统消息，把下面按条数核对统计的地方搅浑。 */
    MB_CHECK_INT(mb_bus_default_config(&config), MB_OK);
    config.publish_node_events = false;
    MB_CHECK_INT(mb_bus_create_ex("async14b", &config, &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "a/b", blocking_publish_from_callback_cb, NULL, NULL),
                 MB_OK);

    g_cb_pub = pub;
    g_cb_calls = 0;
    g_cb_forever = MB_OK;
    g_cb_none = MB_OK;
    g_cb_finite = MB_OK;

    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(g_cb_calls, 1);
    MB_CHECK_INT(g_cb_forever, MB_ERR_WOULD_DEADLOCK);
    MB_CHECK_INT(g_cb_none, MB_OK);
    MB_CHECK_INT(g_cb_finite, MB_OK);

    /* 被拒的那条不能留下任何痕迹：既没入队，也不算「丢弃」——
     * 丢弃是「队列满、等超时」的账，这里压根没走到那一步。
     *
     * 注意 pending 是 0 而不是 2：回调里成功入队的两条会被**同一次** pump
     * 顺带取走（pump 一直跑到队列空才返回），所以它们只体现在账目里。 */
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.published, 3);      /* 被拒的那条不算发布 */
    MB_CHECK_INT(stats.async_enqueued, 3); /* a/b + cb/none + cb/finite */
    MB_CHECK_INT(stats.async_dropped, 0);
    MB_CHECK_INT(stats.async_overwritten, 0);
    MB_CHECK_INT(stats.no_subscriber, 2); /* cb/none 与 cb/finite 都没人订阅 */

    /* pump 结束后「正在 pump」的标记必须被清干净：pump 线程之外
     * 用 MB_WAIT_FOREVER 是合法调用，不能被这条新规则误伤。 */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_node_publish_async(pub, "a/z", "x", 1, NULL, MB_WAIT_FOREVER), MB_OK);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：参数校验
 * ---------------------------------------------------------------------- */

MB_TEST(argument_validation)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_publish_opts_t opts;
    mb_bus_stats_t stats;

    MB_CHECK_INT(mb_bus_create("async15", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    MB_CHECK_INT(mb_bus_publish_async(NULL, "a/b", "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_async(NULL, "a/b", "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_async(pub, NULL, "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    /* 发布主题不允许含通配符 */
    MB_CHECK_INT(mb_node_publish_async(pub, "a/+", "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_async(pub, "a/#", "x", 1, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    /* payload 为 NULL 但长度非 0 */
    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", NULL, 4, NULL, MB_WAIT_NONE),
                 MB_ERR_INVALID_ARG);
    /* qos 不被支持 */
    opts.flags = 0;
    opts.qos = 1;
    MB_CHECK_INT(mb_node_publish_async(pub, "a/b", "x", 1, &opts, MB_WAIT_NONE),
                 MB_ERR_UNSUPPORTED);

    /* 全部被拒绝，队列里应当什么都没有 */
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_dropped, 0);

    MB_CHECK_INT(mb_bus_pump(NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_async_pending(NULL), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(NULL), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：多生产者 + 单 pump 线程
 * ---------------------------------------------------------------------- */

#define PRODUCER_THREADS 4
#define ASYNC_MSGS_PER_THREAD 200

static int g_async_delivered;

static void count_atomic_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (void)user_data;
    __atomic_add_fetch(&g_async_delivered, 1, __ATOMIC_SEQ_CST);
}

typedef struct {
    mb_node_t *node;
    int index;
} producer_arg_t;

static void async_producer_job(void *arg)
{
    producer_arg_t *producer = (producer_arg_t *)arg;
    char topic[24];
    int i;

    snprintf(topic, sizeof(topic), "load/%d", producer->index);
    for (i = 0; i < ASYNC_MSGS_PER_THREAD; ++i) {
        /* 永久等待：队列满时生产者被 pump 线程背压（这是默认策略下的行为；
         * 开了覆盖策略则永不阻塞，前面的消息会被后来的顶掉） */
        if (mb_node_publish_async(producer->node, topic, &i, sizeof(i), NULL,
                                  MB_WAIT_FOREVER) != MB_OK) {
            return;
        }
    }
}

/* 生产者全部退出后由主线程置位；pump 线程靠它知道「不会再有新消息了」。 */
static int g_producers_done;

static void pump_job(void *arg)
{
    mb_bus_t *bus = (mb_bus_t *)arg;

    for (;;) {
        int done = __atomic_load_n(&g_producers_done, __ATOMIC_SEQ_CST);

        (void)mb_bus_pump(bus);

        /* done 之后不会再有新消息进来，所以「刚 pump 完且队列为空」就是终点。
         * 不能改成等某个投递条数：开了覆盖策略时会丢消息，永远等不到。 */
        if (done && mb_bus_async_pending(bus) == 0) {
            break;
        }
        mb_os_sleep_ms(1);
    }
}

MB_TEST(concurrent_producers_single_pump)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    mb_node_t *producers[PRODUCER_THREADS] = { NULL };
    producer_arg_t args[PRODUCER_THREADS];
    thread_t threads[PRODUCER_THREADS];
    thread_job_t descs[PRODUCER_THREADS];
    thread_t pump_thread;
    thread_job_t pump_desc;
    char name[16];
    int i;

    MB_CHECK_INT(mb_bus_create("async16", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "load/#", count_atomic_cb, NULL, NULL), MB_OK);

    g_async_delivered = 0;
    g_producers_done = 0;

    /* 先启动生产者：队列深度有限，它们会堵在那个主题上等 pump */
    for (i = 0; i < PRODUCER_THREADS; ++i) {
        snprintf(name, sizeof(name), "pub%d", i);
        MB_CHECK_INT(mb_node_create(bus, name, &producers[i]), MB_OK);
        args[i].node = producers[i];
        args[i].index = i;
        descs[i].fn = async_producer_job;
        descs[i].arg = &args[i];
        MB_CHECK(thread_start(&threads[i], &descs[i]));
    }

    pump_desc.fn = pump_job;
    pump_desc.arg = bus;
    MB_CHECK(thread_start(&pump_thread, &pump_desc));

    for (i = 0; i < PRODUCER_THREADS; ++i) {
        thread_join(&threads[i]);
    }
    __atomic_store_n(&g_producers_done, 1, __ATOMIC_SEQ_CST);
    thread_join(&pump_thread);

    /* 关键断言是「没有崩溃、没有丢内存、并发入队不会破坏二级结构」。
     * 具体条数取决于策略：默认（阻塞等待）下背压保证一条不丢；
     * 开了覆盖策略则必然有消息被顶掉，只要求「不超过总量且投递过」。 */
#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
    MB_CHECK_INT(__atomic_load_n(&g_async_delivered, __ATOMIC_SEQ_CST),
                 PRODUCER_THREADS * ASYNC_MSGS_PER_THREAD);
#else
    {
        int delivered = __atomic_load_n(&g_async_delivered, __ATOMIC_SEQ_CST);

        MB_CHECK(delivered > 0);
        MB_CHECK(delivered <= PRODUCER_THREADS * ASYNC_MSGS_PER_THREAD);
    }
#endif
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：覆盖与阻塞两种生产者混在同一主题上，空位账目不能乱
 *
 * 针对一个真实存在过的 bug：普通生产者在**锁外**领空位令牌、到**锁内**才提交
 * count++，两步之间留了一个「已预定未提交」的窗口，此时
 * space.count == ASYNC_DEPTH - count - 1。覆盖路径原先假设
 * 「count < DEPTH ⇒ 令牌一定拿得到」并在拿不到时断言，于是在这个窗口里
 * 调试构建直接中止、量产构建则让空位账永久漂移（之后无故超时丢消息，
 * 或环形队列写越界）。
 *
 * 触发条件是**两种模式混在同一个主题上**：
 *   - 全用普通模式：没有覆盖路径，走不到那段代码；
 *   - 全用覆盖模式：没人会「预定后慢慢提交」，窗口不成立。
 * 所以这里两个线程发同一个 topic，一个带 OVERWRITE、一个不带，
 * 都用 MB_WAIT_NONE 高频去撞那个窗口。
 *
 * 注意：把 MB_CONFIG_ASYNC_OVERWRITE_OLDEST 设为 1 时两边都变成覆盖模式，
 * 这条用例不再触发上面那个窗口（但它仍然在验账目不变式）。
 * ---------------------------------------------------------------------- */

#define MIXED_THREADS 2
#define MIXED_MSGS_PER_THREAD 2000

typedef struct {
    mb_node_t *node;
    bool overwrite;
} mixed_arg_t;

static void mixed_producer_job(void *arg)
{
    mixed_arg_t *producer = (mixed_arg_t *)arg;
    mb_publish_opts_t opts;
    int i;

    opts.flags = producer->overwrite ? MB_PUB_FLAG_ASYNC_OVERWRITE : 0u;
    opts.qos = 0;

    for (i = 0; i < MIXED_MSGS_PER_THREAD; ++i) {
        /* 不阻塞：普通那一路会被拒（本次发布丢弃），覆盖那一路按契约必须成功 */
        (void)mb_node_publish_async(producer->node, "mix/x", &i, sizeof(i),
                                    &opts, MB_WAIT_NONE);
    }
}

MB_TEST(mixed_modes_share_a_topic_without_corrupting_the_ledger)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    mb_node_t *pubs[MIXED_THREADS] = { NULL };
    mixed_arg_t args[MIXED_THREADS];
    thread_t threads[MIXED_THREADS];
    thread_job_t descs[MIXED_THREADS];
    thread_t pump_thread;
    thread_job_t pump_desc;
    mb_bus_stats_t stats;
    char name[16];
    int i;

    MB_CHECK_INT(mb_bus_create("async17", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "mix/#", count_atomic_cb, NULL, NULL), MB_OK);

    g_async_delivered = 0;
    g_producers_done = 0;

    for (i = 0; i < MIXED_THREADS; ++i) {
        snprintf(name, sizeof(name), "mixpub%d", i);
        MB_CHECK_INT(mb_node_create(bus, name, &pubs[i]), MB_OK);
        args[i].node = pubs[i];
        args[i].overwrite = (i != 0); /* 0 号普通、1 号覆盖 */
        descs[i].fn = mixed_producer_job;
        descs[i].arg = &args[i];
        MB_CHECK(thread_start(&threads[i], &descs[i]));
    }

    pump_desc.fn = pump_job;
    pump_desc.arg = bus;
    MB_CHECK(thread_start(&pump_thread, &pump_desc));

    for (i = 0; i < MIXED_THREADS; ++i) {
        thread_join(&threads[i]);
    }
    __atomic_store_n(&g_producers_done, 1, __ATOMIC_SEQ_CST);
    thread_join(&pump_thread);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);

    /* 端到端账目：队列已排空，所以每条入过队的消息最终只有两种归宿 ——
     * 被 pump 投递出去，或被后来的覆盖顶掉。这两个数必须严丝合缝。
     * 空位账一旦漂移，这里立刻对不上（不管是多算了还是少算了）。 */
    MB_CHECK_INT((long long)stats.async_enqueued,
                 (long long)__atomic_load_n(&g_async_delivered, __ATOMIC_SEQ_CST) +
                     (long long)stats.async_overwritten);

    /* 每条尝试要么进了队、要么被如实计入丢弃，不能既没进队也不记账 */
    MB_CHECK_INT((long long)stats.async_enqueued + (long long)stats.async_dropped,
                 (long long)MIXED_THREADS * MIXED_MSGS_PER_THREAD);

    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：销毁时不能泄漏队列里的消息
 * ---------------------------------------------------------------------- */

MB_TEST(destroy_drains_queued_messages)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    int i;

    MB_CHECK_INT(mb_bus_create("async17", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    /* 攒一堆消息但从不 pump，然后直接销毁：不应崩溃 / 不应泄漏 */
    for (i = 0; i < 5; ++i) {
        char topic[16];

        snprintf(topic, sizeof(topic), "leak/%d", i % 3);
        (void)mb_node_publish_async(pub, topic, "payload", 7, NULL, MB_WAIT_NONE);
    }
    MB_CHECK(mb_bus_async_pending(bus) > 0);

    mb_bus_destroy(bus); /* 在 asan/valgrind 下这条用例才真正说明问题 */
    MB_CHECK(true);
}

/* -------------------------------------------------------------------------
 * 用例：异步发布 + 等待消费（await）
 *
 * 目标不是「消息能送到」，而是「await 返回时回调确实已经跑完」。
 * 因此这几条用例的关键断言都落在**时序**与**线程归属**上：
 *   - 发布方线程在 pump 把消息投递完之前一直不返回；
 *   - 返回时回调次数、内容、执行线程都符合预期。
 * ---------------------------------------------------------------------- */

/** 轮询等待队列里至少出现 at_least 条消息（避免「主线程先 pump、发布方还没入队」）。 */
static bool wait_until_pending(mb_bus_t *bus, size_t at_least, uint32_t timeout_ms)
{
    uint32_t start = mb_os_time_ms();

    while (mb_bus_async_pending(bus) < at_least) {
        if (mb_os_time_ms() - start > timeout_ms) {
            return false;
        }
        mb_os_sleep_ms(1);
    }
    return true;
}

typedef struct {
    mb_node_t *node;
    const char *topic;
    const char *payload;
    uint32_t timeout_ms;
    mb_err_t result;   /**< 初值用 MB_ERR_STATE 当「还没返回」的哨兵 */
    size_t delivered;
    uint32_t elapsed_ms;
    unsigned long thread_id;
} await_job_t;

/** 与 log_cb 相同，额外记录回调跑在哪个线程上。 */
static void log_cb_record_thread(mb_subscription_t *sub, const mb_message_t *msg,
                                 void *user_data)
{
    g_cb_thread = current_thread_id();
    log_cb(sub, msg, user_data);
}

static void await_publish_job(void *arg)
{
    await_job_t *job = (await_job_t *)arg;
    uint32_t start = mb_os_time_ms();

    job->thread_id = current_thread_id();
    job->result = mb_node_publish_async_wait(job->node, job->topic, job->payload,
                                             strlen(job->payload), NULL,
                                             job->timeout_ms, &job->delivered);
    job->elapsed_ms = mb_os_time_ms() - start;
}

MB_TEST(await_blocks_until_pump_delivers)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    log_t log;

    log_reset(&log);
    MB_CHECK_INT(mb_bus_create("async18", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "a/b", log_cb_record_thread, &log, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "a/b";
    job.payload = "hello";
    job.timeout_ms = 5000;
    job.result = MB_ERR_STATE;
    job.delivered = 999;
    job.elapsed_ms = 0;
    job.thread_id = 0;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    /* 消息确实已经入队，但还没有任何线程 pump 它 */
    MB_CHECK(wait_until_pending(bus, 1, 2000));
    mb_os_sleep_ms(30);
    MB_CHECK_INT(job.result, MB_ERR_STATE); /* 仍在阻塞：字段还没被写 */
    MB_CHECK_INT(log.count, 0);

    g_cb_thread = 0;
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    /* 关键：await 返回时回调已经执行完毕 */
    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.delivered, 1);
    MB_CHECK(job.elapsed_ms >= 20);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.payload[0], "hello");
    MB_CHECK_STR(log.source[0], "pub");

    /* 回调跑在 pump（主）线程上，而不是发布方线程上 */
    MB_CHECK(g_cb_thread == current_thread_id());
    MB_CHECK(g_cb_thread != job.thread_id);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(await_waits_for_all_matching_subscribers)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink_a = NULL;
    mb_node_t *sink_b = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    log_t log_a;
    log_t log_b;

    log_reset(&log_a);
    log_reset(&log_b);

    MB_CHECK_INT(mb_bus_create("async18c", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink_a", &sink_a), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink_b", &sink_b), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink_a, "multi/x", log_cb, &log_a, NULL), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink_b, "multi/x", log_cb, &log_b, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "multi/x";
    job.payload = "v";
    job.timeout_ms = 5000;
    job.result = MB_ERR_STATE;
    job.delivered = 0;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    MB_CHECK(wait_until_pending(bus, 1, 2000));
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    /* 「消费」以整条消息为单位：返回时两个订阅者的回调都该跑完 */
    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.delivered, 2);
    MB_CHECK_INT(log_a.count, 1);
    MB_CHECK_INT(log_b.count, 1);

    mb_bus_destroy(bus);
}

MB_TEST(await_forever_returns_after_delivery)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async18b", "a/b", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "a/b";
    job.payload = "forever";
    job.timeout_ms = MB_WAIT_FOREVER;
    job.result = MB_ERR_STATE;
    job.delivered = 999;
    job.elapsed_ms = 0;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    MB_CHECK(wait_until_pending(bus, 1, 2000));
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.delivered, 1);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.payload[0], "forever");

    mb_bus_destroy(bus);
}

MB_TEST(await_times_out_and_cancels_pending_message)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_stats_t stats;
    size_t delivered = 99;
    uint32_t start;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async19", "t/x", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    /* 没有任何线程 pump：等满超时后应当撤销这条消息，而不是把它留在队列里
     * 等着「迟到的投递」。 */
    start = mb_os_time_ms();
    MB_CHECK_INT(mb_node_publish_async_wait(pub, "t/x", "v", 1, NULL, 100, &delivered),
                 MB_ERR_TIMEOUT);
    MB_CHECK(mb_os_time_ms() - start >= 50);
    MB_CHECK_INT(delivered, 0);
    MB_CHECK_INT(log.count, 0);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_enqueued, 1);
    MB_CHECK_INT(stats.async_canceled, 1);
    MB_CHECK_INT(stats.async_dropped, 0);

    /* 撤销是**当场**完成的：消息不再占着主题空位，也不需要等 pump 来清 */
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(await_with_zero_timeout_is_plain_async_publish)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    size_t delivered = 42;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async20", "z/x", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    /* MB_WAIT_NONE = 只入队，不等 —— 与普通异步发布完全一致 */
    MB_CHECK_INT(mb_node_publish_async_wait(pub, "z/x", "v", 1, NULL, MB_WAIT_NONE,
                                            &delivered),
                 MB_OK);
    MB_CHECK_INT(delivered, 0);
    MB_CHECK_INT(log.count, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 1);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, 1);

    mb_bus_destroy(bus);
}

MB_TEST(await_completes_without_any_subscriber)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    mb_bus_stats_t stats;

    MB_CHECK_INT(mb_bus_create("async21", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "ghost/x";
    job.payload = "nobody";
    job.timeout_ms = 5000;
    job.result = MB_ERR_STATE;
    job.delivered = 999;
    job.elapsed_ms = 0;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    MB_CHECK(wait_until_pending(bus, 1, 2000));
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    /* 「消费」的完整含义是 pump 处理完这条消息，而不是「必须有人订阅」 */
    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.delivered, 0);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    /* no_subscriber = 创建节点时的上线事件 1 条 + 本次 ghost/x 1 条 */
    MB_CHECK_INT(stats.no_subscriber, 2);

    mb_bus_destroy(bus);
}

MB_TEST(await_to_variant_builds_target_topic)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async22", "sink/a", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "sink/a";
    job.payload = "v";
    job.timeout_ms = 5000;
    job.result = MB_ERR_STATE;
    job.delivered = 999;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    MB_CHECK(wait_until_pending(bus, 1, 2000));
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    thread_join(&thread);

    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.delivered, 1);
    MB_CHECK_INT(log.count, 1);
    MB_CHECK_STR(log.topic[0], "sink/a");

    mb_bus_destroy(bus);
}

/* 双向握手：发布方每轮都等到「回调已经把值写进去」才读它。
 * 若 await 提前返回，g_handshake_value 会落后于 i，mismatch 立刻增长。 */
#define HANDSHAKE_ROUNDS 100

static volatile int g_handshake_value;
static volatile int g_handshake_delivered;
static int g_handshake_finished;

static void handshake_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    int value;

    (void)sub;
    (void)user_data;
    memcpy(&value, msg->payload, sizeof(value));
    __atomic_add_fetch(&g_handshake_delivered, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_handshake_value, value, __ATOMIC_SEQ_CST);
}

typedef struct {
    mb_node_t *node;
    int rounds;
    mb_err_t result;
    int mismatches;
} handshake_job_t;

static void handshake_publish_job(void *arg)
{
    handshake_job_t *job = (handshake_job_t *)arg;
    int i;

    job->result = MB_OK;
    job->mismatches = 0;

    for (i = 0; i < job->rounds; ++i) {
        size_t delivered = 0;
        mb_err_t err = mb_node_publish_async_wait(job->node, "hs/set", &i, sizeof(i),
                                                  NULL, 5000, &delivered);

        if (err != MB_OK || delivered != 1) {
            job->result = err;
            break;
        }
        /* await 已经保证回调返回；这里读到的必须是本轮写入的值 */
        if (__atomic_load_n(&g_handshake_value, __ATOMIC_SEQ_CST) != i) {
            job->mismatches++;
        }
    }

    __atomic_store_n(&g_handshake_finished, 1, __ATOMIC_SEQ_CST);
}

MB_TEST(await_handshakes_two_threads_round_by_round)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    mb_node_t *pub = NULL;
    thread_t thread;
    thread_job_t desc;
    handshake_job_t job;

    MB_CHECK_INT(mb_bus_create("async23", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "hs/set", handshake_cb, NULL, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    __atomic_store_n(&g_handshake_value, -1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_handshake_delivered, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_handshake_finished, 0, __ATOMIC_SEQ_CST);

    job.node = pub;
    job.rounds = HANDSHAKE_ROUNDS;
    job.result = MB_ERR_STATE;
    job.mismatches = -1;
    desc.fn = handshake_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    for (;;) {
        mb_bus_pump(bus);

        if (__atomic_load_n(&g_handshake_finished, __ATOMIC_SEQ_CST)) {
            mb_bus_pump(bus); /* 把最后一轮可能残留的队列收干净 */
            break;
        }
        mb_os_sleep_ms(1);
    }
    thread_join(&thread);

    MB_CHECK_INT(job.result, MB_OK);
    MB_CHECK_INT(job.mismatches, 0);
    MB_CHECK_INT(__atomic_load_n(&g_handshake_delivered, __ATOMIC_SEQ_CST),
                 HANDSHAKE_ROUNDS);
    MB_CHECK_INT(__atomic_load_n(&g_handshake_value, __ATOMIC_SEQ_CST),
                 HANDSHAKE_ROUNDS - 1);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：多生产者并发 await（回执生命周期与引用计数的压力测试）
 * ---------------------------------------------------------------------- */

#define AWAIT_STRESS_THREADS 4
#define AWAIT_STRESS_ROUNDS 50

static volatile int g_await_stress_delivered;
static volatile int g_await_stress_done;

static void await_stress_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (void)user_data;
    __atomic_add_fetch(&g_await_stress_delivered, 1, __ATOMIC_SEQ_CST);
}

typedef struct {
    mb_node_t *node;
    int index;
    int ok;
    int failed;
} await_stress_arg_t;

static void await_stress_job(void *arg)
{
    await_stress_arg_t *job = (await_stress_arg_t *)arg;
    char topic[32];
    int i;

    snprintf(topic, sizeof(topic), "stress/%d", job->index);
    for (i = 0; i < AWAIT_STRESS_ROUNDS; ++i) {
        size_t delivered = 0;
        mb_err_t err = mb_node_publish_async_wait(job->node, topic, &i, sizeof(i),
                                                  NULL, 5000, &delivered);

        if (err == MB_OK && delivered == 1) {
            job->ok++;
        } else {
            job->failed++;
        }
    }
    __atomic_add_fetch(&g_await_stress_done, 1, __ATOMIC_SEQ_CST);
}

MB_TEST(await_handles_concurrent_producers)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sink = NULL;
    mb_node_t *pubs[AWAIT_STRESS_THREADS] = { NULL };
    await_stress_arg_t args[AWAIT_STRESS_THREADS];
    thread_t threads[AWAIT_STRESS_THREADS];
    thread_job_t descs[AWAIT_STRESS_THREADS];
    char name[16];
    int i;
    int ok = 0;
    int failed = 0;

    MB_CHECK_INT(mb_bus_create("async23b", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "stress/#", await_stress_cb, NULL, NULL), MB_OK);

    __atomic_store_n(&g_await_stress_delivered, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_await_stress_done, 0, __ATOMIC_SEQ_CST);

    for (i = 0; i < AWAIT_STRESS_THREADS; ++i) {
        snprintf(name, sizeof(name), "stresspub%d", i);
        MB_CHECK_INT(mb_node_create(bus, name, &pubs[i]), MB_OK);
        args[i].node = pubs[i];
        args[i].index = i;
        args[i].ok = 0;
        args[i].failed = 0;
        descs[i].fn = await_stress_job;
        descs[i].arg = &args[i];
        MB_CHECK(thread_start(&threads[i], &descs[i]));
    }

    /* 主线程是唯一 pump：生产者的 await 全都靠它推进 */
    for (;;) {
        mb_bus_pump(bus);

        if (__atomic_load_n(&g_await_stress_done, __ATOMIC_SEQ_CST) ==
                AWAIT_STRESS_THREADS &&
            mb_bus_async_pending(bus) == 0) {
            break;
        }
        mb_os_sleep_ms(1);
    }

    for (i = 0; i < AWAIT_STRESS_THREADS; ++i) {
        thread_join(&threads[i]);
        ok += args[i].ok;
        failed += args[i].failed;
    }

    MB_CHECK_INT(ok, AWAIT_STRESS_THREADS * AWAIT_STRESS_ROUNDS);
    MB_CHECK_INT(failed, 0);
    MB_CHECK_INT(__atomic_load_n(&g_await_stress_delivered, __ATOMIC_SEQ_CST),
                 AWAIT_STRESS_THREADS * AWAIT_STRESS_ROUNDS);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：pump 回调里等待消费必然失败
 * ---------------------------------------------------------------------- */

static mb_node_t *g_await_pub;
static mb_err_t g_await_from_cb_finite;
static mb_err_t g_await_from_cb_forever;
static mb_err_t g_await_from_cb_none;

static void await_from_callback_cb(mb_subscription_t *sub, const mb_message_t *msg,
                                   void *user_data)
{
    size_t delivered = 0;

    (void)sub;
    (void)msg;
    (void)user_data;

    g_await_from_cb_finite =
        mb_node_publish_async_wait(g_await_pub, "wc/finite", "x", 1, NULL, 10, &delivered);
    g_await_from_cb_forever =
        mb_node_publish_async_wait(g_await_pub, "wc/forever", "x", 1, NULL,
                                   MB_WAIT_FOREVER, &delivered);
    g_await_from_cb_none =
        mb_node_publish_async_wait(g_await_pub, "wc/none", "x", 1, NULL, MB_WAIT_NONE,
                                   &delivered);
}

MB_TEST(await_from_pump_callback_is_rejected)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_stats_t stats;

    MB_CHECK_INT(mb_bus_create("async24", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "wc/trigger", await_from_callback_cb, NULL, NULL),
                 MB_OK);

    g_await_pub = pub;
    g_await_from_cb_finite = MB_OK;
    g_await_from_cb_forever = MB_OK;
    g_await_from_cb_none = MB_OK;

    MB_CHECK_INT(mb_node_publish_async(pub, "wc/trigger", "x", 1, NULL, MB_WAIT_NONE),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    /* 有限超时和无限等待都不可能成功：消费这条消息的 pump 正卡在回调里。
     * 库直接拒绝，而不是让发布方白等一个超时周期。 */
    MB_CHECK_INT(g_await_from_cb_finite, MB_ERR_WOULD_DEADLOCK);
    MB_CHECK_INT(g_await_from_cb_forever, MB_ERR_WOULD_DEADLOCK);

    /* MB_WAIT_NONE 只是入队，不受限制 */
    MB_CHECK_INT(g_await_from_cb_none, MB_OK);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    /* published = 2 条节点上线事件 + trigger + wc/none */
    MB_CHECK_INT(stats.published, 4);
    MB_CHECK_INT(stats.async_enqueued, 2);
    MB_CHECK_INT(stats.async_canceled, 0);
    MB_CHECK_INT(stats.async_dropped, 0);

    /* 拒绝不能把 pump 标记弄脏：之后仍可正常 pump */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    mb_bus_destroy(bus);
}

/* -------------------------------------------------------------------------
 * 用例：await 的覆盖相关行为
 * ---------------------------------------------------------------------- */

MB_TEST(await_rejects_explicit_overwrite_flag)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_publish_opts_t opts;
    size_t delivered = 7;

    MB_CHECK_INT(mb_bus_create("async25", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    opts.flags = MB_PUB_FLAG_ASYNC_OVERWRITE;
    opts.qos = 0;
    MB_CHECK_INT(mb_node_publish_async_wait(pub, "ow/x", "v", 1, &opts, 100, &delivered),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(delivered, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
MB_TEST(await_reports_drop_when_overwritten_by_another_publisher)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    thread_t thread;
    thread_job_t desc;
    await_job_t job;
    mb_publish_opts_t opts;
    mb_bus_stats_t stats;
    int i;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async26", "ov/a", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    job.node = pub;
    job.topic = "ov/a";
    job.payload = "victim";
    job.timeout_ms = 5000;
    job.result = MB_ERR_STATE;
    job.delivered = 99;
    desc.fn = await_publish_job;
    desc.arg = &job;
    MB_CHECK(thread_start(&thread, &desc));

    MB_CHECK(wait_until_pending(bus, 1, 2000));

    /* 把该主题剩余的队列位置填满（await 消息是最旧的一条），
     * 再用覆盖策略发一条 —— 被顶掉的恰好是正在等待的那条。 */
    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH - 1; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "ov/a", "fill", 4, NULL, MB_WAIT_NONE),
                     MB_OK);
    }
    opts.flags = MB_PUB_FLAG_ASYNC_OVERWRITE;
    opts.qos = 0;
    MB_CHECK_INT(mb_node_publish_async(pub, "ov/a", "new", 3, &opts, MB_WAIT_NONE), MB_OK);

    thread_join(&thread);

    /* 不能假装「消费成功」，也不能让发布方白等到 5 秒超时 */
    MB_CHECK_INT(job.result, MB_ERR_DROPPED);
    MB_CHECK_INT(job.delivered, 0);
    MB_CHECK(job.elapsed_ms < 2000);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_overwritten, 1);
    MB_CHECK_INT(stats.async_canceled, 0);

    /* 剩下的是 D-1 条 fill + 1 条 new */
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_QUEUE_DEPTH);

    mb_bus_destroy(bus);
}

MB_TEST(await_full_queue_honours_timeout)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_stats_t stats;
    size_t delivered = 9;
    uint32_t start;
    int i;
    log_t log;

    log_reset(&log);
    bus = make_bus_with_sink("async27", "qf/a", &log, &sink);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "qf/a", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }

    /* 队列满：总超时预算先花在等空位上，等不到就丢弃，连回执都不该留下 */
    start = mb_os_time_ms();
    MB_CHECK_INT(mb_node_publish_async_wait(pub, "qf/a", "late", 4, NULL, 100, &delivered),
                 MB_ERR_TIMEOUT);
    MB_CHECK(mb_os_time_ms() - start >= 50);
    MB_CHECK_INT(delivered, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), MB_CONFIG_ASYNC_QUEUE_DEPTH);

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_dropped, 1);
    MB_CHECK_INT(stats.async_canceled, 0);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.count, MB_CONFIG_ASYNC_QUEUE_DEPTH);

    mb_bus_destroy(bus);
}
#endif /* MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0 */

/* -------------------------------------------------------------------------
 * 用例：非阻塞推送 + 完成回调（notify）
 *
 * 与 await 的关键区别：推送函数返回时**回调一定还没跑**，而且推送耗时
 * 不随队列/超时增长；结果由 mb_bus_pump() 在 pump 线程上回调。
 * ---------------------------------------------------------------------- */

#define NOTIFY_MAX_CALLS 4

typedef struct {
    int calls;
    bool success[NOTIFY_MAX_CALLS];
    mb_err_t status[NOTIFY_MAX_CALLS];
    size_t delivered[NOTIFY_MAX_CALLS];
    uint32_t elapsed_since_publish[NOTIFY_MAX_CALLS];
    uint32_t requested_timeout_ms[NOTIFY_MAX_CALLS];
    char topic[NOTIFY_MAX_CALLS][48];
    char payload[NOTIFY_MAX_CALLS][32];
    const mb_node_t *node[NOTIFY_MAX_CALLS];
    const mb_bus_t *bus[NOTIFY_MAX_CALLS];
    unsigned long thread;
} notify_log_t;

static void notify_record_cb(const mb_async_result_t *result, void *user_data)
{
    notify_log_t *log = (notify_log_t *)user_data;
    int i = log->calls;
    size_t n;

    log->thread = current_thread_id();
    if (i < NOTIFY_MAX_CALLS) {
        log->success[i] = result->success;
        log->status[i] = result->status;
        log->delivered[i] = result->delivered;
        log->elapsed_since_publish[i] = result->elapsed_since_publish_ms;
        log->requested_timeout_ms[i] = result->requested_timeout_ms;
        log->node[i] = result->node;
        log->bus[i] = result->bus;
        snprintf(log->topic[i], sizeof(log->topic[i]), "%s",
                 (result->topic != NULL) ? result->topic : "");

        n = result->payload_len;
        if (n >= sizeof(log->payload[i])) {
            n = sizeof(log->payload[i]) - 1;
        }
        if (n > 0 && result->payload != NULL) {
            memcpy(log->payload[i], result->payload, n);
        }
        log->payload[i][n] = '\0';
    }
    log->calls++;
}

MB_TEST(notify_reports_success_from_pump_thread)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    notify_log_t log;
    uint32_t start;
    uint32_t publish_ms;

    memset(&log, 0, sizeof(log));

    MB_CHECK_INT(mb_bus_create("notify1", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "n/x", count_atomic_cb, NULL, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    g_async_delivered = 0;

    start = mb_os_time_ms();
    MB_CHECK_INT(mb_node_publish_async_notify(pub, "n/x", "hello", 5, NULL, 1000,
                                              notify_record_cb, &log),
                 MB_OK);
    publish_ms = mb_os_time_ms() - start;

    /* 立即返回，回调一次都还没跑 */
    MB_CHECK(publish_ms < 100);
    MB_CHECK_INT(log.calls, 0);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(log.calls, 1);
    MB_CHECK(log.success[0]);
    MB_CHECK_INT(log.status[0], MB_OK);
    MB_CHECK_INT(log.delivered[0], 1);
    MB_CHECK_INT(log.requested_timeout_ms[0], 1000);
    MB_CHECK_STR(log.topic[0], "n/x");
    MB_CHECK_STR(log.payload[0], "hello");
    MB_CHECK(log.node[0] == pub);
    MB_CHECK(log.bus[0] == bus);
    MB_CHECK(log.thread == current_thread_id()); /* 回调在 pump 线程上 */
    MB_CHECK_INT(g_async_delivered, 1);

    /* 总线级版本：没有发布者节点，result->node 为 NULL */
    MB_CHECK_INT(mb_bus_publish_async_notify(bus, "n/x", "busmsg", 6, NULL, 1000,
                                             notify_record_cb, &log),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.calls, 2);
    MB_CHECK(log.success[1]);
    MB_CHECK_INT(log.status[1], MB_OK);
    MB_CHECK_INT(log.delivered[1], 1);
    MB_CHECK(log.node[1] == NULL);
    MB_CHECK_STR(log.payload[1], "busmsg");
    MB_CHECK_INT(g_async_delivered, 2);

    mb_bus_destroy(bus);
}

MB_TEST(notify_timeout_fires_on_next_pump)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_bus_stats_t stats;
    notify_log_t log;

    memset(&log, 0, sizeof(log));
    MB_CHECK_INT(mb_bus_create("notify2", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    MB_CHECK_INT(mb_node_publish_async_notify(pub, "ghost/x", "boom", 4, NULL, 60,
                                              notify_record_cb, &log),
                 MB_OK);
    MB_CHECK_INT(log.calls, 0);

    /* 没有任何线程 pump：超时不会自己响 —— 必须由下一次 pump 触发 */
    mb_os_sleep_ms(120);
    MB_CHECK_INT(log.calls, 0);
    MB_CHECK_INT(mb_bus_async_pending(bus), 1);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.calls, 1);
    MB_CHECK(!log.success[0]);
    MB_CHECK_INT(log.status[0], MB_ERR_TIMEOUT);
    MB_CHECK_INT(log.delivered[0], 0);
    MB_CHECK(log.elapsed_since_publish[0] >= 60);
    MB_CHECK_STR(log.topic[0], "ghost/x");
    MB_CHECK_STR(log.payload[0], "boom");
    MB_CHECK(log.node[0] == pub);

    /* 超时后消息已被摘除，不会在之后迟到投递 */
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.async_canceled, 1);

    mb_bus_destroy(bus);
}

typedef struct {
    notify_log_t log;
    int retries;
    mb_err_t retry_err;
    bool retried;
} notify_retry_ctx_t;

static void notify_retry_cb(const mb_async_result_t *result, void *user_data)
{
    notify_retry_ctx_t *ctx = (notify_retry_ctx_t *)user_data;

    notify_record_cb(result, &ctx->log);

    if (!ctx->retried && result->status == MB_ERR_TIMEOUT) {
        ctx->retried = true;
        ctx->retries++;

        /* 用回调拿到的原始信息直接重试 —— 这正是失败回调存在的意义 */
        ctx->retry_err = mb_node_publish_async_notify(result->node, result->topic,
                                                      result->payload, result->payload_len,
                                                      &result->opts, 1000,
                                                      notify_retry_cb, ctx);
    }
}

MB_TEST(notify_no_subscriber_is_reported_immediately)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    notify_log_t log;

    memset(&log, 0, sizeof(log));
    MB_CHECK_INT(mb_bus_create("notify2b", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    MB_CHECK_INT(mb_node_publish_async_notify(pub, "nobody/x", "v", 1, NULL, 1000,
                                              notify_record_cb, &log),
                 MB_OK);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    /* 有 pump、但没人订阅：这是「已处理但没消费」，不是超时 */
    MB_CHECK_INT(log.calls, 1);
    MB_CHECK(!log.success[0]);
    MB_CHECK_INT(log.status[0], MB_ERR_NO_SUBSCRIBER);
    MB_CHECK_INT(log.delivered[0], 0);
    MB_CHECK_INT(log.requested_timeout_ms[0], 1000);
    MB_CHECK(log.elapsed_since_publish[0] < 1000);
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(notify_retry_from_callback_succeeds)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    notify_retry_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.retry_err = MB_ERR_STATE;

    MB_CHECK_INT(mb_bus_create("notify3", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "retry/x", count_atomic_cb, NULL, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    g_async_delivered = 0;

    MB_CHECK_INT(mb_node_publish_async_notify(pub, "retry/x", "cmd", 3, NULL, 60,
                                              notify_retry_cb, &ctx),
                 MB_OK);
    MB_CHECK_INT(ctx.log.calls, 0);

    /* 故意拖过超时：第一次回调应当是 TIMEOUT，并在里面重试 */
    mb_os_sleep_ms(120);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(ctx.retries, 1);
    MB_CHECK_INT(ctx.retry_err, MB_OK);
    MB_CHECK(ctx.retried);
    MB_CHECK_INT(ctx.log.calls, 2);

    MB_CHECK(!ctx.log.success[0]);
    MB_CHECK_INT(ctx.log.status[0], MB_ERR_TIMEOUT);
    MB_CHECK_INT(ctx.log.delivered[0], 0);

    /* 第二次：重试的消息被 sink 消化，回调报告成功 */
    MB_CHECK(ctx.log.success[1]);
    MB_CHECK_INT(ctx.log.status[1], MB_OK);
    MB_CHECK_INT(ctx.log.delivered[1], 1);
    MB_CHECK_STR(ctx.log.topic[1], "retry/x");
    MB_CHECK_STR(ctx.log.payload[1], "cmd");
    MB_CHECK_INT(g_async_delivered, 1);

    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(mb_bus_async_topic_count(bus), 0);

    mb_bus_destroy(bus);
}

#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
MB_TEST(notify_enqueue_failure_reports_via_callback)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sink = NULL;
    mb_bus_stats_t stats;
    notify_log_t log;
    uint32_t start;
    uint32_t publish_ms;
    int i;

    memset(&log, 0, sizeof(log));
    MB_CHECK_INT(mb_bus_create("notify4", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sink", &sink), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sink, "full/x", count_atomic_cb, NULL, NULL), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    g_async_delivered = 0;

    for (i = 0; i < MB_CONFIG_ASYNC_QUEUE_DEPTH; ++i) {
        MB_CHECK_INT(mb_node_publish_async(pub, "full/x", "x", 1, NULL, MB_WAIT_NONE), MB_OK);
    }

    /* 队列满：通知推送仍然必须立刻返回，失败由下一个 pump 回调报告 */
    start = mb_os_time_ms();
    MB_CHECK_INT(mb_node_publish_async_notify(pub, "full/x", "cmd", 3, NULL, 500,
                                              notify_record_cb, &log),
                 MB_OK);
    publish_ms = mb_os_time_ms() - start;
    MB_CHECK(publish_ms < 100);
    MB_CHECK_INT(log.calls, 0);

    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);

    MB_CHECK_INT(log.calls, 1);
    MB_CHECK(!log.success[0]);
    MB_CHECK_INT(log.status[0], MB_ERR_TIMEOUT);
    MB_CHECK_INT(log.delivered[0], 0);
    MB_CHECK_STR(log.topic[0], "full/x");
    MB_CHECK_STR(log.payload[0], "cmd");

    /* 被拒绝的只是通知那条；队列里原有的消息照常投递 */
    MB_CHECK_INT(g_async_delivered, MB_CONFIG_ASYNC_QUEUE_DEPTH);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK(stats.async_dropped >= 1);

    mb_bus_destroy(bus);
}
#endif /* MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0 */

MB_TEST(notify_argument_validation)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    notify_log_t log;
    mb_publish_opts_t opts;

    memset(&log, 0, sizeof(log));
    MB_CHECK_INT(mb_bus_create("notify5", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);

    MB_CHECK_INT(mb_node_publish_async_notify(NULL, "a/b", "x", 1, NULL, 100,
                                              notify_record_cb, &log),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_async_notify(pub, NULL, "x", 1, NULL, 100,
                                              notify_record_cb, &log),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_async_notify(pub, "a/b", "x", 1, NULL, 100, NULL, &log),
                 MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_publish_async_notify(NULL, "a/b", "x", 1, NULL, 100,
                                             notify_record_cb, &log),
                 MB_ERR_INVALID_ARG);

    opts.flags = MB_PUB_FLAG_ASYNC_OVERWRITE;
    opts.qos = 0;
    MB_CHECK_INT(mb_node_publish_async_notify(pub, "a/b", "x", 1, &opts, 100,
                                              notify_record_cb, &log),
                 MB_ERR_INVALID_ARG);

    /* 全部被拒，队列里什么都没有，也不会留下待回调通知 */
    MB_CHECK_INT(mb_bus_async_pending(bus), 0);
    MB_CHECK_INT(log.calls, 0);
    MB_CHECK_INT(mb_bus_pump(bus), MB_OK);
    MB_CHECK_INT(log.calls, 0);

    mb_bus_destroy(bus);
}

static const mb_test_case_t cases[] = {
    MB_CASE(delivery_waits_for_pump),
    MB_CASE(callback_runs_on_the_pump_thread),
    MB_CASE(pump_on_empty_queue_returns_immediately),
    MB_CASE(topics_queue_independently),
    MB_CASE(audience_capacity_is_per_topic_not_per_message),
    MB_CASE(payload_and_topic_are_deep_copied),
#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
    MB_CASE(full_queue_times_out_without_waiting),
    MB_CASE(full_queue_blocks_until_pump_frees_space),
    MB_CASE(full_queue_wait_honours_timeout),
#endif
    MB_CASE(overwrite_oldest_discards_the_head),
    MB_CASE(retained_is_updated_at_enqueue_time),
    MB_CASE(subscription_semantics_match_sync),
    MB_CASE(bus_level_publish_has_no_source),
    MB_CASE(pump_from_callback_is_rejected),
    MB_CASE(blocking_publish_from_callback_is_rejected),
    MB_CASE(argument_validation),
    MB_CASE(concurrent_producers_single_pump),
    MB_CASE(mixed_modes_share_a_topic_without_corrupting_the_ledger),
    MB_CASE(destroy_drains_queued_messages),
    MB_CASE(await_blocks_until_pump_delivers),
    MB_CASE(await_waits_for_all_matching_subscribers),
    MB_CASE(await_forever_returns_after_delivery),
    MB_CASE(await_times_out_and_cancels_pending_message),
    MB_CASE(await_with_zero_timeout_is_plain_async_publish),
    MB_CASE(await_completes_without_any_subscriber),
    MB_CASE(await_to_variant_builds_target_topic),
    MB_CASE(await_handshakes_two_threads_round_by_round),
    MB_CASE(await_handles_concurrent_producers),
    MB_CASE(await_from_pump_callback_is_rejected),
    MB_CASE(await_rejects_explicit_overwrite_flag),
#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
    MB_CASE(await_reports_drop_when_overwritten_by_another_publisher),
    MB_CASE(await_full_queue_honours_timeout),
#endif
    MB_CASE(notify_reports_success_from_pump_thread),
    MB_CASE(notify_timeout_fires_on_next_pump),
    MB_CASE(notify_no_subscriber_is_reported_immediately),
    MB_CASE(notify_retry_from_callback_succeeds),
#if MB_CONFIG_ASYNC_OVERWRITE_OLDEST == 0
    MB_CASE(notify_enqueue_failure_reports_via_callback),
#endif
    MB_CASE(notify_argument_validation),
};

#endif /* MB_CONFIG_ASYNC_MAX_TOPICS > 0 */

const mb_test_suite_t *mb_suite_async(void)
{
#if MB_CONFIG_ASYNC_MAX_TOPICS > 0
    static const mb_test_suite_t suite = {
        "async (async delivery)", cases, sizeof(cases) / sizeof(cases[0])
    };

    return &suite;
#else
    static const mb_test_suite_t empty = {
        "async (disabled by MB_CONFIG_ASYNC_MAX_TOPICS=0)", NULL, 0
    };

    return &empty;
#endif
}
