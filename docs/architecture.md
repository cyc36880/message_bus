# 架构说明

本文档描述 message_bus 的**实际实现**，包括数据结构、关键流程、线程与内存模型，
以及这套设计的**优点、代价与注意事项**。想快速上手请看 [README](../README.md)；
想查函数签名请看 [api.md](api.md)。

---

## 1. 要解决的问题与设计约束

需求来自一个很具体的场景：用 LVGL 做界面，同一份业务代码要同时跑在
**PC 模拟器**和 **MCU（FreeRTOS）**上。界面需要读传感器、控电机，
但不能直接调用硬件驱动，否则 PC 上根本编译不过。

因此定下三条硬约束：

| 约束 | 含义 |
|---|---|
| **零平台耦合** | 业务代码只认主题字符串，不认任何寄存器、HAL 句柄或 OS 头文件 |
| **双端同源** | PC 与 MCU 共用同一份库代码，差异全部收敛到 `port/` 下的一个文件 |
| **MCU 友好** | 不能依赖大堆、不能有 realloc、栈占用要可控、无隐藏线程 |

选消息总线而不是「函数指针注册表」，是因为**发布者不需要知道订阅者存在**。
传感器只管往 `sensor/temp/value` 发，界面来不来、来几个、什么时候来，它都不关心。
这正好也是 MQTT 的模型，所以直接借用了 MQTT 的主题与过滤语义 —— 有现成的成熟规范，
比自创一套规则更不容易出错，用户也更容易理解。

---

## 2. 总体结构

```
┌──────────────────────────── 应用层 ────────────────────────────┐
│  lvgl_ui 节点          motor 节点           sensor 节点         │
│  只认主题字符串         只认 motor/cmd       只认 sensor/...      │
└───────┬────────────────────┬────────────────────┬──────────────┘
        │  subscribe          │  subscribe          │  publish
        │  publish            │                     │
┌───────▼────────────────────▼─────────────────────▼──────────────┐
│                        message_bus（本库）                       │
│                                                                  │
│   mb_topic.c      MQTT 主题校验与通配符匹配（纯函数，可独立使用）  │
│   mb_dispatch.c   ★ 唯一的投递出口：匹配 + 同步回调 + retained 补发│
│   mb_bus.c        总线生命周期、retained 表、系统主题             │
│   mb_node.c       节点生命周期、发布、订阅                        │
│   mb_subscription.c  订阅表与引用计数                             │
│   mb_message.c    消息视图 / 深拷贝（仅 retained 用）             │
│   mb_log.c        分级日志                                        │
└───────────────────────────┬──────────────────────────────────────┘
                            │ 只通过 mb_os.h 这一层
┌───────────────────────────▼──────────────────────────────────────┐
│  port/:  mb_os_win32.c │ mb_os_posix.c │ mb_os_freertos.c │ none  │
│          互斥量（递归锁） · 毫秒时间 · malloc/calloc/free          │
└──────────────────────────────────────────────────────────────────┘
```

分层的意义：**换平台只动最底下一层**。从 PC 模拟器切到 MCU，
业务代码、总线代码、示例代码一行都不用改，只是编译列表里换一个 `.c` 文件
（并定义 `MB_CONFIG_OS`）。

---

## 3. 数据结构

### 3.1 总线 `mb_bus_t`（`src/mb_internal.h`）

总线是**所有对象的生命周期拥有者**，也是唯一的锁边界。

```c
struct mb_bus {
    char       *name;
    mb_mutex_t *lock;        /* 递归互斥量，保护下面所有字段 */

    mb_node_t         **nodes;      /* 节点表（动态数组） */
    size_t              node_count, node_capacity;

    mb_subscription_t **subs;       /* 订阅表，**按 seq 升序** */
    size_t              sub_count, sub_capacity;
    uint64_t            next_sub_seq;

    mb_retained_t      *retained;   /* retained 链表，按 seq 升序 */
    uint64_t            next_retained_seq;

    uint32_t next_message_id;
    uint32_t node_seq;              /* 自动命名 node-N 用 */

    mb_bus_stats_t  stats;
    mb_bus_config_t config;
    bool            destroying;
};
```

两点值得注意：

- **没有全局变量**。所有状态都挂在 `mb_bus_t` 上，因此可以同时存在多条互不干扰的
  总线（测试里就是这么用的）。代价是每个 API 都要先拿到 `bus` 指针。
- **订阅表按 `seq` 升序**。`seq` 是单调递增的创建序号，这个顺序是投递算法的前提
  （见 4.1）。表是有序的，所以查找是 O(n) 线性扫描 —— 见第 6 节的性能讨论。

### 3.2 节点 `mb_node_t`

```c
struct mb_node {
    mb_bus_t *bus;
    char     *name;      /* 独立堆内存，总线内唯一 */
    uint32_t  refcnt;    /* 链表引用 + 每次在途回调各 1 */
    bool      active;
};
```

节点本身很轻：只有名字和引用计数。它**不直接持有订阅数组** ——
订阅统一放在总线的表里，通过 `sub->node` 反向指回来。
这样投递时只需扫一张表，不用遍历节点。

### 3.3 订阅 `mb_subscription_t`

```c
struct mb_subscription {
    uint64_t       seq;            /* 创建序号，决定投递顺序 */
    mb_bus_t      *bus;
    mb_node_t     *node;
    char          *filter;         /* 主题过滤器，如 "sensor/+/value" */
    char          *source_filter;  /* 可选的来源过滤（按节点名），可空 */
    mb_handler_t   handler;
    void          *user_data;
    uint32_t       refcnt;
    bool           active;         /* false = 已退订或所属节点已销毁 */
    uint8_t        flags;          /* SKIP_RETAINED | NOLOCAL */
};
```

`filter` 和 `source_filter` 都是**深拷贝**到订阅自己的堆内存里的。
这意味着调用方可以用栈上的临时字符串去订阅，`subscribe()` 返回后随便释放。

### 3.4 retained 表项 `mb_retained_t`

```c
typedef struct mb_retained {
    struct mb_retained  *next;
    mb_owned_message_t  *msg;     /* 深拷贝的完整消息 */
    uint64_t             seq;
    uint32_t             refcnt;  /* >1 表示正有回调在用 */
    bool                 active;
} mb_retained_t;
```

「更新」一条 retained 的语义是**摘掉旧表项 + 追加新表项**，而不是原地覆盖。
原因是：可能正有回调拿着旧表项的指针在执行（锁外），原地覆盖会把它手里的数据改掉。
摘链 + 延迟释放（靠 `refcnt`）是唯一安全的做法。

---

## 4. 关键流程

### 4.1 发布与投递（`mb_dispatch.c`）

这是整个库最核心、也最容易写错的地方。**投递算法**：

```
publish(topic, payload)
  │
  ├─ 校验主题（不含通配符、长度合法）
  ├─ 若带 RETAIN 标志 → 深拷贝存进 retained 表（空负载 = 清除保留）
  │
  └─ mb_dispatch_message()
       dispatch_depth++            ← 线程局部的递归深度计数
       cursor = 0
       loop:
         ┌─ 锁内 ────────────────────────────────────────────┐
         │ 扫 seq > cursor 的订阅；                          │
         │ 不匹配 → cursor 前移，跳过（省下下轮的重复扫描）  │
         │ 匹配   → 若本批已满：不推进 cursor，留到下一批    │
         │          否则：cursor 前移，refcnt+1（订阅和节点  │
         │                各一次），放进本批数组             │
         └───────────────────────────────────────────────────┘
         ┌─ 锁外 ────────────────────────────────────────────┐
         │ 逐个调用 handler(sub, msg, user_data)             │
         │ 此时回调可安全地 publish / subscribe / unsubscribe│
         └───────────────────────────────────────────────────┘
         ┌─ 锁内 ────────────────────────────────────────────┐
         │ 归还引用（先取 sub->node 再释放 sub！）           │
         │ stats.delivered += n                              │
         └───────────────────────────────────────────────────┘
         if 本批没取满: break
       dispatch_depth--
```

为什么非要分批？因为**回调必须在锁外执行**，而锁外执行意味着订阅表可能被别的线程改动。
分批把「持锁」和「调回调」切成互不重叠的片段，每批之间的表变化都能被正确观察到。

> ⚠️ **一个曾经踩过的坑（已修复，见 `tests/test_pubsub.c` 的
> `batch_boundary_delivers_all_subscribers`）**：游标只能在**真正取走**一条订阅时才推进。
> 如果先推进游标、再发现「本批已满」而把这条推迟到下一批，下一批就会因为
> `seq <= cursor` 把它跳过 —— 于是**每个批次边界之后的第一条匹配订阅都被静默丢弃**。
> 默认批宽 8，20 个订阅者只会收到 18 条消息，而且完全不报错。
> 这个 bug 只在匹配订阅数超过批宽时才显形，写测试时务必覆盖跨批次的场景。

### 4.2 递归发布保护

同步投递带来一个必然问题：**回调里再发布会递归**。

```
on_a() ──publish("x")──► on_b() ──publish("x")──► on_a() ──► ... 栈溢出
```

用 `MB_THREAD_LOCAL` 的深度计数器拦截（`MB_CONFIG_MAX_DISPATCH_DEPTH`，默认 8）。
超限时**一条回调都不调用**，直接返回 `MB_ERR_BUSY`、`stats.dropped++`、打一条 WARN 日志。

注意计数器是**线程局部**的：A 线程的发布深度不该影响 B 线程。
在没有 TLS 支持的平台上宏会退化成普通静态变量，那时只在单线程下语义正确。

### 4.3 订阅时补发 retained（`mb_dispatch_retained_to_subscription`）

```
subscribe("sensor/+/value")
  │
  ├─ 校验过滤器、分配订阅对象、加进订阅表
  │
  └─ 若没设 MB_SUB_FLAG_SKIP_RETAINED：
       遍历 retained 链表（同样分批、同样锁外回调）
       逐条调用 handler，msg->flags 里带上 MB_MSG_FLAG_RETAINED
```

**`subscribe()` 返回前，补发就已经完成了。** 这是 LVGL 场景的关键：
界面节点后启动时，`subscribe("sensor/+/value")` 一返回，界面就已经拿到了当前温度，
不需要「先订阅再主动问一次」。

### 4.4 节点上下线事件

`mb_node_create()` / `mb_node_destroy()` 会自动发布系统主题（可用
`mb_bus_config_t.publish_node_events = false` 关掉）：

| 时机 | 主题 | 保留 | 负载 |
|---|---|---|---|
| 上线 | `$mb/nodes/<name>/connected` | ✅ retained | 节点名 |
| 下线 | `$mb/nodes/<name>/connected` | ✅ retained，**空负载** | — |
| 下线 | `$mb/nodes/<name>/disconnected` | ❌ 纯事件 | 节点名 |

下线为什么要发**两条**？因为 `connected` 是**状态**主题：它靠 retained 记录
「谁现在在线」。节点消失时必须往**同一个主题**发一条空负载消息 ——
按 MQTT 语义这既是「清除保留状态」（后启动的订阅者不会收到已下线节点的残留记录），
又是对在线订阅者的实时通知。而 `disconnected` 是纯事件，不保留，
避免留下一条永远不会被清除的状态。

`mb_bus_destroy()` 销毁节点时**不再发布**下线事件 —— 总线正在关闭，
此时发布没有意义，而且会在锁内触发投递路径。

---

## 5. 线程与内存模型

### 5.1 锁规则（贯穿全部实现）

> **总线锁是递归锁；任何用户回调都必须在锁外调用。**

递归是必需的：节点上线事件会在持锁的路径上触发一次发布，发布又要加锁。
Win32 的 `CRITICAL_SECTION` 天然递归，POSIX 用 `PTHREAD_MUTEX_RECURSIVE`，
FreeRTOS 用 `xSemaphoreCreateRecursiveMutex()`。

实现上有一条铁律，写在内核头注释里：

```c
/* 只有名字里带 `_locked` 后缀的内部函数才可以在已持锁时调用 */
void mb_bus_add_subscription_locked(...);
void mb_node_release_locked(...);
```

这样「这个函数会不会递归加锁」在调用点就能一眼看出来，不需要追进去读实现。

### 5.2 引用计数：解决「回调执行中对象被销毁」

这是同步投递模型里最危险的一类 bug：

```
线程 A: 正在执行 sub 的 handler（锁外）
线程 B: unsubscribe(sub) → 摘链 → free(sub)     ← A 手里的 sub 成了悬垂指针
```

解决办法：投递前给**订阅对象**和**节点**各加一次引用（`refcnt++`），
回调返回后减掉。`unsubscribe` / `destroy` 只是「摘链 + 减掉链表那一份引用」，
**最后一个引用归零时才真正 free**。

有个细节容易写错，代码里专门加了注释：

```c
mb_subscription_t *sub = batch[i];
mb_node_t *node = sub->node;        /* 必须在 sub 可能被释放前取出！ */
mb_subscription_release_locked(bus, sub);   /* 这一步可能把 sub 释放掉 */
mb_node_release_locked(bus, node);
```

由此得到三条对使用者的**保证**：

1. 回调内部可以安全地 `publish` / `subscribe` / `unsubscribe`；
2. 回调内部可以安全地**取消自己**（测试 `unsubscribe_inside_callback` 覆盖）；
3. 回调内部可以安全地销毁自己所属的节点 —— 但**回调返回后该节点指针立即失效**。

### 5.3 内存所有权

| 对象 | 归属 | 说明 |
|---|---|---|
| 发布时的 topic/payload | **调用方** | 投递是零拷贝的，回调直接读调用方的缓冲区 |
| retained 消息 | **总线** | 深拷贝一份（必须的：它要活过这次调用） |
| 订阅的 filter / source_filter | **总线** | 深拷贝，订阅创建后调用方即可释放 |
| 节点名 | **总线** | 深拷贝 |
| 传给回调的 `mb_message_t` | **栈上临时视图** | 仅在回调执行期间有效 |

**零拷贝投递是刻意的**：MCU 上每条消息都 memcpy 一次代价不小。
但因此产生一条硬约束：

> ❌ 回调里**不得**保存 `msg->topic` / `msg->payload` / `msg->source` 指针到回调之外，
> 也不得修改它们指向的内容。需要留存请自行拷贝。

retained 补发是例外 —— 那份数据由总线持有，但**同样只在回调期间有效**
（引用计数只保证内存不释放，不保证内容不被后续更新替换）。

---

## 6. 这套设计的优点

1. **解耦彻底**。发布者不知道订阅者的存在，界面节点可以随时上线/下线，
   传感器代码完全不用改。这是本项目最初要解决的问题，解决得比较干净。

2. **双端同源**。PC 模拟器和 MCU 跑的是同一份 `src/`。模拟器上验证过的逻辑，
   烧进 MCU 行为一致。换芯片只改一个 `.c` 文件。

3. **回调安全且直观**。同步投递 + 锁外回调意味着：
   发布返回时消息已经处理完（无异步延迟、无队列积压）；
   回调里可以随便调用总线 API 而不会死锁。
   调试时栈回溯直接能看到 `publish → handler`，不需要跨线程推理。

4. **无隐藏资源**。库不创建线程、不开队列、不跑定时器。
   所有资源在 `mb_bus_create()` 时分配，`mb_bus_destroy()` 时全部归还。
   对 MCU 的 RAM 预算和启动时序都很友好。

5. **retained 让「状态」和「事件」有了区分**。传感器当前值 / 电机当前转速是
   **状态**（retained），按钮点击是**事件**（不保留）。
   界面后启动时状态自动补齐，事件不会被重放 —— 这正好避开了
   「界面晚启动 3 秒，结果开机时按的那次按钮被重放一遍」的经典 bug。

6. **投递收敛在一个函数里**。将来要改成异步（队列 + 分发线程），
   只改 `mb_dispatch.c`，公开 API 一个都不用动。这是第 7 节里最重要的一条。

---

## 7. 代价与局限（务必知悉）

### 7.1 同步投递 = 发布者被订阅者拖慢

这是最需要权衡的一点。`publish()` 的耗时 = **所有匹配回调的耗时之和**，
而且全部算在**发布者线程**的账上。

```
sensor 任务（1kHz 采样）──publish("sensor/temp")──► 5 个订阅者，每个 200µs
                                          ⇒ 这次发布耗时 1ms
                                          ⇒ 采样周期直接被打乱
```

**什么时候不该用同步投递**：订阅者里有耗时操作（写 Flash、刷屏、等网络）。
**怎么办**：让回调只做「把数据拷进环形缓冲」这一件事，
真正的耗时处理交给消费方的任务在自己的节奏里做。
`examples/03_lvgl_motor_sensor.c` 演示的就是这个模式。

### 7.2 锁竞争：单锁串行化一切

所有 API 都要抢同一把总线锁，而扫描订阅表是 **O(n)** 线性扫描。
`mb_topic_match()` 是逐层字符串比较，本身也不便宜。

粗略的量级：几十个订阅、每秒几千条消息，在 Cortex-M4 上完全没问题。
但如果你的场景是**几万个订阅 + 高频发布**，这套实现会成为瓶颈。
可能的优化方向（目前**没有**实现）：

- 按主题第一层建哈希索引，把 O(n) 降到 O(层级数)；
- 读多写少的场景改用读写锁（`subs` 表多数时候只读）；
- 分片（按主题哈希拆成多把锁）。

对 LVGL 这类「几十个控件订阅十几类主题」的场景，当前的简单实现是合适的 ——
过早引入哈希索引和分片只会让代码难以验证。

### 7.3 投递顺序 = 订阅创建顺序

订阅表按 `seq` 升序，所以回调的调用顺序是**订阅创建的先后**，
跟「谁更重要」「谁先声明」无关，也不是拓扑序。

⚠️ **不要依赖投递顺序来表达业务依赖**。如果 `on_logger` 必须在 `on_motor` 之前跑，
那是设计问题，不是总线能保证的事。需要顺序就在一个回调里按顺序做完。

### 7.4 QoS 只支持 0

`opts.qos != 0` 直接返回 `MB_ERR_UNSUPPORTED`。
进程内总线没有网络丢包，QoS 1/2 的重传与确认机制在这里只是纯粹的复杂度开销。
但这也意味着：**没有投递保证**。回调执行到一半系统掉电，那条消息就没有了。

### 7.5 进程内，不是网络 MQTT

这是**进程内**总线。没有 broker、没有 TCP、没有跨设备通信。
如果你需要「MCU 和 PC 之间通信」，本库只能帮你把两边各自的内部耦合解开，
两端的桥接需要你自己写一个节点，把消息转发到串口/网络。

### 7.6 其他已知限制

- `mb_bus_find_node()` 返回**借用指针**。多线程下若可能并发销毁该节点，
  调用方需自行加锁保护，或改用 `$mb/nodes/+/connected` 事件订阅。
- 发布主题名不得超过 `MB_CONFIG_MAX_TOPIC_LEN`（默认 128），
  `mb_node_publish_to()` 内部用栈缓冲拼主题，超长返回 `MB_ERR_TOO_LONG`。
- 不支持 MQTT 的共享订阅（`$share/...`）。
- `+` 和 `#` 不能用在发布主题里（返回 `MB_ERR_INVALID_ARG`），这点与 MQTT 一致。

---

## 8. ⚠️ 注意事项（踩坑清单）

### 8.1 LVGL 回调里不要直接碰控件 ⚠️ 最重要的一条

LVGL **不是线程安全的**。而消息总线的回调是在**发布者线程**里执行的。
如果传感器任务发布了 `sensor/temp/value`，界面订阅的回调就会在
**传感器任务的线程**里被调用 —— 此时直接调 `lv_label_set_text()` 是在跨线程操作控件，
轻则花屏，重则堆损坏崩溃。

```c
/* ❌ 错误：回调运行在发布者线程，这里碰控件迟早出事 */
static void on_temp(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    lv_label_set_text(ui->temp_label, msg->payload);   /* 危险！ */
}

/* ✅ 做法 1：回调只记值，LVGL 线程在自己的定时器里读 */
static volatile float g_temp;
static void on_temp(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    g_temp = atof(msg->payload);       /* 只写一个变量，不碰控件 */
}
/* 在 LVGL 线程里：lv_timer_create(refresh_ui, 100, NULL); */

/* ✅ 做法 2：把刷新动作抛回 LVGL 线程执行 */
static void set_temp_async(void *arg)                      /* 在 LVGL 线程里跑 */
{
    lv_label_set_text(ui->temp_label, (const char *)arg);
}
static void on_temp(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    lv_async_call(set_temp_async, (void *)msg->payload);   /* 注意：payload 是零拷贝的！ */
}
```

> 上面做法 2 有个陷阱：`payload` 是零拷贝的，`lv_async_call` 是**异步**的，
> 等它真正执行时这次 `publish` 早就返回了，指针已经失效。
> 必须先把数据拷进自己的缓冲再传过去。这正是 5.3 节那条零拷贝约束的实际后果。

`examples/03_lvgl_motor_sensor.c` 用的是做法 1。

### 8.2 不要在回调里做耗时操作

回调阻塞的是**发布者**。在回调里 `vTaskDelay()`、等信号量、写 Flash，
都会直接拖慢甚至卡死发布者的任务。回调应该尽量短。

### 8.3 不要在回调里销毁总线

`mb_bus_destroy()` 需要独占总线，在回调内调用会**死锁**（回调运行时仍持有引用，
销毁路径等不到）。正确做法是由另一个任务在确认没有回调在跑之后再销毁。
头文件里对此有 `@warning`。

### 8.4 `#` 匹配不到 `$` 开头的主题

MQTT 规则，也是刻意的：系统主题不该被 `#` 这种「全收」订阅无意中抓到。
想收节点上下线事件必须显式写 `$mb/nodes/+/connected`。

### 8.5 发布时别把 retained 用在「命令」上

retained 的语义是「这个主题的**当前状态**」。`motor/cmd` 这种命令主题如果用了
retained，后加入的订阅者会收到一条**旧命令并执行一次** —— 通常不是你想要的。

```c
/* ❌ 命令不该 retained：界面后启动会把上次的 "speed=1200" 重放一遍 */
mb_node_publish(ui, "motor/cmd", "speed=1200", 11, &retain_opts);

/* ✅ 状态用 retained，命令不用 */
mb_node_publish(motor, "motor/speed/status", "1200 rpm", 8, &retain_opts);
```

### 8.6 嵌入式上的配置建议

```c
/* 在工程的 mb_conf.h 里（模板见 config/mb_conf_template.h） */
#define MB_CONFIG_OS              MB_OS_FREERTOS
#define MB_CONFIG_MAX_TOPIC_LEN   64     /* 默认 128，省 RAM */
#define MB_CONFIG_MAX_NAME_LEN    24
#define MB_CONFIG_MAX_PAYLOAD_SIZE 256   /* 默认 0（不限）—— 建议设上限 */
#define MB_CONFIG_LOG_LEVEL       1      /* 量产关掉 INFO/DEBUG */
#define MB_CONFIG_ENABLE_CHECKS   0      /* 量产省空间 */
```

也可以把 `MB_CONFIG_MALLOC/CALLOC/FREE` 全部换成静态内存池，
实现「零动态分配」——`mb_os.h` 刻意不提供 `realloc` 就是为了让这件事可行。

### 8.7 该用哪种 OS port

| 场景 | 选择 |
|---|---|
| PC 模拟器（Windows） | `MB_OS_WIN32`（CMake 自动选） |
| PC 模拟器（Linux/macOS） | `MB_OS_POSIX`（CMake 自动选） |
| MCU + FreeRTOS | `MB_OS_FREERTOS` |
| 裸机 / 无 RTOS | `MB_OS_NONE` + 自己提供临界区宏 |
| 其它 RTOS（RT-Thread 等） | 复制 `port/mb_os_none.c` 或 `mb_os_freertos.c` 改造成 `mb_os_xxx.c` |

注意 `MB_OS_NONE` 下的 `mb_os_mutex_*` 不是真锁（用临界区宏模拟），
只在「中断里也调总线 API」时需要特别小心 —— 详见 [porting.md](porting.md)。

---

## 9. 可扩展点

设计时刻意留了几个「只改一处」的扩展点：

| 想做什么 | 改哪里 | 公开 API 要变吗 |
|---|---|---|
| 改成异步投递（队列 + 分发线程） | `mb_dispatch.c` 一个文件 | 不用 |
| 加主题哈希索引提升匹配性能 | `mb_bus.c` 的订阅表 + `mb_dispatch.c` 的扫描 | 不用 |
| 新增 RTOS 支持 | 新增 `port/mb_os_<os>.c`，实现 `mb_os.h` 的 7 个函数 | 不用 |
| 改内存策略（静态池 / TLSF） | `port/mb_os_*.c` 的 malloc/calloc/free | 不用 |
| 支持 QoS 1/2 | `mb_message.c` + `mb_dispatch.c`（需要引入队列与重传） | `qos` 字段已预留 |

`mb_bus_config_t` 结尾的 `void *reserved[4]` 就是为这类扩展预留的：
新增配置项**追加在 reserved 之前**，老代码用 `mb_bus_default_config()` 初始化即可保持兼容。

---

## 10. 与真正的 MQTT 的差异

借用语义，但不假装自己是 MQTT：

| 特性 | 真 MQTT | 本库 |
|---|---|---|
| 传输 | TCP/网络 | 进程内函数调用 |
| 通配符 `+` / `#` / `$` 规则 | ✅ | ✅ 完全一致 |
| retained | ✅ | ✅（含空负载清除语义） |
| QoS 0/1/2 | ✅ | 仅 0 |
| 遗嘱消息（LWT） | ✅ | 无（节点异常退出无法感知） |
| 会话保持 / 离线消息 | ✅ | 无 |
| 共享订阅 `$share` | ✅ | 无 |
| 跨设备 | ✅ | ❌ 进程内 |
| 投递时机 | 异步（网络） | **同步**（调用返回即完成） |

差异集中在「网络」相关的部分 —— 进程内没有网络，这些机制自然也就没有存在的基础。

---

## 延伸阅读

- [api.md](api.md) —— 按模块的 API 参考与示例片段
- [porting.md](porting.md) —— 移植到 FreeRTOS / 裸机 / 新平台，含任务骨架代码
- [topics.md](topics.md) —— 主题与通配符规范、MQTT 对照表
- `examples/03_lvgl_motor_sensor.c` —— 本项目的真实场景，含 LVGL 线程安全示范
- `tests/test_pubsub.c` —— 发布订阅语义的完整测试，也是最好的行为说明
