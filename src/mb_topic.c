/**
 * @file mb_topic.c
 * @brief 主题校验与 MQTT 通配符匹配。
 *
 * 本文件刻意不依赖总线的其它部分，方便单独复用与测试。
 * 匹配规则严格遵循 MQTT 3.1.1 [MQTT-4.7]：
 *   - '/' 分隔层级，空层级（"a//b"、"/a"）合法；
 *   - '+' 匹配恰好一层，且必须独占一层；
 *   - '#' 匹配零层或多层，必须独占最后一层；
 *   - "sport/#" 也匹配 "sport"（'#' 包含父层级）；
 *   - 首字符为 '$' 的主题不被首层通配符（'+' 或 '#'）匹配。
 */
#include "message_bus/mb_topic.h"

#include <string.h>

bool mb_topic_is_system(const char *topic)
{
    return topic != NULL && topic[0] == MB_TOPIC_SYSTEM_PREFIX;
}

size_t mb_topic_level_count(const char *topic)
{
    size_t levels = 1;

    if (topic == NULL || topic[0] == '\0') {
        return 0;
    }
    for (const char *p = topic; *p != '\0'; ++p) {
        if (*p == MB_TOPIC_SEPARATOR) {
            ++levels;
        }
    }
    return levels;
}

mb_err_t mb_topic_validate_topic(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return MB_ERR_INVALID_ARG;
    }
    if (strlen(topic) >= MB_CONFIG_MAX_TOPIC_LEN) {
        return MB_ERR_TOO_LONG;
    }
    if (strchr(topic, MB_TOPIC_WILDCARD_SINGLE) != NULL ||
        strchr(topic, MB_TOPIC_WILDCARD_MULTI) != NULL) {
        /* 发布主题里不允许出现通配符 */
        return MB_ERR_INVALID_ARG;
    }
    return MB_OK;
}

mb_err_t mb_topic_validate_filter(const char *filter)
{
    const char *p;

    if (filter == NULL || filter[0] == '\0') {
        return MB_ERR_INVALID_ARG;
    }
    if (strlen(filter) >= MB_CONFIG_MAX_TOPIC_LEN) {
        return MB_ERR_TOO_LONG;
    }

    p = filter;
    while (*p != '\0') {
        const char *level = p;
        size_t level_len;
        const char *hash;

        while (*p != '\0' && *p != MB_TOPIC_SEPARATOR) {
            ++p;
        }
        level_len = (size_t)(p - level);

        hash = memchr(level, MB_TOPIC_WILDCARD_MULTI, level_len);
        if (hash != NULL) {
            /* '#' 必须独占一层，且必须是最后一层 */
            if (level_len != 1 || *p != '\0') {
                return MB_ERR_INVALID_ARG;
            }
        }

        if (memchr(level, MB_TOPIC_WILDCARD_SINGLE, level_len) != NULL && level_len != 1) {
            /* '+' 必须独占一层 */
            return MB_ERR_INVALID_ARG;
        }

        if (*p == MB_TOPIC_SEPARATOR) {
            ++p;
        }
    }
    return MB_OK;
}

bool mb_topic_match(const char *filter, const char *topic)
{
    const char *f;
    const char *t;

    if (filter == NULL || topic == NULL || filter[0] == '\0' || topic[0] == '\0') {
        return false;
    }

    /* 系统主题不被首层通配符匹配：[MQTT-4.7.2-1] */
    if (topic[0] == MB_TOPIC_SYSTEM_PREFIX &&
        (filter[0] == MB_TOPIC_WILDCARD_SINGLE || filter[0] == MB_TOPIC_WILDCARD_MULTI)) {
        return false;
    }

    f = filter;
    t = topic;
    for (;;) {
        const char *f_end;
        const char *t_end;
        size_t f_len;
        size_t t_len;

        if (*f == MB_TOPIC_WILDCARD_MULTI) {
            /* filter 已通过校验，'#' 必然是最后一层，直接匹配剩余全部 */
            return true;
        }

        f_end = strchr(f, MB_TOPIC_SEPARATOR);
        t_end = strchr(t, MB_TOPIC_SEPARATOR);
        f_len = (f_end != NULL) ? (size_t)(f_end - f) : strlen(f);
        t_len = (t_end != NULL) ? (size_t)(t_end - t) : strlen(t);

        if (!(f_len == 1 && f[0] == MB_TOPIC_WILDCARD_SINGLE)) {
            if (f_len != t_len || memcmp(f, t, f_len) != 0) {
                return false;
            }
        }

        if (f_end == NULL && t_end == NULL) {
            return true; /* 两边同时耗尽 */
        }

        if (t_end == NULL) {
            /* 主题已耗尽，仅当过滤器剩余部分恰好是 "/#" 时匹配（"sport/#" vs "sport"） */
            return f_end != NULL &&
                   strcmp(f_end + 1, "#") == 0;
        }

        if (f_end == NULL) {
            return false; /* 过滤器已耗尽而主题还有层级 */
        }

        f = f_end + 1;
        t = t_end + 1;
    }
}

mb_err_t mb_topic_build(char *buf, size_t buf_size, const char *prefix, const char *suffix)
{
    size_t prefix_len;
    size_t suffix_len;
    size_t total;

    if (buf == NULL || buf_size == 0 || prefix == NULL || prefix[0] == '\0') {
        return MB_ERR_INVALID_ARG;
    }

    prefix_len = strlen(prefix);
    suffix_len = (suffix != NULL) ? strlen(suffix) : 0;

    if (suffix_len == 0) {
        total = prefix_len;
    } else {
        total = prefix_len + 1 + suffix_len;
    }

    if (total >= buf_size || total >= MB_CONFIG_MAX_TOPIC_LEN) {
        return MB_ERR_TOO_LONG;
    }

    memcpy(buf, prefix, prefix_len);
    if (suffix_len > 0) {
        buf[prefix_len] = MB_TOPIC_SEPARATOR;
        memcpy(buf + prefix_len + 1, suffix, suffix_len);
    }
    buf[total] = '\0';
    return MB_OK;
}
