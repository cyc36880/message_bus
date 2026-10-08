/**
 * @file mb_async.h
 * @brief 异步发布 / 订阅：发布者把消息丢进队列就走，投递交给另一个线程。
 *
 * ── 为什么需要它 ────────────────────────────────────────────────────────
 * 同步投递（mb_node_publish）里，回调在**发布者的线程**上跑：
 *
 *     sensor 任务 ──publish()──► on_ui_update() ──► lv_label_set_text()   ❌
 *
 * 界面刷新慢、写 Flash、等网络，全都算在传感器任务的账上。跨线程发消息时
 * 还得先把数据存进中转缓冲，再让接收线程自己去取，非常别扭。
 * 异步投递把这件事交给总线的队列：
 *
 *     sensor 任务 ──publish_async()──► [队列]  ← 立刻返回，不跑任何回调
 *                                        │
 *     ui 任务     ──mb_bus_pump()──────┘ ──► 在这里跑回调（锁外、同步投递同一套路径）
 *
 * 于是回调天然运行在 **pump 线程**上，界面代码可以直接碰控件，不需要中转缓冲。
 *
 * ── 队列长什么样 ────────────────────────────────────────────────────────
 * 队列是**按主题分组**的，主题之间互不挤占：
 *
 *     "a/b" ─► [ 1 ][ 2 ][ 3 ]        每个主题一条 FIFO，深度 = MB_CONFIG_ASYNC_QUEUE_DEPTH
 *     "a/c" ─► [ 7 ]                  最多 MB_CONFIG_ASYNC_MAX_TOPICS 个主题
 *
 * 往 "a/b" 连发内容 1、2，则 a/b 这条条目里积压两条，按 1 → 2 的顺序投递。
 *
 * ── 队列满了怎么办 ──────────────────────────────────────────────────────
 *   - 默认：等。timeout_ms 决定最多等多久，MB_WAIT_FOREVER 表示永久等待，
 *     超时返回 MB_ERR_TIMEOUT 并把消息丢弃（计入 stats.async_dropped）。
 *   - 或者：覆盖。置 MB_PUB_FLAG_ASYNC_OVERWRITE（或把编译期默认值
 *     MB_CONFIG_ASYNC_OVERWRITE_OLDEST 打开），则丢掉该主题里最旧的一条
 *     给新消息腾位置，永不阻塞。
 *
 *     状态类主题（传感器当前值）适合覆盖：消费端慢的时候只保留最新值；
 *     命令类主题（motor/cmd）绝不能覆盖，丢一条就是丢一个动作。
 *
 * ── 使用骨架 ────────────────────────────────────────────────────────────
 * @code
 *   // 【发布方】任意线程
 *   mb_node_publish_async(sensor, "sensor/temp/value", "23.4", 4, NULL, 10);
 *
 *   // 【消费方】独占一个线程
 *   static void ui_task(void *arg) {
 *       mb_bus_t *bus = arg;
 *       for (;;) {
 *           mb_bus_pump(bus);          // 有消息就投递，没有就立刻返回
 *           mb_os_sleep_ms(5);         // 别空转烧 CPU
 *       }
 *   }
 * @endcode
 */

#ifndef MESSAGE_BUS_MB_ASYNC_H
#define MESSAGE_BUS_MB_ASYNC_H

#include "mb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 异步发布
 *
 * 与同名同步 API 的唯一语义差异：
 *   1. 回调**不在本线程**执行，而是在 pump 线程里执行；
 *   2. 因为要活过本次调用，topic / payload / source 会被**深拷贝**一份
 *      （同步投递是零拷贝的）—— 调用方返回后即可释放自己的缓冲区；
 *   3. 队列满时最多等 timeout_ms。
 * 除此之外完全一致：主题校验、retained 语义、错误码都相同。
 * ---------------------------------------------------------------------- */

/**
 * 以节点身份异步发布一条消息。
 *
 * @param node        发布者，非空。
 * @param topic       主题，非空且不含通配符。
 * @param payload     负载；可为 NULL（此时 payload_len 必须为 0）。
 * @param payload_len 负载字节数。
 * @param opts        发布选项，可为 NULL。支持 MB_PUB_FLAG_RETAIN 与
 *                    MB_PUB_FLAG_ASYNC_OVERWRITE；qos 必须为 0。
 * @param timeout_ms  队列满时最多等多少毫秒：
 *                    MB_WAIT_NONE（0）不等待，MB_WAIT_FOREVER 永久等待。
 * @return MB_OK               已进入队列（**不代表已被投递**）
 *         MB_ERR_TIMEOUT      队列满且等不到空位，本条被丢弃
 *         MB_ERR_INVALID_ARG  参数/主题非法
 *         MB_ERR_TOO_LONG     主题或负载超过配置上限
 *         MB_ERR_NO_MEMORY    深拷贝失败
 *         MB_ERR_UNSUPPORTED  qos != 0，或编译期关闭了异步功能
 *         MB_ERR_STATE        总线正在销毁
 *
 * @note 本函数可以被任意多个线程并发调用，是线程安全的。
 * @note 消息的 id / timestamp_ms 在**入队时**确定，不是投递时。
 */
mb_err_t mb_node_publish_async(mb_node_t *node,
                               const char *topic,
                               const void *payload,
                               size_t payload_len,
                               const mb_publish_opts_t *opts,
                               uint32_t timeout_ms);

/**
 * 定向异步发布：主题拼成 `dst_node + "/" + subtopic`，其余同
 * mb_node_publish_async()。
 */
mb_err_t mb_node_publish_to_async(mb_node_t *node,
                                  const char *dst_node,
                                  const char *subtopic,
                                  const void *payload,
                                  size_t payload_len,
                                  const mb_publish_opts_t *opts,
                                  uint32_t timeout_ms);

/**
 * 由总线自身异步发布（source 为 NULL），语义同 mb_bus_publish() 的异步版本。
 */
mb_err_t mb_bus_publish_async(mb_bus_t *bus,
                              const char *topic,
                              const void *payload,
                              size_t payload_len,
                              const mb_publish_opts_t *opts,
                              uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * 队列处理（pump）
 * ---------------------------------------------------------------------- */

/**
 * 取出队列里**当前积压的全部**消息，逐条投递给匹配的订阅者。
 *
 * 这是异步队列的**唯一消费点**，也是异步下标回调真正被执行的地方：
 * 回调在这个线程上、以同步投递的方式被调用，因此
 *   - 回调里可以安全地 publish / subscribe / unsubscribe（含取消自己）；
 *   - 回调是这个线程独占的执行流，**可以直接操作只允许单线程访问的东西**
 *     （例如 LVGL 控件）—— 这正是异步投递最大的好处。
 *
 * @return MB_OK               正常处理完毕（队列本来就是空的时也返回 MB_OK）
 *         MB_ERR_BUSY         另一个线程正在调用本函数
 *         MB_ERR_STATE        总线正在销毁
 *         MB_ERR_UNSUPPORTED  编译期关闭了异步功能
 *         MB_ERR_INVALID_ARG  bus 为 NULL
 *
 * @warning **只允许在一个线程里调用**。两个线程同时 pump 会让同一个订阅者的
 *          回调被并发进入，而本库只承诺「不会重入同一条投递路径」，不承诺
 *          「回调之间互斥」。并发调用会被拒绝并返回 MB_ERR_BUSY，
 *          但不要让程序依赖这个兜底 —— 它是给调试用的，不是设计的一部分。
 * @warning 本函数**不阻塞**：队列空时立刻返回。请在专用线程里循环调用，
 *          并在两次调用之间自行 mb_os_sleep_ms()，否则会空转烧 CPU。
 * @warning 不要在某个回调里对本总线再调一次本函数（会返回 MB_ERR_BUSY）。
 */
mb_err_t mb_bus_pump(mb_bus_t *bus);

/* -------------------------------------------------------------------------
 * 查询
 * ---------------------------------------------------------------------- */

/** @return 队列里还没投递的消息总条数；bus 为 NULL 时为 0。 */
size_t mb_bus_async_pending(const mb_bus_t *bus);

/** @return 当前占用的主题条目数（"a/b"、"a/c" 各算一个）。 */
size_t mb_bus_async_topic_count(const mb_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_ASYNC_H */
