/**
 * @file 05_async_await.c
 * @brief 异步投递 + 等待消费：多线程下的「发命令 → 确认已执行 → 读结果」。
 *
 * 场景和 03 一样，但把电机回调放到了**另一个线程**（pump 线程）上：
 *
 *     ui 线程 ──publish_async_wait(cmd)──► [队列] ──► pump 线程
 *        │                                              │
 *        └──────────── 等回调执行完 ◄──── ack ◄──────────┘
 *
 * 这样 UI 线程既没有被回调拖慢，又能在关键点上确认命令真的生效了 ——
 * 普通异步发布做不到后者，同步发布做不到前者。
 *
 * 编译运行：
 *     cmake --build build --target example_05_async_await
 *     ./build/bin/example_05_async_await
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "message_bus/message_bus.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

/* -------------------------------------------------------------------------
 * 最小线程封装（示例不依赖任何线程库，只演示用法）
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

/* -------------------------------------------------------------------------
 * 节点：motor —— 跑在 pump 线程上
 * ---------------------------------------------------------------------- */

static int g_motor_speed = 0;

static void motor_on_command(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    char command[32];
    size_t len = msg->payload_len;

    (void)sub;
    (void)user_data;

    if (len >= sizeof(command)) {
        len = sizeof(command) - 1;
    }
    memcpy(command, msg->payload, len);
    command[len] = '\0';

    /* 模拟真实硬件的耗时（设置 PWM、等驱动器响应等）。
     * 因为它跑在 pump 线程上，UI 线程不会被这 30ms 拖住 —— 但 await
     * 会在这里等它结束，这正是「确认执行完」的含义。 */
    mb_os_sleep_ms(30);

    if (strncmp(command, "speed=", 6) == 0) {
        g_motor_speed = atoi(command + 6);
        printf("    [motor] (pump thread) got '%s' -> speed = %d\n",
               command, g_motor_speed);
    } else if (strcmp(command, "stop") == 0) {
        g_motor_speed = 0;
        printf("    [motor] (pump thread) got 'stop' -> motor stopped\n");
    }
}

/* -------------------------------------------------------------------------
 * 消费方线程：循环 pump，直到主线程说停
 * ---------------------------------------------------------------------- */

static volatile int g_pump_should_stop = 0;

static void pump_job(void *arg)
{
    mb_bus_t *bus = (mb_bus_t *)arg;

    while (!g_pump_should_stop) {
        mb_bus_pump(bus);
        mb_os_sleep_ms(2); /* 队列空时别空转烧 CPU */
    }
    mb_bus_pump(bus); /* 退出前再收一次尾 */
}

/* -------------------------------------------------------------------------
 * 发布方：UI 线程
 * ---------------------------------------------------------------------- */

static void ui_send_command(mb_node_t *ui, const char *command)
{
    size_t delivered = 0;
    uint32_t start = mb_os_time_ms();
    mb_err_t err = mb_node_publish_async_wait(ui, "motor/cmd", command,
                                              strlen(command), NULL, 1000, &delivered);

    printf("[ui] command '%s' -> %s (delivered=%zu, waited %u ms)\n",
           command, mb_err_to_string(err), delivered, (unsigned)(mb_os_time_ms() - start));

    if (err == MB_OK) {
        /* 走到这里时 motor_on_command() 已经返回：当前转速**必然**已是新值，
         * 不需要再轮询、也不需要额外的条件变量。 */
        printf("[ui] confirmed: callbacks returned, current speed = %d rpm\n", g_motor_speed);
    }
}

int main(void)
{
    mb_bus_t *bus = NULL;
    mb_node_t *ui = NULL;
    mb_node_t *motor = NULL;
    thread_t pump_thread;
    thread_job_t pump_desc;

    if (mb_bus_create("app", &bus) != MB_OK) {
        fprintf(stderr, "bus create failed\n");
        return 1;
    }
    mb_node_create(bus, "ui", &ui);
    mb_node_create(bus, "motor", &motor);
    mb_node_subscribe(motor, "motor/cmd", motor_on_command, NULL, NULL);

    /* 1. 先让消费方线程跑起来 */
    pump_desc.fn = pump_job;
    pump_desc.arg = bus;
    if (!thread_start(&pump_thread, &pump_desc)) {
        fprintf(stderr, "failed to start pump thread\n");
        mb_bus_destroy(bus);
        return 1;
    }

    /* 2. UI 线程：每条命令都等电机回调确认后再发下一条 */
    ui_send_command(ui, "speed=1200");
    ui_send_command(ui, "speed=800");
    ui_send_command(ui, "stop");

    /* 3. 先停消费方并 join，再销毁总线（顺序不能反） */
    g_pump_should_stop = 1;
    thread_join(&pump_thread);
    mb_bus_destroy(bus);
    return 0;
}
