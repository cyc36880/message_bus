/**
 * @file mb_async.c
 * @brief 异步投递：按主题分组的队列，以及由单个专用线程驱动的 pump。
 *
 * ── 和同步投递的关系 ────────────────────────────────────────────────────
 * 同步投递（mb_node_publish）里，回调在**发布者的线程**上跑，publish() 返回时
 * 一切都已经处理完。异步投递把这件工作挪到另一个线程：
 *
 *     发布者线程                      pump 线程
 *     ──────────                     ──────────
 *     publish_async()  ──入队──►  [ a/b: 1, 2 ]
 *                                  [ a/c: 7    ]  ──► mb_bus_pump()
 *                                                        │ 逐条取出
 *                                                        ▼
 *                                                   mb_dispatch_message()
 *                                                   （订阅语义与同步完全一致）
 *
 * 因此发布者不会被订阅者拖慢，代价是消息要**深拷贝**一份（同步投递是零拷贝的），
 * 以及投递时机不再可控 —— 取决于 pump 线程什么时候跑。
 * ───────────────────────────────────────────────────────────────────────
 *
 * ── 队列结构 ────────────────────────────────────────────────────────────
 * 两级：总线上一张主题条目表，每个条目自带一条定长环形队列。
 *   - "a/b"、"a/c" 是两个主题条目；
 *   - 往 "a/b" 连发内容 1、2，则 a/b 这条条目里积压两条。
 * 一个主题积压再多也不会挤掉别的主题的位置，这是分两级而不是用一条扁平队列的
 * 唯一理由。
 *
 * ── 信号量账本（改这个文件时最容易踩的坑） ──────────────────────────────
 *   entry->space   计数恒等于「该条目还剩几个空位」= 深度 - count
 *   pool->free_entries 计数恒等于「还能新建几个主题条目」= 容量 - entry_count
 * 每次 push 消耗一个空位、每次 pop 归还一个；每次建条目消耗一个名额、
 * 每次释放条目归还一个。任何一条路径漏掉或重复，队列就会凭空变满/变空，
 * 而且是**静默**的 —— 所以每个信号量操作要么断言结果，要么显式处理失败。
 */

#include "mb_internal.h"

#if MB_CONFIG_ASYNC_MAX_TOPICS > 0

/** 每个主题条目的队列深度。 */
#define ASYNC_DEPTH MB_CONFIG_ASYNC_QUEUE_DEPTH

/* -------------------------------------------------------------------------
 * 小节：条目表维护（带 _locked 后缀的一律要求已持有总线锁）
 * ---------------------------------------------------------------------- */

static mb_async_entry_t *entry_find_locked(mb_bus_t *bus, const char *topic)
{
    size_t i;

    for (i = 0; i < bus->async.entry_count; ++i) {
        if (strcmp(bus->async.entries[i]->topic, topic) == 0) {
            return bus->async.entries[i];
        }
    }
    return NULL;
}

/**
 * 懒分配条目表与「空条目」信号量。
 *
 * 刻意不在 mb_bus_create() 里分配：不用异步投递的工程（现有的全部用法）
 * 一个字节的 RAM 都不该多花。与 retained 表「用到才分配」的做法一致。
 */
static bool pool_ensure_locked(mb_bus_t *bus)
{
    mb_async_entry_t **entries;
    mb_sem_t *free_entries;

    if (bus->async.entries != NULL) {
        return true;
    }

    entries = (mb_async_entry_t **)mb_os_calloc(MB_CONFIG_ASYNC_MAX_TOPICS, sizeof(*entries));
    free_entries = mb_os_sem_create(MB_CONFIG_ASYNC_MAX_TOPICS, MB_CONFIG_ASYNC_MAX_TOPICS);
    if (entries == NULL || free_entries == NULL) {
        MB_LOG_ERROR("async", "failed to create async queue on bus '%s'", bus->name);
        mb_os_free(entries);
        mb_os_sem_destroy(free_entries);
        return false;
    }

    bus->async.entries = entries;
    bus->async.entry_capacity = MB_CONFIG_ASYNC_MAX_TOPICS;
    bus->async.free_entries = free_entries;
    MB_LOG_DEBUG("async", "async queue created: %d topics x %d deep",
                 (int)MB_CONFIG_ASYNC_MAX_TOPICS, (int)ASYNC_DEPTH);
    return true;
}

/** 新建一个主题条目。调用方必须已从 free_entries 拿到一个名额。 */
static mb_async_entry_t *entry_create_locked(mb_bus_t *bus, const char *topic)
{
    mb_async_entry_t *entry = (mb_async_entry_t *)mb_os_calloc(1, sizeof(*entry));

    if (entry == NULL) {
        return NULL;
    }
    entry->topic = mb_strdup(topic);
    entry->space = mb_os_sem_create(ASYNC_DEPTH, ASYNC_DEPTH);
    if (entry->topic == NULL || entry->space == NULL) {
        mb_os_free(entry->topic);
        mb_os_sem_destroy(entry->space);
        mb_os_free(entry);
        return NULL;
    }

    MB_CONFIG_ASSERT(bus->async.entry_count < bus->async.entry_capacity);
    bus->async.entries[bus->async.entry_count++] = entry;
    return entry;
}

/**
 * 释放一个已经空掉的条目。
 *
 * @warning 调用方必须保证 `count == 0 && waiters == 0`。只要还有发布者阻塞在
 *          `entry->space` 上就把条目（连同信号量）销毁，那个发布者醒来时访问的
 *          就是已释放的内存。
 */
static void entry_release_locked(mb_bus_t *bus, mb_async_entry_t *entry)
{
    size_t i;

    MB_CONFIG_ASSERT(entry->count == 0);
    MB_CONFIG_ASSERT(entry->waiters == 0);

    for (i = 0; i < bus->async.entry_count; ++i) {
        if (bus->async.entries[i] == entry) {
            break;
        }
    }
    MB_CONFIG_ASSERT(i < bus->async.entry_count);
    if (i < bus->async.entry_count) {
        if (i + 1 < bus->async.entry_count) {
            memmove(&bus->async.entries[i], &bus->async.entries[i + 1],
                    (bus->async.entry_count - i - 1) * sizeof(bus->async.entries[0]));
        }
        bus->async.entry_count--;
    }

    mb_os_free(entry->topic);
    mb_os_sem_destroy(entry->space);
    mb_os_free(entry);

    /* 归还一个主题条目名额，唤醒正等「总线满了」的发布者 */
    MB_CONFIG_ASSERT(bus->async.free_entries != NULL);
    (void)mb_os_sem_signal(bus->async.free_entries);
}

/** 把所有已经空掉、且没有发布者在等的条目释放掉。锁内调用。 */
static void entries_release_empty_locked(mb_bus_t *bus)
{
    size_t i = 0;

    while (i < bus->async.entry_count) {
        mb_async_entry_t *entry = bus->async.entries[i];

        if (entry->count == 0 && entry->waiters == 0) {
            entry_release_locked(bus, entry); /* 会前移元素，因此 i 不自增 */
            continue;
        }
        ++i;
    }
}

/** waiters 减一；条目如果已经空了就顺手释放。锁内调用。 */
static void entry_release_waiter_locked(mb_bus_t *bus, mb_async_entry_t *entry)
{
    MB_CONFIG_ASSERT(entry->waiters > 0);
    entry->waiters--;
    if (entry->count == 0 && entry->waiters == 0) {
        entry_release_locked(bus, entry);
    }
}

/** 同上，但自动加锁，供锁外路径使用。 */
static void entry_release_waiter(mb_bus_t *bus, mb_async_entry_t *entry)
{
    MB_BUS_LOCK(bus);
    entry_release_waiter_locked(bus, entry);
    MB_BUS_UNLOCK(bus);
}

/**
 * 取到某个主题的条目，并把它的 waiters 加一。
 *
 * 「waiters」是条目在本线程手里的凭证：只要它大于 0，条目就不会被释放，
 * 因此下面在锁外做的阻塞等待（等空位）才是安全的。
 * 成功返回后，调用方**必须**最终调用 entry_release_waiter*() 归还。
 *
 * @param out_entry 成功时写入条目指针。
 */
static mb_err_t entry_acquire(mb_bus_t *bus, const char *topic, uint32_t timeout_ms,
                              mb_async_entry_t **out_entry)
{
    mb_async_entry_t *entry;
    mb_sem_t *free_entries;

    MB_BUS_LOCK(bus);
    if (bus->destroying) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_STATE;
    }
    if (!pool_ensure_locked(bus)) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_NO_MEMORY;
    }
    entry = entry_find_locked(bus, topic);
    if (entry != NULL) {
        entry->waiters++;
        MB_BUS_UNLOCK(bus);
        *out_entry = entry;
        return MB_OK;
    }
    free_entries = bus->async.free_entries;
    MB_BUS_UNLOCK(bus);

    /* 这是一个还没见过的主题：先申请一个条目名额。
     * 「总线满了」时在这里等 —— 这正是 timeout_ms 的用武之地。 */
    if (!mb_os_sem_wait(free_entries, timeout_ms)) {
        return MB_ERR_TIMEOUT;
    }

    MB_BUS_LOCK(bus);
    if (bus->destroying) {
        MB_BUS_UNLOCK(bus);
        (void)mb_os_sem_signal(free_entries);
        return MB_ERR_STATE;
    }
    /* 等待期间可能有别的线程把同一个主题建好了 */
    entry = entry_find_locked(bus, topic);
    if (entry != NULL) {
        (void)mb_os_sem_signal(free_entries); /* 多要了一个名额，还回去 */
    } else {
        entry = entry_create_locked(bus, topic);
        if (entry == NULL) {
            MB_BUS_UNLOCK(bus);
            (void)mb_os_sem_signal(free_entries);
            return MB_ERR_NO_MEMORY;
        }
    }
    entry->waiters++;
    MB_BUS_UNLOCK(bus);

    *out_entry = entry;
    return MB_OK;
}

/** 记一次「没能入队」。锁外调用。 */
static void count_dropped(mb_bus_t *bus)
{
    MB_BUS_LOCK(bus);
    bus->stats.async_dropped++;
    MB_BUS_UNLOCK(bus);
}

/* -------------------------------------------------------------------------
 * 小节：异步发布
 * ---------------------------------------------------------------------- */

mb_err_t mb_bus_publish_async_internal(mb_bus_t *bus,
                                       const char *source,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       const mb_publish_opts_t *opts,
                                       uint32_t timeout_ms)
{
    mb_publish_opts_t options;
    mb_async_entry_t *entry = NULL;
    mb_owned_message_t *owned;
    mb_message_t view;
    uint8_t msg_flags;
    bool overwrite;
    bool have_token = false;
    mb_err_t err;

    if (bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    /* 校验与同步路径共用同一个函数，保证两条路的报错完全一致 */
    err = mb_publish_prepare(topic, payload, payload_len, opts, &options, &msg_flags);
    if (err != MB_OK) {
        return err;
    }

    /* 从 pump 线程发起**无上限**的等待，是个自相矛盾的请求：能给这个主题归还
     * 空位的只有 pump 自己，而它此刻正卡在你的回调里，永远不会回来。提前拒绝，
     * 而不是让调用方挂到天荒地老 —— 那是连超时都没机会报的挂死。
     *
     * 只拦 MB_WAIT_FOREVER，不拦有限超时：
     *   - 有限超时不会真死锁，拿不到就如实返回 MB_ERR_TIMEOUT（计 async_dropped）；
     *   - 回调里往「另一个还有空位的主题」发布是完全正常的用法，那种调用根本
     *     不会阻塞，一刀切会把这些本来正确的代码一起挡掉。
     * 而 MB_WAIT_FOREVER 表达的是「宁可一直等也不要丢」，从 pump 线程看这句
     * 话无法兑现：队列空时不需要等，队列满时等不到。所以这里报的是死锁，
     * 不是超时。 */
    if (MB_OS_CAN_BLOCK && timeout_ms == MB_WAIT_FOREVER) {
        bool from_pump;

        MB_BUS_LOCK(bus);
        from_pump = bus->pumping && (bus->pump_owner == mb_os_thread_id());
        MB_BUS_UNLOCK(bus);

        if (from_pump) {
            MB_LOG_WARN("async", "mb_node_publish_async(MB_WAIT_FOREVER) called from "
                                 "inside a pump callback on bus '%s'; the pump is the "
                                 "only one who can free a slot, so this would hang "
                                 "forever. Use MB_WAIT_NONE or the overwrite strategy.",
                        bus->name);
            return MB_ERR_WOULD_DEADLOCK;
        }
    }

    /* 条目满时丢弃最旧的一条，而不是等。编译期默认值可以被单次调用的标志覆盖。 */
    overwrite = ((options.flags & MB_PUB_FLAG_ASYNC_OVERWRITE) != 0) ||
                (MB_CONFIG_ASYNC_OVERWRITE_OLDEST != 0);

    /* 深拷贝必须在这里做：本函数返回后调用方随时可以释放自己的缓冲区，
     * 而真正的投递要等到另一个线程调用 mb_bus_pump() 时才发生。
     * 放在锁外是为了不让堆分配与 memcpy 拖长临界区。 */
    owned = mb_owned_message_create(topic, payload, payload_len, source, 0, 0, msg_flags);
    if (owned == NULL) {
        return MB_ERR_NO_MEMORY;
    }

    err = entry_acquire(bus, topic, timeout_ms, &entry);
    if (err != MB_OK) {
        mb_owned_message_free(owned);
        if (err == MB_ERR_TIMEOUT) {
            count_dropped(bus);
        }
        return err;
    }

    if (!overwrite) {
        /* 普通路径：先占一个空位，满了就在这里等 pump 把这个主题的消息取走。
         * 等待必须发生在锁外 —— 可能睡很久，而归还空位的正是 pump 线程，
         * 握着总线锁睡会把它一起锁死。 */
        if (!mb_os_sem_wait(entry->space, timeout_ms)) {
            entry_release_waiter(bus, entry);
            mb_owned_message_free(owned);
            count_dropped(bus);
            return MB_ERR_TIMEOUT;
        }
        have_token = true;
    }

    /* 空位令牌（entry->space）与 entry->count 的对应关系是全部记账依据：
     *
     *     space.count == ASYNC_DEPTH - count - 在途预定
     *
     * 「在途预定」= 已经领到令牌、但还没在锁内提交 count++ 的生产者。
     * 上面普通路径的等待发生在两件事中间，所以这个中间状态真实存在，
     * **谁也不能假设 count < ASYNC_DEPTH 就等于「令牌一定拿得到」**。
     * 覆盖路径原来正是这么假设的（还写了 MB_CONFIG_ASSERT），
     * 于是在混合模式下会真的断言失败 / 记账漂移 —— 这个循环就是为它写的。 */
    for (;;) {
        MB_BUS_LOCK(bus);

        if (bus->destroying) {
            MB_BUS_UNLOCK(bus);
            if (have_token) {
                (void)mb_os_sem_signal(entry->space); /* 没入队，归还刚占的空位 */
            }
            entry_release_waiter(bus, entry);
            mb_owned_message_free(owned);
            return MB_ERR_STATE;
        }

        if (overwrite && !have_token) {
            /* 覆盖路径先不阻塞地抢一个空位：抢到了就是普通入队，
             * 抢不到再谈「覆盖」还是「等」。 */
            have_token = mb_os_sem_wait(entry->space, MB_WAIT_NONE);
        }

        if (have_token) {
            /* 有令牌 ⇒ 队列必然没满（满时 space.count 为 0，抢不到），
             * 可以安全入队 —— 下面那个 count < DEPTH 的断言随之成立。 */
            break;
        }

        /* 走到这里说明 space.count == 0，即 count + 在途预定 == ASYNC_DEPTH */
        if (entry->count == ASYNC_DEPTH) {
            /* 情况一：队列真的满了，扔掉最旧的一条给新消息腾位置。
             * 先退队再入队，净效果 count 不变，所以既不消耗也不归还空位。 */
            mb_owned_message_t *oldest = entry->slots[entry->head];

            entry->slots[entry->head] = NULL;
            entry->head = (entry->head + 1u) % ASYNC_DEPTH;
            entry->count--;
            bus->stats.async_overwritten++;
            mb_owned_message_free(oldest);
            break;
        }

        /* 情况二：count < DEPTH 却拿不到令牌 —— 有别的普通生产者领了空位
         * 还没提交，队列马上会被它填满。此刻既不构成「满」（不该覆盖丢消息），
         * 也没空位可占。只能放锁等一个令牌：等到了说明它提交完了，
         * 回到循环开头重新判断，那时 count 已经 == DEPTH，走上面的覆盖分支。 */
        MB_BUS_UNLOCK(bus);

        /* 用调用方给的超时。覆盖路径若传 MB_WAIT_NONE 就是不等 —— 直接放弃。
         * 这个窗口很窄（另一个生产者夹在领令牌与提交之间），但等不到就必须
         * 如实返回超时，而不是硬塞进去把账记坏。 */
        if (!mb_os_sem_wait(entry->space, timeout_ms)) {
            entry_release_waiter(bus, entry);
            mb_owned_message_free(owned);
            count_dropped(bus);
            return MB_ERR_TIMEOUT;
        }
        have_token = true;
    }

    owned->id = ++bus->next_message_id;
    owned->timestamp_ms = mb_os_time_ms();
    bus->stats.published++;
    bus->stats.async_enqueued++;

    if ((options.flags & MB_PUB_FLAG_RETAIN) != 0) {
        /* retained 在**入队时**就更新（与同步路径「发布时即成为当前状态」一致），
         * 这样即使队列还没被 pump 到，后来的订阅者也能立刻拿到这个值。 */
        mb_owned_message_view(owned, &view);
        mb_bus_retain_locked(bus, &view);
    }

    MB_CONFIG_ASSERT(entry->count < ASYNC_DEPTH);
    entry->slots[(entry->head + entry->count) % ASYNC_DEPTH] = owned;
    entry->count++;

    entry_release_waiter_locked(bus, entry);
    MB_BUS_UNLOCK(bus);

    MB_LOG_DEBUG("async", "queued '%s' (%u bytes) on bus '%s'",
                 topic, (unsigned)payload_len, bus->name);
    return MB_OK;
}

/* -------------------------------------------------------------------------
 * 小节：pump —— 异步队列的唯一消费点
 * ---------------------------------------------------------------------- */

/**
 * 取一批待投递的消息出来。锁内调用。
 *
 * 每个主题条目**最多取一条**，并在各条目之间轮转（next_entry 游标），
 * 这样某个刷屏的主题不会把别的主题饿死。条目内部仍然是严格 FIFO。
 */
static size_t take_batch_locked(mb_bus_t *bus, mb_owned_message_t **batch, size_t max)
{
    mb_async_pool_t *pool = &bus->async;
    size_t n = 0;
    size_t scanned = 0;

    if (pool->entries == NULL || pool->entry_count == 0) {
        return 0;
    }
    if (pool->next_entry >= pool->entry_count) {
        pool->next_entry = 0;
    }

    while (scanned < pool->entry_count && n < max) {
        size_t index = pool->next_entry;
        mb_async_entry_t *entry = pool->entries[index];
        bool returned;

        pool->next_entry = (index + 1) % pool->entry_count;
        ++scanned;

        if (entry->count == 0) {
            continue;
        }

        batch[n++] = entry->slots[entry->head];
        entry->slots[entry->head] = NULL;
        entry->head = (entry->head + 1u) % ASYNC_DEPTH;
        entry->count--;

        /* 归还空位，唤醒可能正在等的发布者。信号量释放是非阻塞的，
         * 所以可以在锁内做 —— 让发布者尽早开工，不必等我们跑完回调。 */
        returned = mb_os_sem_signal(entry->space);
        MB_CONFIG_ASSERT(returned);
        (void)returned;
    }
    return n;
}

mb_err_t mb_bus_pump(mb_bus_t *bus)
{
    mb_owned_message_t *batch[MB_CONFIG_DELIVER_BATCH];

    if (bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    MB_BUS_LOCK(bus);
    if (bus->destroying) {
        MB_BUS_UNLOCK(bus);
        return MB_ERR_STATE;
    }
    if (bus->pumping) {
        /* 异步队列只允许**一个**线程消费：两个线程同时 pump 会让同一个订阅者的
         * 回调被并发进入，而库对回调只承诺「不会重入同一条投递路径」，不承诺
         * 「回调之间互斥」。与其让用户代码莫名其妙地崩，不如直接拒绝。 */
        MB_BUS_UNLOCK(bus);
        MB_LOG_WARN("async", "mb_bus_pump() called again while another call is in "
                             "progress on bus '%s'; it must be driven by exactly "
                             "one thread", bus->name);
        return MB_ERR_BUSY;
    }
    bus->pumping = true;
    bus->pump_owner = mb_os_thread_id();
    MB_BUS_UNLOCK(bus);

    for (;;) {
        size_t n;
        size_t i;

        MB_BUS_LOCK(bus);
        n = take_batch_locked(bus, batch, MB_CONFIG_DELIVER_BATCH);
        if (n > 0) {
            entries_release_empty_locked(bus);
        }
        MB_BUS_UNLOCK(bus);

        if (n == 0) {
            break; /* 队列空了：本函数不阻塞，直接返回 */
        }

        /* 锁外逐条投递 —— 走的还是同步投递那一条路，订阅语义完全不变 */
        for (i = 0; i < n; ++i) {
            mb_message_t view;
            size_t delivered = 0;
            mb_err_t err;

            mb_owned_message_view(batch[i], &view);
            err = mb_dispatch_message(bus, &view, &delivered);

            if (err == MB_OK && delivered == 0) {
                MB_BUS_LOCK(bus);
                bus->stats.no_subscriber++;
                MB_BUS_UNLOCK(bus);
                MB_LOG_DEBUG("async", "no matching subscriber for topic '%s'", view.topic);
            }
            mb_owned_message_free(batch[i]);
        }
    }

    MB_BUS_LOCK(bus);
    bus->pumping = false;
    bus->pump_owner = (mb_thread_id_t)0;
    MB_BUS_UNLOCK(bus);
    return MB_OK;
}

/* -------------------------------------------------------------------------
 * 小节：查询与清理
 * ---------------------------------------------------------------------- */

size_t mb_bus_async_pending(const mb_bus_t *bus)
{
    size_t total = 0;
    size_t i;

    if (bus == NULL) {
        return 0;
    }

    MB_BUS_LOCK((mb_bus_t *)bus);
    for (i = 0; i < bus->async.entry_count; ++i) {
        total += bus->async.entries[i]->count;
    }
    MB_BUS_UNLOCK((mb_bus_t *)bus);
    return total;
}

size_t mb_bus_async_topic_count(const mb_bus_t *bus)
{
    size_t count;

    if (bus == NULL) {
        return 0;
    }

    MB_BUS_LOCK((mb_bus_t *)bus);
    count = bus->async.entry_count;
    MB_BUS_UNLOCK((mb_bus_t *)bus);
    return count;
}

void mb_async_pool_dispose(mb_bus_t *bus)
{
    size_t i;

    if (bus->async.entries == NULL) {
        return;
    }

    for (i = 0; i < bus->async.entry_count; ++i) {
        mb_async_entry_t *entry = bus->async.entries[i];
        uint32_t j;

        /* 队列里还没被 pump 取走的消息也要释放，否则每条都泄漏 */
        for (j = 0; j < ASYNC_DEPTH; ++j) {
            mb_owned_message_free(entry->slots[j]);
        }
        mb_os_free(entry->topic);
        mb_os_sem_destroy(entry->space);
        mb_os_free(entry);
    }

    mb_os_free(bus->async.entries);
    mb_os_sem_destroy(bus->async.free_entries);
    bus->async.entries = NULL;
    bus->async.entry_count = 0;
    bus->async.entry_capacity = 0;
    bus->async.next_entry = 0;
    bus->async.free_entries = NULL;
}

/* -------------------------------------------------------------------------
 * 小节：总线级异步发布
 * ---------------------------------------------------------------------- */

mb_err_t mb_bus_publish_async(mb_bus_t *bus,
                              const char *topic,
                              const void *payload,
                              size_t payload_len,
                              const mb_publish_opts_t *opts,
                              uint32_t timeout_ms)
{
    return mb_bus_publish_async_internal(bus, NULL, topic, payload, payload_len, opts, timeout_ms);
}

#else /* MB_CONFIG_ASYNC_MAX_TOPICS == 0：编译期关闭异步功能 */

void mb_async_pool_dispose(mb_bus_t *bus)
{
    (void)bus;
}

mb_err_t mb_bus_publish_async_internal(mb_bus_t *bus,
                                       const char *source,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       const mb_publish_opts_t *opts,
                                       uint32_t timeout_ms)
{
    (void)bus;
    (void)source;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    return MB_ERR_UNSUPPORTED;
}

mb_err_t mb_bus_publish_async(mb_bus_t *bus, const char *topic, const void *payload,
                              size_t payload_len, const mb_publish_opts_t *opts,
                              uint32_t timeout_ms)
{
    (void)bus;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    return MB_ERR_UNSUPPORTED;
}

mb_err_t mb_bus_pump(mb_bus_t *bus)
{
    (void)bus;
    return MB_ERR_UNSUPPORTED;
}

size_t mb_bus_async_pending(const mb_bus_t *bus)
{
    (void)bus;
    return 0;
}

size_t mb_bus_async_topic_count(const mb_bus_t *bus)
{
    (void)bus;
    return 0;
}

#endif /* MB_CONFIG_ASYNC_MAX_TOPICS */
