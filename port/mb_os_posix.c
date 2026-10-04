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
