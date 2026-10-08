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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb_config.h"
#include "mb_types.h" /* MB_WAIT_FOREVER / MB_WAIT_NONE */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 互斥量 / 信号量类型：按 MB_CONFIG_OS 展开成对应平台的真实类型，
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

typedef union mb_sem {
    HANDLE win32; /**< CreateSemaphore() 的句柄 */
} mb_sem_t;

#elif MB_CONFIG_OS == MB_OS_POSIX

#include <pthread.h>

typedef union mb_mutex {
    pthread_mutex_t posix; /**< PTHREAD_MUTEX_RECURSIVE */
} mb_mutex_t;

/**
 * POSIX 下用「互斥量 + 条件变量」自己实现计数信号量。
 *
 * 刻意不用 sem_t / sem_timedwait：macOS 根本没有 sem_timedwait，
 * 而 pthread_cond_timedwait 在三大平台都可用。
 * 代价是默认条件变量基于 CLOCK_REALTIME 计时，实现里必须用同一个时钟
 * 算截止时刻（见 port/mb_os_posix.c）。
 */
typedef struct mb_posix_sem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t count;
    uint32_t max;
} mb_posix_sem_t;

typedef union mb_sem {
    mb_posix_sem_t posix;
} mb_sem_t;

#elif MB_CONFIG_OS == MB_OS_FREERTOS

/*
 * FreeRTOS 头文件位置随发行版而异：
 *   - ESP-IDF / Arduino-ESP32：<freertos/FreeRTOS.h>、<freertos/semphr.h>
 *   - 独立 FreeRTOS 工程：直接在 include 根目录下的 <FreeRTOS.h>、<semphr.h>
 * 用 __has_include 自动探测，两种布局都能编译，无需工程侧配置。
 */
#if defined(__has_include)
#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include "FreeRTOS.h"
#include "semphr.h"
#endif
#else
#include "FreeRTOS.h"
#include "semphr.h"
#endif

typedef union mb_mutex {
    SemaphoreHandle_t freertos; /**< xSemaphoreCreateRecursiveMutex() 的句柄 */
} mb_mutex_t;

typedef union mb_sem {
    SemaphoreHandle_t freertos; /**< xSemaphoreCreateCounting() 的句柄 */
} mb_sem_t;

#elif MB_CONFIG_OS == MB_OS_NONE

typedef union mb_mutex {
    void *ptr; /**< 裸机下不使用，仅用于占位 */
} mb_mutex_t;

/** 裸机下的计数信号量：只有一个计数，无法阻塞（见下方 mb_os_sem_wait）。 */
typedef struct mb_none_sem {
    uint32_t count;
    uint32_t max;
} mb_none_sem_t;

typedef union mb_sem {
    mb_none_sem_t none;
} mb_sem_t;

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
 * 计数信号量（异步投递的队列满/空靠它阻塞）
 *
 * 语义与互斥量相反：加锁是「等锁被释放」，信号量是「等计数变成正数」。
 * 库只在异步队列里用它，且**永远不会在持有总线锁时做阻塞等待**。
 * ---------------------------------------------------------------------- */

/**
 * 创建一个计数信号量。
 *
 * @param initial 初始计数，必须 <= max。
 * @param max     计数上限；为 0 时创建失败。
 * @return 新信号量；参数非法、内存不足或平台不支持时返回 NULL。
 */
mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max);

/**
 * 等待计数变为正数并把它减一。
 *
 * @param timeout_ms 最多等多少毫秒：
 *                   MB_WAIT_NONE（0）= 不等待，拿不到立刻返回 false；
 *                   MB_WAIT_FOREVER   = 永久等待；
 *                   其它值            = 最多等这么多毫秒。
 * @return true  = 已取得一个计数（调用方**必须**最终归还一次）；
 *         false = 超时，没有取得任何计数。
 *
 * @note 裸机（MB_OS_NONE）无法阻塞：计数为 0 时一律立刻返回 false，
 *       即使 timeout_ms 是 MB_WAIT_FOREVER。详见 docs/porting.md。
 * @note FreeRTOS 下不能在调度器启动前做**阻塞**等待。
 */
bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms);

/**
 * 把计数加一，唤醒一个等待者。
 *
 * @return true  = 成功；
 *         false = 计数已达上限，这次释放被丢弃（调用方多释放了一次）。
 *
 * @note 非阻塞，可以在持有总线锁时调用。
 */
bool mb_os_sem_signal(mb_sem_t *sem);

/**
 * 销毁信号量并释放其内存。
 *
 * @warning 与 mb_os_mutex_destroy 同理：调用前必须保证**没有任何线程**
 *          正阻塞在这个信号量上，否则唤醒后会访问已释放的内存。
 */
void mb_os_sem_destroy(mb_sem_t *sem);

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
 * 线程标识
 *
 * 库用它回答一个问题：**「当前线程是不是那个正在跑 mb_bus_pump() 的线程？」**
 * 会阻塞的异步发布从 pump 线程发起必然死锁（归还空位的就是 pump，而它正卡在
 * 你的回调里），有了线程标识就能提前拒绝，而不是让调用方一直挂到超时。
 *
 * 只需要**相等比较**：同一个线程任何时候调用都必须返回同一个值，
 * 不同线程必须返回不同的值。不要求有序、不要求跨进程唯一，
 * 也不要求线程退出后该值不被复用。
 * ---------------------------------------------------------------------- */

/**
 * 当前线程的标识。
 *
 * 有的平台（POSIX）的线程标识是个不透明句柄，所以这里统一按无符号整数返回：
 * 实现方需要把平台句柄转成整数再返回（指针型句柄直接强转即可，
 * 因为我们只做相等比较，不做解引用）。
 *
 * @note 裸机（MB_OS_NONE）没有线程概念，固定返回 0 即可。
 */
typedef uintptr_t mb_thread_id_t;

/** @return 当前线程的标识；同一线程恒定不变。 */
mb_thread_id_t mb_os_thread_id(void);

/**
 * 本 port 能否**真正阻塞**。
 *
 * 裸机没有调度器，`mb_os_sem_wait()` 只能退化成「计数为 0 就立刻返回 false」，
 * 因此不存在「等自己」的死锁。库据此决定要不要做上面那个 pump 线程检查 ——
 * 不能阻塞的平台上如果也拦，会把本来能正常工作的调用挡掉。
 *
 * 新增 port 时：能阻塞就定义成 1（或干脆不定义，默认为 1）。
 */
#if MB_CONFIG_OS == MB_OS_NONE
#define MB_OS_CAN_BLOCK 0
#else
#define MB_OS_CAN_BLOCK 1
#endif

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
