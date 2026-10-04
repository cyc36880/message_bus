/**
 * @file mb_os.h
 * @brief OS 抽象层（LVGL 风格）。
 *
 * 整个库只通过本文件接触平台相关的原语：互斥量、时间、内存。
 * 想换 RTOS 时**不需要改动库代码**，只要：
 *
 *   1. 在 mb_conf.h 里把 MB_CONFIG_OS 改成目标 OS（例如 MB_OS_FREERTOS）；
 *   2. 在工程编译列表里加入 port/mb_os_freertos.c（而不是 mb_os_win32.c）；
 *   3. 保证该 OS 的头文件在 include 路径里。
 *
 * 若目标 OS 不在内置列表内，可以选择：
 *   - 用 MB_CONFIG_OS = MB_OS_NONE，然后自己提供临界区宏（见 mb_os_none.c）；
 *   - 新增 port/mb_os_<your_os>.c，实现本文件声明的全部函数。
 *
 * 所有函数都不接受 NULL：库内部保证不会传入 NULL。
 */
#ifndef MESSAGE_BUS_MB_OS_H
#define MESSAGE_BUS_MB_OS_H

#include <stddef.h>
#include <stdint.h>

#include "mb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 互斥量类型：按 MB_CONFIG_OS 展开成对应平台的真实类型，
 * 布局对用户可见，因此也可以静态分配（见文档）。
 * ---------------------------------------------------------------------- */
#if MB_CONFIG_OS == MB_OS_WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef union mb_mutex {
    CRITICAL_SECTION win32; /**< 递归锁，Windows 下天然可重入 */
} mb_mutex_t;

#elif MB_CONFIG_OS == MB_OS_POSIX

#include <pthread.h>

typedef union mb_mutex {
    pthread_mutex_t posix; /**< PTHREAD_MUTEX_RECURSIVE */
} mb_mutex_t;

#elif MB_CONFIG_OS == MB_OS_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"

typedef union mb_mutex {
    SemaphoreHandle_t freertos; /**< xSemaphoreCreateRecursiveMutex() 的句柄 */
} mb_mutex_t;

#elif MB_CONFIG_OS == MB_OS_NONE

typedef union mb_mutex {
    void *ptr; /**< 裸机下不使用，仅用于占位 */
} mb_mutex_t;

#else
#error "invalid MB_CONFIG_OS: use MB_OS_NONE / MB_OS_FREERTOS / MB_OS_POSIX / MB_OS_WIN32"
#endif

/* -------------------------------------------------------------------------
 * 互斥量
 *
 * 约定：本库申请到的锁必须是**递归锁**，因为库允许在持有锁的临界区内
 * 再次进入（例如节点上线事件触发的发布路径）。
 * ---------------------------------------------------------------------- */

/**
 * 创建一把递归互斥量。
 * @return 新锁；内存不足或平台不支持时返回 NULL。
 */
mb_mutex_t *mb_os_mutex_create(void);

/** 加锁。同一个执行流重复加锁必须成功（递归）。 */
void mb_os_mutex_lock(mb_mutex_t *mutex);

/** 解锁。必须与 mb_os_mutex_lock 配对，且加锁/解锁次数一致。 */
void mb_os_mutex_unlock(mb_mutex_t *mutex);

/** 销毁锁并释放其内存。调用前必须处于未加锁状态。 */
void mb_os_mutex_destroy(mb_mutex_t *mutex);

/* -------------------------------------------------------------------------
 * 时间
 * ---------------------------------------------------------------------- */

/**
 * @return 单调递增的毫秒计数（自系统启动/库初始化起）。
 *         使用无符号回绕语义，两个时间点相减仍然正确。
 */
uint32_t mb_os_time_ms(void);

/** 睡眠指定毫秒。裸机实现可以退化为忙等或空操作。 */
void mb_os_sleep_ms(uint32_t ms);

/* -------------------------------------------------------------------------
 * 内存
 *
 * 刻意不提供 realloc：很多 RTOS 的堆实现没有它。库内部需要扩容时
 * 一律使用 malloc + memcpy + free，因此移植时只需实现下面三个函数。
 * 嵌入式工程可以在这里接入静态内存池，实现「零动态分配」。
 * ---------------------------------------------------------------------- */

void *mb_os_malloc(size_t size);
void *mb_os_calloc(size_t count, size_t size);
void mb_os_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_OS_H */
