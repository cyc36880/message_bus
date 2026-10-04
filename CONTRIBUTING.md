# 贡献指南

感谢参与。这是一个面向嵌入式（MCU + PC 模拟器双端）的 C 库，
因此下面的约束比一般项目更严格一些，请务必先读完再提 PR。

## 快速开始

```bash
git clone <repo-url> && cd message_bus

cmake --preset debug          # 或 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build --preset debug --parallel
ctest --preset debug          # 326 条断言必须全绿

# 跑一遍示例，肉眼确认输出符合预期
./build/debug/bin/example_01_basic_pubsub
./build/debug/bin/example_02_wildcards_filter
./build/debug/bin/example_03_lvgl_motor_sensor
./build/debug/bin/example_04_node_events
```

不想用 presets 的话，等价的手工命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 提交前必须满足

1. **零告警**。在 `-Wall -Wextra -Wpedantic -Werror` 下必须干净通过：

   ```bash
   cmake --preset werror && cmake --build --preset werror
   ```

2. **测试全绿**，并且新增功能要带新增断言。
   测试是自研的极简框架（`tests/mb_test.h`，无第三方依赖），不要引入外部测试库。

3. **两个 port 都要能编译**。任何改动 `include/`、`src/` 的提交，
   至少要在 `MB_OS_POSIX`（或 WIN32）和 `MB_OS_NONE` 下各编译一次：

   ```bash
   cmake -S . -B b1 -DMB_OS=none -DMB_BUILD_TESTS=OFF -DMB_BUILD_EXAMPLES=OFF
   cmake --build b1
   ```

   改到 `port/mb_os_freertos.c` 时，请确认 FreeRTOS 侧依然满足
   `configUSE_RECURSIVE_MUTEXES = 1` 与 `configSUPPORT_DYNAMIC_ALLOCATION = 1`。

4. **不要破坏公开 API 的兼容性**。1.0.0 之后，`include/message_bus/` 下的任何
   结构体只能**在末尾追加**字段（`mb_bus_config_t` 的 `reserved[]` 就是为此预留的），
   函数签名不得修改。需要改语义时，新增一个 `*_ex()` 变体。
   相应的变更要写进 `CHANGELOG.md`。

## 代码风格

- C99（**不用** C11 特性，除了 `_Thread_local` 那处已带降级宏的用法）。
- 4 空格缩进，不用 Tab，行宽 100。`.clang-format` 已配好：

  ```bash
  clang-format -i include/message_bus/*.h src/*.c port/*.c examples/*.c tests/*.c
  ```

- 命名：公开 API 前缀 `mb_`；内部函数以 `mb_` 开头并带 `_locked` 后缀表示
  「调用时必须已持有总线锁」；类型 `mb_xxx_t`；宏 `MB_XXX`。
- **注释与文档用中文，标识符与 API 用英文。**
- **所有会被打印出来的文本一律用英文** —— `printf` / `puts` / `MB_LOG_*` / `#error` /
  CMake 的 `message()` 与 `option()` 描述，全部算在内。
  原因：不少串口终端和控制台的编码不是 UTF-8，中文会显示成乱码。
  中文只留在**注释**里，因为注释不会进终端。
- 注释写「为什么」，不写「是什么」——代码本身已经说明了是什么。

## 改核心代码时最容易踩的坑

这几条是设计上的硬约束，违反会引入难以复现的崩溃：

1. **绝对不要在持有总线锁时调用用户回调。**
   正确的顺序永远是：持锁收集/加引用 → 解锁 → 调回调 → 再加锁释放引用。
   参考 `src/mb_dispatch.c` 里的游标分批算法。

2. **回调期间对象可能被别的线程销毁**，因此投递前必须给订阅对象和节点**各加一次**
   引用计数，回调返回后才能释放。注意先取出 `sub->node` 再释放 `sub`。

3. **不要引入 `realloc`**。很多 RTOS 堆没有它，扩容一律 `malloc` + `memcpy` + `free`。

4. **不要新增全局可变状态**。所有状态必须挂在 `mb_bus_t` 上，否则多条总线会互相干扰、
   多线程也不安全。

5. **新增投递路径必须收敛到 `mb_dispatch.c`**。将来若要改成异步队列/分发线程，
   只需要改这一个文件。请勿在别处直接遍历 `bus->subs` 调回调。

## 提交信息

采用 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/)：

```
feat(bus): 支持按节点名定向发布
fix(dispatch): 修复回调内退订时的引用计数泄漏
docs(architecture): 补充裸机移植的注意事项
test(topic): 补充 '#' 与 '$' 组合的边界用例
```

正文请说明**为什么**要改，而不只是改了什么。

## 提交 PR 前

- [ ] `cmake --preset werror && cmake --build --preset werror` 零告警
- [ ] `ctest --preset debug` 全绿
- [ ] 至少一个非默认 port 编译通过
- [ ] 新功能有对应测试；行为变更已更新 `CHANGELOG.md`
- [ ] 涉及公开 API 的改动已同步 `docs/api.md`
- [ ] 若改动影响线程模型/生命周期，已同步 `docs/architecture.md`

## 许可

提交代码即表示你同意以本项目的 [MIT 许可证](LICENSE) 发布你的贡献。
