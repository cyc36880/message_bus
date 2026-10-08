/**
 * @file mb_log.c
 * @brief 分级日志实现，以及错误码到短名的映射。
 *
 * 唯一使用 stdio 的地方（vsnprintf 需要）。嵌入式工程可以：
 *   - 把 MB_CONFIG_LOG_LEVEL 设为 0，本文件几乎不产生代码；
 *   - 或调用 mb_log_set_handler() 把输出接管到串口 / RTT。
 */
#include "message_bus/mb_log.h"
#include "message_bus/mb_types.h"

#include <stdarg.h>
#include <stdio.h>

/** 格式化缓冲：够用即可，超长会被截断。 */
#define MB_LOG_BUFFER_SIZE 160

#if MB_CONFIG_LOG_LEVEL > 0

static void mb_log_default_handler(mb_log_level_t level, const char *tag, const char *message, void *user_data);

static mb_log_handler_t s_handler = mb_log_default_handler;
static void *s_handler_user_data = NULL;
static mb_log_level_t s_level = (mb_log_level_t)MB_CONFIG_LOG_LEVEL;

static const char *level_name(mb_log_level_t level)
{
    switch (level) {
    case MB_LOG_LEVEL_ERROR: return "E";
    case MB_LOG_LEVEL_WARN:  return "W";
    case MB_LOG_LEVEL_INFO:  return "I";
    case MB_LOG_LEVEL_DEBUG: return "D";
    default:                 return "?";
    }
}

static void mb_log_default_handler(mb_log_level_t level, const char *tag, const char *message, void *user_data)
{
    (void)user_data;
    fprintf(stderr, "[mb %s/%s] %s\n", level_name(level), (tag != NULL) ? tag : "-", message);
}

#endif /* MB_CONFIG_LOG_LEVEL > 0 */

void mb_log_set_handler(mb_log_handler_t handler, void *user_data)
{
#if MB_CONFIG_LOG_LEVEL > 0
    s_handler = (handler != NULL) ? handler : mb_log_default_handler;
    s_handler_user_data = user_data;
#else
    (void)handler;
    (void)user_data;
#endif
}

void mb_log_set_level(mb_log_level_t level)
{
#if MB_CONFIG_LOG_LEVEL > 0
    if (level > (mb_log_level_t)MB_CONFIG_LOG_LEVEL) {
        level = (mb_log_level_t)MB_CONFIG_LOG_LEVEL; /* 不允许突破编译期上限 */
    }
    s_level = level;
#else
    (void)level;
#endif
}

mb_log_level_t mb_log_get_level(void)
{
#if MB_CONFIG_LOG_LEVEL > 0
    return s_level;
#else
    return MB_LOG_LEVEL_NONE;
#endif
}

const char *mb_err_to_string(mb_err_t err)
{
    switch (err) {
    case MB_OK:                 return "MB_OK";
    case MB_ERR_INVALID_ARG:    return "MB_ERR_INVALID_ARG";
    case MB_ERR_NO_MEMORY:      return "MB_ERR_NO_MEMORY";
    case MB_ERR_NOT_FOUND:      return "MB_ERR_NOT_FOUND";
    case MB_ERR_ALREADY_EXISTS: return "MB_ERR_ALREADY_EXISTS";
    case MB_ERR_FULL:           return "MB_ERR_FULL";
    case MB_ERR_BUSY:           return "MB_ERR_BUSY";
    case MB_ERR_STATE:          return "MB_ERR_STATE";
    case MB_ERR_TOO_LONG:       return "MB_ERR_TOO_LONG";
    case MB_ERR_UNSUPPORTED:    return "MB_ERR_UNSUPPORTED";
    case MB_ERR_TIMEOUT:        return "MB_ERR_TIMEOUT";
    default:                    return "MB_ERR_UNKNOWN";
    }
}

void mb_log_emit(mb_log_level_t level, const char *tag, const char *fmt, ...)
{
#if MB_CONFIG_LOG_LEVEL > 0
    char buffer[MB_LOG_BUFFER_SIZE];
    va_list args;
    int written;

    if (level == MB_LOG_LEVEL_NONE || level > s_level || fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    s_handler(level, tag, buffer, s_handler_user_data);
#else
    (void)level;
    (void)tag;
    (void)fmt;
#endif
}
