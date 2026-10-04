/**
 * @file mb_node.h
 * @brief 节点与订阅 API。
 *
 * 节点是挂在总线上的通讯实体，也是发布/订阅的主体：
 *
 *   - 每个节点有一个在总线内唯一的字符串名字；
 *   - 节点可以发布任意主题的消息（source 字段会记录它的名字）；
 *   - 节点可以持有任意多个订阅，每个订阅 = 主题过滤器 + 回调；
 *   - 销毁节点会自动摘除它的全部订阅，无需手动清理。
 *
 * 约定：把「节点名」当作主题的第一层，就得到类似 MQTT 的定向寻址：
 *   发往 motor 节点的 motor/status → 主题 "motor/status"
 *   该节点订阅自己的全部消息   → mb_node_subscribe_self()
 *   别的节点要收它的消息       → 订阅 "motor/#" 或用 source_filter = "motor"
 */
#ifndef MESSAGE_BUS_MB_NODE_H
#define MESSAGE_BUS_MB_NODE_H

#include "mb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 节点生命周期
 * ---------------------------------------------------------------------- */

/**
 * 在总线上创建一个节点。
 *
 * @param bus      所属总线，非空。
 * @param name     节点名，非空且总线内唯一，长度 < MB_CONFIG_MAX_NAME_LEN。
 *                 传 NULL 时自动命名为 "node-<序号>"。
 * @param out_node 输出参数。
 * @return MB_OK / MB_ERR_INVALID_ARG / MB_ERR_TOO_LONG /
 *         MB_ERR_ALREADY_EXISTS / MB_ERR_NO_MEMORY / MB_ERR_STATE（总线忙）。
 *
 * 若总线启用 publish_node_events，创建成功后总线会发布
 * `$mb/nodes/<name>/connected`（retained），本次调用返回前完成投递。
 */
mb_err_t mb_node_create(mb_bus_t *bus, const char *name, mb_node_t **out_node);

/**
 * 销毁节点，并摘除其全部订阅。
 *
 * @warning 该节点订阅的回调不能再使用此节点指针。
 *          在**本节点自己的回调**内部调用本函数是允许的（引用计数会保证
 *          回调返回前对象不被释放），但回调返回后该指针即失效。
 */
void mb_node_destroy(mb_node_t *node);

/** @return 节点名；node 为 NULL 时返回 NULL。 */
const char *mb_node_name(const mb_node_t *node);

/** @return 所属总线；node 为 NULL 时返回 NULL。 */
mb_bus_t *mb_node_bus(const mb_node_t *node);

/** @return 该节点当前持有的订阅数量。 */
size_t mb_node_subscription_count(const mb_node_t *node);

/* -------------------------------------------------------------------------
 * 发布
 * ---------------------------------------------------------------------- */

/**
 * 以节点身份发布一条消息。
 *
 * 投递是**同步**的：本函数返回时，所有匹配的订阅者回调都已经执行完毕。
 *
 * 投递也是**零拷贝**的：回调里的 msg->topic / msg->payload 直接指向你传进来的
 * 缓冲区，不额外分配内存（这对 MCU 上的堆很关键）。这意味着两条约束：
 *   - 回调**不得**保存这些指针到回调之外，也不得修改指向的内容；
 *   - 需要留存数据，请在回调里自行拷贝。
 * 函数返回后即可释放自己的缓冲区 —— 因为此时投递已经结束。
 *
 * 例外：带 MB_PUB_FLAG_RETAIN 的消息会被深拷贝一份存进 retained 表。
 *
 * @param node        发布者，非空。
 * @param topic       主题，非空且不含通配符，长度 < MB_CONFIG_MAX_TOPIC_LEN。
 * @param payload     负载；可为 NULL（此时 payload_len 必须为 0）。
 * @param payload_len 负载字节数。
 * @param opts        发布选项，可为 NULL（等价于 flags=0, qos=0）。
 * @return MB_OK / MB_ERR_INVALID_ARG / MB_ERR_TOO_LONG /
 *         MB_ERR_NO_MEMORY / MB_ERR_BUSY（超过递归深度限制）/
 *         MB_ERR_UNSUPPORTED（qos != 0）/ MB_ERR_STATE。
 */
mb_err_t mb_node_publish(mb_node_t *node,
                         const char *topic,
                         const void *payload,
                         size_t payload_len,
                         const mb_publish_opts_t *opts);

/**
 * 定向发布给某个节点：把主题拼成 `dst_node + "/" + subtopic`。
 *
 * @param dst_node 目标节点名（字符串，**不要求**该节点当前存在；
 *                 总线上没有这个节点时消息只是无人接收，不算错误）。
 * @param subtopic 目标主题内剩余部分，可为 NULL/空串。
 * @return 同 mb_node_publish()；主题超长返回 MB_ERR_TOO_LONG。
 */
mb_err_t mb_node_publish_to(mb_node_t *node,
                            const char *dst_node,
                            const char *subtopic,
                            const void *payload,
                            size_t payload_len,
                            const mb_publish_opts_t *opts);

/* -------------------------------------------------------------------------
 * 订阅
 * ---------------------------------------------------------------------- */

/**
 * 订阅一个主题过滤器（最常用形式）。
 *
 * @param node      订阅者，非空。
 * @param filter    MQTT 过滤器，非空，可含 '+' / '#'。
 * @param handler   回调，非空。
 * @param user_data 透传给回调的指针，可为 NULL。
 * @param out_sub   可选输出，写入订阅句柄；不需要时传 NULL。
 * @return MB_OK / MB_ERR_INVALID_ARG / MB_ERR_NO_MEMORY / MB_ERR_STATE。
 *
 * 若未设置 MB_SUB_FLAG_SKIP_RETAINED，本函数返回前会把已存在的、匹配的
 * retained 消息补发给 handler（msg->flags 含 MB_MSG_FLAG_RETAINED）。
 */
mb_err_t mb_node_subscribe(mb_node_t *node,
                           const char *filter,
                           mb_handler_t handler,
                           void *user_data,
                           mb_subscription_t **out_sub);

/** 完整形式，支持 source_filter 与标志位。opts->filter 必填。 */
mb_err_t mb_node_subscribe_ex(mb_node_t *node,
                              const mb_subscribe_opts_t *opts,
                              mb_handler_t handler,
                              mb_subscription_t **out_sub);

/**
 * 订阅「发给自己这个节点」的消息，等价于 filter = "<本节点名>/#"。
 * 配合 mb_node_publish_to() 使用。
 */
mb_err_t mb_node_subscribe_self(mb_node_t *node,
                                mb_handler_t handler,
                                void *user_data,
                                mb_subscription_t **out_sub);

/**
 * 取消订阅。
 *
 * @param sub 指向订阅句柄的指针；成功后 *sub 被置为 NULL，可安全重复调用。
 * @return MB_OK / MB_ERR_INVALID_ARG（*sub 为 NULL）。
 *
 * 若某个回调正在执行中，该订阅对象会延迟到回调返回后才真正释放
 * （引用计数保护），因此「回调里取消自己」是安全的。
 */
mb_err_t mb_node_unsubscribe(mb_subscription_t **sub);

/* -------------------------------------------------------------------------
 * 订阅对象查询
 * ---------------------------------------------------------------------- */

/** @return 订阅使用的主题过滤器；sub 为 NULL 时返回 NULL。 */
const char *mb_subscription_filter(const mb_subscription_t *sub);

/** @return 来源过滤器（可能为 NULL，表示不过滤来源）。 */
const char *mb_subscription_source_filter(const mb_subscription_t *sub);

/** @return 订阅所属节点。 */
mb_node_t *mb_subscription_node(const mb_subscription_t *sub);

/** @return 订阅时传入的 user_data。 */
void *mb_subscription_user_data(const mb_subscription_t *sub);

/** 替换 user_data，返回旧值。可从回调内部调用。 */
void *mb_subscription_set_user_data(mb_subscription_t *sub, void *user_data);

/** 该订阅是否仍然有效（未被取消且节点未被销毁）。 */
bool mb_subscription_is_valid(const mb_subscription_t *sub);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_NODE_H */
