/**
 * @file test_bus.c
 * @brief 总线与节点的生命周期、查询、统计。
 */
#include "mb_test.h"
#include "message_bus/message_bus.h"

static void on_any(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    int *count = (int *)user_data;

    (void)sub;
    (void)msg;
    (*count)++;
}

/** 记录最近一条消息的上下文，便于断言主题 / 负载 / 标志。 */
typedef struct {
    int count;
    char topic[64];
    char payload[32];
    char source[32];
    bool has_source;
    bool retained;
    size_t payload_len;
} capture_t;

static void capture(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    capture_t *ctx = (capture_t *)user_data;
    size_t n;

    (void)sub;
    ctx->count++;
    snprintf(ctx->topic, sizeof(ctx->topic), "%s", msg->topic);

    n = msg->payload_len;
    if (n >= sizeof(ctx->payload)) {
        n = sizeof(ctx->payload) - 1;
    }
    if (n > 0 && msg->payload != NULL) {
        memcpy(ctx->payload, msg->payload, n);
    }
    ctx->payload[n] = '\0';
    ctx->payload_len = msg->payload_len;

    ctx->has_source = (msg->source != NULL);
    snprintf(ctx->source, sizeof(ctx->source), "%s", (msg->source != NULL) ? msg->source : "");
    ctx->retained = ((msg->flags & MB_MSG_FLAG_RETAINED) != 0);
}

/** 创建一条不发布节点系统事件的总线，便于精确断言语义计数。 */
static mb_err_t create_quiet_bus(const char *name, mb_bus_t **out)
{
    mb_bus_config_t config;

    MB_CHECK_INT(mb_bus_default_config(&config), MB_OK);
    config.publish_node_events = false;
    return mb_bus_create_ex(name, &config, out);
}

MB_TEST(bus_create_and_destroy)
{
    mb_bus_t *bus = NULL;

    MB_CHECK_INT(mb_bus_create("main", &bus), MB_OK);
    MB_CHECK(bus != NULL);
    MB_CHECK_STR(mb_bus_name(bus), "main");
    MB_CHECK_INT(mb_bus_node_count(bus), 0);
    MB_CHECK_INT(mb_bus_subscription_count(bus), 0);
    MB_CHECK_INT(mb_bus_node_count(NULL), 0);

    mb_bus_destroy(bus);
    mb_bus_destroy(NULL); /* 允许销毁 NULL */
}

MB_TEST(bus_create_invalid_args)
{
    mb_bus_t *bus = NULL;

    MB_CHECK_INT(mb_bus_create(NULL, &bus), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_create("", &bus), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_create("ok", NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_default_config(NULL), MB_ERR_INVALID_ARG);

    {
        char too_long[MB_CONFIG_MAX_NAME_LEN + 8];
        size_t i;

        for (i = 0; i < sizeof(too_long) - 1; ++i) {
            too_long[i] = 'n';
        }
        too_long[sizeof(too_long) - 1] = '\0';
        MB_CHECK_INT(mb_bus_create(too_long, &bus), MB_ERR_TOO_LONG);
    }
}

MB_TEST(node_create_unique_and_auto_name)
{
    mb_bus_t *bus = NULL;
    mb_node_t *a = NULL;
    mb_node_t *b = NULL;
    mb_node_t *auto1 = NULL;
    mb_node_t *auto2 = NULL;

    MB_CHECK_INT(create_quiet_bus("bus", &bus), MB_OK);

    MB_CHECK_INT(mb_node_create(bus, "motor", &a), MB_OK);
    MB_CHECK_STR(mb_node_name(a), "motor");
    MB_CHECK(mb_node_bus(a) == bus);
    MB_CHECK_INT(mb_bus_node_count(bus), 1);

    /* 重名必须拒绝 */
    MB_CHECK_INT(mb_node_create(bus, "motor", &b), MB_ERR_ALREADY_EXISTS);
    MB_CHECK(b == NULL);

    /* name == NULL 自动命名 */
    MB_CHECK_INT(mb_node_create(bus, NULL, &auto1), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, NULL, &auto2), MB_OK);
    MB_CHECK(strncmp(mb_node_name(auto1), "node-", 5) == 0);
    MB_CHECK(strcmp(mb_node_name(auto1), mb_node_name(auto2)) != 0);
    MB_CHECK_INT(mb_bus_node_count(bus), 3);

    mb_bus_destroy(bus);
}

MB_TEST(node_name_validation)
{
    mb_bus_t *bus = NULL;
    mb_node_t *node = NULL;

    MB_CHECK_INT(create_quiet_bus("bus", &bus), MB_OK);

    /* 名字里不能出现会被误认为层级或通配符的字符 */
    MB_CHECK_INT(mb_node_create(bus, "a/b", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(bus, "a+", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(bus, "a#", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(bus, "$sys", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(bus, "", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(NULL, "x", &node), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_node_create(bus, "x", NULL), MB_ERR_INVALID_ARG);
    MB_CHECK_INT(mb_bus_node_count(bus), 0);

    mb_bus_destroy(bus);
}

MB_TEST(node_destroy_removes_its_subscriptions)
{
    mb_bus_t *bus = NULL;
    mb_node_t *publisher = NULL;
    mb_node_t *subscriber = NULL;
    int count = 0;

    MB_CHECK_INT(create_quiet_bus("bus", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &publisher), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sub", &subscriber), MB_OK);

    MB_CHECK_INT(mb_node_subscribe(subscriber, "a/#", on_any, &count, NULL), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(subscriber, "b/#", on_any, &count, NULL), MB_OK);
    MB_CHECK_INT(mb_bus_subscription_count(bus), 2);
    MB_CHECK_INT(mb_node_subscription_count(subscriber), 2);

    MB_CHECK_INT(mb_node_publish(publisher, "a/1", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(count, 1);

    mb_node_destroy(subscriber);
    MB_CHECK_INT(mb_bus_subscription_count(bus), 0);
    MB_CHECK_INT(mb_bus_node_count(bus), 1);

    /* 订阅已随节点一起消失，不应再收到消息 */
    MB_CHECK_INT(mb_node_publish(publisher, "a/1", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(count, 1);

    mb_bus_destroy(bus);
}

MB_TEST(bus_find_node)
{
    mb_bus_t *bus = NULL;
    mb_node_t *node = NULL;
    mb_node_t *found = NULL;

    MB_CHECK_INT(create_quiet_bus("bus", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "motor", &node), MB_OK);

    MB_CHECK_INT(mb_bus_find_node(bus, "motor", &found), MB_OK);
    MB_CHECK(found == node);

    MB_CHECK_INT(mb_bus_find_node(bus, "nope", &found), MB_ERR_NOT_FOUND);
    MB_CHECK(found == NULL);
    MB_CHECK_INT(mb_bus_find_node(NULL, "motor", &found), MB_ERR_INVALID_ARG);

    mb_bus_destroy(bus);
}

MB_TEST(bus_stats)
{
    mb_bus_t *bus = NULL;
    mb_node_t *pub = NULL;
    mb_node_t *sub = NULL;
    mb_bus_stats_t stats;
    int count = 0;

    MB_CHECK_INT(create_quiet_bus("bus", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "pub", &pub), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "sub", &sub), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(sub, "a/#", on_any, &count, NULL), MB_OK);

    MB_CHECK_INT(mb_node_publish(pub, "a/1", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(mb_node_publish(pub, "a/2", "x", 1, NULL), MB_OK);
    MB_CHECK_INT(mb_node_publish(pub, "b/1", "x", 1, NULL), MB_OK); /* 无人订阅 */

    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.published, 3);
    MB_CHECK_INT(stats.delivered, 2);
    MB_CHECK_INT(stats.no_subscriber, 1);
    MB_CHECK_INT(stats.nodes_created, 2);
    MB_CHECK_INT(stats.peak_subscriptions, 1);

    mb_bus_reset_stats(bus);
    MB_CHECK_INT(mb_bus_get_stats(bus, &stats), MB_OK);
    MB_CHECK_INT(stats.published, 0);
    MB_CHECK_INT(stats.delivered, 0);

    MB_CHECK_INT(mb_bus_get_stats(NULL, &stats), MB_ERR_INVALID_ARG);
    mb_bus_destroy(bus);
}

MB_TEST(node_events_are_published)
{
    mb_bus_t *bus = NULL;
    mb_node_t *watcher = NULL;
    mb_node_t *worker = NULL;
    mb_node_t *late = NULL;
    mb_node_t *late2 = NULL;
    capture_t all = { 0 };
    capture_t worker_only = { 0 };
    capture_t after_remove = { 0 };

    /* 本用例用默认配置：publish_node_events = true */
    MB_CHECK_INT(mb_bus_create("bus", &bus), MB_OK);
    MB_CHECK_INT(mb_node_create(bus, "watcher", &watcher), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(watcher, "$mb/nodes/#", capture, &all, NULL), MB_OK);

    /* 订阅时会补发已存在的 retained —— watcher 自己的上线消息 */
    MB_CHECK_INT(all.count, 1);
    MB_CHECK(all.retained);
    MB_CHECK_STR(all.topic, "$mb/nodes/watcher/connected");

    /* 新节点上线：实时投递（非 retained 补发），由总线发布（source 为 NULL） */
    MB_CHECK_INT(mb_node_create(bus, "worker", &worker), MB_OK);
    MB_CHECK_INT(all.count, 2);
    MB_CHECK_STR(all.topic, "$mb/nodes/worker/connected");
    MB_CHECK_STR(all.payload, "worker");
    MB_CHECK(!all.has_source);
    MB_CHECK(!all.retained);

    /* 后启动的订阅者靠 retained 立刻知道 worker 在线 —— 这正是 LVGL 场景的关键 */
    MB_CHECK_INT(mb_node_create(bus, "late", &late), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(late, "$mb/nodes/worker/connected", capture, &worker_only, NULL), MB_OK);
    MB_CHECK_INT(worker_only.count, 1);
    MB_CHECK(worker_only.retained);
    MB_CHECK_STR(worker_only.topic, "$mb/nodes/worker/connected");
    MB_CHECK_STR(worker_only.payload, "worker");

    /* 离线：先向 /connected 发空负载（清 retained + 通知在线者），再发 /disconnected 事件 */
    mb_node_destroy(worker);
    MB_CHECK_INT(all.count, 5); /* late 上线 1 条 + worker 的 connected 清空 + disconnected 事件 */
    MB_CHECK_STR(all.topic, "$mb/nodes/worker/disconnected");
    MB_CHECK_INT(all.payload_len, 6); /* 最后一条是 /disconnected，负载为节点名 */
    MB_CHECK_STR(all.payload, "worker");

    /* 只订阅 /connected 的节点：先收到 retained 补发，再收到一条空负载的清除消息 */
    MB_CHECK_INT(worker_only.count, 2);
    MB_CHECK_INT(worker_only.payload_len, 0);

    /* 保留状态已被清除：此时再订阅 /connected 不应收到任何补发 */
    MB_CHECK_INT(mb_node_create(bus, "late2", &late2), MB_OK);
    MB_CHECK_INT(mb_node_subscribe(late2, "$mb/nodes/worker/connected", capture, &after_remove, NULL), MB_OK);
    MB_CHECK_INT(after_remove.count, 0);

    mb_bus_destroy(bus);
}

static const mb_test_case_t cases[] = {
    MB_CASE(bus_create_and_destroy),
    MB_CASE(bus_create_invalid_args),
    MB_CASE(node_create_unique_and_auto_name),
    MB_CASE(node_name_validation),
    MB_CASE(node_destroy_removes_its_subscriptions),
    MB_CASE(bus_find_node),
    MB_CASE(bus_stats),
    MB_CASE(node_events_are_published),
};

const mb_test_suite_t *mb_suite_bus(void)
{
    static const mb_test_suite_t suite = { "bus (bus and node lifecycle)", cases, sizeof(cases) / sizeof(cases[0]) };

    return &suite;
}
