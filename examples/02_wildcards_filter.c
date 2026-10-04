/**
 * @file 02_wildcards_filter.c
 * @brief 过滤（MQTT 通配符）、来源过滤、NOLOCAL 的完整演示。
 *
 * 一条总线、一个发布者、若干订阅者，每个订阅者用不同的过滤器，
 * 观察谁收到了哪条消息。
 */
#include <stdio.h>
#include <string.h>

#include "message_bus/message_bus.h"

typedef struct {
    const char *name;
} subscriber_t;

static void on_message(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    const subscriber_t *me = (const subscriber_t *)user_data;

    printf("      -> %-22s got %-28s (source %s)\n",
           me->name,
           msg->topic,
           (msg->source != NULL) ? msg->source : "(bus)");
    (void)sub;
}

static void fire(mb_node_t *publisher, const char *topic, const char *payload)
{
    printf("  publish %-28s = %s\n", topic, payload);
    mb_node_publish(publisher, topic, payload, strlen(payload), NULL);
}

int main(void)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sensor_a = NULL;
    mb_node_t *sensor_b = NULL;
    mb_node_t *logger = NULL;
    subscriber_t s_exact = { "exact sensor_a/temp" };
    subscriber_t s_plus = { "single-level +/temp" };
    subscriber_t s_hash = { "multi-level sensor_a/#" };
    subscriber_t s_from_a = { "source sensor_a only" };
    subscriber_t s_nolocal = { "NOLOCAL (skip own)" };
    mb_subscribe_opts_t opts;

    printf("== 02 wildcards and filtering ==\n");

    mb_bus_create("filters", &bus);
    mb_node_create(bus, "sensor_a", &sensor_a);
    mb_node_create(bus, "sensor_b", &sensor_b);
    mb_node_create(bus, "logger", &logger);

    printf("\n[setup subscriptions]\n");
    mb_node_subscribe(logger, "sensor_a/temp", on_message, &s_exact, NULL);
    printf("  logger subscribes to sensor_a/temp\n");
    mb_node_subscribe(logger, "+/temp", on_message, &s_plus, NULL);
    printf("  logger subscribes to +/temp            ('+' matches exactly one level; 'sensor_+/temp' is invalid)\n");
    mb_node_subscribe(logger, "sensor_a/#", on_message, &s_hash, NULL);
    printf("  logger subscribes to sensor_a/#        ('#' matches all remaining levels)\n");

    /* 来源过滤：主题用通配符全收，但只接受某个节点发出的 */
    opts.filter = "#";
    opts.source_filter = "sensor_a";
    opts.flags = 0;
    opts.user_data = &s_from_a;
    mb_node_subscribe_ex(logger, &opts, on_message, NULL);
    printf("  logger subscribes to # with source_filter = sensor_a\n");

    /* NOLOCAL：sensor_a 订阅所有主题，但过滤掉自己发布的 */
    opts.filter = "#";
    opts.source_filter = NULL;
    opts.flags = MB_SUB_FLAG_NOLOCAL;
    opts.user_data = &s_nolocal;
    mb_node_subscribe_ex(sensor_a, &opts, on_message, NULL);
    printf("  sensor_a subscribes to # with the NOLOCAL flag\n");

    printf("\n[publish sensor_a/temp]\n");
    fire(sensor_a, "sensor_a/temp", "23.0");

    printf("\n[publish sensor_b/temp]  ('+/temp' matches it, so s_plus gets it too)\n");
    fire(sensor_b, "sensor_b/temp", "19.5");

    printf("\n[publish sensor_a/room1/humidity]  (only '#' matches deep topics)\n");
    fire(sensor_a, "sensor_a/room1/humidity", "55");

    printf("\n[publish sensor_a]  ('sensor_a/#' also matches the parent level itself)\n");
    fire(sensor_a, "sensor_a", "online");

    printf("\nNotes:\n");
    printf("  - '+' matches one level; '#' matches zero or more and must be the last level;\n");
    printf("  - source_filter filters on the publisher's node NAME, same syntax as topic filters;\n");
    printf("  - NOLOCAL keeps a node from receiving its own messages (useful for forwarders).\n");

    mb_bus_destroy(bus);
    return 0;
}
