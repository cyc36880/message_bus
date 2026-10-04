# 主题与过滤规范

本库的主题语义完整遵循 **MQTT 3.1.1** 的规范（[第 4.7 节](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718106)）。
本文是速查与对照，规则细节以 MQTT 规范为准。

---

## 1. 主题（Topic）

主题是一个以 `/` 分层的 UTF-8 字符串，**区分大小写**。

```
sensor/room1/temperature
│      │     └── 第 3 层
│      └──────── 第 2 层
└─────────────── 第 1 层
```

| 规则 | 说明 |
|---|---|
| 分隔符 | `/`（`MB_TOPIC_SEPARATOR`） |
| 大小写 | 敏感。`Sensor/temp` ≠ `sensor/temp` |
| 前导 `/` | 合法，但会产生一个**空层**：`/a/b` 是 3 层 |
| 尾部 `/` | 同上：`a/b/` 也是 3 层，最后一层为空 |
| 空层 vs 无层 | `a//b` 的中间层是空串，与 `a/b` **不匹配** |
| 最大长度 | `MB_CONFIG_MAX_TOPIC_LEN`（默认 128，含 `'\0'`） |
| 发布时不得含通配符 | 发布主题里出现 `+` 或 `#` 返回 `MB_ERR_INVALID_ARG` |
| 建议字符集 | 只用小写字母、数字、`-`、`_`、`/`，避免转义与编码争议 |

---

## 2. 通配符

### `+` —— 匹配恰好一层

```
过滤器  sensor/+/temperature
匹配    sensor/room1/temperature        ✅  '+' = "room1"
匹配    sensor/room2/temperature        ✅
不匹配  sensor/room1/a/temperature      ❌  多了一层
不匹配  sensor/temperature              ❌  少了一层

过滤器  +/temperature
匹配    room1/temperature               ✅
匹配    room2/temperature               ✅
```

> ⚠️ `+` 必须**独占一整层**。`sensor_+/temperature`、`sensor/r+/temperature`
> 都是**非法**过滤器，`mb_topic_validate_filter()` 会返回 `MB_ERR_INVALID_ARG`。
> 想匹配「以 sensor_ 开头的节点」，请改用**来源过滤**（见第 4 节）。

### `#` —— 匹配剩余所有层（含零层）

```
过滤器  sensor/#
匹配    sensor                          ✅  零层也匹配！
匹配    sensor/room1                    ✅
匹配    sensor/room1/temperature        ✅
不匹配  sensorx/room1                   ❌  必须是完整的一层
```

> ⚠️ `#` 必须**独占最后一层**。`sensor/#/x`、`sensor/a#` 非法。

### `$` —— 系统主题

以 `$` 开头的主题是系统主题，**首层的通配符不能匹配它们**：

```
过滤器  #                              不匹配  $mb/nodes/x/connected
过滤器  +/nodes/x/connected            不匹配  $mb/nodes/x/connected
过滤器  $mb/#                          匹配    $mb/nodes/x/connected   ✅
过滤器  $mb/nodes/+/connected          匹配    $mb/nodes/x/connected   ✅
```

这条规则的作用是防止「订阅 `#` 收全部消息」的调试节点无意中把系统消息也吞掉。
**想收系统消息必须显式写出 `$` 前缀。**

---

## 3. 匹配速查表

| 过滤器 | 主题 | 结果 |
|---|---|---|
| `sport/tennis/player1` | `sport/tennis/player1` | ✅ |
| `sport/tennis/player1` | `sport/tennis/player2` | ❌ |
| `sport/tennis/+` | `sport/tennis/player1` | ✅ |
| `sport/tennis/+` | `sport/tennis/player1/ranking` | ❌ |
| `sport/+/player1` | `sport/tennis/player1` | ✅ |
| `+/+` | `sport/tennis` | ✅ |
| `+` | `sport` | ✅ |
| `+` | `sport/tennis` | ❌ |
| `sport/#` | `sport` | ✅ **（零层也匹配）** |
| `sport/#` | `sport/tennis/player1` | ✅ |
| `#` | `sport/tennis/player1` | ✅ |
| `sport/tennis/#` | `sport/tennis` | ✅ |
| `/finance` | `/finance` | ✅ |
| `+/+` | `/finance` | ❌（前导 `/` 产生空层） |
| `+/+` | `finance` | ❌ |
| `#` | `$SYS/uptime` | ❌ |
| `$SYS/#` | `$SYS/uptime` | ✅ |

---

## 4. 来源过滤（本库的扩展）

MQTT 没有这个概念 —— 因为 MQTT 的发布者身份不由主题决定。
本库把节点的字符串名字也当成一个可过滤维度：

```c
mb_subscribe_opts_t opts;
opts.filter        = "#";               /* 收所有主题 */
opts.source_filter = "sensor_a";        /* 但只收 sensor_a 发的 */
opts.flags         = 0;
opts.user_data     = NULL;
mb_node_subscribe_ex(logger, &opts, on_message, NULL);
```

`source_filter` 的语法**与主题过滤器完全相同**（支持 `+` / `#`）。所以：

```c
opts.source_filter = "sensor/+";   /* sensor_a、sensor_b ... 发的都收 */
opts.source_filter = "sensor/#";   /* 同理，'#'
```

两条注意：

- 由**总线自身**发布的消息（`mb_bus_publish()`，如系统事件）`source` 为 `NULL`，
  **永远不会**命中任何 `source_filter`。
- 来源过滤是在主题匹配**之后**做的第二道过滤，两者都满足才投递。

### `MB_SUB_FLAG_NOLOCAL`

不接收「自己这个节点发布的」消息（对应 MQTT 5 的 NoLocal）。
用于转发型节点，避免自己的消息绕一圈回到自己形成环：

```c
opts.filter = "#";
opts.flags  = MB_SUB_FLAG_NOLOCAL;
mb_node_subscribe_ex(forwarder, &opts, on_message, NULL);
```

---

## 5. 本库的保留主题

总线的节点上下线事件发布在 `$mb/` 命名空间下：

| 主题 | 保留 | 负载 | 说明 |
|---|---|---|---|
| `$mb/nodes/<name>/connected` | ✅ | 节点名 | 上线时发布 |
| `$mb/nodes/<name>/connected` | ✅ | **空** | 下线时发布，清除保留状态 |
| `$mb/nodes/<name>/disconnected` | ❌ | 节点名 | 下线时发布的纯事件 |

因为 `$` 规则，订阅 `#` 收不到它们。要监控全部节点：

```c
/* 实时知道谁上线、谁掉线；后启动的订阅者通过 retained 立刻拿到当前在线列表 */
mb_node_subscribe(monitor, "$mb/nodes/+/connected", on_node_event, NULL, NULL);

/* 只要上线通知 */
mb_node_subscribe(monitor, "$mb/nodes/+/connected", on_node_event, NULL, NULL);
/* 只要掉线通知 */
mb_node_subscribe(monitor, "$mb/nodes/+/disconnected", on_drop, NULL, NULL);
```

可参考 `examples/04_node_events.c`。

---

## 6. 命名建议

本库不强制命名规范，但下面这套在实践中好用：

```
<节点名>/<类别>/<细节>

sensor/room1/temperature         传感器读数
sensor/room1/humidity
motor/speed/status               电机状态（retained）
motor/cmd                        电机命令（不 retained）
ui/btn/start                     界面事件
$mb/...                          留给总线自己
```

**约定：把节点名当作主题的第一层**，就得到类似 MQTT 的定向寻址：

```c
mb_node_publish_to(ui, "motor", "cmd", data, len, NULL);   /* → motor/cmd */
mb_node_subscribe_self(motor, on_my_message, NULL, NULL);  /* → "motor/#" */
mb_node_subscribe(ui, "motor/#", on_motor, NULL, NULL);    /* 收 motor 的全部消息 */
```

好处是「主题」和「谁发的」这两件事天然对齐，调试时看日志一眼就知道消息从哪来。

---

## 7. 状态 vs 事件：什么时候用 retained

这是使用本库时最容易搞错的一点。

| | 状态（State） | 事件（Event） |
|---|---|---|
| 例子 | 当前温度、电机当前转速、继电器当前开关 | 按钮按下、请求重启、一条日志 |
| 用 retained？ | ✅ 要 | ❌ 不要 |
| 理由 | 后加入的订阅者**必须**知道当前值 | 后加入的订阅者**不该**收到历史事件 |

```c
/* ✅ 状态：新界面一订阅就拿到当前转速 */
mb_publish_opts_t retain = { .flags = MB_PUB_FLAG_RETAIN, .qos = 0 };
mb_node_publish(motor, "motor/speed/status", "1200 rpm", 8, &retain);

/* ✅ 事件：不保留，避免"重放" */
mb_publish_opts_t plain = { .flags = 0, .qos = 0 };
mb_node_publish(ui, "motor/cmd", "stop", 4, &plain);
```

如果给 `motor/cmd` 加了 retained，那么界面每次重启订阅时都会收到上一次的命令
**并重新执行一遍** —— 对「停止」这种命令也许无所谓，但对「正转 3000 转」就很危险。

### 清除保留消息

按 MQTT 语义，向同一主题发布一条**空负载**的 retained 消息即可清除：

```c
/* 清除 motor/speed/status 的保留状态；在线订阅者仍会收到这条空消息 */
mb_publish_opts_t opts = { .flags = MB_PUB_FLAG_RETAIN, .qos = 0 };
mb_node_publish(motor, "motor/speed/status", NULL, 0, &opts);
```

节点下线时的 `connected` 清理用的就是这个机制。

---

## 8. 相关 API

```c
/* 匹配与校验（不依赖总线，可单独使用） */
bool     mb_topic_match(const char *filter, const char *topic);
mb_err_t mb_topic_validate_filter(const char *filter);
mb_err_t mb_topic_validate_topic(const char *topic);
bool     mb_topic_is_system(const char *topic);
size_t   mb_topic_level_count(const char *topic);
mb_err_t mb_topic_build(char *buf, size_t buf_size, const char *prefix, const char *suffix);

/* 常量 */
#define MB_TOPIC_SEPARATOR        '/'
#define MB_TOPIC_WILDCARD_SINGLE  '+'
#define MB_TOPIC_WILDCARD_MULTI   '#'
#define MB_TOPIC_SYSTEM_PREFIX    '$'
```

`tests/test_topic.c` 里有完整的匹配用例表，可以直接当行为参考。
