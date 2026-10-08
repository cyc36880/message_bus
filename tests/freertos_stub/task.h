/**
 * @file task.h
 * @brief FreeRTOS 头文件桩 —— 仅供 CI 编译 port/mb_os_freertos.c 使用。
 *
 * 这不是 FreeRTOS。详见同目录 README.md。
 *
 * 真实 FreeRTOS 把 vTaskDelay / xTaskGetTickCount / xTaskGetCurrentTaskHandle
 * 声明在 task.h，port 源码也按这个布局 #include "task.h"，所以桩必须提供该头文件。
 */
#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

void         vTaskDelay(TickType_t ticks);
TickType_t   xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);

#endif /* TASK_H */
