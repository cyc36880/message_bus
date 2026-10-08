/**
 * @file mb_types.h
 * @brief 公共类型：错误码、不透明句柄、消息结构、回调原型、标志位、统计。
 */
#ifndef MESSAGE_BUS_MB_TYPES_H
#define MESSAGE_BUS_MB_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 不透明句柄
 * ---------------------------------------------------------------------- */

typedef struct mb_bus mb_bus_t;                   /**< 总线 */
typedef struct mb_node mb_node_t;                 /**< 节点 */
typedef struct mb_subscription mb_subscription_t; /**< 一次订阅 */

/* -------------------------------------------------------------------------
 * 错误码
 *
 * 所有返回 mb_err_t 的 API 都只会返回下面的值之一。
 * 顺序不是 ABI，但请用符号而不是字面量比较。
 * ---------------------------------------------------------------------- */
typedef enum mb_err {
    MB_OK = 0,                 /**< 成功 */
    MB_ERR_INVALID_ARG = -1,   /**< 参数非法（空指针、空字符串、主题含通配符等） */
    MB_ERR_NO_MEMORY = -2,     /**< 内存分配失败 */
    MB_ERR_NOT_FOUND = -3,     /**< 对象不存在 */
    MB_ERR_ALREADY_EXISTS = -4,/**< 对象已存在（如重名节点） */
    MB_ERR_FULL = -5,          /**< 容量已满 */
    MB_ERR_BUSY = -6,          /**< 资源忙或超过递归深度限制 */
    MB_ERR_STATE = -7,         /**< 对象状态不允许该操作（如总线正在销毁） */
    MB_ERR_TOO_LONG = -8,      /**< 字符串或数据超过配置上限 */
    MB_ERR_UNSUPPORTED = -9,   /**< 当前平台/配置不支持该功能 */
    MB_ERR_TIMEOUT = -10,      /**< 等待超时（异步发布的队列满且等不到空位） */
    MB_ERR_WOULD_DEADLOCK = -11, /**< 这次调用会永久卡死，已被拒绝；见 mb_node_publish_async */
} mb_err_t;

/** @return 错误码对应的英文短名，如 "MB_ERR_NOT_FOUND"。 */
const char *mb_err_to_string(mb_err_t err);

/* -------------------------------------------------------------------------
 * 等待时间
 *
 * 凡是以 `timeout_ms` 为形参的 API 都用这两个值表达「不等待」与「永久等待」，
 * 其余取值按毫秒解释。
 * ---------------------------------------------------------------------- */

/** 不等待：拿不到立刻返回（配合超时错误码使用）。 */
#define MB_WAIT_NONE 0u
/** 永久等待，直到拿到为止。 */
#define MB_WAIT_FOREVER 0xFFFFFFFFu

/* -------------------------------------------------------------------------
 * 消息
 *
 * 消息视图只在回调执行期间有效：投递是零拷贝的，topic / payload / source
 * 直接指向发布者的缓冲区（retained 补发的消息除外，那一份由总线持有）。
 * 因此回调**不得**保存这些指针，也不得修改指向的内容；需要留存请自行拷贝。
 * ---------------------------------------------------------------------- */

/** 该消息来自 retained 保留存储（订阅时补发的历史消息）。 */
#define MB_MSG_FLAG_RETAINED (1u << 0)
/** 该消息由总线自身发布（系统主题，topic 以 '$' 开头）。 */
#define MB_MSG_FLAG_SYSTEM (1u << 1)

typedef struct mb_message {
    const char *topic;      /**< 主题，非空，不以 '\0' 结尾以外的空白修饰 */
    const void *payload;    /**< 负载首地址；payload_len 为 0 时为 NULL */
    size_t payload_len;     /**< 负载字节数 */
    const char *source;     /**< 发布者节点名；总线自身发布时为 NULL */
    uint32_t id;            /**< 总线内单调递增的消息序号 */
    uint32_t timestamp_ms;  /**< 发布时刻的 mb_os_time_ms() */
    uint8_t flags;          /**< MB_MSG_FLAG_* 组合 */
} mb_message_t;

/* -------------------------------------------------------------------------
 * 回调
 * ---------------------------------------------------------------------- */

/**
 * 订阅者回调原型。
 *
 * 调用时机：**在发布者的线程内同步调用**，且**不持有总线锁**，
 * 因此回调内部可以安全地再次调用总线的任何 API
 * （publish / subscribe / unsubscribe，包括取消自己）。
 *
 * 不要在这里做耗时操作：它直接阻塞发布者。LVGL 场景下不要在回调里
 * 直接操作控件，请参考 docs/architecture.md 的「多线程注意事项」。
 *
 * @param sub       本次投递所命中的订阅对象，可用于查询自己订阅了什么。
 * @param msg       消息视图，仅在回调期间有效。
 * @param user_data 订阅时传入的用户指针。
 */
typedef void (*mb_handler_t)(mb_subscription_t *sub, const mb_message_t *msg, void *user_data);

/* -------------------------------------------------------------------------
 * 发布选项
 * ---------------------------------------------------------------------- */

/** 保留消息：总线上保存该主题的最后一条，之后任何匹配的新订阅者会立刻收到它。 */
#define MB_PUB_FLAG_RETAIN (1u << 0)
/**
 * 本条消息强制同步投递（当前版本恒为同步，该标志为兼容与语义显式化保留）。
 */
#define MB_PUB_FLAG_SYNC (1u << 1)
/**
 * 仅对**异步发布**有效：本条消息所在的主题条目已满时，丢弃该条目里最旧的
 * 一条来腾位置，而不是阻塞等待。等价于对这一次调用强制打开「覆盖最旧」，
 * 覆盖 MB_CONFIG_ASYNC_OVERWRITE_OLDEST 的默认值。
 *
 * 同步发布忽略本标志。
 */
#define MB_PUB_FLAG_ASYNC_OVERWRITE (1u << 2)

typedef struct mb_publish_opts {
    uint8_t flags; /**< MB_PUB_FLAG_* 组合，可为 0 */
    uint8_t qos;   /**< 预留：MQTT QoS。当前仅支持 0（至多一次） */
} mb_publish_opts_t;

/* -------------------------------------------------------------------------
 * 订阅选项
 * ---------------------------------------------------------------------- */

/** 不要补发已存在的 retained 消息。 */
#define MB_SUB_FLAG_SKIP_RETAINED (1u << 0)
/** 不接收自己发布的、且源节点等于本订阅所属节点的消息（MQTT 5 NoLocal）。 */
#define MB_SUB_FLAG_NOLOCAL (1u << 1)

typedef struct mb_subscribe_opts {
    const char *filter;        /**< 必填：MQTT 主题过滤器，可含 '+' / '#' */
    const char *source_filter; /**< 可选：对发布者节点名做二次过滤，同样支持通配符 */
    uint8_t flags;             /**< MB_SUB_FLAG_* 组合，可为 0 */
    void *user_data;           /**< 透传给回调的指针，可为 NULL */
} mb_subscribe_opts_t;

/* -------------------------------------------------------------------------
 * 统计
 * ---------------------------------------------------------------------- */
typedef struct mb_bus_stats {
    uint64_t published;            /**< 累计发布的消息条数（含 retained） */
    uint64_t delivered;            /**< 累计投递到回调的次数 */
    uint64_t dropped;              /**< 因超递归深度被丢弃的投递次数 */
    uint64_t no_subscriber;        /**< 发布时没有任何订阅者命中的条数 */
    uint64_t retained_stored;      /**< 当前 retained 表中的条目数 */
    uint64_t nodes_created;        /**< 累计创建的节点数（含已销毁） */
    uint64_t peak_subscriptions;   /**< 订阅表历史峰值 */
    uint64_t async_enqueued;       /**< 累计成功进入异步队列的消息条数 */
    uint64_t async_dropped;        /**< 因队列满、等待超时而没能入队的条数 */
    uint64_t async_overwritten;    /**< 因「覆盖最旧」策略被丢弃的旧消息条数 */
} mb_bus_stats_t;

/* -------------------------------------------------------------------------
 * 总线配置
 *
 * 新增字段只会追加在 reserved 之前，reserved 用于未来扩展；
 * 使用 mb_bus_default_config() 初始化可保证向前兼容。
 * ---------------------------------------------------------------------- */
typedef struct mb_bus_config {
    /**
     * 是否在节点上线/下线时自动发布系统消息，默认 true：
     *
     *   $mb/nodes/<name>/connected     [retained] 负载 = 节点名
     *       上线时发布；下线时发一条**空负载**的同名消息用于清除保留状态，
     *       因此订阅它同时可以看到「上线」与「掉线」，后启动的订阅者也能
     *       通过 retained 立刻知道当前谁在线。
     *   $mb/nodes/<name>/disconnected  [不保留]  负载 = 节点名
     *       仅在掉线时发布的纯事件消息。
     *
     * 注意：按 MQTT 规则，'#' 不匹配以 '$' 开头的主题，
     * 想收这些消息必须显式订阅 `$mb/nodes/+/connected` 之类。
     */
    bool publish_node_events;
    /** 预留，必须为 NULL/0。 */
    void *reserved[4];
} mb_bus_config_t;

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_TYPES_H */
