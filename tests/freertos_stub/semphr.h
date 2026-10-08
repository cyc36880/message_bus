/**
 * @file semphr.h
 * @brief FreeRTOS 头文件桩 —— 仅供 CI 编译 port/mb_os_freertos.c 使用。
 *
 * 这不是 FreeRTOS。详见同目录 README.md。
 *
 * 这里声明的函数被 port 源码分两组使用，**两组都要有**：
 *
 *   1. 递归互斥量  —— 总线内部锁，同步 API 就依赖它；
 *   2. 计数信号量  —— 异步投递的队列空位 / 空闲条目名额。
 *
 * 第 2 组是后加的，当初漏在这个桩里，CI 就挂了。改 port 时请同步本文件。
 */
#ifndef SEMPHR_H
#define SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

/* ---- 递归互斥量（总线内部锁） ---- */
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t        xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t timeout);
BaseType_t        xSemaphoreGiveRecursive(SemaphoreHandle_t s);

/* ---- 计数信号量（异步投递，MB_CONFIG_ASYNC_MAX_TOPICS > 0 时才用到） ---- */
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);

/* ---- 两者共用 ---- */
void vSemaphoreDelete(SemaphoreHandle_t s);

#endif /* SEMPHR_H */
