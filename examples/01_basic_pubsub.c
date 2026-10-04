/**
 * @file 01_basic_pubsub.c
 * @brief 最小可用示例：一条总线、两个节点、一次发布订阅。
 *
 * 演示要点：
 *   - 用函数创建总线（mb_bus_create）
 *   - 节点以总线为对象创建（mb_node_create）
 *   - 订阅 / 发布 / 退订 / 销毁的完整生命周期
 *   - 同步投递：publish 返回时回调已经跑完
 *   - retained 保留消息：晚订阅的节点也能拿到当前值
 */
#include <stdio.h>
#include <string.h>

#include "message_bus/message_bus.h"

/* 订阅者回调：打印收到的消息 */
static void on_temperature(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    const char *who = (const char *)user_data;

    (void)sub;
    printf("  [%s] received '%.*s' = %.*s (source: %s, id %u)\n",
           who,
           (int)strlen(msg->topic), msg->topic,
           (int)msg->payload_len, (const char *)msg->payload,
           (msg->source != NULL) ? msg->source : "(bus)",
           (unsigned)msg->id);
}

int main(void)
{
    mb_bus_t *bus = NULL;
    mb_node_t *sensor = NULL;
    mb_node_t *display = NULL;
    mb_subscription_t *sub = NULL;

    printf("== 01 basic publish/subscribe ==\n");

    /* 1. 创建总线 */
    if (mb_bus_create("main", &bus) != MB_OK) {
        printf("failed to create bus\n");
        return 1;
    }
    printf("bus '%s' created\n", mb_bus_name(bus));

    /* 2. 节点以总线为对象创建 */
    mb_node_create(bus, "sensor", &sensor);
    mb_node_create(bus, "display", &display);

    /* 3. 订阅：display 关心 temperature 主题下的所有值 */
    mb_node_subscribe(display, "sensor/temperature", on_temperature, (void *)"display", &sub);

    /* 4. 发布：payload 是调用方的栈缓冲，publish 内部按需拷贝/引用，
     *    函数返回后即可释放 —— 回调在此期间已经执行完毕。 */
    mb_node_publish(sensor, "sensor/temperature", "23.5", 4, NULL);
    mb_node_publish(sensor, "sensor/temperature", "24.1", 4, NULL);

    /* 5. 没有订阅者的主题不会报错，只是没人收到 */
    mb_node_publish(sensor, "sensor/nobody-cares", "x", 1, NULL);

    /* 6. retained 保留消息：总线上保存最后一条，
     *    之后再订阅的人会立刻收到它 —— 这是「后启动的 UI 也能显示当前温度」的关键 */
    {
        mb_publish_opts_t opts;
        mb_node_t *late_display = NULL;

        opts.flags = MB_PUB_FLAG_RETAIN;
        opts.qos = 0;
        mb_node_publish(sensor, "sensor/temperature", "25.0", 4, &opts);
        printf("published retained message sensor/temperature = 25.0\n");

        mb_node_create(bus, "late_display", &late_display);
        printf("node late_display now subscribes to sensor/temperature:\n");
        mb_node_subscribe(late_display, "sensor/temperature", on_temperature,
                          (void *)"late_display", NULL);

        mb_node_destroy(late_display);
    }

    /* 7. 退订（传入 &sub，成功后 sub 自动置 NULL，可重复调用） */
    mb_node_unsubscribe(&sub);
    printf("display unsubscribed, publishing once more:\n");
    mb_node_publish(sensor, "sensor/temperature", "26.0", 4, NULL);
    printf("  (no output above, as expected)\n");

    /* 8. 销毁总线会级联销毁其上的所有节点与订阅 */
    mb_bus_destroy(bus);
    printf("bus destroyed\n");

    return 0;
}
