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
│   mb_dispatch.c   ★ 唯一的投递出口：匹配 + 回调 + retained 补发    │
│   mb_async.c      异步队列 + pump（可选，MB_CONFIG_ASYNC_MAX_TOPICS）│
│   mb_bus.c        总线生命周期、retained 表、系统主题             │
│   mb_node.c       节点生命周期、发布、订阅                        │
│   mb_subscription.c  订阅表与引用计数                             │
│   mb_message.c    消息视图 / 深拷贝（retained 与异步队列共用）     │
│   mb_log.c        分级日志                                        │
└───────────────────────────┬──────────────────────────────────────┘
                            │ 只通过 mb_os.h 这一层
┌───────────────────────────▼──────────────────────────────────────┐
│  port/:  mb_os_win32.c │ mb_os_posix.c │ mb_os_freertos.c │ none  │
│   互斥量（递归锁） · 计数信号量 · 毫秒时间 · malloc/calloc/free    │
└──────────────────────────────────────────────────────────────────┘
```

两条投递路径并存，但**汇合在同一个出口**：

```
mb_node_publish()        ──直接──┐
                                 ├──► mb_dispatch_message()  ⟶ 订阅者回调
mb_node_publish_async()  ──队列──┘         （投递语义完全相同）
                              ▲
                     mb_bus_pump()（专用线程）
```

所以「订阅」这件事不需要区分同步还是异步 —— 一份订阅代码，两种发布方式都能收到。

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
#if MB_CONFIG_ASYNC_MAX_TOPICS > 0
    mb_async_pool_t async;          /* 异步队列；用到才分配（见 4.5） */
    bool            pumping;        /* 保证只有一个线程在消费队列 */
#endif
};
```

三点值得注意：

- **没有全局变量**。所有状态都挂在 `mb_bus_t` 上，因此可以同时存在多条互不干扰的
  总线（测试里就是这么用的）。代价是每个 API 都要先拿到 `bus` 指针。
- **订阅表按 `seq` 升序**。`seq` 是单调递增的创建序号，这个顺序是投递算法的前提
  （见 4.1）。表是有序的，所以查找是 O(n) 线性扫描 —— 见第 6 节的性能讨论。
- **异步队列是懒分配的**。`mb_bus_create()` 不碰它，第一次 `publish_async()` 才建。
  不用异步投递的工程（也就是在此之前的全部用法）一个字节的 RAM 都不多花。
  这与 retained 表「用到才分配」的做法一致。

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

### 4.5 异步投递（`mb_async.c`）

同步投递把回调算在**发布者**账上。异步投递把它挪到**另一个线程**：

```
发布者线程                                  pump 线程
────────────                                ──────────
publish_async() ──深拷贝──► [ "a/b": 1 2 ]
                            [ "a/c": 7   ] ──► mb_bus_pump()
                                                    │ 逐条取出
                                                    ▼
                                              mb_dispatch_message()
                                              （订阅语义与同步完全一致）
```

**队列是两级的**，这是它最要紧的设计点：

```
bus->async.entries[]            每个条目一条定长环形 FIFO
   ├── "a/b" ─► [ 1 ][ 2 ][   ][   ]     head  ──► 下一个出队位置
   └── "a/c" ─► [ 7 ][   ][   ][   ]     count ──► 当前积压条数
```

> `"a/b"` 和 `"a/c"` 是两个**主题条目**；往 `"a/b"` 连发内容 1、2，
> 则 `a/b` **这一个**条目里积压两条。
> 一个主题积压再多也不会挤掉别的主题的位置 —— 这正是分两级、而不是用一条
> 扁平队列的唯一理由。

条目表容量 `MB_CONFIG_ASYNC_MAX_TOPICS`（默认 16），每条的 FIFO 深度
`MB_CONFIG_ASYNC_QUEUE_DEPTH`（默认 4）。两个数字都必须小 —— 它们直接决定
总线要预留多少堆。

#### 满队列的两种策略

| 策略 | 打开方式 | 行为 |
|---|---|---|
| **等**（默认） | `MB_PUB_FLAG_ASYNC_OVERWRITE` 不置位 | 阻塞最多 `timeout_ms`，`MB_WAIT_FOREVER` 永久等；超时返回 `MB_ERR_TIMEOUT` 并丢弃 |
| **覆盖** | 该标志置位，或 `MB_CONFIG_ASYNC_OVERWRITE_OLDEST = 1` | 扔掉该主题里**最旧**的一条给新消息腾位置，永不阻塞 |

状态类主题（传感器当前值）适合覆盖：消费端慢的时候只保留最新值。
命令类主题（`motor/cmd`）绝不能覆盖 —— 丢一条就是丢一个动作。
所以默认是「等」，覆盖必须显式打开，且可以按**单次发布**粒度选择。

> 从 `mb_bus_pump()` 的回调里发布是唯一的例外，见本章末尾的
> 「回调里发布要走覆盖策略，不能阻塞」。

#### 信号量账本

实现里最容易踩的坑在这里，改 `mb_async.c` 前务必看懂：

| 信号量 | 计数恒等于 |
|---|---|
| `entry->space` | 该条目还剩几个空位 = `QUEUE_DEPTH - count - 在途预定` |
| `pool->free_entries` | 还能新建几个主题条目 = `MAX_TOPICS - entry_count` |

**「在途预定」那一项很容易被漏掉，漏了就是 bug。** 它是已经领到空位令牌、
但还没在锁内提交 `count++` 的生产者。这两步之间必然隔着一次放锁 ——
普通路径要在这里按 `timeout_ms` 阻塞等待（不能在锁内等，否则把归还空位的
pump 一起锁死），所以这个中间状态真实存在。

也就是说：**`count < QUEUE_DEPTH` 并不等于「令牌一定拿得到」。**
覆盖路径曾经就栽在这个假设上（见下）。锁内那两行代码（evict 与 insert）
之间不会有在途预定被提交，所以覆盖分支的账目仍然是干净的。

每次 push 消耗一个空位、每次 pop 归还一个；每次建条目消耗一个名额、
每次释放条目归还一个。**任何一条路径漏掉或重复，队列就会凭空变满/变空，
而且是静默的**（不会崩，只会永远收不到消息）。所以每个信号量操作要么断言结果，
要么显式处理失败。

覆盖策略那两条路径的账目各自要记清：

- **队列真的满了**（`count == QUEUE_DEPTH`，此时令牌必为 0）：先退队再入队，
  净效果 `count` 不变，所以**既不消耗也不归还**令牌；
- **队列没满**：必须老老实实占一个令牌。**不能**假设「没满就一定有令牌」——
  有在途预定占着的时候就没有。拿不到且队列未满时，唯一正确的做法是
  放锁等一个令牌、回循环重判：那一刻队列已经被在途预定填满，自然落到
  上面的覆盖分支。硬塞进去会让空位账永久漂移，之后要么无故超时丢消息，
  要么环形队列写越界。

还有一个必须用 `waiters` 计数器解决的 ABA 问题：发布者等空位时是在**锁外**
阻塞的，如果此时 pump 把这条已经空掉的条目释放了，发布者醒来访问的就是
已释放内存。因此只要还有一个发布者在等，条目就不许被回收
（`entry_release_locked()` 里有断言把这件事钉死）。

#### 为什么 pump 不阻塞

`mb_bus_pump()` 取到队列空就返回，**它自己没有等待原语**。这带来两个好处：

1. 调用方在两次 pump 之间可以干别的（刷 LVGL、睡一会儿），不必把线程
   焊死在库的循环里；
2. 库不创建线程、不需要「停止」语义 —— `mb_bus_destroy()` 之前用户
   自己 join 掉 pump 线程即可，与库其余部分的契约完全一致（第 4 条优点）。

代价是调用方要自己 `mb_os_sleep_ms()`，否则空转烧 CPU。

#### 只允许一个 pump 线程

`mb_bus_pump()` 用 `bus->pumping` 标志（锁内检查并置位）拒绝并发调用，
返回 `MB_ERR_BUSY`。原因：两个线程同时 pump 会让**同一个订阅者的回调被并发进入**，
而本库只承诺「不会重入同一条投递路径」，不承诺「回调之间互斥」。

这个兜底是给调试用的，不是设计的一部分 —— 不要依赖它。

#### 回调里发布要走覆盖策略，不能阻塞

回调是跑在 pump 线程上的，而**归还空位、腾出主题条目名额的正是 pump**。
所以回调里做一次**会阻塞的**异步发布，就是在等一个只有自己才能推进的条件：

- `MB_WAIT_FOREVER` → **永久死锁**，pump 卡在回调里，队列再没人排空；
- 具体毫秒数 → 白等到超时，消息被当作丢弃计入 `stats.async_dropped`。

主题条目名额用尽时（`free_entries` 被等空）同理。**后者不会死锁**（拿不到就
如实超时），而且回调里往「另一个还有空位的主题」发布根本不会阻塞 —— 那是一种
完全正常的写法。所以这两条的处理方式不同：

| 回调里的调用 | 行为 |
| --- | --- |
| `MB_WAIT_NONE` | 正常。这是回调里推荐的写法 |
| 具体毫秒数 | 正常。不阻塞时照常成功；真要阻塞就照常超时（`MB_ERR_TIMEOUT`） |
| `MB_WAIT_FOREVER` + 当前线程是 pump 线程 | **拒绝**，返回 `MB_ERR_WOULD_DEADLOCK`（记一条 WARN 日志），**不计入任何统计** |

被拒的是 `MB_WAIT_FOREVER`，因为它表达的是「宁可一直等也不要丢」，
而从 pump 线程看这句话无法兑现：队列空时不需要等，队列满时等不到。
被拒的调用不产生任何副作用 —— 没入队、也没算「丢弃」（`async_dropped`
记的是「队列满、等超时」，这里压根没走到那一步）。

> 怎么检测的：`mb_os.h` 提供 `mb_os_thread_id()`（只需相等比较），
> `mb_bus_t` 记下 `pump_owner`，发布前比对一次。裸机没有调度器、信号量
> 本来就不会阻塞，所以用 `MB_OS_CAN_BLOCK` 把这个检查整个编译掉。
> 三方 port 需要补一个 `mb_os_thread_id()`，见 docs/porting.md。

回调里要发布，就传 `MB_WAIT_NONE`，或者走覆盖策略
（`MB_PUB_FLAG_ASYNC_OVERWRITE` / `MB_CONFIG_ASYNC_OVERWRITE_OLDEST = 1`）
**并同样传 `MB_WAIT_NONE`**：覆盖策略配上不等待时永不阻塞，正是为这种场景准备的。
（覆盖路径在「空位被另一个生产者预定、但队列还没满」的窗口里本来需要等一等，
传 `MB_WAIT_NONE` 就退化成直接返回 `MB_ERR_TIMEOUT`，不会卡住。）
这条约束也写在 `mb_async.h` 的 `mb_bus_pump()` 注释里。

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

4. **无隐藏资源**。库不创建线程、不跑定时器。异步队列是**用到才分配**的，
   并且只分堆、不建线程 —— `mb_bus_pump()` 由用户自己的线程驱动。
   所有资源在 `mb_bus_destroy()` 时全部归还。
   对 MCU 的 RAM 预算和启动时序都很友好。

5. **retained 让「状态」和「事件」有了区分**。传感器当前值 / 电机当前转速是
   **状态**（retained），按钮点击是**事件**（不保留）。
   界面后启动时状态自动补齐，事件不会被重放 —— 这正好避开了
   「界面晚启动 3 秒，结果开机时按的那次按钮被重放一遍」的经典 bug。

6. **投递收敛在一个函数里**。异步投递（4.5）就是靠这条落地的：队列取出的消息
   交给**同一个** `mb_dispatch_message()`，所以订阅语义、retained 补发、
   引用计数保护、递归深度保护全部自动一致，一行都不用重写。

   > 补一句诚实的复盘：当初在第 9 节里写「只改 `mb_dispatch.c`」，实际做完是
   > **新增 `mb_async.c` + 给 OS 层加计数信号量 + 给 `mb_bus_t` 加两个字段**。
   > 真正没错的是「投递语义不用动」这半句 —— 那才是省事的地方。
   > 公开 API 也没有改，只是**新增**了一族 `_async`。

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
**两个办法**：

1. 仍然用同步，但让回调只做「把数据拷进环形缓冲」这一件事，
   真正的耗时处理交给消费方的任务在自己的节奏里做。
   `examples/03_lvgl_motor_sensor.c` 演示的就是这个模式。
2. **改用异步投递**（4.5）：`mb_node_publish_async()` 入队即返回，
   回调改在 pump 线程上跑。发布方彻底不被拖累。

方案 2 的代价是消息要**深拷贝**一份（同步投递是零拷贝的），
而且投递时机变得不可控 —— 取决于 pump 线程什么时候被驱动。

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

LVGL **不是线程安全的**。而**同步**投递的回调是在**发布者线程**里执行的。
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

#### ✅ 做法 3（推荐）：改用异步投递，让回调本来就跑在 LVGL 线程里

上面两个做法都是在绕开「回调不在 LVGL 线程里」这个前提。
**异步投递直接把这个前提消掉** —— 让 LVGL 的刷新定时器顺手驱动 pump：

```c
/* 发布方：传感器任务，任意线程。入队即返回，不再被刷屏拖慢 */
mb_node_publish_async(sensor, "sensor/temp/value", buf, len, NULL, MB_WAIT_NONE);

/* 消费方：pump 在 LVGL 线程里跑（lv_timer 回调里调一次即可） */
static void pump_timer(lv_timer_t *t)
{
    mb_bus_pump(bus);      /* 有消息就投递，没有就立刻返回，不阻塞 UI 线程 */
}

/* 于是订阅回调天然运行在 LVGL 线程上，可以直接碰控件 —— 不需要任何中转 */
static void on_temp(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    lv_label_set_text(ui->temp_label, msg->payload);   /* ✅ 安全，这就是 4.5 的意义 */
}
```

条件是**驱动 pump 的那个线程必须是唯一操作控件的线程**。`mb_bus_pump()`
本身用 `bus->pumping` 兜底拒绝并发调用，但那是给调试用的 —— 设计上就该
保证只有一个线程在 pump（见 4.5）。

另外注意 `MB_WAIT_NONE` 与 `MB_WAIT_FOREVER` 的选择：在 LVGL 定时器里
**绝不能**用 `MB_WAIT_FOREVER` 发布 —— 队列满时会阻塞 UI 线程；
而如果 pump 也在同一个线程里，那就直接**死锁**了。用
`MB_WAIT_NONE` + 覆盖策略，或者干脆让发布方是另一个线程。

### 8.2 不要在回调里做耗时操作

回调阻塞的是**驱动它的那个线程**：

| 投递方式 | 回调在哪个线程上跑 |
|---|---|
| `mb_node_publish()` | 发布者 |
| `mb_node_publish_async()` | 调 `mb_bus_pump()` 的那个线程 |

在回调里 `vTaskDelay()`、等信号量、写 Flash，都会直接拖慢甚至卡死这个线程。
异步投递只是**换了一个被拖慢的线程**，并没有让耗时操作变便宜 —— 但它至少
把「传感器任务」和「刷屏」解耦了，这通常正是要的效果。回调仍应尽量短。

### 8.3 不要在回调里销毁总线

`mb_bus_destroy()` 需要独占总线，在回调内调用会**死锁**（回调运行时仍持有引用，
销毁路径等不到）。正确做法是由另一个任务在确认没有回调在跑之后再销毁。
头文件里对此有 `@warning`。

对异步投递来说这条要再强调一次：销毁总线前必须**先让 pump 线程停下来并 join**，
否则 pump 线程可能在 `mb_bus_destroy()` 已经释放总线之后（或过程中）去访问它。
库**不会**在销毁时去唤醒阻塞在队列上的发布者 —— 那样会把一个安静睡着的线程
变成正在解引用的线程，只会让问题更难查。所以契约仍然是：
**先 join 所有线程，再销毁总线**。

关闭顺序有一个容易踩反的地方：**生产者用 `MB_WAIT_FOREVER` 时，不能先停 pump。**
唤醒阻塞生产者的正是 pump（它 pop 一条就归还一个空位），先把 pump 停掉，
阻塞在满队列上的生产者就再也没人叫醒，`thread_join()` 会**永久挂住**。
顺序必须是「先停生产者 → 反复 pump 直到队列排空 → 再停 pump」。

```c
/* ✅ 正确的关闭顺序 */
g_producers_should_stop = true;
thread_join(&producer_thread);      /* 生产者先停 */

/* 它可能还有消息堵在队列里/或者它自己正阻塞在等空位，所以要继续 pump
 * 把队列排空 —— pump 一停，阻塞的生产者就永远等不到空位了 */
do {
    mb_bus_pump(bus);
} while (mb_bus_async_pending(bus) > 0);

thread_join(&pump_thread);          /* pump 最后停 */
mb_bus_destroy(bus);
```

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

/* 不用异步投递就关掉，一个字节的 RAM 都不多花 */
#define MB_CONFIG_ASYNC_MAX_TOPICS 0
/* 要用的话把这两个数字压到刚好够用 —— 它们直接决定预留多少堆 */
/* #define MB_CONFIG_ASYNC_MAX_TOPICS  8 */
/* #define MB_CONFIG_ASYNC_QUEUE_DEPTH 2 */
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
只在「中断里也调总线 API」时需要特别小心。**异步投递在裸机上无法真正阻塞**，
满队列一律退化成 `MB_ERR_TIMEOUT` —— 详见 [porting.md](porting.md) 第 2.1 节。

---

## 9. 可扩展点

设计时刻意留了几个「只改一处」的扩展点：

| 想做什么 | 改哪里 | 公开 API 要变吗 |
|---|---|---|
| ~~改成异步投递~~（**已完成**，见 4.5） | 新增 `mb_async.c`，投递仍走 `mb_dispatch.c` | 不用（只**新增**了 `_async` 一族） |
| 加主题哈希索引提升匹配性能 | `mb_bus.c` 的订阅表 + `mb_dispatch.c` 的扫描 | 不用 |
| 新增 RTOS 支持 | 新增 `port/mb_os_<os>.c`，实现 `mb_os.h` 的 11 个函数 | 不用 |
| 改内存策略（静态池 / TLSF） | `port/mb_os_*.c` 的 malloc/calloc/free | 不用 |
| 支持 QoS 1/2 | `mb_message.c` + `mb_dispatch.c`（需要引入队列与重传） | `qos` 字段已预留 |

> 异步投递那一行是事后补的：第 6 节第 6 条记了实际改动的范围与当初估计的偏差。

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
| 投递时机 | 异步（网络） | **两种都有**：`publish()` 同步（调用返回即完成），`publish_async()` + `mb_bus_pump()` 异步 |

差异集中在「网络」相关的部分 —— 进程内没有网络，这些机制自然也就没有存在的基础。
反过来，「异步」在进程内本来不是必需的，本库加它纯粹是为了**换线程**（4.5），
不是为了补偿网络延迟。

---

## 延伸阅读

- [api.md](api.md) —— 按模块的 API 参考与示例片段
- [porting.md](porting.md) —— 移植到 FreeRTOS / 裸机 / 新平台，含任务骨架代码
- [topics.md](topics.md) —— 主题与通配符规范、MQTT 对照表
- `include/message_bus/mb_async.h` —— 异步投递的完整说明与使用骨架
- `examples/03_lvgl_motor_sensor.c` —— 本项目的真实场景，含 LVGL 线程安全示范
- `tests/test_pubsub.c` —— 发布订阅语义的完整测试，也是最好的行为说明
- `tests/test_async.c` —— 异步队列的行为说明（含满队列、覆盖、并发 pump）
