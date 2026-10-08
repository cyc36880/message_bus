/**
 * @file mb_os_freertos.c
 * @brief FreeRTOS 的 OS 抽象层实现（MCU 目标平台）。
 *
 * 启用方式：在工程的 mb_conf.h 中写
 *
 *     #define MB_CONFIG_OS MB_OS_FREERTOS
 *
 * 用 CMake 或手工维护编译列表时，把本文件加进去（PC 构建时用的是
 * mb_os_win32.c / mb_os_posix.c）；Arduino / PlatformIO 则由 library.json
 * 自动编译，无需额外配置。本文件未选中时编译为空目标文件。
 *
 * 前置条件：
 *   - FreeRTOS.h / task.h / semphr.h 在 include 路径中（ESP-IDF 与
 *     Arduino-ESP32 位于 freertos/ 子目录，本文件会自动探测）；
 *   - configUSE_RECURSIVE_MUTEXES = 1（总线内部锁需要可重入）；
 *   - configSUPPORT_DYNAMIC_ALLOCATION = 1（本文件用 pvPortMalloc；
 *     若为 0，请复制本文件并把三个内存函数改成静态内存池实现）；
 *   - 使用**异步发布**时还需要 configUSE_COUNTING_SEMAPHORES = 1。
 *
 * 同步 API 不使用任何阻塞式原语，因此可以在任意任务（含定时器任务）中
 * 调用，也可以在启动调度器之前创建总线与节点。
 *
 * ⚠️ 异步 API 例外：mb_node_publish_async() 在队列满时会阻塞、
 * mb_bus_pump() 需要被某个任务反复调用，两者都要求**调度器已经在跑**。
 */
#include "message_bus/mb_config.h"

/*
 * 只有被选中的后端才产生代码；未选中时本文件编译为空目标文件。
 * 这样 Arduino / PlatformIO 这类会递归扫描并编译库内所有 .c 的构建系统
 * 无需配置源文件过滤，也不会因为多编译了别的 port 文件而触发 #error。
 * 若选中的后端文件缺失，会在链接阶段报 undefined reference to mb_os_*。
 */
#if MB_CONFIG_OS == MB_OS_FREERTOS

#include "message_bus/mb_os.h"

#include <string.h>

/* mb_os.h 已按发行版布局引入 FreeRTOS.h / semphr.h（ESP-IDF 与 Arduino-ESP32
 * 位于 freertos/ 子目录）；vTaskDelay / xTaskGetTickCount 还需要 task.h。 */
#if defined(__has_include)
#if __has_include("freertos/task.h")
#include "freertos/task.h"
#else
#include "task.h"
#endif
#else
#include "task.h"
#endif

#if defined(configUSE_RECURSIVE_MUTEXES) && (configUSE_RECURSIVE_MUTEXES != 1)
#error "message_bus requires FreeRTOS configUSE_RECURSIVE_MUTEXES = 1"
#endif

/* 异步投递的队列满/空靠计数信号量阻塞；关掉异步就不需要这一项 */
#if MB_CONFIG_ASYNC_MAX_TOPICS > 0
#if defined(configUSE_COUNTING_SEMAPHORES) && (configUSE_COUNTING_SEMAPHORES != 1)
#error "message_bus async publishing requires FreeRTOS configUSE_COUNTING_SEMAPHORES = 1"
#endif
#endif

#if defined(configSUPPORT_DYNAMIC_ALLOCATION) && (configSUPPORT_DYNAMIC_ALLOCATION == 0)
#error "mb_os_freertos.c requires configSUPPORT_DYNAMIC_ALLOCATION = 1; \
if you must avoid dynamic allocation, copy this file and reimplement \
mb_os_malloc/calloc/free on top of a static pool"
#endif

/** 老版本 FreeRTOS 只有 portTICK_RATE_MS。 */
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS (1000 / configTICK_RATE_HZ)
#endif

mb_mutex_t *mb_os_mutex_create(void)
{
    mb_mutex_t *mutex = (mb_mutex_t *)pvPortMalloc(sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }
    mutex->freertos = xSemaphoreCreateRecursiveMutex();
    if (mutex->freertos == NULL) {
        vPortFree(mutex);
        return NULL;
    }
    return mutex;
}

void mb_os_mutex_lock(mb_mutex_t *mutex)
{
    /* portMAX_DELAY：总线临界区都很短，不存在长时间持锁 */
    (void)xSemaphoreTakeRecursive(mutex->freertos, portMAX_DELAY);
}

void mb_os_mutex_unlock(mb_mutex_t *mutex)
{
    (void)xSemaphoreGiveRecursive(mutex->freertos);
}

void mb_os_mutex_destroy(mb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return;
    }
    if (mutex->freertos != NULL) {
        vSemaphoreDelete(mutex->freertos);
    }
    vPortFree(mutex);
}

mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max)
{
    mb_sem_t *sem;

    if (max == 0 || initial > max) {
        return NULL;
    }

    sem = (mb_sem_t *)pvPortMalloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    /* 注意 UBaseType_t 在部分移植上是 8 位，max 超过 255 会被静默截断 */
    sem->freertos = xSemaphoreCreateCounting((UBaseType_t)max, (UBaseType_t)initial);
    if (sem->freertos == NULL) {
        vPortFree(sem);
        return NULL;
    }
    return sem;
}

bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms)
{
    TickType_t ticks;

    /* 必须先判永久等待：0xFFFFFFFF 毫秒经过 pdMS_TO_TICKS 会溢出成一个
     * 有限的、不可预期的 tick 数。 */
    if (timeout_ms == MB_WAIT_FOREVER) {
        ticks = portMAX_DELAY;
    } else {
        /* pdMS_TO_TICKS 是向下取整：1ms 在 100Hz 的 tick 下会变成 0 tick，
         * 也就是「不等」，与「最多等 1ms」的承诺不符。这里向上取整。
         *
         * 不用 (a + b - 1) / b 那个常见写法：timeout_ms 接近 0xFFFFFFFF 时
         * 加法会溢出，一个大超时会被算成接近 0 —— 变成「立刻超时」，
         * 而且是静默的。先除后补余数不会溢出。 */
        uint32_t period = (uint32_t)portTICK_PERIOD_MS;
        uint32_t whole = timeout_ms / period;

        ticks = (TickType_t)(whole + (((timeout_ms % period) != 0u) ? 1u : 0u));
    }

    return xSemaphoreTake(sem->freertos, ticks) == pdTRUE;
}

bool mb_os_sem_signal(mb_sem_t *sem)
{
    return xSemaphoreGive(sem->freertos) == pdTRUE;
}

void mb_os_sem_destroy(mb_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    if (sem->freertos != NULL) {
        vSemaphoreDelete(sem->freertos);
    }
    vPortFree(sem);
}

uint32_t mb_os_time_ms(void)
{
    /* 先扩到 64 位再乘，避免 TickType_t 为 32 位时溢出 */
    return (uint32_t)((uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS);
}

void mb_os_sleep_ms(uint32_t ms)
{
    /* 注意：调度器启动前调用 vTaskDelay 是非法操作 */
    vTaskDelay(pdMS_TO_TICKS(ms));
}

mb_thread_id_t mb_os_thread_id(void)
{
    /* TaskHandle_t 就是 TCB 指针，同一任务恒定、不同任务互异，正是要的语义。
     * 调度器启动前它可能返回 NULL —— 库只做相等比较，NULL 与 NULL 相等
     * 也不会误判（此时不可能有线程在 pump）。 */
    return (mb_thread_id_t)xTaskGetCurrentTaskHandle();
}

void *mb_os_malloc(size_t size)
{
    return pvPortMalloc(size);
}

void *mb_os_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr;

    if (count != 0 && total / count != size) {
        return NULL; /* 乘法溢出 */
    }
    ptr = pvPortMalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void mb_os_free(void *ptr)
{
    vPortFree(ptr);
}

#endif /* MB_CONFIG_OS == MB_OS_FREERTOS */
