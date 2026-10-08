# message_bus

> MQTT 风格的**进程内**消息总线，用 C99 写成，用来把 LVGL 界面与硬件驱动解耦。
> 同一份代码在 PC 模拟器和 MCU（FreeRTOS）上跑，换平台只需换一个 `.c` 文件。

[![CI](https://github.com/OWNER/message_bus/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/message_bus/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C99](https://img.shields.io/badge/C-C99-blue.svg)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen.svg)

---

## 为什么需要它

用 LVGL 做界面时，很快就会撞上这个矛盾：

- 界面需要**控制电机**、**显示传感器数据**；
- 但同一份界面代码还要在 **PC 模拟器**上跑（不然没法调试），
  而 PC 上根本没有那些寄存器。

如果界面直接调用 `HAL_GPIO_WritePin()`，PC 模拟器立刻编不过。
于是需要一个解耦层 —— 但不是「函数指针注册表」那种，
而是**发布者不需要知道订阅者存在**的那种。

消息总线就是这个解耦层。所有通讯实体以**节点**的形式挂在总线上，
彼此只认识**主题字符串**：

```c
/* 传感器：我只管发，谁在听、有几个在听，我都不关心 */
mb_node_publish(sensor, "sensor/temp/value", "23.4", 4, &retain);

/* 界面：我只管订阅，温度从哪来、是 ADC 还是模拟器，我也不关心 */
mb_node_subscribe(ui, "sensor/+/value", on_temperature, NULL, NULL);

/* 电机：我只认命令主题 */
mb_node_subscribe(motor, "motor/cmd", on_command, NULL, NULL);
```

三方互不相识。到 MCU 上时，把电机回调里的 `printf` 换成 PWM 设置、
把传感器的定时发布换成 ADC 采样，**其余代码一行都不用改**。

---

## 特性

- **MQTT 3.1.1 主题语义**：`/` 分层、`+` 单层通配、`#` 多层通配、
  `$` 系统主题保护规则，与规范完全一致。
- **保留消息（retained）**：发布时打标记，总线上保存该主题最后一条。
  界面后启动时 `subscribe()` 一返回就拿到了当前值 —— 这是 LVGL 场景的关键。
- **同步 / 异步两种投递，订阅代码共用一份**。默认同步（零拷贝、调用返回即完成）；
  需要跨线程时改 `mb_node_publish_async()`，回调就挪到自己的 pump 线程上跑，
  发布方不再被刷屏拖慢。订阅端一行都不用改。
- **异步队列按主题分组**：`"a/b"` 与 `"a/c"` 互不挤占；队列满时可选
  「等一会儿」或「覆盖最旧的」。
- **线程安全**：总线内嵌递归互斥锁 + 引用计数。回调**一律在锁外**执行，
  因此回调里可以安全地 publish / subscribe / unsubscribe，**包括取消自己**。
- **零动态分配的余地**：内存走可替换的钩子，可接静态内存池；
  刻意不用 `realloc`（很多 RTOS 堆没有它）。
- **零全局状态**：所有状态挂在 `mb_bus_t` 上，可以有任意多条互不干扰的总线。
- **无隐藏资源**：不创建线程、不跑定时器；异步队列用到才分配。
  资源在 `create` 时分配、`destroy` 时全部归还。
- **LVGL 风格配置**：`MB_CONF_PATH` 机制 + 全部选项 `#ifndef` 可覆盖。
  不用异步投递可以编译期整体关掉，一个字节的 RAM 都不多花。
- **无第三方依赖**：只需要 C99 标准库。

---

## 快速开始

```bash
git clone https://github.com/OWNER/message_bus.git
cd message_bus

cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

./build/debug/bin/example_03_lvgl_motor_sensor
```

不想用 presets 的话：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 最小示例

```c
#include <stdio.h>
#include <string.h>
#include "message_bus/message_bus.h"

static void on_temperature(mb_subscription_t *sub, const mb_message_t *msg, void *user_data)
{
    (void)sub; (void)user_data;
    printf("temperature = %.*s C\n", (int)msg->payload_len, (const char *)msg->payload);
}

int main(void)
{
    mb_bus_t  *bus    = NULL;
    mb_node_t *sensor = NULL;
    mb_node_t *ui     = NULL;

    mb_bus_create("app", &bus);                 /* 1. 创建一条总线 */
    mb_node_create(bus, "sensor", &sensor);     /* 2. 节点挂在总线上 */
    mb_node_create(bus, "ui",     &ui);

    mb_node_subscribe(ui, "sensor/+/value", on_temperature, NULL, NULL);

    /* 3. 发布：同步投递，返回时 on_temperature 已经执行完了 */
    mb_publish_opts_t opts = { .flags = MB_PUB_FLAG_RETAIN, .qos = 0 };
    mb_node_publish(sensor, "sensor/temp/value", "23.4", 4, &opts);

    mb_bus_destroy(bus);                        /* 4. 级联释放一切 */
    return 0;
}
```

```
temperature = 23.4 C
```

---

## 集成到你的工程

### 方式 1：CMake 子目录（推荐）

```cmake
add_subdirectory(third_party/message_bus)
target_link_libraries(your_app PRIVATE message_bus::message_bus)
```

### 方式 2：安装后 `find_package`

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix /your/prefix
```

```cmake
find_package(message_bus 1.0 REQUIRED)
target_link_libraries(your_app PRIVATE message_bus::message_bus)
```

### 方式 3：MCU 工程手工集成

把 `include/`、`src/` 下的文件加入编译列表，**再加一个 port 文件**
（`port/mb_os_freertos.c`），然后定义：

```c
#define MB_CONFIG_OS MB_OS_FREERTOS
```

详细的移植步骤、FreeRTOS 任务骨架代码和静态内存池方案见
**[docs/porting.md](docs/porting.md)**。

---

## 平台支持

| 平台 | `MB_CONFIG_OS` | 状态 |
|---|---|---|
| Windows（LVGL 模拟器） | `MB_OS_WIN32` | ✅ 已实现 |
| Linux / macOS | `MB_OS_POSIX` | ✅ 已实现 |
| **FreeRTOS（MCU）** | `MB_OS_FREERTOS` | ✅ 已实现，需 `configUSE_RECURSIVE_MUTEXES = 1`；用异步投递还需 `configUSE_COUNTING_SEMAPHORES = 1` |
| 裸机 / 无 RTOS | `MB_OS_NONE` | ✅ 已实现（临界区宏可覆盖）。**异步投递无法真正阻塞**，见 porting.md § 2.1 |
| 其它 RTOS | — | 新增一个 `port/mb_os_xxx.c`，只需实现 7 个函数（有异步时 11 个） |

CMake 下 `-DMB_OS=auto`（默认）会按平台自动选择。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | **架构说明**：数据结构、投递算法、**异步队列与 pump**、线程与内存模型、优点、**代价与局限**、踩坑清单 |
| [docs/api.md](docs/api.md) | 按模块的 API 参考，含可运行片段（异步部分见第 8 节） |
| [docs/porting.md](docs/porting.md) | 移植到 FreeRTOS / 裸机 / 新 RTOS，含任务骨架代码 |
| [docs/topics.md](docs/topics.md) | 主题与通配符规范、MQTT 对照表、状态 vs 事件的取舍 |
| [tools/README.md](tools/README.md) | **可视化设计器**：节点多到理不清时，用网页手工梳理总线/节点/发布/订阅，数据存 XML，并按配置实时显示对应的 C 代码 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 如何参与，以及改核心代码时最容易踩的坑 |
| [CHANGELOG.md](CHANGELOG.md) | 版本历史与已知限制 |

---

## ⚠️ 三条最重要的注意事项

### 1. 不要在 LVGL 回调里直接碰控件

**同步**投递的回调是在**发布者的线程**里执行的，而 LVGL 不是线程安全的。

```c
/* ❌ 回调运行在传感器任务的线程里，这里碰控件迟早花屏或崩溃 */
lv_label_set_text(label, msg->payload);

/* ✅ 做法 A：回调只记值（或投进队列），由 LVGL 线程刷新 */
g_temp = atof(msg->payload);

/* ✅ 做法 B（推荐）：改用异步投递 —— 回调本来就跑在 pump 线程上，
 *    让 LVGL 的定时器驱动 pump，回调里就能直接碰控件了 */
mb_node_publish_async(sensor, "sensor/temp/value", buf, len, NULL, MB_WAIT_NONE);
lv_timer_create(pump_timer, 5, bus);   /* 定时器里调一次 mb_bus_pump(bus) */
```

详见 [architecture.md § 8.1](docs/architecture.md#81-lvgl-回调里不要直接碰控件--最重要的一条)。

### 2. 回调里别做耗时操作

回调阻塞的是**驱动它的那个线程** —— 同步投递下是发布者，异步投递下是
调 `mb_bus_pump()` 的那个线程。`publish()` 的耗时 = 所有匹配回调的耗时之和。
回调里 `vTaskDelay()`、写 Flash、等网络，都会直接拖慢这个线程。

异步投递只是**换了一个被拖慢的线程**（通常正是要的效果），并没有让耗时操作变便宜。

### 3. 回调里的消息指针只在回调期间有效

投递是**零拷贝**的，`msg->payload` 直接指向发布者的缓冲区。
保存这个指针到回调之外，或者用它做异步操作（如 `lv_async_call`），都是悬垂指针。

```c
/* ❌ 异步执行时 publish 早就返回了，payload 已失效 */
lv_async_call(set_label, (void *)msg->payload);

/* ✅ 先拷贝到自己的缓冲 */
static char buf[32];
snprintf(buf, sizeof(buf), "%.*s", (int)msg->payload_len, (const char *)msg->payload);
lv_async_call(set_label, buf);
```

---

## 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `MB_BUILD_EXAMPLES` | `ON` | 编译 `examples/` |
| `MB_BUILD_TESTS` | `ON` | 编译 `tests/` 并注册 CTest |
| `MB_BUILD_SHARED` | `OFF` | 编译为动态库（默认静态库） |
| `MB_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` |
| `MB_OS` | `auto` | `auto` / `none` / `freertos` / `posix` / `win32` |
| `MB_FREERTOS_INCLUDE_DIR` | — | 使用 `MB_OS=freertos` 时指向 `FreeRTOS.h` 所在目录 |

主要编译期配置（详见 [config/mb_conf_template.h](config/mb_conf_template.h)）：

| 宏 | 默认 | 说明 |
|---|---|---|
| `MB_CONFIG_MAX_TOPIC_LEN` | 128 | 主题最大长度（含 `'\0'`） |
| `MB_CONFIG_MAX_NAME_LEN` | 32 | 节点名最大长度 |
| `MB_CONFIG_MAX_PAYLOAD_SIZE` | 0 | payload 上限；0 = 不限 |
| `MB_CONFIG_DELIVER_BATCH` | 8 | 每批投递的订阅者数（只影响栈占用） |
| `MB_CONFIG_MAX_DISPATCH_DEPTH` | 8 | 递归发布深度上限；超限丢弃并返回 `MB_ERR_BUSY` |
| `MB_CONFIG_ASYNC_MAX_TOPICS` | 16 | 异步队列的主题条目数；**0 = 编译期关掉异步** |
| `MB_CONFIG_ASYNC_QUEUE_DEPTH` | 4 | 每个主题条目内的 FIFO 深度 |
| `MB_CONFIG_ASYNC_OVERWRITE_OLDEST` | 0 | 0 = 队列满时等待，1 = 覆盖最旧的一条 |
| `MB_CONFIG_LOG_LEVEL` | 2 | 0 关 / 1 ERROR / 2 WARN / 3 INFO / 4 DEBUG |
| `MB_CONFIG_ENABLE_CHECKS` | 1 | 内部检查与断言 |

---

## 目录结构

```
message_bus/
├── include/message_bus/   # 公开头文件（安装这一份）
│   ├── message_bus.h      #   总入口
│   ├── mb_config.h        #   编译期配置（支持 MB_CONF_PATH 覆盖）
│   ├── mb_bus.h  mb_node.h  mb_topic.h  mb_async.h
│   ├── mb_os.h  mb_log.h  mb_types.h  mb_version.h
├── src/                   # 实现（私有头 mb_internal.h 不外装）
│   ├── mb_dispatch.c      #   ★ 唯一的投递出口（同步与异步共用）
│   ├── mb_async.c         #   ★ 异步队列 + pump（可选）
│   ├── mb_bus.c  mb_node.c  mb_subscription.c
│   ├── mb_topic.c  mb_message.c  mb_log.c
├── port/                  # 平台层：每个平台一个文件，7 个函数（+4 个有异步时）
│   ├── mb_os_win32.c  mb_os_posix.c
│   ├── mb_os_freertos.c   #   ★ MCU 目标平台
│   └── mb_os_none.c       #   裸机
├── config/mb_conf_template.h   # 复制到你的工程里改
├── examples/              # 4 个可运行示例
├── tests/                 # 600+ 条断言，CTest 驱动，无第三方依赖
├── docs/                  # 架构 / API / 移植 / 主题
├── tools/                 # 可视化设计器（纯前端，不参与 C 构建）
│   ├── message-bus-designer.html   #   ★ 单文件、零依赖，双击即可用
│   ├── web-selftest.js             #   用 cscript 校验网页的主题规则与固件一致
│   └── examples/                   #   可直接导入设计器的示例 XML
└── CMakeLists.txt  CMakePresets.json  .github/workflows/ci.yml
```

---

## 测试

```bash
ctest --preset debug --output-on-failure
```

```
message_bus unit tests
  topic (topics and wildcards)
  bus (bus and node lifecycle)
  pubsub (publish/subscribe semantics)
  async (async delivery)
  threads (thread safety)

612 assertions, 0 failed
ALL PASSED
```

`threads` 套件做的是 4 线程 × 500 条消息的并发发布，外加一个线程持续
订阅/退订，用来验证引用计数确实经得起「回调执行中对象被另一个线程销毁」。

`async` 套件覆盖两级队列的独立性与 FIFO、非阻塞 pump、满队列的等待/超时/覆盖
三条路径、深拷贝、retained 语义，以及「4 个生产者线程 + 1 个 pump 线程」
的背压（生产者用 `MB_WAIT_FOREVER`，一条都不许丢）。

CI 还会在 Linux 上跑一遍 ASan + UBSan。

---

## 已知限制

- **同步投递是默认的**：回调在发布者线程内执行，不适合放耗时操作。
  需要跨线程时用异步投递（`mb_node_publish_async()` + `mb_bus_pump()`）。
- **裸机上异步投递无法真正阻塞**：满队列一律退化成 `MB_ERR_TIMEOUT`，
  建议改用覆盖策略或直接关掉异步。详见
  [porting.md § 2.1](docs/porting.md#21-裸机上没有真正的阻塞)。
- **QoS 仅支持 0**：`opts.qos != 0` 返回 `MB_ERR_UNSUPPORTED`。
- **进程内**：这不是网络 MQTT，没有 broker、没有跨设备通信。
- **单锁串行**：订阅表是 O(n) 线性扫描。几十个订阅、每秒几千条消息没问题；
  几万订阅 + 高频发布的场景需要加哈希索引（目前未实现）。
- **无遗嘱消息（LWT）**：节点异常退出（而非正常 `destroy`）无法被感知。

权衡的细节见 [architecture.md § 6–7](docs/architecture.md#6-这套设计的优点)。

---

## 许可

[MIT](LICENSE)
