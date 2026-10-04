/**
 * @file mb_node.c
 * @brief 节点生命周期、发布、订阅。
 */
#include "mb_internal.h"

/* -------------------------------------------------------------------------
 * 小节：内部工具
 * ---------------------------------------------------------------------- */

/** 把无符号整数写成十进制字符串，返回写入的字符数（不含 '\0'）。 */
static size_t u32_to_dec(char *buf, size_t size, uint32_t value)
{
    char reversed[10];
    size_t digits = 0;
    size_t i;

    do {
        reversed[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    if (digits + 1 > size) {
        return 0;
    }
    for (i = 0; i < digits; ++i) {
        buf[i] = reversed[digits - 1 - i];
    }
    buf[digits] = '\0';
    return digits;
}

/** 生成自动节点名 "node-<n>"。 */
static void make_auto_name(char *buf, size_t size, uint32_t seq)
{
    static const char prefix[] = "node-";
    const size_t prefix_len = sizeof(prefix) - 1;

    if (prefix_len >= size) {
        buf[0] = '\0';
        return;
    }
    memcpy(buf, prefix, prefix_len);
    (void)u32_to_dec(buf + prefix_len, size - prefix_len, seq);
}

/**
 * 节点名必须能安全地当作主题的一层使用，因此禁止出现：
 *   '/'（会破坏层级结构）、'+' 与 '#'（会被当成通配符）、首字符 '$'（系统主题前缀）。
 */
static mb_err_t validate_node_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return MB_ERR_INVALID_ARG;
    }
    if (strlen(name) >= MB_CONFIG_MAX_NAME_LEN) {
        return MB_ERR_TOO_LONG;
    }
    if (strchr(name, MB_TOPIC_SEPARATOR) != NULL ||
        strchr(name, MB_TOPIC_WILDCARD_SINGLE) != NULL ||
        strchr(name, MB_TOPIC_WILDCARD_MULTI) != NULL ||
        name[0] == MB_TOPIC_SYSTEM_PREFIX) {
        return MB_ERR_INVALID_ARG;
    }
    return MB_OK;
}

/** 名字长度已由 validate_node_name 保证，这里只做一次带长度限制的拷贝。 */
static void copy_name(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);

    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static mb_node_t *find_node_locked(const mb_bus_t *bus, const char *name)
{
    size_t i;

    for (i = 0; i < bus->node_count; ++i) {
        if (strcmp(bus->nodes[i]->name, name) == 0) {
            return bus->nodes[i];
        }
    }
    return NULL;
}

static bool ensure_node_capacity_locked(mb_bus_t *bus, size_t needed)
{
    mb_node_t **grown;
    size_t capacity;

    if (bus->node_capacity >= needed) {
        return true;
    }

    capacity = (bus->node_capacity > 0) ? bus->node_capacity : 4;
    while (capacity < needed) {
        capacity *= 2;
    }

    /* 不用 realloc：很多 RTOS 的堆实现没有它 */
    grown = (mb_node_t **)mb_os_malloc(capacity * sizeof(*grown));
    if (grown == NULL) {
        MB_LOG_ERROR("node", "failed to grow node table: %u entries requested", (unsigned)capacity);
        return false;
    }
    if (bus->node_count > 0) {
        memcpy(grown, bus->nodes, bus->node_count * sizeof(*grown));
    }
    mb_os_free(bus->nodes);
    bus->nodes = grown;
    bus->node_capacity = capacity;
    return true;
}

/* -------------------------------------------------------------------------
 * 小节：节点表维护（锁内）
 * ---------------------------------------------------------------------- */

bool mb_bus_add_node_locked(mb_bus_t *bus, mb_node_t *node)
{
    if (!ensure_node_capacity_locked(bus, bus->node_count + 1)) {
        return false;
    }
    bus->nodes[bus->node_count++] = node;
    bus->stats.nodes_created++;
    return true;
}

void mb_bus_remove_node_locked(mb_bus_t *bus, mb_node_t *node)
{
    size_t index;

    for (index = 0; index < bus->node_count; ++index) {
        if (bus->nodes[index] == node) {
            break;
        }
    }
    if (index == bus->node_count) {
        MB_CONFIG_ASSERT(false); /* 节点不在表中，属于内部错误 */
        return;
    }

    if (index + 1 < bus->node_count) {
        memmove(&bus->nodes[index], &bus->nodes[index + 1],
                (bus->node_count - index - 1) * sizeof(bus->nodes[0]));
    }
    bus->node_count--;
}

void mb_node_release_locked(mb_bus_t *bus, mb_node_t *node)
{
    MB_CONFIG_ASSERT(bus == node->bus);
    MB_CONFIG_ASSERT(node->refcnt > 0);

    node->refcnt--;
    if (node->refcnt > 0) {
        return;
    }

    MB_CONFIG_ASSERT(!node->active);
    mb_os_free(node->name);
    mb_os_free(node);
}

void mb_node_detach_subscriptions_locked(mb_bus_t *bus, mb_node_t *node)
{
    size_t i = 0;

    while (i < bus->sub_count) {
        mb_subscription_t *sub = bus->subs[i];

        if (sub->node != node) {
            ++i;
            continue;
        }
        mb_subscription_detach_locked(bus, sub);
        mb_subscription_release_locked(bus, sub);
        /* detach 会前移元素，因此 i 不自增 */
    }
}

/* -------------------------------------------------------------------------
 * 小节：生命周期
 * ---------------------------------------------------------------------- */

mb_err_t mb_node_create(mb_bus_t *bus, const char *name, mb_node_t **out_node)
{
    char auto_name[MB_CONFIG_MAX_NAME_LEN];
    mb_node_t *node;
    mb_err_t err;

    if (bus == NULL || out_node == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    *out_node = NULL;

    MB_BUS_LOCK(bus);

    if (bus->destroying) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_STATE;
    }

    if (name == NULL) {
        make_auto_name(auto_name, sizeof(auto_name), ++bus->node_seq);
        name = auto_name;
    }

    err = validate_node_name(name);
    if (err != MB_OK) {
        MB_BUS_UNLOCK(bus);
        return err;
    }
    if (find_node_locked(bus, name) != NULL) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_ALREADY_EXISTS;
    }

    node = (mb_node_t *)mb_os_calloc(1, sizeof(*node));
    if (node == NULL) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_NO_MEMORY;
    }
    node->name = mb_strdup(name);
    if (node->name == NULL) {
        mb_os_free(node);
        MB_BUS_UNLOCK(bus);
        return MB_ERR_NO_MEMORY;
    }
    node->bus = bus;
    node->refcnt = 1; /* 节点表持有的一份引用 */
    node->active = true;

    if (!mb_bus_add_node_locked(bus, node)) {
        mb_os_free(node->name);
        mb_os_free(node);
        MB_BUS_UNLOCK(bus);
        return MB_ERR_NO_MEMORY;
    }

    MB_BUS_UNLOCK(bus);

    MB_LOG_DEBUG("node", "node '%s' attached to bus '%s'", node->name, bus->name);

    if (bus->config.publish_node_events) {
        mb_bus_publish_node_event(bus, node->name, true);
    }

    *out_node = node;
    return MB_OK;
}

void mb_node_destroy(mb_node_t *node)
{
    mb_bus_t *bus;
    char name[MB_CONFIG_MAX_NAME_LEN];
    bool notify = false;

    if (node == NULL) {
        return;
    }
    bus = node->bus;

    MB_BUS_LOCK(bus);
    if (node->active) {
        copy_name(name, sizeof(name), node->name);
        notify = bus->config.publish_node_events && !bus->destroying;

        node->active = false;
        mb_node_detach_subscriptions_locked(bus, node);
        mb_bus_remove_node_locked(bus, node);
        mb_node_release_locked(bus, node); /* 释放节点表持有的引用 */
    } else {
        notify = false;
    }
    MB_BUS_UNLOCK(bus);

    if (notify) {
        MB_LOG_DEBUG("node", "node '%s' detached from bus '%s'", name, bus->name);
        /* 空负载 + retained，用于清除上线时留下的 retained 消息 */
        mb_bus_publish_node_event(bus, name, false);
    }
}

const char *mb_node_name(const mb_node_t *node)
{
    return (node != NULL) ? node->name : NULL;
}

mb_bus_t *mb_node_bus(const mb_node_t *node)
{
    return (node != NULL) ? node->bus : NULL;
}

size_t mb_node_subscription_count(const mb_node_t *node)
{
    mb_bus_t *bus;
    size_t count = 0;
    size_t i;

    if (node == NULL) {
        return 0;
    }
    bus = node->bus;

    MB_BUS_LOCK(bus);
    for (i = 0; i < bus->sub_count; ++i) {
        if (bus->subs[i]->node == node) {
            ++count;
        }
    }
    MB_BUS_UNLOCK(bus);
    return count;
}

/* -------------------------------------------------------------------------
 * 小节：发布
 * ---------------------------------------------------------------------- */

mb_err_t mb_node_publish(mb_node_t *node,
                         const char *topic,
                         const void *payload,
                         size_t payload_len,
                         const mb_publish_opts_t *opts)
{
    if (node == NULL || topic == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    return mb_bus_publish_internal(node->bus, node->name, topic, payload, payload_len, opts);
}

mb_err_t mb_node_publish_to(mb_node_t *node,
                            const char *dst_node,
                            const char *subtopic,
                            const void *payload,
                            size_t payload_len,
                            const mb_publish_opts_t *opts)
{
    char topic[MB_CONFIG_MAX_TOPIC_LEN];
    mb_err_t err;

    if (node == NULL || dst_node == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    if (validate_node_name(dst_node) != MB_OK) {
        return MB_ERR_INVALID_ARG;
    }

    err = mb_topic_build(topic, sizeof(topic), dst_node, subtopic);
    if (err != MB_OK) {
        return err;
    }
    return mb_bus_publish_internal(node->bus, node->name, topic, payload, payload_len, opts);
}

/* -------------------------------------------------------------------------
 * 小节：订阅
 * ---------------------------------------------------------------------- */

mb_err_t mb_node_subscribe_ex(mb_node_t *node,
                              const mb_subscribe_opts_t *opts,
                              mb_handler_t handler,
                              mb_subscription_t **out_sub)
{
    mb_bus_t *bus;
    mb_subscription_t *sub;
    mb_err_t err;

    if (out_sub != NULL) {
        *out_sub = NULL;
    }
    if (node == NULL || opts == NULL || opts->filter == NULL || handler == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    err = mb_topic_validate_filter(opts->filter);
    if (err != MB_OK) {
        MB_LOG_WARN("sub", "invalid topic filter: '%s'", opts->filter);
        return err;
    }
    if (opts->source_filter != NULL) {
        err = mb_topic_validate_filter(opts->source_filter);
        if (err != MB_OK) {
            MB_LOG_WARN("sub", "invalid source filter: '%s'", opts->source_filter);
            return err;
        }
    }

    /* 先在锁外把对象准备好，尽量缩短临界区 */
    sub = (mb_subscription_t *)mb_os_calloc(1, sizeof(*sub));
    if (sub == NULL) {
        return MB_ERR_NO_MEMORY;
    }
    sub->filter = mb_strdup(opts->filter);
    sub->source_filter = (opts->source_filter != NULL) ? mb_strdup(opts->source_filter) : NULL;
    if (sub->filter == NULL || (opts->source_filter != NULL && sub->source_filter == NULL)) {
        mb_os_free(sub->filter);
        mb_os_free(sub->source_filter);
        mb_os_free(sub);
        return MB_ERR_NO_MEMORY;
    }

    bus = node->bus;
    sub->bus = bus;
    sub->node = node;
    sub->handler = handler;
    sub->user_data = opts->user_data;
    sub->flags = opts->flags;
    sub->refcnt = 1; /* 订阅表持有的一份引用 */
    sub->active = true;

    MB_BUS_LOCK(bus);
    if (bus->destroying || !node->active) {
        MB_BUS_UNLOCK(bus);
        mb_os_free(sub->filter);
        mb_os_free(sub->source_filter);
        mb_os_free(sub);
        return MB_ERR_STATE;
    }
    sub->seq = ++bus->next_sub_seq;
    if (!mb_bus_add_subscription_locked(bus, sub)) {
        MB_BUS_UNLOCK(bus);
        mb_os_free(sub->filter);
        mb_os_free(sub->source_filter);
        mb_os_free(sub);
        return MB_ERR_NO_MEMORY;
    }
    MB_BUS_UNLOCK(bus);

    MB_LOG_DEBUG("sub", "node '%s' subscribed to '%s'", node->name, sub->filter);

    if ((sub->flags & MB_SUB_FLAG_SKIP_RETAINED) == 0) {
        mb_dispatch_retained_to_subscription(bus, sub);
    }

    if (out_sub != NULL) {
        *out_sub = sub;
    }
    return MB_OK;
}

mb_err_t mb_node_subscribe(mb_node_t *node,
                           const char *filter,
                           mb_handler_t handler,
                           void *user_data,
                           mb_subscription_t **out_sub)
{
    mb_subscribe_opts_t opts;

    opts.filter = filter;
    opts.source_filter = NULL;
    opts.flags = 0;
    opts.user_data = user_data;

    return mb_node_subscribe_ex(node, &opts, handler, out_sub);
}

mb_err_t mb_node_subscribe_self(mb_node_t *node,
                                mb_handler_t handler,
                                void *user_data,
                                mb_subscription_t **out_sub)
{
    char filter[MB_CONFIG_MAX_TOPIC_LEN];
    mb_subscribe_opts_t opts;
    mb_err_t err;

    if (node == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    err = mb_topic_build(filter, sizeof(filter), node->name, "#");
    if (err != MB_OK) {
        return err;
    }

    opts.filter = filter;
    opts.source_filter = NULL;
    opts.flags = 0;
    opts.user_data = user_data;

    return mb_node_subscribe_ex(node, &opts, handler, out_sub);
}

mb_err_t mb_node_unsubscribe(mb_subscription_t **sub_ptr)
{
    mb_subscription_t *sub;
    mb_bus_t *bus;

    if (sub_ptr == NULL || *sub_ptr == NULL) {
        return MB_ERR_INVALID_ARG;
    }
    sub = *sub_ptr;
    bus = sub->bus;

    MB_BUS_LOCK(bus);
    if (sub->active) {
        mb_subscription_detach_locked(bus, sub);
        mb_subscription_release_locked(bus, sub); /* 释放订阅表持有的引用 */
    }
    MB_BUS_UNLOCK(bus);

    *sub_ptr = NULL;
    return MB_OK;
}

/* -------------------------------------------------------------------------
 * 小节：订阅对象查询
 * ---------------------------------------------------------------------- */

const char *mb_subscription_filter(const mb_subscription_t *sub)
{
    return (sub != NULL) ? sub->filter : NULL;
}

const char *mb_subscription_source_filter(const mb_subscription_t *sub)
{
    return (sub != NULL) ? sub->source_filter : NULL;
}

mb_node_t *mb_subscription_node(const mb_subscription_t *sub)
{
    return (sub != NULL) ? sub->node : NULL;
}

void *mb_subscription_user_data(const mb_subscription_t *sub)
{
    return (sub != NULL) ? sub->user_data : NULL;
}

void *mb_subscription_set_user_data(mb_subscription_t *sub, void *user_data)
{
    void *previous;

    if (sub == NULL) {
        return NULL;
    }

    MB_BUS_LOCK(sub->bus);
    previous = sub->user_data;
    sub->user_data = user_data;
    MB_BUS_UNLOCK(sub->bus);
    return previous;
}

bool mb_subscription_is_valid(const mb_subscription_t *sub)
{
    bool valid;

    if (sub == NULL) {
        return false;
    }

    MB_BUS_LOCK(sub->bus);
    valid = sub->active;
    MB_BUS_UNLOCK(sub->bus);
    return valid;
}
