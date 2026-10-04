/**
 * @file mb_conf_template.h
 * @brief message_bus 工程级配置模板（LVGL 的 lv_conf.h 风格）。
 *
 * ── 用法 ────────────────────────────────────────────────────────────────
 * 1. 把本文件复制到你工程的某个 include 目录下，例如 `mb_conf.h`；
 * 2. 在编译选项中加上：
 *        -DMB_CONF_PATH="\"mb_conf.h\""
 *    （CMake：target_compile_definitions(app PRIVATE MB_CONF_PATH="\"mb_conf.h\"")）
 * 3. 按需修改下面的宏。未定义的项会自动回落到 mb_config.h 里的默认值，
 *    因此**可以只写你关心的那几项**。
 *
 * 这份文件不会进入库的编译，只影响你自己的工程，升级库时不会被覆盖。
 * ───────────────────────────────────────────────────────────────────────
 */
#ifndef MB_CONF_H
#define MB_CONF_H

/*===========================================================================
 * OS 抽象层：决定 port/ 下哪个实现文件参与编译
 *
 *   MB_OS_NONE      裸机 / 无 OS（需要你自己提供临界区宏，见文件末尾）
 *   MB_OS_FREERTOS  FreeRTOS（编译 port/mb_os_freertos.c）
 *   MB_OS_POSIX     Linux / macOS（编译 port/mb_os_posix.c）
 *   MB_OS_WIN32     Windows（编译 port/mb_os_win32.c）
 *
 * PC 模拟器构建时不用写这一项：CMake 会自动按平台选择。
 * MCU 工程里必须显式写成 MB_OS_FREERTOS 或 MB_OS_NONE。
 *===========================================================================*/
/* #define MB_CONFIG_OS MB_OS_FREERTOS */

/*===========================================================================
 * 容量限制
 *===========================================================================*/

/** 主题字符串最大长度（含 '\0'）。内存紧张时可降到 64。 */
/* #define MB_CONFIG_MAX_TOPIC_LEN 128 */

/** 节点名最大长度（含 '\0'）。 */
/* #define MB_CONFIG_MAX_NAME_LEN 32 */

/** 单条消息 payload 最大字节数；0 = 不限制。
 *  嵌入式建议设一个上限（例如 256），避免一条大消息吃光堆。 */
/* #define MB_CONFIG_MAX_PAYLOAD_SIZE 0 */

/*===========================================================================
 * 运行期行为
 *===========================================================================*/

/** 每批投递的订阅者数量，只影响栈占用。 */
/* #define MB_CONFIG_DELIVER_BATCH 8 */

/** 同步投递的最大递归深度；0 = 不限制（危险）。
 *  回调里再发布同一主题会递归，这个上限防止栈溢出。 */
/* #define MB_CONFIG_MAX_DISPATCH_DEPTH 8 */

/*===========================================================================
 * 日志
 *
 *   0 = 关闭   1 = ERROR   2 = WARN   3 = INFO   4 = DEBUG
 * 低于该等级的日志在编译期就被消除，不占 Flash 也不占 RAM。
 *===========================================================================*/
/* #define MB_CONFIG_LOG_LEVEL 2 */

/** 内部断言与参数检查；量产固件可以设为 0 以省空间。 */
/* #define MB_CONFIG_ENABLE_CHECKS 1 */

/*===========================================================================
 * 裸机（MB_OS_NONE）专用钩子
 *
 * 只在 MB_CONFIG_OS == MB_OS_NONE 时生效。
 * 临界区必须是可嵌套（递归）语义 —— 库内部存在嵌套加锁的路径。
 *===========================================================================*/
/*
#define MB_CONFIG_CRITICAL_ENTER()     __disable_irq()
#define MB_CONFIG_CRITICAL_EXIT()      __enable_irq()
*/

/** 毫秒计数，通常接到 SysTick 上（库只用它给消息打时间戳）。 */
/* #define MB_CONFIG_TIME_MS()            (systick_ms) */

/** 毫秒级睡眠（库内部目前不调用，预留给用户）。 */
/* #define MB_CONFIG_SLEEP_MS(ms)         delay_ms(ms) */

/** 内存钩子：可以全部换成静态内存池，实现零动态分配。 */
/* #define MB_CONFIG_MALLOC(size)          my_pool_alloc(size) */
/* #define MB_CONFIG_CALLOC(count, size)   my_pool_calloc(count, size) */
/* #define MB_CONFIG_FREE(ptr)             my_pool_free(ptr) */

#endif /* MB_CONF_H */
