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
 *     若为 0，请复制本文件并把三个内存函数改成静态内存池实现）。
 *
 * 本文件不使用任何阻塞式的队列/任务，因此可以在任意任务（含定时器任务）
 * 中调用总线 API，也可以在启动调度器之前创建总线与节点。
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
