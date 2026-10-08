# API 参考

按模块组织。设计原理与权衡见 [architecture.md](architecture.md)，
主题与通配符规则见 [topics.md](topics.md)。

所有公开 API 都在 `#include "message_bus/message_bus.h"` 之下（它就是总入口，
会引入其余全部头文件）。也支持 C++（各头文件均带 `extern "C"` 守卫）。

**约定**：返回 `mb_err_t` 的函数以 `MB_OK`（0）表示成功，负数为错误码。
所有接受指针的 API 都会做 NULL 检查（返回 `MB_ERR_INVALID_ARG`），
但 `*_name()`、`*_count()` 这类查询函数对 NULL 返回 `NULL`/0 而不是报错。

---

## 目录

- [1. 错误码](#1-错误码)
- [2. 总线 `mb_bus.h`](#2-总线-mb_bush)
- [3. 节点 `mb_node.h`](#3-节点-mb_nodeh)
- [4. 订阅](#4-订阅)
- [5. 主题工具 `mb_topic.h`](#5-主题工具-mb_topich)
- [6. 日志 `mb_log.h`](#6-日志-mb_logh)
- [7. 消息与标志位 `mb_types.h`](#7-消息与标志位-mb_typesh)
- [8. 异步投递 `mb_async.h`](#8-异步投递-mb_asynch)
- [9. 版本 `mb_version.h`](#9-版本-mb_versionh)
- [10. 最小可运行示例](#10-最小可运行示例)

---

## 1. 错误码

```c
typedef enum mb_err {
    MB_OK               =  0,  /* 成功 */
    MB_ERR_INVALID_ARG  = -1,  /* 参数非法：空指针、空串、主题含通配符 */
    MB_ERR_NO_MEMORY    = -2,  /* 内存分配失败 */
    MB_ERR_NOT_FOUND    = -3,  /* 对象不存在 */
    MB_ERR_ALREADY_EXISTS = -4,/* 对象已存在（如重名节点） */
    MB_ERR_FULL         = -5,  /* 容量已满 */
    MB_ERR_BUSY         = -6,  /* 资源忙，或超过递归投递深度 */
    MB_ERR_STATE        = -7,  /* 对象状态不允许该操作（如总线正在销毁） */
    MB_ERR_TOO_LONG     = -8,  /* 超过配置的长度上限 */
    MB_ERR_UNSUPPORTED  = -9,  /* 当前平台/配置不支持（如 qos != 0） */
    MB_ERR_TIMEOUT      = -10, /* 异步发布时队列满，等到超时仍没有空位 */
    MB_ERR_WOULD_DEADLOCK = -11,/* 这次调用会永久卡死，已被拒绝（pump 回调里做
                                 * MB_WAIT_FOREVER 的异步发布） */
} mb_err_t;

const char *mb_err_to_string(mb_err_t err);   /* → "MB_ERR_NOT_FOUND" */
```

---

## 2. 总线 `mb_bus.h`

### 生命周期

```c
mb_err_t mb_bus_create(const char *name, mb_bus_t **out_bus);
mb_err_t mb_bus_create_ex(const char *name, const mb_bus_config_t *config, mb_bus_t **out_bus);
mb_err_t mb_bus_default_config(mb_bus_config_t *config);
void     mb_bus_destroy(mb_bus_t *bus);
```

| 函数 | 说明 |
|---|---|
| `mb_bus_create` | 默认配置（`publish_node_events = true`）。等价于 `mb_bus_create_ex(name, NULL, &bus)` |
| `mb_bus_create_ex` | 显式配置；`config` 传 `NULL` 即默认配置 |
| `mb_bus_default_config` | 先填默认值再改字段。**新增字段时用它保证向前兼容** |
| `mb_bus_destroy` | 级联销毁：所有节点（连同其订阅）→ retained 表 → 总线本身。传 `NULL` 安全 |

```c
mb_bus_t *bus;
if (mb_bus_create("main", &bus) != MB_OK) {
    /* 内存不足 */
}
...
mb_bus_destroy(bus);
```

> ⚠️ `mb_bus_destroy()` 要求**没有其它线程正在使用该总线，也没有回调正在执行**。
> 在回调内部调用它会死锁。

### 查询

```c
const char *mb_bus_name(const mb_bus_t *bus);
mb_err_t    mb_bus_get_stats(const mb_bus_t *bus, mb_bus_stats_t *out_stats);
void        mb_bus_reset_stats(mb_bus_t *bus);
size_t      mb_bus_node_count(const mb_bus_t *bus);
size_t      mb_bus_subscription_count(const mb_bus_t *bus);
mb_err_t    mb_bus_find_node(const mb_bus_t *bus, const char *name, mb_node_t **out_node);
```

```c
mb_bus_stats_t stats;
mb_bus_get_stats(bus, &stats);
printf("发布 %llu 条，投递 %llu 次，无人订阅 %llu，丢弃 %llu\n",
       (unsigned long long)stats.published, (unsigned long long)stats.delivered,
       (unsigned long long)stats.no_subscriber, (unsigned long long)stats.dropped);
```

统计字段含义：

| 字段 | 含义 | 什么时候该关注 |
|---|---|---|
| `published` | 累计发布条数（含 retained） | — |
| `delivered` | 累计投递到回调次数 | 一条消息投给 N 个订阅算 N 次 |
| `dropped` | 因超递归深度被丢弃 | **不为 0 说明回调里有发布环** |
| `no_subscriber` | 发布时无人订阅的条数 | 偏高通常是主题名拼错了 |
| `retained_stored` | 当前 retained 条目数 | 用来估内存占用 |
| `nodes_created` | 累计创建节点数（含已销毁） | — |
| `peak_subscriptions` | 订阅数历史峰值 | 用来给静态内存池定容 |
| `async_enqueued` | 累计成功进入异步队列的条数 | 与 `published` 对照，看有多少走了异步 |
| `async_dropped` | 因队列满、等待超时没能入队的条数 | **不为 0 就是真丢了消息**，要么加大队列/超时，要么改用覆盖策略 |
| `async_overwritten` | 因「覆盖最旧」被顶掉的旧消息条数 | 覆盖是刻意丢的；偏高说明消费端跟不上生产端 |

> ⚠️ `mb_bus_find_node()` 返回的是**借用指针**：只要没有其它线程销毁该节点就一直有效。
> 多线程下若可能并发销毁，请自行用外部机制保护，或改用
> `$mb/nodes/+/connected` 事件订阅。

### 总线级发布

```c
mb_err_t mb_bus_publish(mb_bus_t *bus, const char *topic,
                        const void *payload, size_t payload_len,
                        const mb_publish_opts_t *opts);
```

由**总线自身**发布（`msg->source == NULL`）。用于系统主题或不属于任何节点的广播。

```c
/* opts 传 NULL 等价于 flags = 0, qos = 0 */
mb_bus_publish(bus, "$mb/app/notice", "reboot", 6, NULL);
```

---

## 3. 节点 `mb_node.h`

### 生命周期

```c
mb_err_t    mb_node_create(mb_bus_t *bus, const char *name, mb_node_t **out_node);
void        mb_node_destroy(mb_node_t *node);
const char *mb_node_name(const mb_node_t *node);
mb_bus_t   *mb_node_bus(const mb_node_t *node);
size_t      mb_node_subscription_count(const mb_node_t *node);
```

`name` 传 `NULL` 会自动命名为 `"node-<序号>"`（序号是总线内单调递增的计数器）：

```c
mb_node_t *motor;
mb_node_create(bus, "motor", &motor);      /* 显式命名 */
mb_node_t *anon;
mb_node_create(bus, NULL, &anon);          /* → "node-1" */
```

节点名不得包含 `/`、`+`、`#`，也不得以 `$` 开头（会与系统主题冲突）。
重名返回 `MB_ERR_ALREADY_EXISTS`。

`mb_node_destroy()` 会**自动摘除该节点的全部订阅**，不需要手动清理。
销毁后其订阅的回调不会再被调用。

### 发布

```c
mb_err_t mb_node_publish(mb_node_t *node, const char *topic,
                         const void *payload, size_t payload_len,
                         const mb_publish_opts_t *opts);

mb_err_t mb_node_publish_to(mb_node_t *node, const char *dst_node,
                            const char *subtopic,
                            const void *payload, size_t payload_len,
                            const mb_publish_opts_t *opts);
```

`mb_node_publish_to()` 是语法糖，把主题拼成 `dst_node + "/" + subtopic`：

```c
/* 下面两行完全等价 */
mb_node_publish_to(ui, "motor", "cmd/speed", "1200", 4, NULL);
mb_node_publish(ui, "motor/cmd/speed", "1200", 4, NULL);
```

目标节点**不要求存在** —— 没有这个节点时消息只是无人接收，不算错误。

**投递是同步的**：函数返回时所有匹配的回调都已执行完毕。
**投递是零拷贝的**：回调里的指针直接指向你传入的缓冲区，因此回调不得保存这些指针。

```c
/* 状态消息：用 retained，后订阅者能立刻拿到当前值 */
mb_publish_opts_t retain = { .flags = MB_PUB_FLAG_RETAIN, .qos = 0 };
mb_node_publish(motor, "motor/speed/status", "1200 rpm", 8, &retain);

/* 事件消息：不保留，避免后订阅者"重放"历史事件 */
mb_publish_opts_t plain = { .flags = 0, .qos = 0 };
mb_node_publish(ui, "motor/cmd", "stop", 4, &plain);
```

---

## 4. 订阅

### 创建

```c
/* 最常用形式 */
mb_err_t mb_node_subscribe(mb_node_t *node, const char *filter,
                           mb_handler_t handler, void *user_data,
                           mb_subscription_t **out_sub);

/* 完整形式：支持 source_filter 与标志位 */
mb_err_t mb_node_subscribe_ex(mb_node_t *node, const mb_subscribe_opts_t *opts,
                              mb_handler_t handler, mb_subscription_t **out_sub);

/* 订阅"发给自己这个节点"的消息，等价于 filter = "<本节点名>/#" */
mb_err_t mb_node_subscribe_self(mb_node_t *node, mb_handler_t handler,
                                void *user_data, mb_subscription_t **out_sub);
```

`out_sub` 传 `NULL` 表示不需要订阅句柄：

```c
mb_node_subscribe(ui, "sensor/+/value", on_value, NULL, NULL);
```

回调原型：

```c
typedef void (*mb_handler_t)(mb_subscription_t *sub,
                             const mb_message_t *msg,
                             void *user_data);
```

> **回调在发布者的线程内同步执行，且不持有总线锁。**
> 因此回调里可以安全地 `publish` / `subscribe` / `unsubscribe`，包括取消自己。
> 但不要做耗时操作（会阻塞发布者），LVGL 场景下不要直接碰控件
> —— 详见 [architecture.md 第 8.1 节](architecture.md#81-lvgl-回调里不要直接碰控件--最重要的一条)。

### 订阅选项

```c
typedef struct mb_subscribe_opts {
    const char *filter;        /* 必填：MQTT 主题过滤器，可含 '+' / '#' */
    const char *source_filter; /* 可选：对发布者节点名做二次过滤，支持通配符 */
    uint8_t     flags;         /* MB_SUB_FLAG_* */
    void       *user_data;     /* 透传给回调 */
} mb_subscribe_opts_t;
```

| 标志 | 效果 |
|---|---|
| `MB_SUB_FLAG_SKIP_RETAINED` | 不补发已存在的 retained 消息 |
| `MB_SUB_FLAG_NOLOCAL` | 不接收本节点自己发布的消息（对应 MQTT 5 NoLocal） |

```c
mb_subscribe_opts_t opts;
opts.filter        = "#";
opts.source_filter = "sensor/+";       /* 只收传感器节点发的 */
opts.flags         = MB_SUB_FLAG_NOLOCAL;
opts.user_data     = &my_ctx;
mb_node_subscribe_ex(logger, &opts, on_message, NULL);
```

### 取消与查询

```c
mb_err_t mb_node_unsubscribe(mb_subscription_t **sub);   /* 成功后 *sub 置 NULL */

const char *mb_subscription_filter(const mb_subscription_t *sub);
const char *mb_subscription_source_filter(const mb_subscription_t *sub);
mb_node_t  *mb_subscription_node(const mb_subscription_t *sub);
void       *mb_subscription_user_data(const mb_subscription_t *sub);
void       *mb_subscription_set_user_data(mb_subscription_t *sub, void *user_data);
bool        mb_subscription_is_valid(const mb_subscription_t *sub);
```

`mb_node_unsubscribe()` 接收的是**句柄的地址**，成功后把句柄置 NULL，
因此可以安全地重复调用（第二次返回 `MB_ERR_INVALID_ARG`）。这个设计是为了防止悬垂指针：

```c
mb_subscription_t *sub = NULL;
mb_node_subscribe(ui, "a/b", on_ab, NULL, &sub);
/* ... */
mb_node_unsubscribe(&sub);      /* sub 现在 == NULL */
mb_node_unsubscribe(&sub);      /* 安全：返回 MB_ERR_INVALID_ARG */
```

**在回调里取消自己也是安全的** —— 引用计数会保证对象活到回调返回之后：

```c
static void on_once(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    mb_subscription_t *self = sub;

    handle(msg);
    mb_node_unsubscribe(&self);   /* ✅ 安全，obj 延迟释放 */
}
```

---

## 5. 主题工具 `mb_topic.h`

这一组是纯函数，**不依赖总线**，可以单独当字符串工具用。

```c
bool     mb_topic_match(const char *filter, const char *topic);
mb_err_t mb_topic_validate_filter(const char *filter);
mb_err_t mb_topic_validate_topic(const char *topic);
bool     mb_topic_is_system(const char *topic);
size_t   mb_topic_level_count(const char *topic);
mb_err_t mb_topic_build(char *buf, size_t buf_size, const char *prefix, const char *suffix);
```

```c
mb_topic_match("sensor/+/temp", "sensor/room1/temp");    /* true  */
mb_topic_match("sensor/#",      "sensor");               /* true（零层也匹配） */
mb_topic_match("#",             "$mb/nodes/x/connected");/* false（'$' 规则） */
mb_topic_match("sensor/#",      "sensorx");              /* false */

mb_topic_validate_filter("sensor/+");      /* MB_OK */
mb_topic_validate_filter("sensor_+");      /* MB_ERR_INVALID_ARG（'+' 未独占一层） */
mb_topic_validate_filter("sensor/#/x");    /* MB_ERR_INVALID_ARG（'#' 未在末层） */
mb_topic_validate_topic("a/+");            /* MB_ERR_INVALID_ARG（发布不得含通配符） */

mb_topic_level_count("a/b/c");             /* 3 */
mb_topic_is_system("$mb/x");               /* true */

char buf[64];
mb_topic_build(buf, sizeof(buf), "motor", "cmd/speed");  /* → "motor/cmd/speed" */
```

规则细节与完整匹配表见 [topics.md](topics.md)。

---

## 6. 日志 `mb_log.h`

两级过滤：编译期（`MB_CONFIG_LOG_LEVEL`）决定宏是否产生代码，
运行期（`mb_log_set_level`）可在现场调整，但会被截断到编译期上限。

```c
void           mb_log_set_handler(mb_log_handler_t handler, void *user_data);
void           mb_log_set_level(mb_log_level_t level);
mb_log_level_t mb_log_get_level(void);
void           mb_log_emit(mb_log_level_t level, const char *tag, const char *fmt, ...);

/* 宏：低于编译期等级的调用不产生任何代码 */
MB_LOG_ERROR(tag, fmt, ...);
MB_LOG_WARN(tag, fmt, ...);
MB_LOG_INFO(tag, fmt, ...);
MB_LOG_DEBUG(tag, fmt, ...);
```

默认输出到 `stderr`。嵌入式上换成串口 / RTT：

```c
static void my_log(mb_log_level_t level, const char *tag,
                   const char *message, void *user_data)
{
    (void)user_data;
    printf("[mb %c/%s] %s\r\n", "NEWI"[level], tag, message);
}

/* 在 main 里、创建总线之前 */
mb_log_set_handler(my_log, NULL);
mb_log_set_level(MB_LOG_LEVEL_WARN);      /* 现场压掉 INFO/DEBUG */
```

完全关掉：把 `MB_CONFIG_LOG_LEVEL` 设为 0（编译期消除，不占 Flash）。

---

## 7. 消息与标志位 `mb_types.h`

### 消息视图

```c
typedef struct mb_message {
    const char *topic;       /* 主题 */
    const void *payload;     /* 负载；payload_len == 0 时为 NULL */
    size_t      payload_len;
    const char *source;      /* 发布者节点名；总线自身发布时为 NULL */
    uint32_t    id;          /* 总线内单调递增的消息序号（从 1 开始） */
    uint32_t    timestamp_ms;/* 发布时刻的 mb_os_time_ms() */
    uint8_t     flags;       /* MB_MSG_FLAG_* */
} mb_message_t;
```

| 标志 | 含义 |
|---|---|
| `MB_MSG_FLAG_RETAINED` | 这条是订阅时补发的 retained 历史消息 |
| `MB_MSG_FLAG_SYSTEM` | 系统主题（topic 以 `$` 开头） |

> ⚠️ **消息视图只在回调执行期间有效**，且底层数据是发布者的缓冲区（零拷贝）。
> 回调**不得**保存 `topic`/`payload`/`source` 指针，也不得修改其内容。
> 需要留存请自行拷贝。

### 发布选项

```c
typedef struct mb_publish_opts {
    uint8_t flags;   /* MB_PUB_FLAG_* */
    uint8_t qos;     /* 预留；当前仅支持 0，非 0 返回 MB_ERR_UNSUPPORTED */
} mb_publish_opts_t;
```

| 标志 | 效果 |
|---|---|
| `MB_PUB_FLAG_RETAIN` | 总线保存该主题最后一条；新订阅者会立刻收到。**空负载 = 清除保留** |
| `MB_PUB_FLAG_SYNC` | 强制同步投递（同步路径本来就是同步，此标志为语义显式化保留） |
| `MB_PUB_FLAG_ASYNC_OVERWRITE` | 仅供异步发布：该主题队列满时**丢弃最旧的一条**而不是等待（见第 8 节） |

### 总线配置

```c
typedef struct mb_bus_config {
    bool   publish_node_events;   /* 默认 true：节点上下线时自动发布系统消息 */
    void  *reserved[4];           /* 预留，必须为 NULL/0 */
} mb_bus_config_t;
```

`publish_node_events` 打开时（默认）：

| 主题 | 保留 | 负载 |
|---|---|---|
| `$mb/nodes/<name>/connected` | ✅ | 节点名（上线时） |
| `$mb/nodes/<name>/connected` | ✅ | **空**（下线时，用于清除保留状态） |
| `$mb/nodes/<name>/disconnected` | ❌ | 节点名（下线时） |

### 统计

见 [第 2 节](#查询) 的字段说明表。

---

## 8. 异步投递 `mb_async.h`

同步投递（`mb_node_publish()`）的回调在**发布者线程**上跑。异步投递把回调挪到
**pump 线程**上，代价是消息要被深拷贝一份。设计原理与队列结构见
[architecture.md 第 4.5 节](architecture.md#45-异步投递mb_asyncch)。

> 编译期可以用 `MB_CONFIG_ASYNC_MAX_TOPICS 0` 完全去掉异步功能 ——
> 此时下面所有函数都返回 `MB_ERR_UNSUPPORTED`（查询函数返回 0），
> 并且不占用任何 RAM。默认是打开的（16 个主题 × 4 条）。

### 发布

```c
mb_err_t mb_node_publish_async(mb_node_t *node, const char *topic,
                               const void *payload, size_t payload_len,
                               const mb_publish_opts_t *opts, uint32_t timeout_ms);
mb_err_t mb_node_publish_to_async(mb_node_t *node, const char *dst_node,
                                  const char *subtopic, const void *payload,
                                  size_t payload_len, const mb_publish_opts_t *opts,
                                  uint32_t timeout_ms);
mb_err_t mb_bus_publish_async(mb_bus_t *bus, const char *topic,
                              const void *payload, size_t payload_len,
                              const mb_publish_opts_t *opts, uint32_t timeout_ms);
```

与同名同步 API 的**唯一**语义差异：

1. 回调**不在本线程**执行，而是在调 `mb_bus_pump()` 的线程里执行；
2. `topic` / `payload` / `source` 会被**深拷贝** —— 本函数返回后调用方
   可以立刻释放/复用自己的缓冲区（同步投递是零拷贝的，没这个自由度）；
3. 队列满时最多等 `timeout_ms`。

其余完全一致：主题校验、retained 语义、错误码、无人订阅的统计。
`msg->id` 与 `msg->timestamp_ms` 在**入队时**确定，不是投递时。

`timeout_ms`：

| 值 | 含义 |
|---|---|
| `MB_WAIT_NONE`（0） | 不等待，队列满立刻返回 `MB_ERR_TIMEOUT` |
| `MB_WAIT_FOREVER`（`0xFFFFFFFFu`） | 永久等待，直到该主题腾出空位 |
| 其它 | 最多等这么多毫秒 |

| 返回值 | 含义 |
|---|---|
| `MB_OK` | 已进入队列 —— **不代表已被投递** |
| `MB_ERR_TIMEOUT` | 队列满且等不到空位，本条已丢弃（计入 `async_dropped`） |
| `MB_ERR_INVALID_ARG` / `MB_ERR_TOO_LONG` / `MB_ERR_UNSUPPORTED` | 与同步版本完全相同 |
| `MB_ERR_NO_MEMORY` | 深拷贝失败 |
| `MB_ERR_STATE` | 总线正在销毁 |
| `MB_ERR_WOULD_DEADLOCK` | 在 [`mb_bus_pump()`](#队列处理) 的回调里调用，且 `timeout_ms` 是 `MB_WAIT_FOREVER`。**没有任何副作用**：没入队，也不算丢弃 |

```c
/* 传感器任务：入队即返回，不再被刷屏拖慢 */
if (mb_node_publish_async(sensor, "sensor/temp/value", buf, len, NULL, 10) != MB_OK) {
    /* 注意：超时 = 这条消息真的丢了，不是"稍后重试" */
}
```

**覆盖最旧**：置 `MB_PUB_FLAG_ASYNC_OVERWRITE`（或把编译期默认
`MB_CONFIG_ASYNC_OVERWRITE_OLDEST` 设为 1），队列满时丢掉该主题里最旧的一条
给新消息腾位置，永不阻塞。状态类主题（传感器当前值）适合覆盖；命令类主题
绝不能覆盖。

```c
mb_publish_opts_t opts = { 0 };
opts.flags = MB_PUB_FLAG_ASYNC_OVERWRITE;
/* 只保留最新值：消费端慢的时候旧读数被顶掉，正是想要的 */
mb_node_publish_async(sensor, "sensor/temp/value", buf, len, &opts, MB_WAIT_NONE);
```

> 覆盖只影响「**同一主题内**积压太多」，与主题条目总数无关：
> `MB_CONFIG_ASYNC_MAX_TOPICS` 个条目名额用尽时，新主题仍然只能等（或超时）——
> 否则会把别的主题挤掉，那就不叫「按主题分组」了。

> ⚠️ **从 `mb_bus_pump()` 的回调里做 `MB_WAIT_FOREVER` 的异步发布会被拒绝**，
> 返回 `MB_ERR_WOULD_DEADLOCK`。回调跑在 pump 线程上，而腾空位的正是 pump ——
> 它此刻卡在你的回调里，等不到自己，硬等就是**永久死锁**。库会比对线程标识
> 识别出这种情况并直接拒绝，而不是让程序挂死。
>
> 传具体毫秒数**不受影响**：回调里往「另一个还有空位的主题」发布根本不会阻塞，
> 那种写法应该正常成功；真要阻塞就照常超时（`MB_ERR_TIMEOUT` + `async_dropped`）。
>
> 回调里要发布就传 `MB_WAIT_NONE`，或用 `MB_PUB_FLAG_ASYNC_OVERWRITE`
> 走覆盖策略，**并且同样传 `MB_WAIT_NONE`**（覆盖 + 不等待时永不阻塞）。

### 队列处理

```c
mb_err_t mb_bus_pump(mb_bus_t *bus);
```

取出队列里**当前积压的全部**消息，逐条投递给匹配的订阅者。
这是异步队列的**唯一消费点**，也是异步订阅的回调真正被执行的地方。

| 返回值 | 含义 |
|---|---|
| `MB_OK` | 正常处理完毕（队列本来就是空的时也返回 `MB_OK`） |
| `MB_ERR_BUSY` | 另一个线程（或某个回调内）正在 pump |
| `MB_ERR_STATE` | 总线正在销毁 |
| `MB_ERR_UNSUPPORTED` | 编译期关闭了异步功能 |
| `MB_ERR_INVALID_ARG` | `bus` 为 `NULL` |

⚠️ **只允许在一个线程里调用**。两个线程同时 pump 会让同一个订阅者的回调被
**并发进入**，而本库只承诺「不会重入同一条投递路径」，不承诺「回调之间互斥」。
并发调用会被拒绝并返回 `MB_ERR_BUSY`，但那是给调试用的兜底，不是设计的一部分。

⚠️ **本函数不阻塞**：队列空时立刻返回。请在专用线程里循环调用，
两次之间自行 `mb_os_sleep_ms()`，否则空转烧 CPU。

```c
static void ui_task(void *arg)
{
    mb_bus_t *bus = arg;

    for (;;) {
        mb_bus_pump(bus);      /* 有消息就投递，没有就立刻返回 */
        mb_os_sleep_ms(5);     /* 别空转 */
    }
}

/* 也可以挂到 LVGL 的定时器上，回调就天然跑在 LVGL 线程里，可以直接碰控件 */
lv_timer_create(pump_timer, 5, bus);
```

因为回调走的是**同步投递那条同一路径**，所以在回调里可以安全地
`publish` / `subscribe` / `unsubscribe`（含取消自己）。

### 查询

```c
size_t mb_bus_async_pending(const mb_bus_t *bus);      /* 还没投递的消息总条数 */
size_t mb_bus_async_topic_count(const mb_bus_t *bus);  /* 当前占用的主题条目数 */
```

```c
/* 检查泵线程是否跟得上：积压条数持续接近
 * MB_CONFIG_ASYNC_MAX_TOPICS × MB_CONFIG_ASYNC_QUEUE_DEPTH 就该扩容或降频了 */
printf("积压 %zu 条，占用 %zu 个主题\n",
       mb_bus_async_pending(bus), mb_bus_async_topic_count(bus));
```

条目在**被取空后立刻回收**，所以空闲时 `mb_bus_async_topic_count()` 会回到 0。

### 宏

| 宏 | 默认 | 含义 |
|---|---|---|
| `MB_WAIT_NONE` | `0u` | 不等待 |
| `MB_WAIT_FOREVER` | `0xFFFFFFFFu` | 永久等待 |

### 关闭顺序

`mb_bus_destroy()` **不会**唤醒阻塞在队列上的发布者（那样只会把一个安静睡着的
线程变成正在解引用的线程）。所以销毁前必须先把所有线程 join 掉：

```c
/* ✅ 正确的关闭顺序（生产者不会永久阻塞时） */
g_pump_should_stop = true;
thread_join(&pump_thread);
mb_bus_destroy(bus);
```

> ⚠️ **生产者用了 `MB_WAIT_FOREVER` 时不能先停 pump。** 阻塞在满队列上的生产者
> 等的是「pump 腾出空位」，先停 pump 就再没人叫醒它，`thread_join()` 会永久挂住。
> 那种情况下顺序要反过来：先停生产者 → 循环 pump 直到 `mb_bus_async_pending()`
> 归零 → 最后停 pump。完整的代码见
> [architecture.md § 8.3](architecture.md#83-不要在回调里销毁总线)。

---

## 9. 版本 `mb_version.h`

```c
#define MB_VERSION_MAJOR   1
#define MB_VERSION_MINOR   1
#define MB_VERSION_PATCH   0
#define MB_VERSION_STRING  "1.1.0"
#define MB_VERSION_NUMBER  ((1 << 16) | (1 << 8) | 0)

const char  *mb_version_string(void);   /* 运行期版本字符串 */
unsigned int mb_version_number(void);   /* 运行期版本整数 */
```

---

## 10. 最小可运行示例

```c
#include <stdio.h>
#include <string.h>
#include "message_bus/message_bus.h"

/* 订阅者回调：运行在发布者线程里，不在锁内 */
static void on_temperature(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)user_data;
    printf("temperature = %.*s C\n", (int)msg->payload_len, (const char *)msg->payload);
}

static void on_command(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub;
    (void)user_data;
    printf("motor got command: %.*s\n", (int)msg->payload_len, (const char *)msg->payload);
}

int main(void)
{
    mb_bus_t  *bus   = NULL;
    mb_node_t *sensor = NULL;
    mb_node_t *motor  = NULL;
    mb_node_t *ui     = NULL;

    mb_bus_create("app", &bus);

    mb_node_create(bus, "sensor", &sensor);
    mb_node_create(bus, "motor",  &motor);
    mb_node_create(bus, "ui",     &ui);

    /* 界面订阅传感器；电机订阅命令。双方互不认识。 */
    mb_node_subscribe(ui,    "sensor/+/value", on_temperature, NULL, NULL);
    mb_node_subscribe(motor, "motor/cmd",      on_command,     NULL, NULL);

    /* 传感器上报（retained：后启动的界面也能立刻拿到当前值） */
    {
        mb_publish_opts_t opts = { .flags = MB_PUB_FLAG_RETAIN, .qos = 0 };
        const char *value = "23.4";

        mb_node_publish(sensor, "sensor/temp/value", value, strlen(value), &opts);
    }

    /* 界面按钮 → 发命令给电机 */
    {
        mb_publish_opts_t opts = { .flags = 0, .qos = 0 };

        mb_node_publish(ui, "motor/cmd", "speed=1200", 10, &opts);
    }

    /* 排查"消息发出去没人收"时很有用 */
    {
        mb_bus_stats_t stats;

        mb_bus_get_stats(bus, &stats);
        printf("发布 %llu 条，投递 %llu 次，无人订阅 %llu 条\n",
               (unsigned long long)stats.published,
               (unsigned long long)stats.delivered,
               (unsigned long long)stats.no_subscriber);
    }

    mb_bus_destroy(bus);
    return 0;
}
```

输出：

```
temperature = 23.4 C
motor got command: speed=1200
```

更多可运行示例见 [`examples/`](../examples/)：

| 示例 | 内容 |
|---|---|
| `01_basic_pubsub` | 最小发布订阅 |
| `02_wildcards_filter` | `+` / `#` / `$`、来源过滤、NOLOCAL |
| `03_lvgl_motor_sensor` | ★ 本项目的真实场景，含 LVGL 线程安全示范 |
| `04_node_events` | 节点上下线事件与 retained 补发 |
