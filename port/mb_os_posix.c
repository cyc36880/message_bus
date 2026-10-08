/**
 * @file mb_os_posix.c
 * @brief POSIX（Linux / macOS / MSYS2）的 OS 抽象层实现。
 *
 * 注意 _POSIX_C_SOURCE 必须在任何头文件之前定义，否则在 -std=c99 下
 * glibc 不会暴露 PTHREAD_MUTEX_RECURSIVE 与 clock_gettime。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "message_bus/mb_config.h"

/*
 * 只有被选中的后端才产生代码；未选中时本文件编译为空目标文件。
 * 这样 Arduino / PlatformIO 这类会递归扫描并编译库内所有 .c 的构建系统
 * 无需配置源文件过滤，也不会因为多编译了别的 port 文件而触发 #error。
 */
#if MB_CONFIG_OS == MB_OS_POSIX

#include "message_bus/mb_os.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

mb_mutex_t *mb_os_mutex_create(void)
{
    mb_mutex_t *mutex = (mb_mutex_t *)malloc(sizeof(*mutex));
    pthread_mutexattr_t attr;

    if (mutex == NULL) {
        return NULL;
    }
    if (pthread_mutexattr_init(&attr) != 0) {
        free(mutex);
        return NULL;
    }
    /* 总线的锁必须是递归锁：节点上线事件会在持锁路径中再次进入发布流程 */
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutex->posix, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(mutex);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return mutex;
}

void mb_os_mutex_lock(mb_mutex_t *mutex)
{
    (void)pthread_mutex_lock(&mutex->posix);
}

void mb_os_mutex_unlock(mb_mutex_t *mutex)
{
    (void)pthread_mutex_unlock(&mutex->posix);
}

void mb_os_mutex_destroy(mb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return;
    }
    (void)pthread_mutex_destroy(&mutex->posix);
    free(mutex);
}

/* -------------------------------------------------------------------------
 * 计数信号量：互斥量 + 条件变量
 *
 * 为什么不用 sem_t/sem_timedwait：macOS 没有 sem_timedwait，而
 * pthread_cond_timedwait 在 Linux / macOS / MSYS2 上都有。
 * 代价是**时钟必须自洽**：默认属性创建的条件变量按 CLOCK_REALTIME 计时，
 * 所以截止时刻也只能用 CLOCK_REALTIME 算，绝不能拿 mb_os_time_ms() 的
 * 单调时钟（CLOCK_MONOTONIC）去凑 —— 两者基准不同，算出来的「绝对时刻」
 * 可能已经过期，导致每次带超时的等待都立刻返回。
 * ---------------------------------------------------------------------- */

mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max)
{
    mb_sem_t *sem;

    if (max == 0 || initial > max) {
        return NULL;
    }

    sem = (mb_sem_t *)malloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&sem->posix.mutex, NULL) != 0) {
        free(sem);
        return NULL;
    }
    if (pthread_cond_init(&sem->posix.cond, NULL) != 0) {
        (void)pthread_mutex_destroy(&sem->posix.mutex);
        free(sem);
        return NULL;
    }
    sem->posix.count = initial;
    sem->posix.max = max;
    return sem;
}

/** 把「现在 + ms 毫秒」写成 CLOCK_REALTIME 的绝对截止时刻。 */
static void sem_deadline(struct timespec *ts, uint32_t ms)
{
    (void)clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms)
{
    bool acquired = false;

    (void)pthread_mutex_lock(&sem->posix.mutex);

    /* 必须写成循环：条件变量允许**虚假唤醒**，醒来不等于拿到了计数。 */
    while (sem->posix.count == 0) {
        int rc;

        if (timeout_ms == MB_WAIT_FOREVER) {
            rc = pthread_cond_wait(&sem->posix.cond, &sem->posix.mutex);
        } else {
            struct timespec deadline;

            sem_deadline(&deadline, timeout_ms);
            rc = pthread_cond_timedwait(&sem->posix.cond, &sem->posix.mutex, &deadline);
        }
        if (rc != 0) {
            break; /* ETIMEDOUT = 超时；其它错误码也不该在这里重试 */
        }
    }

    if (sem->posix.count > 0) {
        sem->posix.count--;
        acquired = true;
    }
    (void)pthread_mutex_unlock(&sem->posix.mutex);
    return acquired;
}

bool mb_os_sem_signal(mb_sem_t *sem)
{
    bool signaled = false;

    (void)pthread_mutex_lock(&sem->posix.mutex);
    if (sem->posix.count < sem->posix.max) {
        sem->posix.count++;
        signaled = true;
        (void)pthread_cond_signal(&sem->posix.cond);
    }
    (void)pthread_mutex_unlock(&sem->posix.mutex);
    return signaled;
}

void mb_os_sem_destroy(mb_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    (void)pthread_cond_destroy(&sem->posix.cond);
    (void)pthread_mutex_destroy(&sem->posix.mutex);
    free(sem);
}

uint32_t mb_os_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

void mb_os_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;

    while (nanosleep(&ts, &ts) != 0) {
        /* 被信号打断时 nanosleep 会回填剩余时间，继续睡 */
    }
}

void *mb_os_malloc(size_t size)
{
    return malloc(size);
}

void *mb_os_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void mb_os_free(void *ptr)
{
    free(ptr);
}

#endif /* MB_CONFIG_OS == MB_OS_POSIX */
