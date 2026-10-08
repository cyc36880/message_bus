# FreeRTOS 头文件桩

这**不是** FreeRTOS，也**不能**用来跑任何东西。它唯一的用途是让 CI 能在没有
真实 FreeRTOS 的 Linux 机器上**编译** `port/mb_os_freertos.c`，从而保证这个
port 至少语法正确、符号齐全。

## 为什么是文件而不是 CI 里现写的 heredoc

这套桩以前直接写在 `.github/workflows/ci.yml` 的 `cat > ... <<'EOF'` 里。
结果是它和 port 一起演进时没人看得见：异步投递往 port 层加了计数信号量，
桩没跟着加，CI 才在 `implicit-function-declaration` 上挂掉。放进仓库以后，
改 port 时桩的缺失会出现在同一个 diff 里。

## 维护约定

**port 源码里每出现一个新的 FreeRTOS API 或类型，这里就要补一条声明。**
判断方法：CI 的 `交叉 port 编译（freertos / none）` job 就是唯一的验收标准 ——
它挂了就是这里漏了。

这里只做「声明存在」，不做「行为正确」：

- 所有函数**只有声明，没有定义**。这个桩只参与编译（`-c`），从不链接，
  所以链接期永远不会去找它们的实现。
- 配置宏按「启用全部可选特性」写（递归互斥量、动态分配、计数信号量），
  这样 port 里那些 `#error` 守卫走的是「配置正确」那条分支。
  要验证守卫本身会不会正确报错，得把对应宏改成 0，而不是改这里。

## 与真实 FreeRTOS 的布局差异

真实 FreeRTOS 各移植版的头文件布局并不统一（ESP-IDF 与 Arduino-ESP32 放在
`freertos/` 子目录下），`mb_os_freertos.c` 用 `__has_include` 兼容了这一点。
本桩一律用**平铺**布局，即 `FreeRTOS.h` / `task.h` / `semphr.h` 与其它头文件同级，
对应 `__has_include("freertos/task.h")` 为假的那条分支。

类型的归属尽量照抄真实 FreeRTOS：`BaseType_t` / `UBaseType_t` / `TickType_t`
在 `portmacro.h`，`pdTRUE` / `pdFALSE` 在 `projdefs.h`。
