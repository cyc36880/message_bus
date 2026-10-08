/**
 * @file mb_config.h
 * @brief message_bus 编译期配置（LVGL 风格）。
 *
 * 所有配置项都写成 `#ifndef xxx / #define xxx / #endif` 的形式，
 * 因此有三种覆盖方式，优先级由低到高：
 *
 *   1. 本文件的默认值；
 *   2. 编译命令行的 -D 参数，例如 `-DMB_CONFIG_MAX_TOPIC_LEN=256`；
 *   3. 外部配置文件：`-DMB_CONF_PATH="\"my_mb_conf.h\""`，
 *      或在工程里定义 MB_CONF_PATH 指向配置文件（参考 config/mb_conf_template.h）。
 *
 * 推荐嵌入式工程把 config/mb_conf_template.h 复制到工程根目录并按需修改，
 * 再通过 MB_CONF_PATH 引入，这样库代码保持不动。
 */
#ifndef MESSAGE_BUS_MB_CONFIG_H
#define MESSAGE_BUS_MB_CONFIG_H

/* -------------------------------------------------------------------------
 * 外部配置文件（可选）
 * ---------------------------------------------------------------------- */
#ifdef MB_CONF_PATH
#include MB_CONF_PATH
#endif

/* -------------------------------------------------------------------------
 * OS 抽象层选择
 *
 * MB_CONFIG_OS 决定 port/ 目录下哪一个实现文件参与编译：
 *   MB_OS_NONE     裸机 / 无 OS：不加锁（或使用用户提供的临界区宏），无线程
 *   MB_OS_FREERTOS FreeRTOS：递归互斥量 + xTaskGetTickCount
 *   MB_OS_POSIX    Linux / macOS / MSYS2：pthread
 *   MB_OS_WIN32    Windows：CRITICAL_SECTION
 *
 * 裸机下总线依然是「多线程安全」的，只是安全性由用户提供的临界区保证，
 * 详见 docs/porting.md。
 * ---------------------------------------------------------------------- */
#ifndef MB_OS_NONE
#define MB_OS_NONE 0
#endif
#ifndef MB_OS_FREERTOS
#define MB_OS_FREERTOS 1
#endif
#ifndef MB_OS_POSIX
#define MB_OS_POSIX 2
#endif
#ifndef MB_OS_WIN32
#define MB_OS_WIN32 3
#endif

/* 默认不启用任何 OS；CMake 会按平台自动传入 MB_OS_WIN32 / MB_OS_POSIX。
 * 嵌入式工程请在 mb_conf.h 中显式写 MB_CONFIG_OS = MB_OS_FREERTOS。 */
#ifndef MB_CONFIG_OS
#define MB_CONFIG_OS MB_OS_NONE
#endif

/* -------------------------------------------------------------------------
 * 容量限制（0 表示不限制）
 * ---------------------------------------------------------------------- */

/** 主题字符串最大长度，含结尾 '\0'。 */
#ifndef MB_CONFIG_MAX_TOPIC_LEN
#define MB_CONFIG_MAX_TOPIC_LEN 128
#endif

/** 节点名称最大长度，含结尾 '\0'。 */
#ifndef MB_CONFIG_MAX_NAME_LEN
#define MB_CONFIG_MAX_NAME_LEN 32
#endif

/** 单条消息 payload 的最大字节数；0 表示不限制。 */
#ifndef MB_CONFIG_MAX_PAYLOAD_SIZE
#define MB_CONFIG_MAX_PAYLOAD_SIZE 0
#endif

/* -------------------------------------------------------------------------
 * 异步投递（队列 + pump 线程）
 *
 * 异步队列是**两级**结构：
 *   - 总线里最多容纳 MB_CONFIG_ASYNC_MAX_TOPICS 个**主题条目**
 *     （"a/b"、"a/c" 各占一个条目）；
 *   - 每个条目内是一条深度为 MB_CONFIG_ASYNC_QUEUE_DEPTH 的 FIFO
 *     （往 "a/b" 连发内容 1、2，该条目里就积压两条）。
 *
 * 把 MB_CONFIG_ASYNC_MAX_TOPICS 设为 0 即可在编译期完全去掉异步功能，
 * 此时 mb_node_publish_async() / mb_bus_pump() 一律返回 MB_ERR_UNSUPPORTED，
 * 且不产生任何代码与 RAM 开销。
 * ---------------------------------------------------------------------- */

/** 异步队列能容纳的主题条目数；0 = 编译期关闭异步功能。 */
#ifndef MB_CONFIG_ASYNC_MAX_TOPICS
#define MB_CONFIG_ASYNC_MAX_TOPICS 16
#endif

/** 每个主题条目内允许积压的消息条数。 */
#ifndef MB_CONFIG_ASYNC_QUEUE_DEPTH
#define MB_CONFIG_ASYNC_QUEUE_DEPTH 4
#endif

/**
 * 主题条目满时的**默认**策略：
 *   0 = 阻塞等待（最多等发布时传入的 timeout_ms，超时返回 MB_ERR_TIMEOUT）
 *   1 = 丢弃该条目里最旧的一条，给新消息腾位置（永不阻塞）
 *
 * 单次发布可以用 MB_PUB_FLAG_ASYNC_OVERWRITE 覆盖这里的默认值。
 * 「状态」类主题（传感器当前值）建议用覆盖：消费端慢时只保留最新值；
 * 「命令/事件」类主题必须用等待或报错，丢弃会丢动作。
 */
#ifndef MB_CONFIG_ASYNC_OVERWRITE_OLDEST
#define MB_CONFIG_ASYNC_OVERWRITE_OLDEST 0
#endif

#if MB_CONFIG_ASYNC_MAX_TOPICS > 0 && MB_CONFIG_ASYNC_QUEUE_DEPTH < 1
#error "MB_CONFIG_ASYNC_QUEUE_DEPTH must be >= 1 when async is enabled"
#endif

/* -------------------------------------------------------------------------
 * 运行期行为
 * ---------------------------------------------------------------------- */

/**
 * 一次匹配循环中批量取出的订阅者数量。
 * 只影响栈占用（每个指针一个槽），不影响正确性：
 * 一批回调结束后会继续扫描下一批。
 */
#ifndef MB_CONFIG_DELIVER_BATCH
#define MB_CONFIG_DELIVER_BATCH 8
#endif

/* 必须 >= 1：投递循环用「本批是否取满」判断是否还有下一批，
 * 为 0 时永远取不满，会死循环。 */
#if MB_CONFIG_DELIVER_BATCH < 1
#error "MB_CONFIG_DELIVER_BATCH must be >= 1"
#endif

/**
 * 同步投递的最大递归深度。
 *
 * 回调里再 publish 同一主题会形成递归；超过该深度时消息被丢弃、
 * 计入 stats.dropped 并返回 MB_ERR_BUSY，避免栈溢出。
 * 0 表示不做限制（危险，仅在你确定无环时使用）。
 */
#ifndef MB_CONFIG_MAX_DISPATCH_DEPTH
#define MB_CONFIG_MAX_DISPATCH_DEPTH 8
#endif

/**
 * 编译期日志等级：0=关闭 1=ERROR 2=WARN 3=INFO 4=DEBUG。
 * 低于该等级的日志在编译期就被完全消除，不产生任何代码或字符串常量。
 */
#ifndef MB_CONFIG_LOG_LEVEL
#define MB_CONFIG_LOG_LEVEL 2
#endif

/**
 * 是否启用内部断言（mb_assert）与参数检查。
 * Release 构建可设为 0 以节省空间；参数检查被关闭时传入非法参数属于未定义行为。
 */
#ifndef MB_CONFIG_ENABLE_CHECKS
#define MB_CONFIG_ENABLE_CHECKS 1
#endif

/* -------------------------------------------------------------------------
 * 内部断言
 * ---------------------------------------------------------------------- */
#ifndef MB_CONFIG_ASSERT
#if MB_CONFIG_ENABLE_CHECKS
#include <assert.h>
#define MB_CONFIG_ASSERT(cond) assert(cond)
#else
#define MB_CONFIG_ASSERT(cond) ((void)0)
#endif
#endif

#endif /* MESSAGE_BUS_MB_CONFIG_H */
