/**
 * @file mb_internal.h
 * @brief 库内部数据结构与函数声明。**不对外安装**。
 *
 * 锁规则（贯穿全部实现，务必遵守）：
 *   - bus->lock 保护下面所有结构体的字段，包括引用计数；
 *   - 只有名字里带 `_locked` 后缀的内部函数才可以在已持锁时调用；
 *   - **任何用户回调都必须在锁外调用**，调用前先给订阅对象和节点各加一次引用。
 */
#ifndef MESSAGE_BUS_MB_INTERNAL_H
#define MESSAGE_BUS_MB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "message_bus/message_bus.h"

/* -------------------------------------------------------------------------
 * 线程局部存储：用于同步投递的递归深度计数。
 * 得不到 TLS 支持时退化为普通静态变量（仅在单线程下语义正确）。
 * ---------------------------------------------------------------------- */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MB_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
#define MB_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
#define MB_THREAD_LOCAL __thread
#else
#define MB_THREAD_LOCAL
#endif

#define MB_BUS_LOCK(bus) mb_os_mutex_lock((bus)->lock)
#define MB_BUS_UNLOCK(bus) mb_os_mutex_unlock((bus)->lock)

/* -------------------------------------------------------------------------
 * 内部拥有的消息（深拷贝），仅 retained 表使用。
 * 普通投递是零拷贝的：回调期间直接引用调用方的缓冲区。
 * ---------------------------------------------------------------------- */
typedef struct mb_owned_message {
    char *topic;         /**< 独立堆内存 */
    void *payload;       /**< 独立堆内存；payload_len 为 0 时为 NULL */
    size_t payload_len;
    char *source;        /**< 独立堆内存；可为 NULL */
    uint32_t id;
    uint32_t timestamp_ms;
    uint8_t flags;
} mb_owned_message_t;

/* -------------------------------------------------------------------------
 * retained 保留消息表项（链表，按 seq 升序）
 * ---------------------------------------------------------------------- */
typedef struct mb_retained {
    struct mb_retained *next;
    mb_owned_message_t *msg;
    uint64_t seq;    /**< 用于订阅补发时的分批游标 */
    uint32_t refcnt; /**< >1 表示正有回调在使用它 */
    bool active;
} mb_retained_t;

/* -------------------------------------------------------------------------
 * 订阅
 * ---------------------------------------------------------------------- */
struct mb_subscription {
    uint64_t seq;          /**< 创建序号；总线的订阅表按它升序排列 */
    mb_bus_t *bus;
    mb_node_t *node;
    char *filter;          /**< 主题过滤器（独立堆内存） */
    char *source_filter;   /**< 来源过滤器，可为 NULL */
    mb_handler_t handler;
    void *user_data;
    uint32_t refcnt;       /**< 链表引用 + 每次在途回调各 1 */
    bool active;           /**< false = 已取消订阅或所属节点已销毁 */
    uint8_t flags;
};

/* -------------------------------------------------------------------------
 * 节点
 * ---------------------------------------------------------------------- */
struct mb_node {
    mb_bus_t *bus;
    char *name;       /**< 独立堆内存 */
    uint32_t refcnt;  /**< 链表引用 + 每次在途回调各 1 */
    bool active;
};

/* -------------------------------------------------------------------------
 * 异步队列：按主题分组的 FIFO
 *
 * 结构是**两级**的：总线上有一张主题条目表，每个条目自带一条固定深度的
 * 环形队列。这样 "a/b" 上积压再多也不会挤掉 "a/c" 的位置。
 *
 * 信号量账本（是这套实现的关键，两条不变式必须同时成立）：
 *   entry->space 的计数 == MB_CONFIG_ASYNC_QUEUE_DEPTH - entry->count
 *     —— 每成功 push 一条就消耗一个空位，每 pop 一条就归还一个空位。
 *   pool->free_entries 的计数 == entry_capacity - entry_count
 *     —— 每建一个条目消耗一个名额，每释放一个条目归还一个。
 * 任何一条路径漏掉一次消耗/归还，都会让队列凭空变满或凭空清空。
 * ---------------------------------------------------------------------- */
#if MB_CONFIG_ASYNC_MAX_TOPICS > 0
typedef struct mb_async_entry {
    char *topic; /**< 主题串的深拷贝，条目被释放前一直有效 */
    mb_owned_message_t *slots[MB_CONFIG_ASYNC_QUEUE_DEPTH]; /**< 环形队列本体 */
    uint32_t head;    /**< 下一个待取出的槽位 */
    uint32_t count;   /**< 当前积压条数 */
    /**
     * 正阻塞在 space 信号量上的发布者数量。
     * 条目**只有在 count == 0 且 waiters == 0 时**才能被释放：
     * 否则会连信号量一起销毁，把正等在上面的发布者丢进已释放内存。
     */
    uint32_t waiters;
    mb_sem_t *space; /**< 空位计数信号量，初值 = MB_CONFIG_ASYNC_QUEUE_DEPTH */
} mb_async_entry_t;

typedef struct mb_async_pool {
    mb_async_entry_t **entries; /**< 指针数组，与 bus->subs 同样风格，按创建顺序 */
    size_t entry_count;
    size_t entry_capacity; /**< = MB_CONFIG_ASYNC_MAX_TOPICS */
    size_t next_entry;     /**< pump 的轮转游标，保证各主题之间公平 */
    mb_sem_t *free_entries; /**< 空条目计数信号量，初值 = entry_capacity */
} mb_async_pool_t;
#endif

/* -------------------------------------------------------------------------
 * 总线
 * ---------------------------------------------------------------------- */
struct mb_bus {
    char *name;
    mb_mutex_t *lock; /**< 递归互斥量，保护本结构所有字段 */

    mb_node_t **nodes;
    size_t node_count;
    size_t node_capacity;

    mb_subscription_t **subs; /**< 按 seq 升序 */
    size_t sub_count;
    size_t sub_capacity;
    uint64_t next_sub_seq;

    mb_retained_t *retained; /**< 按 seq 升序 */
    uint64_t next_retained_seq;

    uint32_t next_message_id;
    uint32_t node_seq; /**< 自动命名 "node-N" 用 */

    mb_bus_stats_t stats;
    mb_bus_config_t config;
    bool destroying;

#if MB_CONFIG_ASYNC_MAX_TOPICS > 0
    /** 异步队列；**首次使用时才分配**，不用异步的总线一个字节都不花。 */
    mb_async_pool_t async;
    /** 是否有线程正在 mb_bus_pump()。异步队列只允许被一个线程消费。 */
    bool pumping;
#endif
};

/* -------------------------------------------------------------------------
 * mb_message.c
 * ---------------------------------------------------------------------- */
char *mb_strdup(const char *s);
mb_owned_message_t *mb_owned_message_create(const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            const char *source,
                                            uint32_t id,
                                            uint32_t timestamp_ms,
                                            uint8_t flags);
void mb_owned_message_free(mb_owned_message_t *msg);
void mb_owned_message_view(const mb_owned_message_t *msg, mb_message_t *out_view);

/* -------------------------------------------------------------------------
 * mb_subscription.c
 * ---------------------------------------------------------------------- */
/** 追加到订阅表（保持 seq 升序）。@return false 表示扩容失败。 */
bool mb_bus_add_subscription_locked(mb_bus_t *bus, mb_subscription_t *sub);
/** 从订阅表摘除并置 active=false，但不释放链表引用（由调用方决定何时释放）。 */
void mb_subscription_detach_locked(mb_bus_t *bus, mb_subscription_t *sub);
void mb_subscription_retain_locked(mb_subscription_t *sub);
/** 释放一次引用；引用归零且已摘除时真正 free。 */
void mb_subscription_release_locked(mb_bus_t *bus, mb_subscription_t *sub);

/* -------------------------------------------------------------------------
 * mb_node.c
 * ---------------------------------------------------------------------- */
/** 追加到节点表。@return false 表示扩容失败。 */
bool mb_bus_add_node_locked(mb_bus_t *bus, mb_node_t *node);
/** 从节点表摘除（不释放引用，由调用方决定何时释放）。 */
void mb_bus_remove_node_locked(mb_bus_t *bus, mb_node_t *node);
void mb_node_release_locked(mb_bus_t *bus, mb_node_t *node);
/** 摘除某个节点的全部订阅（节点销毁时调用）。 */
void mb_node_detach_subscriptions_locked(mb_bus_t *bus, mb_node_t *node);

/* -------------------------------------------------------------------------
 * mb_dispatch.c
 * ---------------------------------------------------------------------- */
/**
 * 同步投递一条消息给所有匹配的订阅者。
 * @param out_delivered 可选，写入实际调用的回调次数。
 * @return MB_OK；MB_ERR_BUSY 表示超过递归深度限制，此时**没有任何**回调被调用。
 */
mb_err_t mb_dispatch_message(mb_bus_t *bus, const mb_message_t *msg, size_t *out_delivered);

/** 把已存在的 retained 消息补发给刚建立的订阅。 */
void mb_dispatch_retained_to_subscription(mb_bus_t *bus, mb_subscription_t *sub);

/* -------------------------------------------------------------------------
 * mb_async.c
 * ---------------------------------------------------------------------- */
/** 异步发布的公共实现（同步/异步共用发布前的校验与 retained 处理）。 */
mb_err_t mb_bus_publish_async_internal(mb_bus_t *bus,
                                       const char *source,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       const mb_publish_opts_t *opts,
                                       uint32_t timeout_ms);

/** 释放异步队列（连同队列里还没投递的消息）。由 mb_bus_destroy 调用。 */
void mb_async_pool_dispose(mb_bus_t *bus);

/* -------------------------------------------------------------------------
 * mb_bus.c
 * ---------------------------------------------------------------------- */

/**
 * 发布前的公共校验：qos 检查 → 主题校验 → payload 合法性 → 长度上限，
 * 并把 opts 归一化成显式值。**不接触总线状态、不加锁**。
 *
 * @param out_opts 写入归一化后的选项（opts 为 NULL 时是 flags=0/qos=0）。
 * @param out_flags 写入这条消息对外可见的 mb_message_t.flags。
 * @return MB_OK / MB_ERR_UNSUPPORTED / MB_ERR_INVALID_ARG / MB_ERR_TOO_LONG
 */
mb_err_t mb_publish_prepare(const char *topic,
                            const void *payload,
                            size_t payload_len,
                            const mb_publish_opts_t *opts,
                            mb_publish_opts_t *out_opts,
                            uint8_t *out_flags);

/** 处理 MB_PUB_FLAG_RETAIN：空负载清除保留，否则深拷贝存进 retained 表。锁内调用。 */
void mb_bus_retain_locked(mb_bus_t *bus, const mb_message_t *msg);

mb_err_t mb_bus_publish_internal(mb_bus_t *bus,
                                 const char *source,
                                 const char *topic,
                                 const void *payload,
                                 size_t payload_len,
                                 const mb_publish_opts_t *opts);

/** 发布 `$mb/nodes/<name>/connected`（retained）或 `.../disconnected`（清空）。 */
void mb_bus_publish_node_event(mb_bus_t *bus, const char *node_name, bool connected);

/**
 * 释放一次 retained 表项的引用；引用归零且已摘除时真正 free。
 * 投递 retained 消息时先 +1，回调返回后再调用本函数。
 */
void mb_retained_release_locked(mb_bus_t *bus, mb_retained_t *entry);

/** 从 retained 链表中摘除并释放其持有的那一份引用。 */
void mb_retained_remove_locked(mb_bus_t *bus, mb_retained_t *entry, mb_retained_t *prev);

#endif /* MESSAGE_BUS_MB_INTERNAL_H */
