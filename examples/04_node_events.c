/**
 * @file 04_node_events.c
 * @brief 节点上下线事件与系统主题（$mb/...）。
 *
 * 总线上每个节点上线/下线时都会自动发布系统消息：
 *
 *   $mb/nodes/<名字>/connected      [retained] 负载 = 节点名
 *   $mb/nodes/<名字>/disconnected   [不保留]  负载 = 节点名
 *
 * 典型用途：界面想知道「现在有哪些传感器在线」，就不用挨个去问，
 * 订阅 $mb/nodes/+/connected 即可 —— 已有的在线节点会通过 retained
 * 立刻补发，后来的变化会实时推送。
 *
 * 注意 MQTT 的一条重要规则：'#' 不匹配以 '$' 开头的系统主题，
 * 想收系统消息必须显式写出前缀（如 "$mb/#"）。
 */
#include <stdio.h>
#include <string.h>

#include "message_bus/message_bus.h"

static void on_node_event(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    const char *who = (const char *)user_data;

    (void)sub;
    printf("  [%s] %-42s payload='%.*s'%s\n",
           who,
           msg->topic,
           (int)msg->payload_len, (msg->payload != NULL) ? (const char *)msg->payload : "",
           ((msg->flags & MB_MSG_FLAG_RETAINED) != 0) ? "  (retained replay)" : "");
}

static void on_anything(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)user_data;
    printf("  [catch-all] received %s\n", msg->topic);
}

int main(void)
{
    mb_bus_t *bus = NULL;
    mb_node_t *monitor = NULL;
    mb_node_t *sensor1 = NULL;
    mb_node_t *sensor2 = NULL;
    mb_node_t *sensor3 = NULL;
    mb_node_t *late_watcher = NULL;

    printf("== 04 node connect/disconnect events ==\n\n");

    mb_bus_create("events", &bus);

    /* 先挂一个「监控」节点：它能看到自己以及后续节点的上下线 */
    mb_node_create(bus, "monitor", &monitor);
    mb_node_subscribe(monitor, "$mb/nodes/#", on_node_event, (void *)"monitor", NULL);

    printf("[setup monitor] monitor subscribes to $mb/nodes/#\n");
    printf("  the retained replay at subscribe time (monitor itself) is the first line below\n\n");

    printf("[sensor1 online]\n");
    mb_node_create(bus, "sensor1", &sensor1);

    printf("\n[sensor2 online]\n");
    mb_node_create(bus, "sensor2", &sensor2);

    printf("\n[a late watcher] late_watcher subscribes to $mb/nodes/+/connected only now\n");
    printf("  (retained tells it immediately that sensor1 and sensor2 are already online)\n");
    printf("  note: monitor and late_watcher both print below -- their filters overlap\n");
    mb_node_create(bus, "late_watcher", &late_watcher);
    mb_node_subscribe(late_watcher, "$mb/nodes/+/connected", on_node_event, (void *)"late  ", NULL);

    printf("\n[sensor1 offline]\n");
    printf("  first an empty payload on /connected (clears retained), then the /disconnected event\n");
    mb_node_destroy(sensor1);

    printf("\n[sensor3 online]\n");
    mb_node_create(bus, "sensor3", &sensor3);

    printf("\n[the '$' rule] subscribing to '#' does NOT receive system topics\n");
    mb_node_subscribe(sensor2, "#", on_anything, NULL, NULL);
    printf("  sensor2 subscribed to '#'; now publish one normal and one system message:\n");
    mb_node_publish(sensor3, "normal/topic", "hello", 5, NULL);
    mb_bus_publish(bus, "$mb/custom/notice", "system", 6, NULL);
    printf("  only the normal message reaches '#'; $mb/custom/notice is skipped\n");

    printf("\n[teardown] destroy the bus (no disconnect events are emitted)\n");
    mb_bus_destroy(bus);

    return 0;
}
