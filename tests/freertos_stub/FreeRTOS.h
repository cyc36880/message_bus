/**
 * @file FreeRTOS.h
 * @brief FreeRTOS 头文件桩 —— 仅供 CI 编译 port/mb_os_freertos.c 使用。
 *
 * 这不是 FreeRTOS。详见同目录 README.md。
 *
 * 配置宏按「启用全部可选特性」给值，让 port 里的 #error 守卫走
 * 「配置正确」那条分支；要验证守卫本身，请把对应宏改成 0 再编。
 */
#ifndef FREERTOS_H
#define FREERTOS_H

#include <stddef.h>

#include "projdefs.h"
#include "portmacro.h"

/* 总线内部锁是可重入互斥量，必需 */
#define configUSE_RECURSIVE_MUTEXES      1

/* mb_os_freertos.c 用 pvPortMalloc/vPortFree */
#define configSUPPORT_DYNAMIC_ALLOCATION 1

/* 异步投递的队列满/空靠计数信号量阻塞，开异步时必需 */
#define configUSE_COUNTING_SEMAPHORES    1

#define configTICK_RATE_HZ               1000

typedef void *TaskHandle_t;

void *pvPortMalloc(size_t size);
void  vPortFree(void *p);

#endif /* FREERTOS_H */
