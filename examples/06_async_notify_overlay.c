/**
 * @file 06_async_notify_overlay.c
 * @brief 非阻塞推送 + 完成回调：点击 UI 后显示等待遮罩，结果出来再关闭。
 *
 * 关键点：推送函数**不会阻塞**，UI 线程可以继续刷帧；成功或超时后，
 * 由 pump 线程调用 on_click_done()，在那里关闭遮罩、显示错误或重试。
 *
 *     UI 线程：show_overlay() -> publish_async_notify() -> 继续渲染
 *     pump 线程：投递消息 -> 回调 on_click_done() -> hide_overlay()
 *
 * 编译运行：
 *     cmake --build build --target example_06_async_notify_overlay
 *     ./build/bin/example_06_async_notify_overlay
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

/* -------------------------------------------------------------------------
 * 模拟 LVGL：遮罩状态 + 只允许 UI/pump 线程访问的“控件”
 * ---------------------------------------------------------------------- */

static volatile int g_overlay_visible = 0;
static volatile int g_pending_callback = 0;
static int g_retries_left = 0;

static void overlay_show(const char *what)
{
    g_overlay_visible = 1;
    printf("[ui] overlay ON  (%s)\n", what);
}

static void overlay_hide(void)
{
    g_overlay_visible = 0;
    printf("[ui] overlay OFF\n");
}

/* -------------------------------------------------------------------------
 * 电机节点：真正消费命令的地方
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

    mb_os_sleep_ms(20); /* 模拟 PWM / 驱动器响应 */

    if (strncmp(command, "speed=", 6) == 0) {
        g_motor_speed = atoi(command + 6);
    } else if (strcmp(command, "stop") == 0) {
        g_motor_speed = 0;
    }
    printf("[motor] (pump thread) applied '%s', speed = %d\n", command, g_motor_speed);
}

/* -------------------------------------------------------------------------
 * 完成回调：成功或超时都会到这里，运行在 pump 线程
 * ---------------------------------------------------------------------- */

static void on_click_done(const mb_async_result_t *result, void *user_data)
{
    (void)user_data;

    overlay_hide();
    printf("[ui] callback: %s, status=%s, delivered=%zu, "
           "elapsed_since_publish=%u ms\n",
           result->success ? "SUCCESS" : "FAILED",
           mb_err_to_string(result->status), result->delivered,
           (unsigned)result->elapsed_since_publish_ms);

    if (result->success) {
        g_pending_callback = 0;
        return;
    }

    /* 失败时原始 topic / payload / opts 都在 result 里：可以直接重试。
     * 这里只重试一次，避免无订阅者时无限循环。 */
    if (g_retries_left > 0) {
        g_retries_left--;
        printf("[ui] retrying the same message once...\n");
        overlay_show("retry");
        if (mb_node_publish_async_notify(result->node, result->topic, result->payload,
                                         result->payload_len, &result->opts,
                                         result->requested_timeout_ms,
                                         on_click_done, NULL) == MB_OK) {
            return; /* 新的通知会再次回调，pending 保持 1 */
        }
        overlay_hide();
    }

    printf("[ui] giving up: show error toast\n");
    g_pending_callback = 0;
}

/* -------------------------------------------------------------------------
 * pump 线程
 * ---------------------------------------------------------------------- */

static volatile int g_pump_should_stop = 0;
static volatile int g_pump_paused = 0;

static void pump_job(void *arg)
{
    mb_bus_t *bus = (mb_bus_t *)arg;

    while (!g_pump_should_stop) {
        /* 完成回调（成功 / 超时）也从这里触发，所以 pump 必须持续被调用 */
        if (!g_pump_paused) {
            mb_bus_pump(bus);
        }
        mb_os_sleep_ms(2);
    }
    mb_bus_pump(bus);
}

/* -------------------------------------------------------------------------
 * 模拟一次点击：显示遮罩 -> 非阻塞推送 -> 继续渲染
 * ---------------------------------------------------------------------- */

static void simulate_click(mb_node_t *ui, const char *topic, const char *command,
                           uint32_t timeout_ms, int retries)
{
    mb_err_t err;

    overlay_show(command);
    g_retries_left = retries;
    g_pending_callback = 1;

    err = mb_node_publish_async_notify(ui, topic, command, strlen(command), NULL,
                                       timeout_ms, on_click_done, NULL);
    if (err != MB_OK) {
        printf("[ui] publish failed immediately: %s\n", mb_err_to_string(err));
        overlay_hide();
        g_pending_callback = 0;
        return;
    }

    printf("[ui] publish returned immediately; UI keeps rendering\n");
    for (int frame = 0; frame < 5; ++frame) {
        printf("  [ui] frame %d rendered (overlay %s)\n",
               frame, g_overlay_visible ? "ON" : "OFF");
        mb_os_sleep_ms(10);
    }

    while (g_pending_callback) {
        mb_os_sleep_ms(5);
    }
}

/* 模拟「电机忙 / pump 没被驱动」：暂停 pump 一段超过 timeout 的时间，
 * 让消息一直躺在队列里。pump 恢复后，先触发超时回调，回调里重试并成功。 */
static void simulate_timeout_click(mb_node_t *ui, const char *topic, const char *command)
{
    mb_err_t err;

    g_pump_paused = 1;
    overlay_show(command);
    g_retries_left = 1;
    g_pending_callback = 1;

    err = mb_node_publish_async_notify(ui, topic, command, strlen(command), NULL,
                                       100, on_click_done, NULL);
    if (err != MB_OK) {
        printf("[ui] publish failed immediately: %s\n", mb_err_to_string(err));
        overlay_hide();
        g_pending_callback = 0;
        g_pump_paused = 0;
        return;
    }

    printf("[ui] pump is busy; UI keeps rendering while the request waits\n");
    for (int frame = 0; frame < 4; ++frame) {
        printf("  [ui] frame %d rendered (overlay %s)\n",
               frame, g_overlay_visible ? "ON" : "OFF");
        mb_os_sleep_ms(40); /* 累计超过 timeout_ms */
    }

    printf("[ui] pump resumes: expect TIMEOUT, then a successful retry\n");
    g_pump_paused = 0;

    while (g_pending_callback) {
        mb_os_sleep_ms(5);
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

    pump_desc.fn = pump_job;
    pump_desc.arg = bus;
    if (!thread_start(&pump_thread, &pump_desc)) {
        fprintf(stderr, "failed to start pump thread\n");
        mb_bus_destroy(bus);
        return 1;
    }

    /* 1. 正常命令：被 motor 消费，回调里关闭遮罩 */
    simulate_click(ui, "motor/cmd", "speed=1200", 1000, 0);

    /* 2. 没有节点监听的主题：pump 处理后立刻以 MB_ERR_NO_SUBSCRIBER 回调，
     *    关闭遮罩并显示错误，不重试。 */
    simulate_click(ui, "motor/does-not-exist", "stop", 1000, 0);

    /* 3. 超时路径：pump 被占用超过 timeout，消息投递不出去。
     *    恢复后触发超时回调，再在回调里用 result 的信息重试并成功。 */
    simulate_timeout_click(ui, "motor/cmd", "stop");

    g_pump_should_stop = 1;
    thread_join(&pump_thread);
    mb_bus_destroy(bus);
    return 0;
}
