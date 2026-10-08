/**
 * @file mb_bus.c
 * @brief 总线的创建、销毁、发布与查询。
 */
#include "mb_internal.h"

/* -------------------------------------------------------------------------
 * 小节：retained 保留消息表
 *
 * 表按 seq 升序排列（见 mb_dispatch.c 的分批补发算法）。
 * 「更新」语义 = 摘掉旧表项 + 追加新表项：这样在途回调手里的旧消息
 * 不会被提前释放（由引用计数兜底）。
 * ---------------------------------------------------------------------- */

static void retained_unlink_locked(mb_bus_t *bus, mb_retained_t *entry, mb_retained_t *prev)
{
    if (prev != NULL) {
        prev->next = entry->next;
    } else {
        bus->retained = entry->next;
    }
    entry->next = NULL;
    entry->active = false;
    if (bus->stats.retained_stored > 0) {
        bus->stats.retained_stored--;
    }
}

void mb_retained_release_locked(mb_bus_t *bus, mb_retained_t *entry)
{
    MB_CONFIG_ASSERT(bus != NULL);
    MB_CONFIG_ASSERT(entry->refcnt > 0);

    entry->refcnt--;
    if (entry->refcnt > 0) {
        return;
    }
    MB_CONFIG_ASSERT(!entry->active);

    mb_owned_message_free(entry->msg);
    mb_os_free(entry);
}

/** 从表中摘除并释放「表持有的那一份引用」。 */
void mb_retained_remove_locked(mb_bus_t *bus, mb_retained_t *entry, mb_retained_t *prev)
{
    retained_unlink_locked(bus, entry, prev);
    mb_retained_release_locked(bus, entry);
}

static mb_retained_t *retained_find_locked(mb_bus_t *bus, const char *topic, mb_retained_t **out_prev)
{
    mb_retained_t *entry = bus->retained;
    mb_retained_t *prev = NULL;

    while (entry != NULL) {
        if (entry->active && strcmp(entry->msg->topic, topic) == 0) {
            break;
        }
        prev = entry;
        entry = entry->next;
    }
    *out_prev = prev;
    return entry;
}

static void retained_store_locked(mb_bus_t *bus, const mb_message_t *msg)
{
    mb_retained_t *entry;
    mb_retained_t *prev = NULL;
    mb_retained_t *tail;
    mb_owned_message_t *owned;

    /* 同一主题只保留最后一条：先摘掉旧的 */
    entry = retained_find_locked(bus, msg->topic, &prev);
    if (entry != NULL) {
        mb_retained_remove_locked(bus, entry, prev);
    }

    owned = mb_owned_message_create(msg->topic, msg->payload, msg->payload_len, msg->source,
                                    msg->id, msg->timestamp_ms, msg->flags);
    entry = (mb_retained_t *)mb_os_calloc(1, sizeof(*entry));
    if (owned == NULL || entry == NULL) {
        MB_LOG_ERROR("bus", "failed to store retained message '%s' (out of memory)", msg->topic);
        mb_owned_message_free(owned);
        mb_os_free(entry);
        return;
    }

    entry->msg = owned;
    entry->seq = ++bus->next_retained_seq;
    entry->refcnt = 1;
    entry->active = true;
    entry->next = NULL;

    if (bus->retained == NULL) {
        bus->retained = entry;
    } else {
        tail = bus->retained;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = entry;
    }
    bus->stats.retained_stored++;

    MB_LOG_DEBUG("bus", "retained updated: '%s' (%u bytes)", owned->topic, (unsigned)owned->payload_len);
}

static void retained_clear_locked(mb_bus_t *bus, const char *topic)
{
    mb_retained_t *prev = NULL;
    mb_retained_t *entry = retained_find_locked(bus, topic, &prev);

    if (entry != NULL) {
        mb_retained_remove_locked(bus, entry, prev);
        MB_LOG_DEBUG("bus", "retained cleared: '%s'", topic);
    }
}

void mb_bus_retain_locked(mb_bus_t *bus, const mb_message_t *msg)
{
    MB_CONFIG_ASSERT(bus != NULL);
    MB_CONFIG_ASSERT(msg != NULL);

    /* MQTT 语义：空负载的 retained 发布用于清除该主题的保留消息，
     * 但这条消息本身仍然正常投递给在线订阅者。 */
    if (msg->payload_len == 0) {
        retained_clear_locked(bus, msg->topic);
    } else {
        retained_store_locked(bus, msg);
    }
}

/* -------------------------------------------------------------------------
 * 小节：生命周期
 * ---------------------------------------------------------------------- */

mb_err_t mb_bus_default_config(mb_bus_config_t *config)
{
    if (config == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    config->publish_node_events = true;
    return MB_OK;
}

mb_err_t mb_bus_create(const char *name, mb_bus_t **out_bus)
{
    return mb_bus_create_ex(name, NULL, out_bus);
}

mb_err_t mb_bus_create_ex(const char *name, const mb_bus_config_t *config, mb_bus_t **out_bus)
{
    mb_bus_t *bus;

    if (out_bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    *out_bus = NULL;

    if (name == NULL || name[0] == '\0') {
        return MB_ERR_INVALID_ARG;
    }
    if (strlen(name) >= MB_CONFIG_MAX_NAME_LEN) {
        return MB_ERR_TOO_LONG;
    }

    bus = (mb_bus_t *)mb_os_calloc(1, sizeof(*bus));
    if (bus == NULL) {
        return MB_ERR_NO_MEMORY;
    }

    if (config != NULL) {
        bus->config = *config;
    } else {
        (void)mb_bus_default_config(&bus->config);
    }

    bus->name = mb_strdup(name);
    bus->lock = mb_os_mutex_create();
    if (bus->name == NULL || bus->lock == NULL) {
        mb_os_mutex_destroy(bus->lock);
        mb_os_free(bus->name);
        mb_os_free(bus);
        return MB_ERR_NO_MEMORY;
    }

    MB_LOG_INFO("bus", "bus '%s' created (OS=%d, compile-time topic limit=%d bytes)",
                bus->name, (int)MB_CONFIG_OS, (int)MB_CONFIG_MAX_TOPIC_LEN);

    *out_bus = bus;
    return MB_OK;
}

void mb_bus_destroy(mb_bus_t *bus)
{
    mb_retained_t *entry;

    if (bus == NULL) {
        return;
    }

    MB_BUS_LOCK(bus);
    bus->destroying = true;

    /* 1. 销毁所有节点（连带它们的订阅）。此阶段不再发布下线事件。 */
    while (bus->node_count > 0) {
        mb_node_t *node = bus->nodes[0];

        node->active = false;
        mb_node_detach_subscriptions_locked(bus, node);
        mb_bus_remove_node_locked(bus, node);
        mb_node_release_locked(bus, node);
    }

    /* 2. 兜底：正常情况下不会有残留订阅。 */
    while (bus->sub_count > 0) {
        mb_subscription_t *sub = bus->subs[0];

        mb_subscription_detach_locked(bus, sub);
        mb_subscription_release_locked(bus, sub);
    }

    /* 3. 释放 retained 表。 */
    entry = bus->retained;
    bus->retained = NULL;
    while (entry != NULL) {
        mb_retained_t *next = entry->next;

        mb_owned_message_free(entry->msg);
        mb_os_free(entry);
        entry = next;
    }
    bus->stats.retained_stored = 0;

    /* 4. 释放异步队列（连同队列里还没被 pump 取走的消息）。 */
    mb_async_pool_dispose(bus);

    /* 5. 释放容器。 */
    mb_os_free(bus->nodes);
    mb_os_free(bus->subs);
    bus->nodes = NULL;
    bus->subs = NULL;
    MB_BUS_UNLOCK(bus);

    MB_LOG_INFO("bus", "bus '%s' destroyed", bus->name);

    mb_os_mutex_destroy(bus->lock);
    mb_os_free(bus->name);
    mb_os_free(bus);
}

/* -------------------------------------------------------------------------
 * 小节：查询
 * ---------------------------------------------------------------------- */

const char *mb_bus_name(const mb_bus_t *bus)
{
    return (bus != NULL) ? bus->name : NULL;
}

mb_err_t mb_bus_get_stats(const mb_bus_t *bus, mb_bus_stats_t *out_stats)
{
    if (bus == NULL || out_stats == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    MB_BUS_LOCK(bus);
    *out_stats = bus->stats;
    MB_BUS_UNLOCK(bus);
    return MB_OK;
}

void mb_bus_reset_stats(mb_bus_t *bus)
{
    uint64_t retained;

    if (bus == NULL) {
        return;
    }

    MB_BUS_LOCK(bus);
    retained = bus->stats.retained_stored;
    memset(&bus->stats, 0, sizeof(bus->stats));
    bus->stats.retained_stored = retained; /* 表内容没有变，保留当前值 */
    MB_BUS_UNLOCK(bus);
}

size_t mb_bus_node_count(const mb_bus_t *bus)
{
    size_t count;

    if (bus == NULL) {
        return 0;
    }

    MB_BUS_LOCK(bus);
    count = bus->node_count;
    MB_BUS_UNLOCK(bus);
    return count;
}

size_t mb_bus_subscription_count(const mb_bus_t *bus)
{
    size_t count;

    if (bus == NULL) {
        return 0;
    }

    MB_BUS_LOCK(bus);
    count = bus->sub_count;
    MB_BUS_UNLOCK(bus);
    return count;
}

mb_err_t mb_bus_find_node(const mb_bus_t *bus, const char *name, mb_node_t **out_node)
{
    size_t i;
    mb_err_t err = MB_ERR_NOT_FOUND;

    if (bus == NULL || name == NULL || out_node == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    *out_node = NULL;

    MB_BUS_LOCK(bus);
    for (i = 0; i < bus->node_count; ++i) {
        if (strcmp(bus->nodes[i]->name, name) == 0) {
            *out_node = bus->nodes[i];
            err = MB_OK;
            break;
        }
    }
    MB_BUS_UNLOCK(bus);
    return err;
}

/* -------------------------------------------------------------------------
 * 小节：发布
 * ---------------------------------------------------------------------- */

mb_err_t mb_bus_publish(mb_bus_t *bus,
                        const char *topic,
                        const void *payload,
                        size_t payload_len,
                        const mb_publish_opts_t *opts)
{
    if (bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    return mb_bus_publish_internal(bus, NULL, topic, payload, payload_len, opts);
}

mb_err_t mb_publish_prepare(const char *topic,
                            const void *payload,
                            size_t payload_len,
                            const mb_publish_opts_t *opts,
                            mb_publish_opts_t *out_opts,
                            uint8_t *out_flags)
{
    mb_err_t err;

    MB_CONFIG_ASSERT(out_opts != NULL);
    MB_CONFIG_ASSERT(out_flags != NULL);

    out_opts->flags = 0;
    out_opts->qos = 0;
    if (opts != NULL) {
        *out_opts = *opts;
    }
    if (out_opts->qos != 0) {
        /* 进程内总线不引入 QoS 1/2 的重传与确认机制，见 docs/architecture.md */
        return MB_ERR_UNSUPPORTED;
    }

    err = mb_topic_validate_topic(topic);
    if (err != MB_OK) {
        MB_LOG_WARN("bus", "invalid publish topic: '%s'", (topic != NULL) ? topic : "(null)");
        return err;
    }
    if (payload_len > 0 && payload == NULL) {
        return MB_ERR_INVALID_ARG;
    }
#if MB_CONFIG_MAX_PAYLOAD_SIZE > 0
    if (payload_len > MB_CONFIG_MAX_PAYLOAD_SIZE) {
        return MB_ERR_TOO_LONG;
    }
#endif

    /* 注意：发布选项**不**进入对外可见的 flags。
     * MB_PUB_FLAG_RETAIN 与 MB_MSG_FLAG_RETAINED 恰好都是 1u<<0，
     * 把 opts->flags 原样塞进去会让「实时收到的消息」被误判成 retained 补发。 */
    *out_flags = mb_topic_is_system(topic) ? MB_MSG_FLAG_SYSTEM : 0;
    return MB_OK;
}

mb_err_t mb_bus_publish_internal(mb_bus_t *bus,
                                 const char *source,
                                 const char *topic,
                                 const void *payload,
                                 size_t payload_len,
                                 const mb_publish_opts_t *opts)
{
    mb_message_t msg;
    mb_publish_opts_t options;
    size_t delivered = 0;
    mb_err_t err;

    if (bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    err = mb_publish_prepare(topic, payload, payload_len, opts, &options, &msg.flags);
    if (err != MB_OK) {
        return err;
    }

    msg.topic = topic;
    msg.payload = (payload_len > 0) ? payload : NULL;
    msg.payload_len = payload_len;
    msg.source = source;

    MB_BUS_LOCK(bus);
    if (bus->destroying) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_STATE;
    }
    msg.id = ++bus->next_message_id;
    msg.timestamp_ms = mb_os_time_ms();
    bus->stats.published++;
    MB_BUS_UNLOCK(bus);

    if ((options.flags & MB_PUB_FLAG_RETAIN) != 0) {
        MB_BUS_LOCK(bus);
        mb_bus_retain_locked(bus, &msg);
        MB_BUS_UNLOCK(bus);
    }

    err = mb_dispatch_message(bus, &msg, &delivered);
    if (err != MB_OK) {
        return err;
    }

    if (delivered == 0) {
        MB_BUS_LOCK(bus);
        bus->stats.no_subscriber++;
        MB_BUS_UNLOCK(bus);
        MB_LOG_DEBUG("bus", "no matching subscriber for topic '%s'", topic);
    }
    return MB_OK;
}

/* -------------------------------------------------------------------------
 * 小节：系统主题
 * ---------------------------------------------------------------------- */

/** 拼出 `$mb/nodes/<name>/<suffix>`，返回是否成功。 */
static bool build_node_topic(char *out, size_t out_size, const char *node_name, const char *suffix)
{
    static const char prefix[] = "$mb/nodes/";
    size_t name_len = strlen(node_name);
    size_t suffix_len = strlen(suffix);
    size_t offset = 0;

    if (sizeof(prefix) - 1 + name_len + suffix_len + 1 > out_size) {
        return false;
    }

    memcpy(out + offset, prefix, sizeof(prefix) - 1);
    offset += sizeof(prefix) - 1;
    memcpy(out + offset, node_name, name_len);
    offset += name_len;
    memcpy(out + offset, suffix, suffix_len);
    offset += suffix_len;
    out[offset] = '\0';
    return true;
}

void mb_bus_publish_node_event(mb_bus_t *bus, const char *node_name, bool connected)
{
    char topic[MB_CONFIG_MAX_NAME_LEN + 32];
    mb_publish_opts_t opts;
    size_t name_len;

    MB_CONFIG_ASSERT(bus != NULL);
    MB_CONFIG_ASSERT(node_name != NULL);

    name_len = strlen(node_name);
    opts.qos = 0;

    /* ── 上线 ──
     * $mb/nodes/<name>/connected 是**状态**主题：retained，负载为节点名。
     * 后启动的订阅者能立刻知道谁在线（LVGL 场景的关键）。 */
    if (connected) {
        if (!build_node_topic(topic, sizeof(topic), node_name, "/connected")) {
            MB_CONFIG_ASSERT(false);
            return;
        }
        opts.flags = MB_PUB_FLAG_RETAIN;
        (void)mb_bus_publish_internal(bus, NULL, topic, node_name, name_len, &opts);
        return;
    }

    /* ── 下线 ──
     * 1) 向同一个 connected 主题发一条空负载的 retained 消息：
     *    按 MQTT 规则清除保留状态，同时实时通知在线的订阅者；
     * 2) 再向 disconnected 主题发一条**不保留**的纯事件消息。 */
    if (build_node_topic(topic, sizeof(topic), node_name, "/connected")) {
        opts.flags = MB_PUB_FLAG_RETAIN;
        (void)mb_bus_publish_internal(bus, NULL, topic, NULL, 0, &opts);
    } else {
        MB_CONFIG_ASSERT(false);
    }

    if (build_node_topic(topic, sizeof(topic), node_name, "/disconnected")) {
        opts.flags = 0; /* 事件消息，不保留：避免留下永远不会被清除的状态 */
        (void)mb_bus_publish_internal(bus, NULL, topic, node_name, name_len, &opts);
    } else {
        MB_CONFIG_ASSERT(false);
    }
}
