/**
 * @file 03_lvgl_motor_sensor.c
 * @brief 本项目的真实场景仿真：LVGL 界面 / 电机 / 传感器通过消息总线解耦。
 *
 * 三个节点互为「陌生人」：
 *   - lvgl_ui  只知道主题名，不知道电机与传感器在哪个文件、哪个寄存器；
 *   - sensor   只管往 sensor/... 主题上发数据，不关心有没有界面；
 *   - motor    只订阅 motor/cmd，不关心命令是界面发的还是别的逻辑发的。
 *
 * 在 PC 上跑的是模拟器：publish 打印一行日志就代表「硬件动了」；
 * 到了 MCU 上，把 motor 的回调换成 HAL_GPIO_WritePin / PWM_SetDuty，
 * 把 sensor 的定时发布换成 ADC 采样，**其余代码一行都不用改**。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "message_bus/message_bus.h"

/* -------------------------------------------------------------------------
 * 这里用全局变量代替真实工程里的句柄（LVGL 控件指针 / 电机句柄）
 * ---------------------------------------------------------------------- */
static mb_node_t *g_sensor = NULL;
static mb_node_t *g_motor = NULL;
static mb_node_t *g_ui = NULL;

/* -------------------------------------------------------------------------
 * 节点：sensor —— 采集并上报
 * ---------------------------------------------------------------------- */

/** 上报一个传感器值；用 retained 让后启动的界面也能立刻显示当前值。 */
static void sensor_report(const char *channel, double value)
{
    char topic[64];
    char payload[32];
    mb_publish_opts_t opts;

    snprintf(topic, sizeof(topic), "sensor/%s/value", channel);
    snprintf(payload, sizeof(payload), "%.1f", value);

    opts.flags = MB_PUB_FLAG_RETAIN;
    opts.qos = 0;
    mb_node_publish(g_sensor, topic, payload, strlen(payload), &opts);
}

/* -------------------------------------------------------------------------
 * 节点：motor —— 只认 motor/cmd 主题
 * ---------------------------------------------------------------------- */

static int g_motor_speed = 0;

static void motor_on_command(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    char command[64];
    size_t len = msg->payload_len;

    (void)sub;
    (void)user_data;

    if (len >= sizeof(command)) {
        len = sizeof(command) - 1;
    }
    memcpy(command, msg->payload, len);
    command[len] = '\0';

    /* ↓↓↓ 真实工程里这里是 HAL_xxx：设置 PWM、使能驱动器、读编码器 ↓↓↓ */
    if (strncmp(command, "speed=", 6) == 0) {
        char status[64];
        mb_publish_opts_t opts;

        g_motor_speed = atoi(command + 6);
        printf("    [motor ] command '%s' -> speed set to %d rpm\n", command, g_motor_speed);

        /* 上报状态；retained，界面后启动也能显示当前转速 */
        snprintf(status, sizeof(status), "%d rpm", g_motor_speed);
        opts.flags = MB_PUB_FLAG_RETAIN;
        opts.qos = 0;
        mb_node_publish(g_motor, "motor/speed/status", status, strlen(status), &opts);
    } else if (strcmp(command, "stop") == 0) {
        mb_publish_opts_t opts;

        g_motor_speed = 0;
        printf("    [motor ] command 'stop' -> motor stopped\n");

        opts.flags = MB_PUB_FLAG_RETAIN; /* 状态类消息始终保留最新值 */
        opts.qos = 0;
        mb_node_publish(g_motor, "motor/speed/status", "stopped", 7, &opts);
    }
}

/* -------------------------------------------------------------------------
 * 节点：lvgl_ui —— 只订阅，不发布硬件相关的东西
 * ---------------------------------------------------------------------- */

/*
 * ⚠ 重要：回调是在**发布者线程**里同步执行的。
 * 在真实工程中，传感器采样往往在另一个任务/线程里，而 LVGL 不是线程安全的，
 * 因此**不要在回调里直接调用 lv_label_set_text() 等控件 API**。
 *
 * 推荐做法（任选其一）：
 *   1. 回调里只把值写入一个变量 / 环形缓冲，由 LVGL 线程在自己的定时器里读取刷新；
 *   2. 回调里调用 lv_async_call()，把控件刷新动作抛回 LVGL 线程执行。
 * 本示例用做法 1，用 printf 模拟界面刷新。
 */
static char g_ui_temp_text[32] = "(no data)";
static char g_ui_motor_text[32] = "(no data)";

static void ui_on_temperature(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)user_data;
    snprintf(g_ui_temp_text, sizeof(g_ui_temp_text), "%.*s C",
             (int)msg->payload_len, (const char *)msg->payload);
    /* 真实工程：这里只记值；下面这行换成由 LVGL 线程执行的刷新 */
    printf("    [ui    ] temperature label -> \"%s\"\n", g_ui_temp_text);
}

static void ui_on_motor_status(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)user_data;
    snprintf(g_ui_motor_text, sizeof(g_ui_motor_text), "%.*s",
             (int)msg->payload_len, (const char *)msg->payload);
    printf("    [ui    ] motor label -> \"%s\"\n", g_ui_motor_text);
}

/* 按钮事件：界面发命令给电机节点，而不是直接调用电机驱动 */
static void ui_button_set_speed(int rpm)
{
    char command[32];
    mb_publish_opts_t opts;

    /* 命令不是状态，不要用 retained：否则后加入的订阅者会「重放」一条旧命令 */
    snprintf(command, sizeof(command), "speed=%d", rpm);
    opts.flags = 0;
    opts.qos = 0;

    printf("    [ui    ] button pressed -> publish motor/cmd = %s\n", command);
    /* 等价写法：mb_node_publish_to(g_ui, "motor", "cmd", ...) */
    mb_node_publish(g_ui, "motor/cmd", command, strlen(command), &opts);
}

/* -------------------------------------------------------------------------
 * main：把三个节点挂到同一条总线上，然后模拟一段运行时序
 * ---------------------------------------------------------------------- */
int main(void)
{
    mb_bus_t *bus = NULL;
    mb_bus_stats_t stats;

    printf("== 03 LVGL / motor / sensor decoupling ==\n\n");

    mb_bus_create("app", &bus);
    mb_node_create(bus, "sensor", &g_sensor);
    mb_node_create(bus, "motor", &g_motor);
    mb_node_create(bus, "lvgl_ui", &g_ui);

    printf("[wiring] set up subscriptions (nodes never know each other)\n");
    /* 界面只认识主题 */
    mb_node_subscribe(g_ui, "sensor/+/value", ui_on_temperature, NULL, NULL);
    printf("  lvgl_ui subscribes to sensor/+/value\n");
    mb_node_subscribe(g_ui, "motor/+/status", ui_on_motor_status, NULL, NULL);
    printf("  lvgl_ui subscribes to motor/+/status\n");
    /* 电机只认识命令主题 */
    mb_node_subscribe(g_motor, "motor/cmd", motor_on_command, NULL, NULL);
    printf("  motor   subscribes to motor/cmd\n");

    printf("\n[t=0ms] motor reports initial state (retained)\n");
    mb_node_publish(g_motor, "motor/speed/status", "0 rpm", 5, NULL);

    printf("\n[t=100ms] sensor samples once\n");
    sensor_report("temp", 23.4);

    printf("\n[t=200ms] UI button: set speed to 1200\n");
    ui_button_set_speed(1200);

    printf("\n[t=300ms] sensor samples again\n");
    sensor_report("temp", 23.9);

    printf("\n[t=400ms] UI button: stop\n");
    {
        mb_publish_opts_t opts;

        opts.flags = 0;
        opts.qos = 0;
        mb_node_publish(g_ui, "motor/cmd", "stop", 4, &opts);
    }

    printf("\n[t=500ms] a 'late' UI node comes online -- retained gives it the current values at once\n");
    {
        mb_node_t *late_ui = NULL;

        mb_node_create(bus, "late_ui", &late_ui);
        mb_node_subscribe(late_ui, "sensor/+/value", ui_on_temperature, NULL, NULL);
        mb_node_subscribe(late_ui, "motor/+/status", ui_on_motor_status, NULL, NULL);
        mb_node_destroy(late_ui);
    }

    /* 统计信息：排查「消息发出去没人收」时很有用 */
    mb_bus_get_stats(bus, &stats);
    printf("\n[stats] published %u, delivered %u, no_subscriber %u, retained %u\n",
           (unsigned)stats.published, (unsigned)stats.delivered,
           (unsigned)stats.no_subscriber, (unsigned)stats.retained_stored);

    printf("\n[final UI state] temperature = %s, motor = %s\n", g_ui_temp_text, g_ui_motor_text);

    mb_bus_destroy(bus);
    printf("\nbus destroyed; all nodes and subscriptions released\n");
    return 0;
}
