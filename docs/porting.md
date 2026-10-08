# 移植指南

本库只需要平台提供 **11 个函数**：一把递归锁（4 个操作）、一个计数信号量（4 个操作）、
毫秒时间、内存分配（3 个）。信号量只为**异步投递**（`mb_async.h`）服务 ——
把 `MB_CONFIG_ASYNC_MAX_TOPICS` 设为 0 关掉异步功能后，前 4 个函数就是全部。
所有平台相关代码都收敛在 `port/` 下的**单个 `.c` 文件**里，换平台不需要改动 `src/` 任何一行。

每个 port 文件都用 `#if MB_CONFIG_OS == MB_OS_XXX` 包住整个实现，**未被选中时编译为空目标文件**。
因此 Arduino / PlatformIO 这类会递归扫描并编译库内所有 `.c` 的构建系统，即使多编译了别的
port 文件也不会有副作用（只会多出一个空 `.o`）。若选中的后端文件缺失，会在链接阶段提示
`undefined reference to mb_os_mutex_create`。

---

## 0. 选择 port

| 场景 | `MB_CONFIG_OS` | 产生代码的文件 |
|---|---|---|
| PC 模拟器（Windows） | `MB_OS_WIN32` | `port/mb_os_win32.c` |
| PC 模拟器（Linux / macOS） | `MB_OS_POSIX` | `port/mb_os_posix.c` |
| MCU + FreeRTOS | `MB_OS_FREERTOS` | `port/mb_os_freertos.c` |
| 裸机 / 无 RTOS | `MB_OS_NONE` | `port/mb_os_none.c` |
| 其它 RTOS | 见第 5 节 | 自己新增一个 |

用 CMake 时它会自动按平台选择（`MB_OS=auto`）：

```bash
cmake -S . -B build              # 自动选 win32 或 posix
cmake -S . -B build -DMB_OS=freertos -DMB_FREERTOS_INCLUDE_DIR=/path/to/FreeRTOS-Kernel/include
cmake -S . -B build -DMB_OS=none
```

---

## 1. FreeRTOS（MCU 目标平台）

### 1.1 前置条件

在 `FreeRTOSConfig.h` 里必须有：

```c
#define configUSE_RECURSIVE_MUTEXES       1   /* 必需：总线锁是可重入的 */
#define configSUPPORT_DYNAMIC_ALLOCATION  1   /* 必需：本 port 用 pvPortMalloc */
```

缺任何一项都会在编译期被 `#error` 拦住，不会拖到运行期才发现。

`configUSE_RECURSIVE_MUTEXES = 0` 时 `xSemaphoreCreateRecursiveMutex()` 不存在；
`configSUPPORT_DYNAMIC_ALLOCATION = 0` 时 `pvPortMalloc()` 不存在。
两者都必须为 1，或者照第 5 节自己写一个静态分配的 port。

### 1.2 集成到你的 MCU 工程

不用 CMake 时，手工把**这些文件**加进编译列表：

```
include/message_bus/*.h        ← 头文件路径
src/mb_bus.c
src/mb_dispatch.c
src/mb_log.c
src/mb_message.c
src/mb_node.c
src/mb_subscription.c
src/mb_topic.c
port/mb_os_freertos.c          ← 只加这一个 port 文件
```

并在工程的头文件搜索路径与编译定义里加上：

```c
#define MB_CONFIG_OS MB_OS_FREERTOS
```

（写在你的 `mb_conf.h` 里，用 `-DMB_CONF_PATH="\"mb_conf.h\""` 引入；
或者直接在 IDE 的预定义宏里填。）

完整配置项见 [`config/mb_conf_template.h`](../config/mb_conf_template.h)。

### 1.2.1 Arduino / PlatformIO（ESP32 等）

库自带 [`library.json`](../library.json)，PlatformIO 会自动：

- 把 `include/` 加进头文件搜索路径；
- 编译 `src/*.c` 与 `port/*.c`（未被选中的 port 编译为空文件）。

工程侧只需要：

```ini
; platformio.ini
build_flags =
    -D MB_CONF_PATH="\"mb_conf.h\""
```

并让 `mb_conf.h` 里写着：

```c
#define MB_CONFIG_OS MB_OS_FREERTOS
```

Arduino-ESP32 / ESP-IDF 的 FreeRTOS 头文件位于 `freertos/` 子目录
（`freertos/FreeRTOS.h`、`freertos/semphr.h`、`freertos/task.h`），
而独立 FreeRTOS 工程通常直接在根目录。`mb_os.h` 与 `mb_os_freertos.c`
用 `__has_include` 自动探测这两种布局，**无需任何额外的 `-I` 配置**。

`configUSE_RECURSIVE_MUTEXES` 与 `configSUPPORT_DYNAMIC_ALLOCATION` 在
Arduino-ESP32 的默认配置里已经是 `1`，可直接使用。

### 1.3 任务骨架

下面是一个完整的、可以直接抄的 FreeRTOS 集成示例。
它演示了本库推荐的用法：**一个任务采集、一个任务消费界面、总线在 main 里创建**。

```c
/* -------------------------------------------------------------------------
 * app_message_bus.h —— 全局总线句柄与节点句柄
 * ---------------------------------------------------------------------- */
#ifndef APP_MESSAGE_BUS_H
#define APP_MESSAGE_BUS_H

#include "message_bus/message_bus.h"

extern mb_bus_t  *g_bus;
extern mb_node_t *g_sensor_node;
extern mb_node_t *g_motor_node;
extern mb_node_t *g_ui_node;

void app_bus_init(void);          /* 启动调度器之前调用 */
void sensor_task(void *arg);      /* 传感器采集任务 */
void ui_task(void *arg);          /* LVGL 任务 */
void motor_task(void *arg);       /* 电机控制任务 */

#endif
```

```c
/* -------------------------------------------------------------------------
 * app_message_bus.c
 * ---------------------------------------------------------------------- */
#include "app_message_bus.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>

mb_bus_t  *g_bus         = NULL;
mb_node_t *g_sensor_node = NULL;
mb_node_t *g_motor_node  = NULL;
mb_node_t *g_ui_node     = NULL;

/* ⚠️ 跨任务传递数据用的队列。回调运行在**发布者的任务**里，
 *    所以界面不能在回调里直接碰控件，只能把数据丢进队列。 */
static QueueHandle_t g_ui_update_queue;

typedef struct {
    char label[16];
    char text[32];
} ui_update_t;

/* ── 电机：只认 motor/cmd ── */
static void on_motor_cmd(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    char cmd[32];
    size_t len = msg->payload_len;
    mb_publish_opts_t opts;

    (void)sub;
    (void)user_data;

    if (len >= sizeof(cmd)) {
        len = sizeof(cmd) - 1;
    }
    memcpy(cmd, msg->payload, len);
    cmd[len] = '\0';

    /* ↓ 这里换成真正的 HAL 调用：PWM、GPIO、编码器 ↓ */
    if (strncmp(cmd, "speed=", 6) == 0) {
        motor_set_speed(atoi(cmd + 6));
    } else if (strcmp(cmd, "stop") == 0) {
        motor_stop();
    }

    /* 上报状态。retained：界面后启动也能立刻拿到当前转速 */
    {
        char status[32];

        snprintf(status, sizeof(status), "%d rpm", motor_get_speed());
        opts.flags = MB_PUB_FLAG_RETAIN;
        opts.qos   = 0;
        mb_node_publish(g_motor_node, "motor/speed/status", status, strlen(status), &opts);
    }
}

/* ── 界面：回调只把数据塞进队列，绝不碰控件 ── */
static void on_sensor_value(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    ui_update_t upd;
    BaseType_t woken = pdFALSE;

    (void)sub;

    /* user_data 里放的是控件名，避免在回调里反查 */
    snprintf(upd.label, sizeof(upd.label), "%s", (const char *)user_data);
    snprintf(upd.text, sizeof(upd.text), "%.*s",
             (int)msg->payload_len, (const char *)msg->payload);

    /* 从任务上下文调用：用 FromISR 版本才需要 woken；
     * 这里回调运行在普通任务里，但保留 woken 参数无害 */
    (void)xQueueSend(g_ui_update_queue, &upd, 0);
    (void)woken;
}

/* ── 初始化：必须在 vTaskStartScheduler() 之前调用 ── */
void app_bus_init(void)
{
    mb_bus_config_t cfg;
    mb_publish_opts_t opts;

    mb_bus_default_config(&cfg);        /* publish_node_events = true */
    mb_bus_create_ex("app", &cfg, &g_bus);

    mb_node_create(g_bus, "sensor", &g_sensor_node);
    mb_node_create(g_bus, "motor",  &g_motor_node);
    mb_node_create(g_bus, "ui",     &g_ui_node);

    g_ui_update_queue = xQueueCreate(8, sizeof(ui_update_t));

    /* 界面只认主题，不认识电机和传感器 */
    mb_node_subscribe(g_ui_node, "sensor/+/value", on_sensor_value, (void *)"temp", NULL);
    mb_node_subscribe(g_ui_node, "motor/+/status", on_sensor_value, (void *)"motor", NULL);

    /* 电机只认命令主题 */
    mb_node_subscribe(g_motor_node, "motor/cmd", on_motor_cmd, NULL, NULL);

    /* 上报一次初始状态（retained），界面后启动也能显示 */
    opts.flags = MB_PUB_FLAG_RETAIN;
    opts.qos   = 0;
    mb_node_publish(g_motor_node, "motor/speed/status", "0 rpm", 5, &opts);

    /* 在启动调度器之前加锁/解锁是安全的：锁已经能用了，
     * 但绝对不要在这里调用 mb_os_sleep_ms()（vTaskDelay 在调度器启动前非法）。 */
}

/* ── 传感器任务：1Hz 采样 ── */
void sensor_task(void *arg)
{
    (void)arg;

    for (;;) {
        char payload[16];
        mb_publish_opts_t opts;

        snprintf(payload, sizeof(payload), "%.1f", sensor_read_temperature());

        opts.flags = MB_PUB_FLAG_RETAIN;   /* 当前值是"状态"，用 retained */
        opts.qos   = 0;
        mb_node_publish(g_sensor_node, "sensor/temp/value", payload, strlen(payload), &opts);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── 界面任务：消费队列 + 跑 LVGL ── */
void ui_task(void *arg)
{
    ui_update_t upd;

    (void)arg;

    for (;;) {
        /* 拿不到就等，拿到就刷新。控件只在这个任务里被碰。 */
        if (xQueueReceive(g_ui_update_queue, &upd, portMAX_DELAY) == pdTRUE) {
            ui_apply_update(upd.label, upd.text);   /* lv_label_set_text 等 */
        }

        /* 真实工程里这里是 lv_timer_handler() 的节奏，不是 portMAX_DELAY 阻塞 */
    }
}

/* ── main ── */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    app_bus_init();                       /* 调度器启动前建好总线 */

    xTaskCreate(sensor_task, "sensor", 512, NULL, 3, NULL);
    xTaskCreate(motor_task,  "motor",  512, NULL, 3, NULL);
    xTaskCreate(ui_task,     "ui",    2048, NULL, 2, NULL);

    vTaskStartScheduler();                /* 不返回 */
    for (;;) {
    }
}
```

### 1.4 FreeRTOS 上的注意事项

| 事项 | 说明 |
|---|---|
| 调度器启动前可用 | 创造总线/节点/订阅、发布消息都可以在 `main()` 里做（会自动投递到已存在的订阅）。<br>**但不要调用 `mb_os_sleep_ms()`** —— 库内部也不调用它，它只是留给用户的。 |
| 中断里不要直接发布 | 投递会执行用户回调，回调可能耗时。正确做法是在 ISR 里 `xQueueSendToBackFromISR()`，由任务取出后再 `publish`。 |
| 栈占用 | 投递时栈上会有 `MB_CONFIG_DELIVER_BATCH` 个指针（默认 8 × 4 字节 = 32 字节），加上回调自身的栈。回调里再发布会嵌套。 |
| 优先级反转 | 总线锁是普通互斥量（`xSemaphoreCreateRecursiveMutex` 自带优先级继承）。回调运行在**发布者的优先级**上，低优先级任务发布时可能被其订阅者的工作量拖慢。 |
| 堆 | 默认用 `pvPortMalloc`。若 `configSUPPORT_DYNAMIC_ALLOCATION = 0`，见第 5 节。 |
| 不要在回调里 `vTaskDelay` | 阻塞的是发布者任务。回调应当尽量短。 |
| 异步投递需要 `configUSE_COUNTING_SEMAPHORES = 1` | `mb_os_sem_*` 用 `xSemaphoreCreateCounting()` 实现。没打开这个开关时编译期会直接 `#error`，不会到运行期才炸。 |
| 异步 API 需要调度器已启动 | 计数信号量在调度器启动前不能用，`mb_node_publish_async()` / `mb_bus_pump()` 必须在任务里调用（`mb_node_publish()` 则不受限）。 |

---

## 2. 裸机 / 无 RTOS（`MB_OS_NONE`）

裸机模式下没有真的锁，`mb_os_mutex_*` 退化成一对宏：

```c
/* 在 mb_conf.h 里提供（不提供则默认为空操作） */
#define MB_CONFIG_CRITICAL_ENTER()   __disable_irq()
#define MB_CONFIG_CRITICAL_EXIT()    __enable_irq()
```

三条必须遵守的约定：

1. **临界区必须是可嵌套（递归）语义。** 库内部存在「已持锁时再次加锁」的路径
   （节点上线事件在持锁路径上触发发布）。上面的 `__disable_irq()` / `__enable_irq()`
   在嵌套时会在内层就把中断打开，**这是错的**，需要自己实现计数：

   ```c
   static volatile uint32_t g_crit_depth = 0;
   static volatile uint32_t g_saved_primask;

   #define MB_CONFIG_CRITICAL_ENTER()                       \
       do {                                                 \
           uint32_t primask = __get_PRIMASK();              \
           __disable_irq();                                 \
           if (g_crit_depth++ == 0) g_saved_primask = primask; \
       } while (0)

   #define MB_CONFIG_CRITICAL_EXIT()                        \
       do {                                                 \
           if (--g_crit_depth == 0) __set_PRIMASK(g_saved_primask); \
       } while (0)
   ```

2. **不要在中断里调用总线 API。** 裸机下总线只保证「主循环单线程」安全。
   如果非要在 ISR 里发布，上面的临界区会关中断，但**用户回调也会在中断上下文里执行** ——
   这几乎肯定不是你想要的。标准做法仍然是 ISR 里置标志，主循环里发布。

3. **时间基准要接上**，否则所有消息的 `timestamp_ms` 都是 0：

   ```c
   /* 在 SysTick 中断里自增，然后： */
   #define MB_CONFIG_TIME_MS()  (g_systick_ms)
   ```

内存钩子也可以换成静态内存池，实现零动态分配：

```c
#define MB_CONFIG_MALLOC(size)        my_pool_alloc(size)
#define MB_CONFIG_CALLOC(count, size) my_pool_calloc(count, size)
#define MB_CONFIG_FREE(ptr)           my_pool_free(ptr)
```

### 2.1 裸机上没有真正的阻塞

`mb_os_sem_*` 在裸机下只能用临界区模拟计数，**无法让出一个执行流**。因此：

| 场景 | 裸机上的实际行为 |
|---|---|
| `mb_node_publish_async(..., MB_WAIT_FOREVER)` | 退化成不等待；队列满时立刻返回 `MB_ERR_TIMEOUT` |
| `mb_node_publish_async(..., 100)` | 同上（超时参数被忽略） |
| 条目名额用尽时发布新主题 | 同上 |
| `mb_bus_pump()` | 与其它平台一致（本来就不阻塞） |

也就是说 **`MB_ERR_TIMEOUT` 在裸机上是常态而不是异常**，发布方必须每次都检查返回值。
推荐二选一：

```c
/* 方案 A：状态类主题用覆盖，永不因为队列满而丢新值 */
#define MB_CONFIG_ASYNC_OVERWRITE_OLDEST 1

/* 方案 B：干脆关掉异步，回到同步投递（单线程裸机本来也不需要它） */
#define MB_CONFIG_ASYNC_MAX_TOPICS 0
```

裸机上异步投递的价值本来就有限 —— 它解决的是「跨线程」，而裸机只有一个主循环。
如果只是想让回调别在 ISR 里跑，`main()` 里 `mb_bus_pump()` 一次即可，
队列深度给 1 就够。

---

## 3. PC 模拟器（`MB_OS_POSIX` / `MB_OS_WIN32`）

这两个 port 是给 LVGL 模拟器用的，**不需要做任何事** —— CMake 会自动选：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/bin/example_03_lvgl_motor_sensor
```

| 平台 | 锁 | 信号量 | 时间 | 内存 |
|---|---|---|---|---|
| POSIX | `pthread_mutex_t`，`PTHREAD_MUTEX_RECURSIVE` | `pthread_mutex_t` + `pthread_cond_t` 手搓计数信号量 | `clock_gettime(CLOCK_MONOTONIC)` | `malloc` / `calloc` / `free` |
| Win32 | `CRITICAL_SECTION`（天然可重入） | `CreateSemaphore` + `WaitForSingleObject` | `GetTickCount64()` | 同上 |

> POSIX 为什么手搓而不是用 `sem_t`？因为 `sem_timedwait()` 是 POSIX.1-2001 的可选项，
> **macOS 至今没有实现**，而本库要能在 Mac 上编译。条件变量是普适的。
> 注意 `pthread_cond_timedwait()` 默认走 `CLOCK_REALTIME`，所以超时时刻也用
> `clock_gettime(CLOCK_REALTIME)` 算 —— 拿 `CLOCK_MONOTONIC` 去填会算出荒谬的等待时长。

用 POSIX port 时需要链接 pthread（CMake 的 `Threads::Threads` 已自动处理）。
手工编译时记得加 `-lpthread`。

---

## 4. 新增一个 port（其它 RTOS）

以 RT-Thread 为例。**只需实现 7 个函数**（不用异步投递的话；用到异步还要再加 4 个
信号量函数，见本节末尾）：

```c
/* port/mb_os_rtthread.c */
#include "message_bus/mb_config.h"

/* 未被选中时编译为空文件：这样递归扫描 .c 的构建系统（Arduino/PlatformIO）
 * 可以放心地把所有 port 文件一起编译。需要在 mb_config.h 里加 MB_OS_RTTHREAD。 */
#if MB_CONFIG_OS == MB_OS_RTTHREAD

#include "message_bus/mb_os.h"
#include <rtthread.h>
#include <string.h>

mb_mutex_t *mb_os_mutex_create(void)
{
    mb_mutex_t  *mutex = (mb_mutex_t *)rt_malloc(sizeof(*mutex));
    rt_mutex_t   m;

    if (mutex == NULL) return NULL;

    /* RT-Thread 的互斥量默认支持递归（持有者重入时计数递增） */
    m = rt_mutex_create("mb", RT_IPC_FLAG_PRIO);
    if (m == RT_NULL) { rt_free(mutex); return NULL; }
    mutex->ptr = m;
    return mutex;
}

void mb_os_mutex_lock(mb_mutex_t *mutex)    { rt_mutex_take((rt_mutex_t)mutex->ptr, RT_WAITING_FOREVER); }
void mb_os_mutex_unlock(mb_mutex_t *mutex)  { rt_mutex_release((rt_mutex_t)mutex->ptr); }

void mb_os_mutex_destroy(mb_mutex_t *mutex)
{
    if (mutex == NULL) return;
    if (mutex->ptr != NULL) rt_mutex_delete((rt_mutex_t)mutex->ptr);
    rt_free(mutex);
}

uint32_t mb_os_time_ms(void)                { return (uint32_t)rt_tick_get_millisecond(); }
void     mb_os_sleep_ms(uint32_t ms)        { rt_thread_mdelay((rt_int32_t)ms); }

void *mb_os_malloc(size_t size)             { return rt_malloc(size); }
void *mb_os_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void  *ptr;

    if (count != 0 && total / count != size) return NULL;   /* 溢出检查 */
    ptr = rt_malloc(total);
    if (ptr != NULL) memset(ptr, 0, total);
    return ptr;
}
void  mb_os_free(void *ptr)                 { rt_free(ptr); }

#endif /* MB_CONFIG_OS == MB_OS_RTTHREAD */
```

顺便还要在 `mb_os.h` 里加上对应的类型分支：

```c
#elif MB_CONFIG_OS == MB_OS_RTTHREAD
#include <rtthread.h>
typedef union mb_mutex {
    void *ptr;
} mb_mutex_t;

typedef union mb_sem {
    rt_sem_t rtthread;
} mb_sem_t;
```

### 4.1 如果需要异步投递：再实现 4 个信号量函数

`mb_os_sem_*` 必须是**计数信号量**（不是二值的），并且 `mb_os_sem_wait()`
要支持带超时的等待。RT-Thread 上直接映射：

```c
mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max)
{
    mb_sem_t *sem = (mb_sem_t *)rt_malloc(sizeof(*sem));

    if (sem == NULL) return NULL;
    if (max == 0 || initial > max) { rt_free(sem); return NULL; }

    /* RT_IPC_FLAG_PRIO：等待者按优先级排队，不是 FIFO。库不依赖唤醒顺序。 */
    sem->rtthread = rt_sem_create("mb", initial, RT_IPC_FLAG_PRIO);
    if (sem->rtthread == RT_NULL) { rt_free(sem); return NULL; }
    return sem;
}

bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms)
{
    rt_int32_t ticks;

    if (sem == NULL) return false;
    if (timeout_ms == MB_WAIT_FOREVER) {
        ticks = RT_WAITING_FOREVER;
    } else {
        /* 向上取整：宁可多等不到一个 tick，也不能少等到 0（= 不等待）。
         * 先除后补余数，不要写 (a + b - 1) / b —— timeout_ms 接近
         * 0xFFFFFFFF 时那个加法会溢出，把大超时算成「立刻超时」。 */
        uint32_t period = RT_TICK_PER_SECOND / 1000;
        uint32_t whole  = timeout_ms / period;

        ticks = (rt_int32_t)(whole + (((timeout_ms % period) != 0) ? 1 : 0));
    }
    return rt_sem_take(sem->rtthread, ticks) == RT_EOK;
}

bool mb_os_sem_signal(mb_sem_t *sem)
{
    if (sem == NULL) return false;
    return rt_sem_release(sem->rtthread) == RT_EOK;
}

void mb_os_sem_destroy(mb_sem_t *sem)
{
    if (sem == NULL) return;
    if (sem->rtthread != RT_NULL) rt_sem_delete(sem->rtthread);
    rt_free(sem);
}
```

`mb_os_sem_wait()` 返回 `false` 表示**超时**（不是错误）；调用方据此返回
`MB_ERR_TIMEOUT`。`MB_WAIT_NONE`（0）表示不等待，必须立即返回当前是否拿得到。

### 移植检查清单

- [ ] 锁是**递归**的（同一个执行流重复加锁必须成功）
- [ ] `mb_os_time_ms()` 单调递增，且**无符号回绕语义**正确（`t2 - t1` 在回绕后仍正确）
- [ ] `mb_os_calloc()` 做了乘法溢出检查
- [ ] **没有实现/使用 `realloc`** —— 库刻意不用它，很多 RTOS 堆没有
- [ ] 用 `#if MB_CONFIG_OS == ...` 包住整个实现（未选中时编译为空文件）
- [ ] 跑通 `ctest` 里的 `threads` 套件（4 线程 × 500 条消息的并发测试）
- [ ] 用到异步时：跑通 `async` 套件；`mb_os_sem_wait()` 的毫秒超时要**向上取整**成 tick
      （向下取整会让 1ms 变成 0，退化成不等待）
- [ ] 用到异步时：`mb_os_sem_signal()` 从**任意线程**调用都必须安全（`mb_bus_pump()`
      会在锁内调用它来唤醒阻塞中的发布者）

最后一条最重要 —— 它是唯一能真正验证「锁确实是对的」的手段，
在 PC 上用 POSIX port 跑一遍，再上目标板。

---

## 5. 静态分配（`configSUPPORT_DYNAMIC_ALLOCATION = 0`）

如果你必须完全避免动态分配，有两条路：

**路线 A（推荐）：换内存后端，保留 FreeRTOS port**

不用改 `mb_os_freertos.c`，只要让它的三个内存函数走你的静态池。最简单的做法是
复制一份 `mb_os_freertos.c` 到你的工程里，把 `mb_os_malloc/calloc/free` 的实现换成池：

```c
static uint8_t g_pool[16 * 1024];
/* ... 自己实现一个简单的块分配器，或者用第三方（如 o1heap） ... */
```

**路线 B：用 `MB_OS_NONE` + 静态池钩子**

如第 2 节所示，`MB_OS_NONE` 把内存分配完全交给 `MB_CONFIG_MALLOC/CALLOC/FREE` 三个宏，
接上静态池即可 —— 代价是失去真正的锁（除非你同时提供临界区宏）。

无论哪条路，**总线本身的可变对象数量是固定的**：
节点表、订阅表、retained 表会在运行期增长（发布 retained 消息时分配），
所以池的大小要按「最大节点数 + 最大订阅数 + 最大 retained 条目数」来估算。
`mb_bus_get_stats()` 的 `peak_subscriptions` 和 `retained_stored` 可以帮你测出实际峰值。

---

## 6. 移植后务必验证

```bash
# 1. 严格告警下零警告（能提前发现大量移植错误）
cmake -S . -B build -DMB_OS=freertos \
      -DMB_FREERTOS_INCLUDE_DIR=... -DMB_WARNINGS_AS_ERRORS=ON
cmake --build build

# 2. 在 PC 上先用 POSIX/Win32 port 跑通全部测试（并发测试只有 PC 能跑）
cmake -S . -B build-pc && cmake --build build-pc && ctest --test-dir build-pc

# 3. 上板后，用 mb_bus_get_stats() 观察实际行为是否符合预期
mb_bus_stats_t stats;
mb_bus_get_stats(bus, &stats);
/* published / delivered / dropped / no_subscriber / retained_stored
 * / nodes_created / peak_subscriptions */
```

`no_subscriber` 偏高通常意味着主题名拼错了；`dropped` 不为 0 说明回调里有发布环。
