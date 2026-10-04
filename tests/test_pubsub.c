/**
 * @file test_pubsub.c
 * @brief 发布 / 订阅语义：过滤、retained、回调内的安全操作、递归保护。
 */
#include "mb_test.h"
#include "message_bus/message_bus.h"

/* -------------------------------------------------------------------------
 * 测试用回调与记录结构
 * ---------------------------------------------------------------------- */

typedef struct {
    int count;
    char topic[64];
    char payload[32];
    char source[32];
    size_t payload_len;
    bool retained;
} record_t;

static void record(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    record_t *ctx = (record_t *)user_data;
    size_t n;

    (void)sub;
    ctx->count++;
    snprintf(ctx->topic, sizeof(ctx->topic), "%s", msg->topic);
    snprintf(ctx->source, sizeof(ctx->source), "%s", (msg->source != NULL) ? msg->source : "");

    n = msg->payload_len;
    if (n >= sizeof(ctx->payload)) {
        n = sizeof(ctx->payload) - 1;
    }
    if (n > 0 && msg->payload != NULL) {
        memcpy(ctx->payload, msg->payload, n);
    }
    ctx->payload[n] = '\0';
    ctx->payload_len = msg->payload_len;
    ctx->retained = ((msg->flags & MB_MSG_FLAG_RETAINED) != 0);
}

static void count_only(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)msg;
    (*(int *)user_data)++;
}

static mb_bus_t *make_bus(const char *name)
{
    mb_bus_t *bus = NULL;

    MB_CHECK_INT(mb_bus_create(name, &bus), MB_OK);
    return bus;
}

/* ---- 回调内退订自己：验证引用计数保护 ---- */
static void unsubscribe_self_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    mb_subscription_t *handle = sub; /* 回调拿到的就是自己的订阅句柄 */

    (void)msg;
    (*(int *)user_data)++;
    MB_CHECK_INT(mb_node_unsubscribe(&handle), MB_OK);
    MB_CHECK(handle == NULL);
}

/* ---- 回调内再发布同一主题：验证递归深度保护 ---- */
static int g_handler_calls;
static bool g_busy_seen;

static void republish_cb(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    mb_node_t *node = mb_subscription_node(sub);

    (void)user_data;
    g_handler_calls++;
    if (mb_node_publish(node, msg->topic, "x", 1, NULL) == MB_ERR_BUSY) {
        g_busy_seen = true;
    }
}

/* -------------------------------------------------------------------------
 * 用例
 * ---------------------------------------------------------------------- */

MB_TEST(publish_delivers_topic_payload_and_source)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *sensor = NULL;
    mb_node_t *ui = NULL;
    record_t rec = { 0 };

    MB_CHECK_INT(mb_node_create(bus, "sensor", &sensor), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "ui", &ui), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(ui, "sensor/temp", record, &rec, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(sensor, "sensor/temp", "23.5", 4, NULL), MB_OK);

    /* 同步投递：publish 返回时回调已经执行完毕 */
    MB_CHECK_INT(rec.count, 1);
    MB_CHECK_STR(rec.topic, "sensor/temp");
    MB_CHECK_STR(rec.payload, "23.5");
    MB_CHECK_INT(rec.payload_len, 4);
    MB_CHECK_STR(rec.source, "sensor");

    mb_bus_destroy(bus);
}

MB_TEST(wildcard_filtering)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *sensor = NULL;
    mb_node_t *ui = NULL;
    int single_level = 0;
    int multi_level = 0;

    MB_CHECK_INT(mb_node_create(bus, "sensor", &sensor), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "ui", &ui), MB_OK);

    MB_CHECK_INT(mb_node_subscribe(ui, "sensor/+/temp", count_only, &single_level, NULL), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(ui, "sensor/#", count_only, &multi_level, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(sensor, "sensor/room1/temp", "20", 2, NULL), MB_OK);
    MB_CHECK_INT(mb_node_publish(sensor, "sensor/room1/humidity", "50", 2, NULL), MB_OK);
    MB_CHECK_INT(mb_node_publish(sensor, "sensor", "x", 1, NULL), MB_OK);

    MB_CHECK_INT(single_level, 1); /* 只有 sensor/room1/temp */
    MB_CHECK_INT(multi_level, 3);  /* "sensor/#" 连 "sensor" 本身都匹配 */

    mb_bus_destroy(bus);
}

MB_TEST(source_filter_and_nolocal)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *a = NULL;
    mb_node_t *b = NULL;
    mb_node_t *watcher = NULL;
    record_t from_a = { 0 };
    record_t from_any = { 0 };
    record_t nolocal = { 0 };
    mb_subscribe_opts_t opts;

    MB_CHECK_INT(mb_node_create(bus, "nodeA", &a), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "nodeB", &b), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "watcher", &watcher), MB_OK);

    /* 只关心 nodeA 发的消息 */
    opts.filter = "data/#";
    opts.source_filter = "nodeA";
    opts.flags = 0;
    opts.user_data = &from_a;
    MB_CHECK_INT(mb_node_subscribe_ex(watcher, &opts, record, NULL), MB_OK);

    /* 不过滤来源 */
    MB_CHECK_INT(mb_node_subscribe(watcher, "data/#", record, &from_any, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(a, "data/x", "1", 1, NULL), MB_OK);
    MB_CHECK_INT(mb_node_publish(b, "data/x", "2", 1, NULL), MB_OK);

    MB_CHECK_INT(from_a.count, 1);
    MB_CHECK_STR(from_a.source, "nodeA");
    MB_CHECK_INT(from_any.count, 2);

    /* NOLOCAL：nodeA 不接收自己发的消息 */
    opts.filter = "data/#";
    opts.source_filter = NULL;
    opts.flags = MB_SUB_FLAG_NOLOCAL;
    opts.user_data = &nolocal;
    MB_CHECK_INT(mb_node_subscribe_ex(a, &opts, record, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(a, "data/y", "3", 1, NULL), MB_OK); /* a 自己发 → 收不到 */
    MB_CHECK_INT(mb_node_publish(b, "data/y", "4", 1, NULL), MB_OK); /* b 发 → 收得到 */
    MB_CHECK_INT(nolocal.count, 1);
    MB_CHECK_STR(nolocal.source, "nodeB");

    mb_bus_destroy(bus);
}

MB_TEST(retained_messages)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *sensor = NULL;
    mb_node_t *ui = NULL;
    mb_node_t *late = NULL;
    mb_node_t *later = NULL;
    record_t rec = { 0 };
    record_t late_rec = { 0 };
    record_t skip_rec = { 0 };
    record_t after_clear = { 0 };
    mb_publish_opts_t pub;
    mb_subscribe_opts_t sub_opts;

    MB_CHECK_INT(mb_node_create(bus, "sensor", &sensor), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "ui", &ui), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "late", &late), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "later", &later), MB_OK);

    pub.flags = MB_PUB_FLAG_RETAIN;
    pub.qos = 0;

    /* 先发布、后订阅：retained 让晚到的订阅者也能拿到当前值 */
    MB_CHECK_INT(mb_node_publish(sensor, "sensor/temp", "21", 2, &pub), MB_OK);

    MB_CHECK_INT(mb_node_subscribe(late, "sensor/temp", record, &late_rec, NULL), MB_OK);
    MB_CHECK_INT(late_rec.count, 1);
    MB_CHECK(late_rec.retained);
    MB_CHECK_STR(late_rec.payload, "21");
    MB_CHECK_STR(late_rec.source, "sensor");

    /* 订阅时会先补发一次已有的 retained */
    MB_CHECK_INT(mb_node_subscribe(ui, "sensor/temp", record, &rec, NULL), MB_OK);
    MB_CHECK_INT(rec.count, 1);
    MB_CHECK(rec.retained);

    /* 更新 retained：在线订阅者收到实时消息 */
    MB_CHECK_INT(mb_node_publish(sensor, "sensor/temp", "22", 2, &pub), MB_OK);
    MB_CHECK_INT(rec.count, 2);
    MB_CHECK(!rec.retained);
    MB_CHECK_STR(rec.payload, "22");

    /* 明确要求不要补发 retained */
    sub_opts.filter = "sensor/temp";
    sub_opts.source_filter = NULL;
    sub_opts.flags = MB_SUB_FLAG_SKIP_RETAINED;
    sub_opts.user_data = &skip_rec;
    MB_CHECK_INT(mb_node_subscribe_ex(ui, &sub_opts, record, NULL), MB_OK);
    MB_CHECK_INT(skip_rec.count, 0);

    /* 空负载 + retained = 清除保留消息；但这条消息本身仍投递给在线订阅者 */
    MB_CHECK_INT(mb_node_publish(sensor, "sensor/temp", NULL, 0, &pub), MB_OK);
    MB_CHECK_INT(rec.count, 3);
    MB_CHECK_INT(skip_rec.count, 1);
    MB_CHECK_INT(skip_rec.payload_len, 0);

    /* 清除之后，新订阅者不再收到补发 */
    MB_CHECK_INT(mb_node_subscribe(later, "sensor/temp", record, &after_clear, NULL), MB_OK);
    MB_CHECK_INT(after_clear.count, 0);

    mb_bus_destroy(bus);
}

MB_TEST(unsubscribe_inside_callback)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *pub = NULL;
    mb_node_t *sub_node = NULL;
    int count = 0;

    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sub", &sub_node), MB_OK);

    MB_CHECK_INT(mb_node_subscribe(sub_node, "x/#", unsubscribe_self_cb, &count, NULL), MB_OK);
    MB_CHECK_INT(mb_bus_subscription_count(bus), 1);

    MB_CHECK_INT(mb_node_publish(pub, "x/1", "a", 1, NULL), MB_OK);
    MB_CHECK_INT(count, 1);
    MB_CHECK_INT(mb_bus_subscription_count(bus), 0); /* 回调里已退订 */

    MB_CHECK_INT(mb_node_publish(pub, "x/2", "b", 1, NULL), MB_OK);
    MB_CHECK_INT(count, 1); /* 不应再被调用 */

    mb_bus_destroy(bus);
}

MB_TEST(publish_inside_callback_and_recursion_guard)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *echo = NULL;
    mb_bus_stats_t stats;

    MB_CHECK_INT(mb_node_create(bus, "echo", &echo), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(echo, "echo/#", republish_cb, NULL, NULL), MB_OK);

    g_handler_calls = 0;
    g_busy_seen = false;

    /* 回调里再发布同一主题 → 递归；深度上限会把最内层那次挡掉 */
    MB_CHECK_INT(mb_node_publish(echo, "echo/x", "a", 1, NULL), MB_OK);

#if MB_CONFIG_MAX_DISPATCH_DEPTH > 0
    MB_CHECK_INT(g_handler_calls, MB_CONFIG_MAX_DISPATCH_DEPTH);
    MB_CHECK(g_busy_seen);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.dropped, 1);
#else
    MB_CHECK(!g_busy_seen);
#endif

    mb_bus_destroy(bus);
}

MB_TEST(node_to_node_addressing)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *controller = NULL;
    mb_node_t *motor = NULL;
    record_t cmd = { 0 };

    MB_CHECK_INT(mb_node_create(bus, "controller", &controller), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "motor", &motor), MB_OK);

    /* 订阅「发给自己」的全部消息 */
    MB_CHECK_INT(mb_node_subscribe_self(motor, record, &cmd, NULL), MB_OK);

    /* 定向发往 motor 节点 */
    MB_CHECK_INT(mb_node_publish_to(controller, "motor", "cmd/speed", "1000", 4, NULL), MB_OK);
    MB_CHECK_INT(cmd.count, 1);
    MB_CHECK_STR(cmd.topic, "motor/cmd/speed");
    MB_CHECK_STR(cmd.source, "controller");

    /* 不带子主题就是裸节点名 */
    MB_CHECK_INT(mb_node_publish_to(controller, "motor", NULL, "stop", 4, NULL), MB_OK);
    MB_CHECK_INT(cmd.count, 2);
    MB_CHECK_STR(cmd.topic, "motor");

    /* 目标名非法 */
    MB_CHECK_INT(mb_node_publish_to(controller, "bad/name", "x", "a", 1, NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish_to(controller, NULL, "x", "a", 1, NULL), MB_ERR_INVALID_ARG);

    mb_bus_destroy(bus);
}

MB_TEST(publish_argument_validation)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *node = NULL;
    mb_publish_opts_t opts;

    MB_CHECK_INT(mb_node_create(bus, "n", &node), MB_OK);

    /* 发布主题不允许通配符 */
    MB_CHECK_INT(mb_node_publish(node, "a/+", "x", 1, NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish(node, "a/#", "x", 1, NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish(node, "", "x", 1, NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish(node, NULL, "x", 1, NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_publish(NULL, "a", "x", 1, NULL), MB_ERR_INVALID_ARG);

    /* 有长度却没有数据 */
    MB_CHECK_INT(mb_node_publish(node, "a", NULL, 5, NULL), MB_ERR_INVALID_ARG);

    /* 进程内总线不支持 QoS 1/2（不做重传确认） */
    opts.flags = 0;
    opts.qos = 1;
    MB_CHECK_INT(mb_node_publish(node, "a", "x", 1, &opts), MB_ERR_UNSUPPORTED);

    /* 总线级发布同样受主题校验约束 */
    MB_CHECK_INT(mb_bus_publish(bus, "sys/x", "y", 1, NULL), MB_OK);
    MB_CHECK_INT(mb_bus_publish(bus, "sys/#", "y", 1, NULL), MB_ERR_INVALID_ARG);

    /* 无人订阅属于正常情况，返回成功并计入统计 */
    {
        mb_bus_stats_t stats;

        MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
        MB_CHECK(stats.no_subscriber >= 2);
    }

    mb_bus_destroy(bus);
}

MB_TEST(subscription_accessors)
{
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *node = NULL;
    mb_subscription_t *sub = NULL;
    int marker = 42;
    mb_subscribe_opts_t opts;

    MB_CHECK_INT(mb_node_create(bus, "n", &node), MB_OK);

    opts.filter = "a/#";
    opts.source_filter = "src/+";
    opts.flags = 0;
    opts.user_data = &marker;
    MB_CHECK_INT(mb_node_subscribe_ex(node, &opts, count_only, &sub), MB_OK);
    MB_CHECK(sub != NULL);

    MB_CHECK_STR(mb_subscription_filter(sub), "a/#");
    MB_CHECK_STR(mb_subscription_source_filter(sub), "src/+");
    MB_CHECK(mb_subscription_node(sub) == node);
    MB_CHECK(mb_subscription_user_data(sub) == &marker);
    MB_CHECK(mb_subscription_is_valid(sub));

    {
        int other = 7;

        MB_CHECK(mb_subscription_set_user_data(sub, &other) == &marker);
        MB_CHECK(mb_subscription_user_data(sub) == &other);
    }

    MB_CHECK_INT(mb_node_unsubscribe(&sub), MB_OK);
    MB_CHECK(sub == NULL);
    MB_CHECK_INT(mb_node_unsubscribe(&sub), MB_ERR_INVALID_ARG); /* 重复退订是安全的 */
    MB_CHECK_INT(mb_node_unsubscribe(NULL), MB_ERR_INVALID_ARG);

    mb_bus_destroy(bus);
}

/*
 * 回归测试：匹配的订阅者数量超过 MB_CONFIG_DELIVER_BATCH 时，
 * 分批投递必须一条不漏。
 *
 * 曾经的 bug：收集批次时先把游标推到了当前订阅上，再发现「本批已满」而
 * 把它推迟到下一批 —— 下一批又因为 seq <= 游标而跳过它，于是每个批次
 * 边界之后的第一条匹配订阅都被静默丢弃。默认批宽 8，所以 20 个订阅者
 * 只会收到 18 条。
 */
MB_TEST(batch_boundary_delivers_all_subscribers)
{
    enum { SUBSCRIBERS = 25 }; /* 远大于默认批宽 8，跨 4 个批次 */
    mb_bus_t *bus = make_bus("bus");
    mb_node_t *publisher = NULL;
    mb_node_t *watcher = NULL;
    int count = 0;
    int i;

    MB_CHECK_INT(mb_node_create(bus, "pub", &publisher), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "watcher", &watcher), MB_OK);

    for (i = 0; i < SUBSCRIBERS; ++i) {
        /* 全部订阅同一个具体主题，保证 25 条订阅都命中，且共用一个计数器 */
        MB_CHECK_INT(mb_node_subscribe(watcher, "batch/probe", count_only, &count, NULL), MB_OK);
    }

    MB_CHECK_INT(mb_node_publish(publisher, "batch/probe", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(count, SUBSCRIBERS);

    /* 再插一条不匹配的订阅，确认「跳过不匹配项」不会打乱游标推进 */
    MB_CHECK_INT(mb_node_subscribe(watcher, "other/topic", count_only, &count, NULL), MB_OK);
    count = 0;
    MB_CHECK_INT(mb_node_publish(publisher, "batch/probe", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(count, SUBSCRIBERS);

    /* retained 补发走的是另一套批次循环，同样要验 */
    {
        mb_publish_opts_t opts;
        mb_node_t *late = NULL;

        opts.flags = MB_PUB_FLAG_RETAIN;
        opts.qos = 0;
        MB_CHECK_INT(mb_node_publish(publisher, "batch/retained", "y", 1, &opts), MB_OK);

        MB_CHECK_INT(mb_node_create(bus, "late", &late), MB_OK);
        count = 0;
        for (i = 0; i < SUBSCRIBERS; ++i) {
            MB_CHECK_INT(mb_node_subscribe(late, "batch/retained", count_only, &count, NULL), MB_OK);
        }
        /* 每条新订阅都应立刻收到 1 条 retained 补发 */
        MB_CHECK_INT(count, SUBSCRIBERS);
        mb_node_destroy(late);
    }

    mb_bus_destroy(bus);
}

static const mb_test_case_t cases[] = {
    MB_CASE(publish_delivers_topic_payload_and_source),
    MB_CASE(wildcard_filtering),
    MB_CASE(source_filter_and_nolocal),
    MB_CASE(retained_messages),
    MB_CASE(unsubscribe_inside_callback),
    MB_CASE(publish_inside_callback_and_recursion_guard),
    MB_CASE(node_to_node_addressing),
    MB_CASE(publish_argument_validation),
    MB_CASE(subscription_accessors),
    MB_CASE(batch_boundary_delivers_all_subscribers),
};

const mb_test_suite_t *mb_suite_pubsub(void)
{
    static const mb_test_suite_t suite = { "pubsub (publish/subscribe semantics)", cases, sizeof(cases) / sizeof(cases[0]) };

    return &suite;
}
