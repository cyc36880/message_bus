/**
 * @file mb_bus.h
 * @brief 总线 API：创建、销毁、直接发布、查询。
 *
 * 典型用法：
 * @code
 *   mb_bus_t *bus;
 *   mb_bus_create("main", &bus);          // 1. 创建一条总线
 *   mb_node_t *ui, *motor;
 *   mb_node_create(bus, "ui", &ui);       // 2. 节点挂在总线上
 *   mb_node_create(bus, "motor", &motor);
 *   mb_node_subscribe(motor, "motor/cmd", on_cmd, NULL, NULL);
 *   mb_node_publish(ui, "motor/cmd", "run", 3, NULL);
 *   mb_bus_destroy(bus);                  // 3. 销毁总线（级联销毁所有节点）
 * @endcode
 */
#ifndef MESSAGE_BUS_MB_BUS_H
#define MESSAGE_BUS_MB_BUS_H

#include "mb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 生命周期
 * ---------------------------------------------------------------------- */

/**
 * 用默认配置创建一条总线。
 *
 * 默认配置：publish_node_events = true。
 * 等价于 mb_bus_create_ex(name, NULL, out_bus)。
 *
 * @param name    总线名，非空，长度 < MB_CONFIG_MAX_NAME_LEN。仅用于日志与调试。
 * @param out_bus 输出参数，成功时写入总线句柄。
 * @return MB_OK / MB_ERR_INVALID_ARG / MB_ERR_TOO_LONG / MB_ERR_NO_MEMORY
 */
mb_err_t mb_bus_create(const char *name, mb_bus_t **out_bus);

/**
 * 用显式配置创建总线。
 * @param config 为 NULL 时使用默认配置。
 */
mb_err_t mb_bus_create_ex(const char *name, const mb_bus_config_t *config, mb_bus_t **out_bus);

/**
 * 填充默认配置，供调用方按需修改后再传给 mb_bus_create_ex()。
 * @return MB_OK / MB_ERR_INVALID_ARG
 */
mb_err_t mb_bus_default_config(mb_bus_config_t *config);

/**
 * 销毁总线。
 *
 * 会依次：销毁所有节点（含其全部订阅）→ 释放 retained 消息 → 释放总线。
 * 节点的销毁**不会**再发布下线事件（总线已在关闭中）。
 *
 * @warning 调用前必须保证没有其它线程正在使用该总线，也没有回调正在执行。
 *          在回调内部调用本函数会死锁。
 */
void mb_bus_destroy(mb_bus_t *bus);

/* -------------------------------------------------------------------------
 * 查询
 * ---------------------------------------------------------------------- */

/** @return 总线名；bus 为 NULL 时返回 NULL。 */
const char *mb_bus_name(const mb_bus_t *bus);

/** 拷贝一份当前统计快照。 */
mb_err_t mb_bus_get_stats(const mb_bus_t *bus, mb_bus_stats_t *out_stats);

/** 清零统计计数器（不影响 retained 表与订阅）。 */
void mb_bus_reset_stats(mb_bus_t *bus);

/** @return 当前挂载的节点数；bus 为 NULL 时返回 0。 */
size_t mb_bus_node_count(const mb_bus_t *bus);

/** @return 当前的订阅总数；bus 为 NULL 时返回 0。 */
size_t mb_bus_subscription_count(const mb_bus_t *bus);

/**
 * 按名字查找节点。
 *
 * @warning 返回的是**借用指针**：只要没有其它线程销毁该节点就一直有效。
 *          多线程下若可能并发销毁，请自行用外部机制保护，或改用事件订阅
 *          （$mb/nodes/+/connected / disconnected）。
 *
 * @return MB_OK 并写出节点；MB_ERR_NOT_FOUND；MB_ERR_INVALID_ARG。
 */
mb_err_t mb_bus_find_node(const mb_bus_t *bus, const char *name, mb_node_t **out_node);

/* -------------------------------------------------------------------------
 * 总线级发布
 * ---------------------------------------------------------------------- */

/**
 * 由总线自身发布一条消息（source 为 NULL）。
 *
 * 用于系统主题或不属于任何节点的广播。语义与 mb_node_publish() 完全一致：
 * 投递是**零拷贝**的（回调直接读调用方的缓冲区），但因为是同步投递，
 * 本函数返回时所有回调都已执行完毕，所以调用方随后即可释放自己的缓冲。
 *
 * @param opts 可为 NULL，表示无标志、QoS 0。
 */
mb_err_t mb_bus_publish(mb_bus_t *bus,
                        const char *topic,
                        const void *payload,
                        size_t payload_len,
                        const mb_publish_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_BUS_H */
