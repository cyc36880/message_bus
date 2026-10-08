/**
 * @file portmacro.h
 * @brief FreeRTOS 头文件桩 —— 仅供 CI 编译 port/mb_os_freertos.c 使用。
 *
 * 这不是 FreeRTOS。详见同目录 README.md。
 *
 * 真实 FreeRTOS 把下面这三个基础类型定义在各移植版的 portmacro.h 里。
 * 它们必须在这儿，因为 port 源码（以及 semphr.h 的计数信号量声明）用到了
 * UBaseType_t —— 这正是当初桩不全导致 CI 编译失败的那个类型。
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

typedef uint32_t      TickType_t;
typedef int           BaseType_t;
typedef unsigned long UBaseType_t;

/** 1 tick = 1ms，即 configTICK_RATE_HZ = 1000。 */
#define portTICK_PERIOD_MS 1u

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif /* PORTMACRO_H */
