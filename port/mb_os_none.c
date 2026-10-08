/**
 * @file mb_os_none.c
 * @brief 无 OS（裸机）的 OS 抽象层实现。
 *
 * 适用于「裸机主循环 + 状态机」的用法：没有 RTOS、没有任务、没有信号量。
 * 此时总线本身不做任何并发保护，**调用方必须保证**不会有两个执行流
 * （主循环与中断）同时调用总线 API。
 *
 * 如果确实需要在中断里发布消息，可以在 mb_conf.h 里给出临界区实现，
 * 本文件会自动使用它们：
 *
 *     #define MB_CONFIG_CRITICAL_ENTER()  __disable_irq()
 *     #define MB_CONFIG_CRITICAL_EXIT()   __enable_irq()
 *
 * 或在 FreeRTOS 上使用 taskENTER_CRITICAL()/taskEXIT_CRITICAL()。
 * 注意临界区必须是可嵌套的（递归）语义，因为库内部存在嵌套加锁的路径。
 *
 * 时间基准与睡眠同样通过宏注入，默认是「时间恒为 0、睡眠为空操作」。
 */
#include "message_bus/mb_config.h"

/*
 * 只有被选中的后端才产生代码；未选中时本文件编译为空目标文件。
 * 这样 Arduino / PlatformIO 这类会递归扫描并编译库内所有 .c 的构建系统
 * 无需配置源文件过滤，也不会因为多编译了别的 port 文件而触发 #error。
 */
#if MB_CONFIG_OS == MB_OS_NONE

#include "message_bus/mb_os.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * 可由 mb_conf.h 覆盖的钩子
 * ---------------------------------------------------------------------- */
#ifndef MB_CONFIG_CRITICAL_ENTER
#define MB_CONFIG_CRITICAL_ENTER() ((void)0)
#endif
#ifndef MB_CONFIG_CRITICAL_EXIT
#define MB_CONFIG_CRITICAL_EXIT() ((void)0)
#endif

/** 返回毫秒计数；裸机下通常接到 SysTick 计数上。 */
#ifndef MB_CONFIG_TIME_MS
#define MB_CONFIG_TIME_MS() ((uint32_t)0)
#endif

/** 毫秒级睡眠；裸机下默认空操作（不要在主循环里依赖它做精确延时）。 */
#ifndef MB_CONFIG_SLEEP_MS
#define MB_CONFIG_SLEEP_MS(ms) ((void)(ms))
#endif

/** 内存钩子，方便接入静态内存池。 */
#ifndef MB_CONFIG_MALLOC
#define MB_CONFIG_MALLOC(size) malloc(size)
#endif
#ifndef MB_CONFIG_CALLOC
#define MB_CONFIG_CALLOC(count, size) calloc((count), (size))
#endif
#ifndef MB_CONFIG_FREE
#define MB_CONFIG_FREE(ptr) free(ptr)
#endif

/* -------------------------------------------------------------------------
 * 实现
 * ---------------------------------------------------------------------- */

mb_mutex_t *mb_os_mutex_create(void)
{
    mb_mutex_t *mutex = (mb_mutex_t *)MB_CONFIG_MALLOC(sizeof(*mutex));

    return mutex; /* 内容不使用 */
}

void mb_os_mutex_lock(mb_mutex_t *mutex)
{
    (void)mutex;
    MB_CONFIG_CRITICAL_ENTER();
}

void mb_os_mutex_unlock(mb_mutex_t *mutex)
{
    (void)mutex;
    MB_CONFIG_CRITICAL_EXIT();
}

void mb_os_mutex_destroy(mb_mutex_t *mutex)
{
    MB_CONFIG_FREE(mutex);
}

/* -------------------------------------------------------------------------
 * 计数信号量：裸机下**无法阻塞**
 *
 * 没有调度器就没有「让出 CPU 再被唤醒」这回事，所以：
 *   - 计数为正时照常减一并返回 true；
 *   - 计数为 0 时立刻返回 false —— 即使传进来的是 MB_WAIT_FOREVER。
 *
 * 后果：异步发布在「主题条目满」时不会等待，而是直接返回 MB_ERR_TIMEOUT
 * （计入 stats.async_dropped）。裸机上请把 timeout_ms 传 0，并在主循环里
 * 反复调用 mb_bus_pump()，用「满了就丢/覆盖」的策略代替阻塞。
 * 若确实需要阻塞语义，请改用 FreeRTOS 或其它 RTOS 的 port。
 * ---------------------------------------------------------------------- */

mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max)
{
    mb_sem_t *sem;

    if (max == 0 || initial > max) {
        return NULL;
    }

    sem = (mb_sem_t *)MB_CONFIG_MALLOC(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    sem->none.count = initial;
    sem->none.max = max;
    return sem;
}

bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms)
{
    bool acquired = false;

    (void)timeout_ms; /* 裸机下无法等待，超时参数被忽略 */

    MB_CONFIG_CRITICAL_ENTER();
    if (sem->none.count > 0) {
        sem->none.count--;
        acquired = true;
    }
    MB_CONFIG_CRITICAL_EXIT();
    return acquired;
}

bool mb_os_sem_signal(mb_sem_t *sem)
{
    bool signaled = false;

    MB_CONFIG_CRITICAL_ENTER();
    if (sem->none.count < sem->none.max) {
        sem->none.count++;
        signaled = true;
    }
    MB_CONFIG_CRITICAL_EXIT();
    return signaled;
}

void mb_os_sem_destroy(mb_sem_t *sem)
{
    MB_CONFIG_FREE(sem);
}

uint32_t mb_os_time_ms(void)
{
    return (uint32_t)MB_CONFIG_TIME_MS();
}

void mb_os_sleep_ms(uint32_t ms)
{
    MB_CONFIG_SLEEP_MS(ms);
}

mb_thread_id_t mb_os_thread_id(void)
{
    /* 裸机没有线程概念，只有一个执行流，固定返回 0 即可。
     * 配合 MB_OS_CAN_BLOCK == 0，库在裸机上不做 pump 线程检查
     * （这里信号量本来就不会阻塞，谈不上「等自己」）。 */
    return (mb_thread_id_t)0;
}

void *mb_os_malloc(size_t size)
{
    return MB_CONFIG_MALLOC(size);
}

void *mb_os_calloc(size_t count, size_t size)
{
    return MB_CONFIG_CALLOC(count, size);
}

void mb_os_free(void *ptr)
{
    MB_CONFIG_FREE(ptr);
}

#endif /* MB_CONFIG_OS == MB_OS_NONE */
