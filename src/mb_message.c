/**
 * @file mb_message.c
 * @brief 内部消息对象：深拷贝、释放、生成对外视图。
 *
 * 普通投递是零拷贝的（回调期间直接引用调用方的缓冲区），
 * 只有 retained 保留消息需要长期持有，因此走这里的深拷贝路径。
 */
#include "mb_internal.h"

char *mb_strdup(const char *s)
{
    char *copy;
    size_t len;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1;
    copy = (char *)mb_os_malloc(len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

mb_owned_message_t *mb_owned_message_create(const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            const char *source,
                                            uint32_t id,
                                            uint32_t timestamp_ms,
                                            uint8_t flags)
{
    mb_owned_message_t *msg;

    if (topic == NULL) {
        return NULL;
    }

    msg = (mb_owned_message_t *)mb_os_calloc(1, sizeof(*msg));
    if (msg == NULL) {
        return NULL;
    }

    msg->topic = mb_strdup(topic);
    if (msg->topic == NULL) {
        goto fail;
    }

    if (payload_len > 0) {
        if (payload == NULL) {
            goto fail;
        }
        msg->payload = mb_os_malloc(payload_len);
        if (msg->payload == NULL) {
            goto fail;
        }
        memcpy(msg->payload, payload, payload_len);
    }
    msg->payload_len = payload_len;

    if (source != NULL) {
        msg->source = mb_strdup(source);
        if (msg->source == NULL) {
            goto fail;
        }
    }

    msg->id = id;
    msg->timestamp_ms = timestamp_ms;
    msg->flags = flags;
    return msg;

fail:
    mb_owned_message_free(msg);
    return NULL;
}

void mb_owned_message_free(mb_owned_message_t *msg)
{
    if (msg == NULL) {
        return;
    }
    mb_os_free(msg->topic);
    mb_os_free(msg->payload);
    mb_os_free(msg->source);
    mb_os_free(msg);
}

void mb_owned_message_view(const mb_owned_message_t *msg, mb_message_t *out_view)
{
    MB_CONFIG_ASSERT(msg != NULL);
    MB_CONFIG_ASSERT(out_view != NULL);

    out_view->topic = msg->topic;
    out_view->payload = msg->payload;
    out_view->payload_len = msg->payload_len;
    out_view->source = msg->source;
    out_view->id = msg->id;
    out_view->timestamp_ms = msg->timestamp_ms;
    out_view->flags = msg->flags;
}
