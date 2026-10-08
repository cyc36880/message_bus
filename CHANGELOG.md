# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/) 与
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的组织方式。

## [未发布]

### 新增

- **异步投递（`mb_async.h`）**。此前投递一律是同步的：回调在**发布者线程**上跑，
  所以跨线程发消息时，接收方得先把数据转存进中转缓冲、再在自己的线程里取出来处理，
  消息一多就很别扭。现在发布方可以只入队：

  ```c
  mb_node_publish_async(sensor, "sensor/temp/value", buf, len, NULL, MB_WAIT_NONE);
  /* 任意线程（通常独占一个）驱动队列，回调就在这里跑 */
  mb_bus_pump(bus);
  ```

  回调走的是**同步投递那条同一路径**（`mb_dispatch_message()`），所以订阅语义、
  retained 补发、引用计数保护、递归深度保护全部自动一致 —— **订阅端一行都不用改**。

- **队列是两级的**：总线上一张主题条目表，每个条目自带一条 FIFO。
  `"a/b"` 与 `"a/c"` 是两个条目，互不挤占；往 `"a/b"` 连发内容 1、2
  则只在 `a/b` 这一个条目里积压两条。

- **满队列的两种策略**：

  | 策略 | 打开方式 | 行为 |
  |---|---|---|
  | 等待（默认） | — | 最多等 `timeout_ms`，`MB_WAIT_FOREVER` 永久等；超时返回 `MB_ERR_TIMEOUT` |
  | 覆盖最旧 | `MB_PUB_FLAG_ASYNC_OVERWRITE` 或 `MB_CONFIG_ASYNC_OVERWRITE_OLDEST = 1` | 丢弃该主题最旧的一条，永不阻塞 |

  编译期默认 + 单次发布覆盖，因为状态类主题（传感器当前值）适合覆盖、
  命令类主题（`motor/cmd`）绝不能覆盖。

- **新 API**：`mb_node_publish_async()`、`mb_node_publish_to_async()`、
  `mb_bus_publish_async()`、`mb_bus_pump()`、`mb_bus_async_pending()`、
  `mb_bus_async_topic_count()`。同步 API 签名**未变**。

- **`mb_os.h` 新增计数信号量**（`mb_os_sem_create/wait/signal/destroy`），
  四个 port 全部实现。POSIX 用手搓的条件变量信号量而非 `sem_t` ——
  因为 `sem_timedwait()` 是可选项，**macOS 至今没有实现**。

- **新配置项**：`MB_CONFIG_ASYNC_MAX_TOPICS`（默认 16，**设为 0 可在编译期
  完全去掉异步功能，不占任何 RAM**）、`MB_CONFIG_ASYNC_QUEUE_DEPTH`（默认 4）、
  `MB_CONFIG_ASYNC_OVERWRITE_OLDEST`（默认 0）。

- **新统计字段**：`async_enqueued`、`async_dropped`、`async_overwritten`。

- **`tests/test_async.c`**：17 个用例 / 225 条断言，覆盖两级队列的独立性与 FIFO、
  非阻塞 pump、满队列的等待/超时/覆盖三条路径、深拷贝、retained 语义、
  回调线程归属，以及「4 个生产者线程 + 1 个 pump 线程」的背压
  （生产者 `MB_WAIT_FOREVER`，一条都不许丢）。

### 修复

- **补上 `mb_err_to_string()` 的实现**。它从 1.0.0 起就声明在 `mb_types.h` 里，
  但**从未在任何源文件中定义过** —— 任何调用它的用户代码都会撞上
  `undefined reference to mb_err_to_string`。现在实现在 `src/mb_log.c`，
  并补上了 `MB_ERR_TIMEOUT` 一项。
- **修正 `mb_async.c` 覆盖策略的空位记账错误**（本版本新增的代码，在测试中发现）：
  覆盖最旧消息时只推进了队头却没有递减 `count`，导致该主题队列的
  `count` 比实际多 1，第二次覆盖就会越界写入。已在 `entry->count--` 处修正。

### 兼容性

- **版本升到 1.1.0**。`mb_node_publish()` 等**同步 API 的签名与语义完全未变**，
  新增的异步 API 是一族并行的 `_async` 函数，订阅端代码一行都不用改。
- ⚠️ **`mb_bus_stats_t` 末尾追加了 3 个字段**。这对**源码**兼容（用
  `mb_bus_get_stats()` 取值即可），但对**预编译二进制**是 ABI 破坏：
  旧版库编译的调用方若把这个结构体按旧尺寸分配在栈上，会短 24 字节。
  请连同库一起重新编译。
- **自建 port 若要支持异步投递，需要补 4 个函数**。`mb_os.h` 新增了计数信号量
  `mb_os_sem_create/wait/signal/destroy`（各约 5 行，仓库内四个 port 已全部实现，可照抄）。
  **不用异步投递则不必补** —— 把这四个函数的调用全部包在
  `#if MB_CONFIG_ASYNC_MAX_TOPICS > 0` 之内，所以把该宏设为 0 时
  `mb_async.c` 整个编译为空，不会产生对这四个符号的引用
  （已用 `nm` 验证目标文件里没有相关的未定义符号）。
- **FreeRTOS 用户注意**：异步投递需要 `configUSE_COUNTING_SEMAPHORES = 1`，
  没打开时编译期直接 `#error`（而不是到运行期才出问题）。

  时直接 `#include "FreeRTOS.h"`，而 Arduino-ESP32 / ESP-IDF 的头文件位于
  `freertos/` 子目录，导致编译报 `FreeRTOS.h: No such file or directory`。
  现在 `mb_os.h` 与 `mb_os_freertos.c` 用 `__has_include` 自动探测两种布局
  （`freertos/FreeRTOS.h` 与根目录 `FreeRTOS.h`），无需工程侧额外配置。
- **各 port 文件改为「未选中即编译为空」**（`#if MB_CONFIG_OS == ...` 包住实现，
  替代原来的 `#error`）。Arduino / PlatformIO 会递归扫描并编译库内所有 `.c`，
  原来会因多编译了其它 port 文件而中断；现在多余的 port 只产生空目标文件。
- **新增 `library.json`**，让 PlatformIO 自动编译 `src/*.c` 与 `port/*.c`
  并暴露 `include/` 头文件路径（原先 `port/` 不在默认扫描范围内，
  使用总线 API 时会报 `undefined reference to mb_os_mutex_create`）。

### 新增

- **可视化设计器** `tools/message-bus-designer.html`：单文件、零依赖的网页工具，
  用于在节点过多、代码难以阅读时手工梳理总线 / 节点 / 发布 / 订阅的关系。
  表格与关系图两种视图可一键切换，支持注释，数据以 XML 导入 / 导出 / 保存到本地。
  校验规则由 `src/mb_topic.c`、`src/mb_dispatch.c` 逐行移植，
  并由 `tools/web-selftest.js`（`cscript`）用 `tests/test_topic.c` 的同一张用例表验证一致性。
- **设计器：按配置生成 C 代码**。每条总线、每个节点各有一个代码开关，
  点开即显示该对象对应的 `mb_bus_create()` / `mb_node_create()` / 发布 / 订阅
  以及回调空壳；名字会洗成合法标识符，可省的参数不生成额外变量
  （发布无 `retain` 则传 `NULL`，订阅无附加选项则用 `mb_node_subscribe()`）。
  生成的片段经 `gcc -std=c99 -Wall -Wextra -Wpedantic` 编译无警告，并可链接运行。
  代码带 **C 语法着色**（类型 / 字符串 / 注释 / 宏 / 函数名），且只上色不改字 ——
  逐行文本与生成的源码完全一致，复制出去仍是干净的 C。
- **设计器：改动配置时新代码行会高亮**。被新增或被改动的代码行以**背景高亮**
  闪一次（1.8 s，含一条淡出的左侧竖标）后自动复原，文字颜色不受影响。
  重绘时会保留代码块的滚动位置；闪的行若在视野外，只滚动代码块自身，不移动页面。
- **设计器：关系图支持拖拽平移**。在画布空白处按住左键拖动即可平移视野
  （拖动节点仍然是移动节点）。
- **设计器：线条可拖动改形、可改颜色、带流向动画**。同一对节点之间的线会
  **自动岔开车道**（第一条上偏 26 px、第二条下偏 26 px……），修掉了
  「`lvgl_ui` → `motor` 和 `motor` → `lvgl_ui` 两个标签压在同一个交点上、
  怎么拖都分不开」的问题 —— 根因是两条反向边算出来的形状完全一样；
  车道按**无序**节点对分组，反向的那条才会落到另一侧。线本身加粗了透明的
  命中带（1.4 px 的线几乎点不中），按住线上的标签即可拖动改形，
  单击选中后可改颜色；偏移量与颜色写进 XML 的新 `<edge>` 元素
  （schema 1.1.0，1.0.0 的文件照样能读）。线上还叠了一条流动虚线，
  `stroke-dashoffset` 递减使虚线朝终点跑，直观显示数据由源头流向目标，
  `prefers-reduced-motion` 下自动停止。
- **设计器：关系图支持 Ctrl + 滚轮缩放**。以光标为锚点，范围 35% – 300%，
  图例右侧另有 `−` `100%` `+` 按钮。缩放只是视野状态，不写进 XML 也不写进
  浏览器存储；`svgPoint()` 相应带上 viewBox 换算，缩放后拖节点不再偏移。

## [1.0.0] - 2026-10-04

首个可用版本。目标是「LVGL 界面与硬件驱动解耦」，因此核心 API 与语义在此版本冻结。

### 新增

- **总线**：`mb_bus_create()` / `mb_bus_create_ex()` / `mb_bus_destroy()`，
  支持同时存在多条互不干扰的总线，无全局状态。
- **节点**：`mb_node_create()`（名字唯一，传 NULL 自动命名 `node-N`）、`mb_node_destroy()`
  （级联摘除其全部订阅）。
- **发布**：`mb_node_publish()`、`mb_node_publish_to()`、`mb_bus_publish()`（总线级 / 系统消息）。
- **订阅**：`mb_node_subscribe()`、`mb_node_subscribe_ex()`（支持 `source_filter` 与标志位）、
  `mb_node_subscribe_self()`、`mb_node_unsubscribe()`。
- **MQTT 风格主题**：`/` 分层、`+` 单层通配、`#` 多层通配（含 `sport/#` 匹配 `sport`）、
  `$` 系统主题不被首层通配符匹配；`mb_topic_*` 系列工具函数可独立使用。
- **保留消息（retained）**：`MB_PUB_FLAG_RETAIN`，空负载发布即清除保留；
  新订阅者在 `subscribe()` 返回前收到匹配的 retained 历史消息。
- **节点上下线事件**：`$mb/nodes/<name>/connected`（retained，下线时发空负载清除）与
  `$mb/nodes/<name>/disconnected`（纯事件，不保留）。
- **线程安全**：总线递归互斥锁 + 订阅/节点引用计数，回调**一律在锁外**执行，
  因此回调内可安全地 publish / subscribe / unsubscribe（包括取消自己）。
- **递归发布保护**：`MB_CONFIG_MAX_DISPATCH_DEPTH`（默认 8），超限返回 `MB_ERR_BUSY`
  并计入 `stats.dropped`。
- **OS 抽象层**：`MB_OS_NONE`（裸机）/ `MB_OS_FREERTOS` / `MB_OS_POSIX` / `MB_OS_WIN32`，
  由 `MB_CONFIG_OS` 宏选择，LVGL 风格。
- **配置系统**：`MB_CONF_PATH` 机制 + `config/mb_conf_template.h` 模板，
  所有选项均可用 `#ifndef` 覆盖。
- **构建与集成**：CMake 目标 `message_bus::message_bus`，支持静态/动态库、
  `install` + `find_package(message_bus)`、`CMakePresets.json`。
- **测试**：387 条断言，覆盖主题匹配表、总线生命周期、发布订阅语义、retained、
  递归保护，以及 4 线程 × 500 条消息的并发压力测试。
- **示例**：4 个可独立运行的示例，含 LVGL/电机/传感器的真实场景仿真。
- **文档**：`docs/architecture.md`（架构与优缺点）、`api.md`、`porting.md`、`topics.md`。

### 已知限制

- 投递是**同步**的：回调在发布者线程内执行，不能用于跨任务解耦耗时操作。
- QoS 仅支持 0；`opts.qos != 0` 返回 `MB_ERR_UNSUPPORTED`。
- 消息不跨进程 —— 这是**进程内**总线，不是网络 MQTT broker。
- `mb_bus_find_node()` 返回借用指针，多线程下并发销毁节点需调用方自行加锁。

[未发布]: https://github.com/OWNER/message_bus/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/OWNER/message_bus/releases/tag/v1.0.0
