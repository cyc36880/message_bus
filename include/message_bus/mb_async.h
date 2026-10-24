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
 * ── 想等订阅方处理完再继续？──────────────────────────────────────────────
 * 普通异步发布返回时只代表「进了队列」。若发布方必须在订阅方处理完之后才能
 * 往下走（命令确认、请求-应答、逐帧推进），用 mb_node_publish_async_wait()：
 * 它在入队后阻塞，直到 pump 把这条消息投递给所有匹配订阅者、回调返回为止
 * （可设超时）。回调仍然跑在 pump 线程上，两边因此可以在关键点上对齐。
 * 详见本文件下半部分的「异步发布 + 等待消费（await）」。
 *
 * 如果连等都不能等（例如 UI 线程要立刻回去刷遮罩），改用
 * mb_node_publish_async_notify()：登记完成回调后立刻返回，成功/超时/失败
 * 都由 pump 线程回调通知，回调里可以拿到原始请求并重试。详见
 * 「非阻塞推送 + 完成回调（notify）」。
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
 *         MB_ERR_WOULD_DEADLOCK  在 mb_bus_pump() 的回调里调用，且 timeout_ms
 *                                 是 MB_WAIT_FOREVER（见 mb_bus_pump 的警告）
 *
 * @note 本函数可以被任意多个线程并发调用，是线程安全的。
 * @note 消息的 id / timestamp_ms 在**入队时**确定，不是投递时。
 * @note 返回 MB_ERR_WOULD_DEADLOCK 的调用**没有任何副作用**：没入队，
 *       也不算「丢弃」（`stats.async_dropped` 与此无关）。
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
 * 异步发布 + 等待消费（await）
 *
 * ── 解决什么问题 ────────────────────────────────────────────────────────
 * 普通异步发布是「入队即返回」，发布方并不知道订阅方什么时候真正处理完：
 *
 *     A 线程: publish_async(cmd) ──► 立刻继续执行
 *     B 线程:                        pump() ──► on_cmd() ──► 真正生效
 *
 * 如果 A 接下来就要读「命令执行后的结果」，它必须自己加条件变量、轮询状态，
 * 或者干脆退回同步发布（但那会把回调拉回 A 的线程）。await 系列把这件事
 * 做进总线：**入队后阻塞，直到 pump 把这条消息投递给所有匹配的订阅者并
 * 执行完回调**（或超时），然后返回。回调仍然跑在 pump 线程上，两边因此
 * 可以在收发的关键点上对齐。
 *
 *     A 线程: publish_async_wait(cmd, 100) ──┐
 *     B 线程:                                  pump() ──► on_cmd() ──► 生效
 *     A 线程: ◄──────────────────────────────── 返回（回调已结束）
 *
 * 返回 MB_OK 时，out_delivered 是实际执行的回调次数。0 表示发布时没有任何
 * 订阅者命中 —— 消息已被 pump 处理（这一点与同步发布不同：同步发布没人订阅
 * 也照样返回 MB_OK，但这里**仍然必须有 pump 在跑**，否则只能等到超时）。
 * **如果业务上要求「一定要有节点消费」，请检查 `out_delivered > 0`**：
 * 库不会把「无人订阅」当成错误，因为那与普通发布、与 MQTT 的语义都一致。
 *
 * ── 超时与撤销 ──────────────────────────────────────────────────────────
 * timeout_ms 是**总预算**，同时覆盖两段等待：
 *   1. 队列满时等空位（与普通异步发布相同）；
 *   2. 入队后等 pump 消化。
 *
 * 超时返回 MB_ERR_TIMEOUT，并尽量让这条消息**不再被投递**：
 *   - 若它还在队列里，会被**当场从队列摘除**并计入 `stats.async_canceled` ——
 *     回调一定不会执行，主题空位也立刻还回去；
 *   - 若 pump 已经把它取走、回调正在执行（窗口很窄），撤销已来不及：
 *     调用方仍然拿到 MB_ERR_TIMEOUT，但回调**可能仍会执行**。需要严格
 *     二选一时请把超时设得足够宽，或在回调侧做幂等。
 *
 * 未超时但消息被丢弃时，返回真实原因：
 *   - MB_ERR_DROPPED：另一条普通发布用了「覆盖最旧」策略，把这条等待中的
 *     消息顶掉了。await 消息自身永不参与覆盖，但挡不住别的发布者覆盖它。
 *
 * ── 限制（都很重要）────────────────────────────────────────────────────
 *   - **等待模式（timeout_ms != MB_WAIT_NONE）下不能与
 *     MB_PUB_FLAG_ASYNC_OVERWRITE 同时使用**，会返回 MB_ERR_INVALID_ARG。
 *     编译期默认值 MB_CONFIG_ASYNC_OVERWRITE_OLDEST 对 await 消息不生效
 *     （否则「等待被消费」就无从谈起）。
 *   - **不能在 mb_bus_pump() 的回调里调用**（MB_WAIT_NONE 除外）：
 *     消费这条消息的正是当前被回调占住的 pump，等下去只会等到超时。
 *     检测到 pump 线程时直接返回 MB_ERR_WOULD_DEADLOCK，且没有任何副作用。
 *   - **裸机（MB_OS_NONE）无法真正阻塞**：入队后会立刻按「等不到」处理，
 *     撤销消息并返回 MB_ERR_TIMEOUT。裸机上请改用普通异步发布 + 轮询
 *     `mb_bus_async_pending()`，或者干脆用同步发布。
 *   - 每次调用多一次小的堆分配（回执 + 信号量）。它是给**关键握手点**用的，
 *     不要拿它做高频数据通路 —— 那等于把异步的吞吐优势又还回去了。
 *   - 等待期间**必须有另一个线程在跑 `mb_bus_pump()`**；如果调用线程自己
 *     就是唯一的 pump，它会在等待里卡到超时（回调没人驱动）。
 *   - 销毁总线前必须让所有 await 调用先返回（与其它 API 的销毁契约一致）。
 * ─────────────────────────────────────────────────────────────────────── */

/**
 * 以节点身份异步发布，并等待消息被消费（或超时）。
 *
 * @param node          发布者，非空。
 * @param topic         主题，非空且不含通配符。
 * @param payload       负载；可为 NULL（此时 payload_len 必须为 0）。
 * @param payload_len   负载字节数。
 * @param opts          发布选项，可为 NULL。qos 必须为 0；
 *                      不允许 MB_PUB_FLAG_ASYNC_OVERWRITE。
 * @param timeout_ms    总超时：MB_WAIT_NONE 表示只入队不等（等价于
 *                      mb_node_publish_async），MB_WAIT_FOREVER 表示一直等。
 * @param out_delivered 可选输出：MB_OK 时写入实际执行的回调次数。
 *
 * @return MB_OK               已投递完毕，所有匹配回调都已在 pump 线程上返回
 *         MB_ERR_TIMEOUT      总超时；消息已撤销，或在极窄窗口里已开始投递
 *         MB_ERR_DROPPED      等待期间被别的发布的覆盖策略顶掉
 *         MB_ERR_BUSY         投递时超过递归深度（见 MB_CONFIG_MAX_DISPATCH_DEPTH）
 *         MB_ERR_WOULD_DEADLOCK  在 pump 回调里调用（timeout_ms != MB_WAIT_NONE）
 *         MB_ERR_INVALID_ARG  参数非法，或等待模式下用了 MB_PUB_FLAG_ASYNC_OVERWRITE
 *         其余错误码与 mb_node_publish_async() 相同
 *
 * @note 线程安全。回调在 pump 线程上执行，本函数在调用者线程上阻塞。
 */
mb_err_t mb_node_publish_async_wait(mb_node_t *node,
                                    const char *topic,
                                    const void *payload,
                                    size_t payload_len,
                                    const mb_publish_opts_t *opts,
                                    uint32_t timeout_ms,
                                    size_t *out_delivered);

/** 定向版本：主题拼成 `dst_node + "/" + subtopic`，其余同
 *  mb_node_publish_async_wait()。 */
mb_err_t mb_node_publish_to_async_wait(mb_node_t *node,
                                       const char *dst_node,
                                       const char *subtopic,
                                       const void *payload,
                                       size_t payload_len,
                                       const mb_publish_opts_t *opts,
                                       uint32_t timeout_ms,
                                       size_t *out_delivered);

/** 总线级版本（source 为 NULL），其余同 mb_node_publish_async_wait()。 */
mb_err_t mb_bus_publish_async_wait(mb_bus_t *bus,
                                   const char *topic,
                                   const void *payload,
                                   size_t payload_len,
                                   const mb_publish_opts_t *opts,
                                   uint32_t timeout_ms,
                                   size_t *out_delivered);

/* -------------------------------------------------------------------------
 * 非阻塞推送 + 完成回调（notify）
 *
 * ── 解决什么问题 ────────────────────────────────────────────────────────
 * 有些场景既不能阻塞调用线程，又必须知道这次推送最后有没有被消费。
 * 典型的是「点击 UI → 弹出等待遮罩 → 推送命令」：
 *
 *     on_click() {
 *         show_overlay();                              // 先亮遮罩
 *         mb_node_publish_async_notify(...);           // 立刻返回，不卡 UI
 *         ...                                          // UI 继续跑，遮罩转圈
 *     }
 *
 *     on_done(result) {                                // pump 线程
 *         hide_overlay();                              // 成功或超时后关掉
 *         if (!result->success) show_error(result->status);
 *     }
 *
 * 和 await 的区别：await 会**阻塞发布方线程**直到投递完成；notify 只登记
 * 一个通知就返回，结果由 mb_bus_pump() 在 pump 线程上回调。失败时回调里
 * 拿到了原始 topic / payload / opts，可以直接再次调用推送函数重试。
 *
 * ── 回调时机 ────────────────────────────────────────────────────────────
 * 通知由 mb_bus_pump() 驱动，和普通异步消息共用同一个线程：
 *   - 消息投递完且至少有一个订阅者命中 → 回调，`result->success = true`；
 *   - 消息被 pump 处理但**无人订阅** → 回调，status = MB_ERR_NO_SUBSCRIBER；
 *   - 到达 timeout_ms 仍未投递 → 撤销队列里的消息，回调，status = MB_ERR_TIMEOUT；
 *   - 入队时就失败（队列满、无空位等）→ 下次 pump 回调，status 为当时错误码。
 *
 * `result->elapsed_since_publish_ms` 是从调用**本次**推送函数到**这次回调触发**
 * 的实测间隔（毫秒），用于记录、调试与调优；重试产生的新通知会重新计时。
 * `result->requested_timeout_ms` 只是原始超时配置，方便原样重试，不是实际等待时间。
 * 每次登记只会回调一次；回调里可以再次调用推送函数（重试），
 * 但请自行限制重试次数/退避，否则失败会一直重试下去。
 *
 * ── 重要限制 ────────────────────────────────────────────────────────────
 *   - **库不创建定时器**：超时只在 mb_bus_pump() 被调用时检查。pump 不跑，
 *     回调就永远不会触发。UI 场景通常由 LVGL 定时器驱动 pump，正好合适。
 *   - 回调运行在 pump 线程上，**锁外**执行，可以安全地操作只能在 pump 线程
 *     访问的对象（例如 LVGL 控件），也可以 publish / subscribe / unsubscribe。
 *   - 不能在回调里再次调用 mb_bus_pump()（返回 MB_ERR_BUSY）。
 *   - **推送函数本身不阻塞**：内部用 MB_WAIT_NONE 入队，队列满也不等，
 *     直接登记一个失败通知。
 *   - 等待/通知模式都不接受 MB_PUB_FLAG_ASYNC_OVERWRITE（返回
 *     MB_ERR_INVALID_ARG）；否则「等它被消化」会被覆盖策略破坏。
 *     `MB_WAIT_FOREVER` 表示只等成功/失败，永不因超时回调。
 *   - 关闭异步功能时所有函数返回 MB_ERR_UNSUPPORTED。
 *   - 销毁总线时**不会**为未完成的通知触发回调，它们会被直接释放。请先
 *     驱动 pump 让它们出结果，或自行保证销毁时没有未完成通知。
 *   - 每次推送多两次小的堆分配（请求拷贝 + 通知/回执）。适合 UI 命令、
 *     点击确认这类低频关键路径，不适合高频数据通路。
 * ─────────────────────────────────────────────────────────────────────── */

/**
 * 以节点身份非阻塞推送，并在成功或超时时回调。
 *
 * @param node      发布者，非空。
 * @param topic     主题，非空且不含通配符。
 * @param payload   负载；可为 NULL（此时 payload_len 必须为 0）。
 * @param payload_len 负载字节数。
 * @param opts      发布选项，可为 NULL；qos 必须为 0；不允许 OVERWRITE。
 * @param timeout_ms 从本次调用起最多等多久；MB_WAIT_FOREVER 表示不超时。
 * @param on_done   完成回调，非空。在 pump 线程上调用，最多一次。
 * @param user_data 透传给 on_done 的指针。
 *
 * @return MB_OK               通知已登记（是否推送成功由回调报告）
 *         MB_ERR_INVALID_ARG  参数非法，或用了 MB_PUB_FLAG_ASYNC_OVERWRITE
 *         MB_ERR_NO_MEMORY    连通知对象都分配不出来，**不会**有回调
 *         MB_ERR_STATE        总线已在销毁，**不会**有回调
 *         其余错误码同 mb_node_publish_async()
 */
mb_err_t mb_node_publish_async_notify(mb_node_t *node,
                                      const char *topic,
                                      const void *payload,
                                      size_t payload_len,
                                      const mb_publish_opts_t *opts,
                                      uint32_t timeout_ms,
                                      mb_async_done_handler_t on_done,
                                      void *user_data);

/** 定向版本：主题拼成 `dst_node + "/" + subtopic`。 */
mb_err_t mb_node_publish_to_async_notify(mb_node_t *node,
                                         const char *dst_node,
                                         const char *subtopic,
                                         const void *payload,
                                         size_t payload_len,
                                         const mb_publish_opts_t *opts,
                                         uint32_t timeout_ms,
                                         mb_async_done_handler_t on_done,
                                         void *user_data);

/** 总线级版本（source 为 NULL）。 */
mb_err_t mb_bus_publish_async_notify(mb_bus_t *bus,
                                     const char *topic,
                                     const void *payload,
                                     size_t payload_len,
                                     const mb_publish_opts_t *opts,
                                     uint32_t timeout_ms,
                                     mb_async_done_handler_t on_done,
                                     void *user_data);

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
 * 本函数同时负责推进 `*_async_notify()` 的完成通知：
 *   - 消息投递完 → 在返回前触发成功回调；
 *   - 某个通知超过 timeout_ms 仍未投递 → 撤销消息并触发超时回调；
 *   - 因此**超时回调的时效取决于本函数被调用的频率**。
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
 * @warning 若有线程正在 `*_async_wait()` 等回执，必须保证本函数被持续驱动 ——
 *          否则那些发布方只能一直等到超时。
 * @warning 不要在某个回调里对本总线再调一次本函数（会返回 MB_ERR_BUSY）。
 * @warning **从 pump 的回调里做 MB_WAIT_FOREVER 的异步发布会被拒绝**，
 *          返回 MB_ERR_WOULD_DEADLOCK。回调跑在 pump 线程上，而给队列腾出空位的
 *          正是 pump —— 它此刻正卡在你的回调里，永远等不到自己。库会比对
 *          mb_os_thread_id() 识别出这种情况并直接拒绝（记一条 WARN），
 *          而不是让程序挂死；被拒的调用没有任何副作用。
 *          MB_WAIT_NONE 与具体毫秒数不受影响（后者不阻塞时照常成功，
 *          真要阻塞就照常返回 MB_ERR_TIMEOUT 并计入 stats.async_dropped）。
 *          要在回调里发布，就传 MB_WAIT_NONE，或改用
 *          MB_PUB_FLAG_ASYNC_OVERWRITE / MB_CONFIG_ASYNC_OVERWRITE_OLDEST
 *          走覆盖策略，并且同样传 MB_WAIT_NONE（覆盖 + 不等待时永不阻塞）。
 * @note  裸机（MB_OS_NONE）不做上面这项检查：那里信号量本来就不会阻塞，
 *        谈不上「等自己」，见 mb_os.h 的 MB_OS_CAN_BLOCK。
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
