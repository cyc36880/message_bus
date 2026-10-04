/**
 * @file mb_topic.h
 * @brief 主题（topic）与通配符过滤（MQTT 3.1.1 规则）。
 *
 * 本模块不依赖总线的其它部分，可以单独拿来当字符串工具用。
 *
 * ── 规则速查 ────────────────────────────────────────────────────────────
 *   主题用 '/' 分层：            sensor/room1/temperature
 *   '+' 匹配**恰好一层**：       sensor/+/temperature  → sensor/room1/temperature
 *   '#' 匹配**剩余所有层**：     sensor/#              → sensor、sensor/room1、sensor/room1/x
 *   '#' 必须独占最后一层：       "sensor/#" 合法；"sensor/a#"、"sensor/#/x" 非法
 *   '+' 必须独占一层：           "sensor/+" 合法；"sensor/r+" 非法
 *   以 '$' 开头的主题（系统主题）不被首层的通配符匹配：
 *                                "#" 不匹配 "$mb/nodes/x/connected"
 *                                "$mb/#" 可以匹配
 * ───────────────────────────────────────────────────────────────────────
 */
#ifndef MESSAGE_BUS_MB_TOPIC_H
#define MESSAGE_BUS_MB_TOPIC_H

#include <stdbool.h>
#include <stddef.h>

#include "mb_config.h"
#include "mb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_TOPIC_SEPARATOR '/'       /**< 层级分隔符 */
#define MB_TOPIC_WILDCARD_SINGLE '+' /**< 单层通配符 */
#define MB_TOPIC_WILDCARD_MULTI '#'  /**< 多层通配符 */
#define MB_TOPIC_SYSTEM_PREFIX '$'   /**< 系统主题前缀 */

/**
 * 判断一条**过滤器**是否与一条**主题**匹配。
 *
 * @param filter 订阅时使用的过滤器，可含 '+' / '#'。
 * @param topic  发布时使用的具体主题，不得含通配符。
 * @return true 表示匹配。任一参数为 NULL 或空串时返回 false。
 */
bool mb_topic_match(const char *filter, const char *topic);

/**
 * 校验一条过滤器是否合法。
 * @return MB_OK；或 MB_ERR_INVALID_ARG（空串、通配符位置错误）、
 *         MB_ERR_TOO_LONG（超过 MB_CONFIG_MAX_TOPIC_LEN）。
 */
mb_err_t mb_topic_validate_filter(const char *filter);

/**
 * 校验一条发布主题是否合法（不允许出现通配符）。
 * @return MB_OK；或 MB_ERR_INVALID_ARG、MB_ERR_TOO_LONG。
 */
mb_err_t mb_topic_validate_topic(const char *topic);

/** 是否为系统主题（以 '$' 开头，通配符匹配规则特殊）。 */
bool mb_topic_is_system(const char *topic);

/** @return 层级数；"a/b/c" → 3，"" → 0。 */
size_t mb_topic_level_count(const char *topic);

/**
 * 拼接 `prefix + "/" + suffix` 到 buf。
 * 用于按约定构造「发往某节点」的主题，如 prefix="motor"、suffix="cmd/speed"。
 *
 * @param buf      输出缓冲
 * @param buf_size 缓冲大小
 * @param prefix   前缀，非空
 * @param suffix   后缀；为 NULL 或空串时结果就是 prefix
 * @return MB_OK 或 MB_ERR_TOO_LONG（缓冲不足，buf 内容未定义）。
 */
mb_err_t mb_topic_build(char *buf, size_t buf_size, const char *prefix, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_TOPIC_H */
