/**
 * @file mb_log.h
 * @brief 分级日志。
 *
 * 两级过滤：
 *   - 编译期：MB_CONFIG_LOG_LEVEL，低于该等级的宏展开为空，不产生任何代码；
 *   - 运行期：mb_log_set_level()，可在现场调整，默认等于编译期等级。
 *
 * 默认实现把日志写到 stderr；嵌入式工程应调用 mb_log_set_handler()
 * 换成串口 / RTT 输出，或把等级设为 MB_LOG_LEVEL_NONE 完全关掉。
 */
#ifndef MESSAGE_BUS_MB_LOG_H
#define MESSAGE_BUS_MB_LOG_H

#include "mb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mb_log_level {
    MB_LOG_LEVEL_NONE = 0,
    MB_LOG_LEVEL_ERROR = 1,
    MB_LOG_LEVEL_WARN = 2,
    MB_LOG_LEVEL_INFO = 3,
    MB_LOG_LEVEL_DEBUG = 4,
} mb_log_level_t;

/**
 * 自定义日志输出回调。
 * @param level    本条日志的等级
 * @param tag      模块标签，如 "bus"、"node"
 * @param message  已格式化好的完整文本（不带换行）
 * @param user_data mb_log_set_handler 传入的指针
 */
typedef void (*mb_log_handler_t)(mb_log_level_t level, const char *tag, const char *message, void *user_data);

/** 替换日志输出函数；传 NULL 恢复默认（stderr）。 */
void mb_log_set_handler(mb_log_handler_t handler, void *user_data);

/** 设置运行期等级；会截断到编译期上限 MB_CONFIG_LOG_LEVEL。 */
void mb_log_set_level(mb_log_level_t level);

/** @return 当前运行期等级。 */
mb_log_level_t mb_log_get_level(void);

/** 直接输出一条日志（一般不用直接调用，用下面的宏即可）。 */
void mb_log_emit(mb_log_level_t level, const char *tag, const char *fmt, ...);

/* -------------------------------------------------------------------------
 * 宏：低于编译期等级的调用不产生任何代码
 * ---------------------------------------------------------------------- */
#if MB_CONFIG_LOG_LEVEL >= 1
#define MB_LOG_ERROR(tag, ...) mb_log_emit(MB_LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#else
#define MB_LOG_ERROR(tag, ...) ((void)0)
#endif

#if MB_CONFIG_LOG_LEVEL >= 2
#define MB_LOG_WARN(tag, ...) mb_log_emit(MB_LOG_LEVEL_WARN, tag, __VA_ARGS__)
#else
#define MB_LOG_WARN(tag, ...) ((void)0)
#endif

#if MB_CONFIG_LOG_LEVEL >= 3
#define MB_LOG_INFO(tag, ...) mb_log_emit(MB_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#else
#define MB_LOG_INFO(tag, ...) ((void)0)
#endif

#if MB_CONFIG_LOG_LEVEL >= 4
#define MB_LOG_DEBUG(tag, ...) mb_log_emit(MB_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#else
#define MB_LOG_DEBUG(tag, ...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_LOG_H */
