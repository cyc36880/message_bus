# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/) 与
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的组织方式。

## [未发布]

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
