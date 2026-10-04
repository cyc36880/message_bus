/**
 * @file message_bus.h
 * @brief message_bus 总入口：引入本文件即可使用全部公开 API。
 *
 * message_bus 是一套用于 LVGL 与硬件之间解耦的进程内消息总线：
 * 所有通讯实体以「节点」形式挂在总线上，通过 MQTT 风格的主题字符串
 * 发布 / 订阅消息。
 *
 * 同一份业务代码可以不加修改地跑在 PC 模拟器（Windows/POSIX）与
 * MCU（FreeRTOS / 裸机）上，差异只在于编译期选择的 OS 抽象层。
 *
 * 快速开始见 examples/01_basic_pubsub.c，设计说明见 docs/architecture.md。
 */
#ifndef MESSAGE_BUS_H
#define MESSAGE_BUS_H

#include "mb_bus.h"
#include "mb_config.h"
#include "mb_log.h"
#include "mb_node.h"
#include "mb_os.h"
#include "mb_topic.h"
#include "mb_types.h"
#include "mb_version.h"

#endif /* MESSAGE_BUS_H */
