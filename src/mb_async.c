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
 *
 * 除了 pump 的 pop，**await 超时撤销**（queue_remove_by_ack_locked）也会让
 * count 减一，因此它必须同时归还一个空位 —— 两条路径共用「count-- 就 signal」
 * 的规则，改其中一条时别忘了另一条。
 */

#include "mb_internal.h"

#if MB_CONFIG_ASYNC_MAX_TOPICS > 0

/** 每个主题条目的队列深度。 */
#define ASYNC_DEPTH MB_CONFIG_ASYNC_QUEUE_DEPTH

/* -------------------------------------------------------------------------
 * 小节：异步回执（await 发布）
 *
 * 回执的生命周期与记账规则：
 *   - 创建时 refcnt = 1，代表发布方手里的那一份；
 *   - 消息真正入队时 refcnt++，代表队列里的消息那一份；
 *   - 投递结束 / 被撤销 / 被覆盖 / 总线销毁时调用 ack_finish_locked()，
 *     它只结束一次（completed 标志），并归还消息那一份引用；
 *   - 发布方醒来后归还自己那一份，归零才 free。
 * 所有函数（除 ack_create）都要求持有总线锁。
 * ---------------------------------------------------------------------- */

static mb_async_ack_t *ack_create(bool need_done)
{
    mb_async_ack_t *ack = (mb_async_ack_t *)mb_os_calloc(1, sizeof(*ack));

    if (ack == NULL) {
        return NULL;
    }
    if (need_done) {
        ack->done = mb_os_sem_create(0, 1);
        if (ack->done == NULL) {
            mb_os_free(ack);
            return NULL;
        }
    }
    ack->refcnt = 1; /* 发布方 / 完成通知 */
    ack->status = MB_OK;
    return ack;
}

/** 锁内：归还一份引用；归零时销毁。 */
static void ack_release_locked(mb_async_ack_t *ack)
{
    MB_CONFIG_ASSERT(ack->refcnt > 0);

    ack->refcnt--;
    if (ack->refcnt > 0) {
        return;
    }
    if (ack->done != NULL) {
        mb_os_sem_destroy(ack->done);
    }
    mb_os_free(ack);
}

/**
 * 锁内：结束回执（只结束一次）并归还消息持有的引用。
 *
 * @param status 结束原因；MB_OK 表示消息已被正常投递（delivered 有效）。
 */
static void ack_finish_locked(mb_async_ack_t *ack, mb_err_t status, size_t delivered)
{
    if (!ack->completed) {
        ack->completed = true;
        ack->dispatching = false;
        ack->status = status;
        ack->delivered = delivered;
        if (ack->notify != NULL) {
            /* 非阻塞通知：把结果转写给它，由 service 在 pump 线程上回调。
             * 「没有人订阅」在通知语义里不算成功 —— 它等的是一个消费者。 */
            ack->notify->resolved = true;
            ack->notify->status =
                (status == MB_OK && delivered == 0) ? MB_ERR_NO_SUBSCRIBER : status;
            ack->notify->delivered = delivered;
        }
        if (ack->done != NULL) {
            /* await：非阻塞，可以在锁内做。发布方只会在锁外等这个信号量。 */
            (void)mb_os_sem_signal(ack->done);
        }
    }
    ack_release_locked(ack);
}

/** 锁内：丢弃一条尚未投递的消息；带回调的先把回执结束掉。 */
static void owned_message_discard_locked(mb_owned_message_t *msg, mb_err_t status)
{
    if (msg->ack != NULL) {
        mb_async_ack_t *ack = msg->ack;

        msg->ack = NULL;
        ack_finish_locked(ack, status, 0);
    }
    mb_owned_message_free(msg);
}

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

/**
 * 锁内：把某条**还在队列里**的消息按回执找出来并摘掉。
 *
 * await 超时时用它把消息当场撤销，而不是只打个标记等 pump 来清：
 * pump 可能根本没在跑（这正是超时的原因），留着它会把主题的空位一直占住，
 * 之后同一主题的发布就会莫名地跟着超时。
 *
 * @return 被摘下的消息（调用方负责释放）；没找到返回 NULL。
 */
static mb_owned_message_t *queue_remove_by_ack_locked(mb_bus_t *bus, mb_async_ack_t *ack)
{
    size_t i;

    for (i = 0; i < bus->async.entry_count; ++i) {
        mb_async_entry_t *entry = bus->async.entries[i];
        uint32_t offset;

        for (offset = 0; offset < entry->count; ++offset) {
            uint32_t index = (entry->head + offset) % ASYNC_DEPTH;
            mb_owned_message_t *msg = entry->slots[index];

            if (msg == NULL || msg->ack != ack) {
                continue;
            }

            /* 环形队列中途抽走一条：把它后面的元素逐个前移，保持 FIFO 次序 */
            while (offset + 1u < entry->count) {
                uint32_t next_index = (entry->head + offset + 1u) % ASYNC_DEPTH;

                entry->slots[index] = entry->slots[next_index];
                index = next_index;
                offset++;
            }
            entry->slots[index] = NULL;
            entry->count--;
            (void)mb_os_sem_signal(entry->space);
            return msg;
        }
    }
    return NULL;
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

mb_err_t mb_bus_publish_async_ack_internal(mb_bus_t *bus,
                                           const char *source,
                                           const char *topic,
                                           const void *payload,
                                           size_t payload_len,
                                           const mb_publish_opts_t *opts,
                                           uint32_t timeout_ms,
                                           mb_async_ack_t *ack)
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

    /* 条目满时丢弃最旧的一条，而不是等。编译期默认值可以被单次调用的标志覆盖。
     *
     * 带回执（等待消费）的消息**永不参与覆盖**：被顶掉意味着发布方等的那条
     * 消息凭空消失，这与「等到它被消化」的语义直接冲突，而且覆盖是最旧优先，
     * 一条正在被等待的消息很可能恰好就是最旧的那条。 */
    overwrite = (ack == NULL) &&
                (((options.flags & MB_PUB_FLAG_ASYNC_OVERWRITE) != 0) ||
                 (MB_CONFIG_ASYNC_OVERWRITE_OLDEST != 0));

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
            /* 被顶掉的可能是另一条正在 await 的消息：如实通知它的发布方
             * 「消息被丢弃了」，而不是让它一直等到超时。 */
            owned_message_discard_locked(oldest, MB_ERR_DROPPED);
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
    if (ack != NULL) {
        /* 入队动作与「消息持有一份回执引用」必须在同一个临界区里完成：
         * pump 一旦能取到这条消息，就会在投递后调用 ack_finish_locked()。 */
        ack->refcnt++;
        owned->ack = ack;
    }
    entry->slots[(entry->head + entry->count) % ASYNC_DEPTH] = owned;
    entry->count++;

    entry_release_waiter_locked(bus, entry);
    MB_BUS_UNLOCK(bus);

    MB_LOG_DEBUG("async", "queued '%s' (%u bytes) on bus '%s'",
                 topic, (unsigned)payload_len, bus->name);
    return MB_OK;
}

mb_err_t mb_bus_publish_async_internal(mb_bus_t *bus,
                                       const char *source,
                                       const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       const mb_publish_opts_t *opts,
                                       uint32_t timeout_ms)
{
    return mb_bus_publish_async_ack_internal(bus, source, topic, payload, payload_len,
                                             opts, timeout_ms, NULL);
}

/* -------------------------------------------------------------------------
 * 小节：入队并等待被消化（await）
 * ---------------------------------------------------------------------- */

/** @return 距离 deadline 还剩多少毫秒（无符号回绕安全）。 */
static uint32_t time_remaining(uint32_t deadline)
{
    uint32_t left = deadline - mb_os_time_ms();

    /* 无符号减法只能区分「差多少」，不能区分前后：结果落在有符号负半区
     * 说明已经越过 deadline。要求单次等待 < 2^31 毫秒（约 24 天），
     * 与 MB_WAIT_FOREVER 的语义区分得开。 */
    if (left > 0x7FFFFFFFu) {
        return 0;
    }
    return left;
}

mb_err_t mb_bus_publish_async_wait_internal(mb_bus_t *bus,
                                            const char *source,
                                            const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            const mb_publish_opts_t *opts,
                                            uint32_t timeout_ms,
                                            size_t *out_delivered)
{
    mb_async_ack_t *ack;
    uint32_t deadline = 0;
    uint32_t wait_ms;
    mb_err_t err;

    if (out_delivered != NULL) {
        *out_delivered = 0;
    }
    if (bus == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    /* MB_WAIT_NONE 的含义是「只入队，不等」，与 mb_node_publish_async 完全一样。
     * 不为它分配回执：等 0 毫秒没有任何意义，只会白花一次堆分配。 */
    if (timeout_ms == MB_WAIT_NONE) {
        return mb_bus_publish_async_internal(bus, source, topic, payload, payload_len,
                                             opts, MB_WAIT_NONE);
    }

    /* 「等待消费」与「覆盖最旧」互相矛盾：覆盖会主动丢掉最旧的那条，
     * 而等待方要的恰恰是「这条一定被消化」。与其让调用方在两个语义里猜，
     * 不如直接拒绝。注意编译期默认值不在此列 —— 回执消息永不参与覆盖
     * （见 mb_bus_publish_async_ack_internal 里的 overwrite 计算）。 */
    if (opts != NULL && (opts->flags & MB_PUB_FLAG_ASYNC_OVERWRITE) != 0) {
        MB_LOG_WARN("async", "MB_PUB_FLAG_ASYNC_OVERWRITE cannot be combined with "
                             "publish_async_wait on bus '%s'", bus->name);
        return MB_ERR_INVALID_ARG;
    }

    /* 在 pump 回调里等待自己消费：这条消息要等当前回调返回后 pump 才能继续取，
     * 而调用方正卡在回调里 —— 不管超时多长都不可能成功。直接拒绝，
     * 不浪费调用方的超时时间（MB_WAIT_NONE 已在上面返回，不受影响）。 */
    if (MB_OS_CAN_BLOCK) {
        bool from_pump;

        MB_BUS_LOCK(bus);
        from_pump = bus->pumping && (bus->pump_owner == mb_os_thread_id());
        MB_BUS_UNLOCK(bus);

        if (from_pump) {
            MB_LOG_WARN("async", "mb_node_publish_async_wait() called from inside a "
                                 "pump callback on bus '%s'; the pump cannot consume "
                                 "the message while this callback is running",
                        bus->name);
            return MB_ERR_WOULD_DEADLOCK;
        }
    }

    ack = ack_create(true);
    if (ack == NULL) {
        return MB_ERR_NO_MEMORY;
    }

    if (timeout_ms != MB_WAIT_FOREVER) {
        deadline = mb_os_time_ms() + timeout_ms;
    }

    /* 第一步：入队。队列满时这里会按 timeout_ms 等空位，会消耗总预算。 */
    err = mb_bus_publish_async_ack_internal(bus, source, topic, payload, payload_len,
                                            opts, timeout_ms, ack);
    if (err != MB_OK) {
        MB_BUS_LOCK(bus);
        ack_release_locked(ack); /* 只有发布方一份引用；消息没入队 */
        MB_BUS_UNLOCK(bus);
        return err;
    }

    /* 第二步：等 pump 消化。等待期间不持锁，pump 线程可以自由投递。 */
    wait_ms = (timeout_ms == MB_WAIT_FOREVER) ? MB_WAIT_FOREVER : time_remaining(deadline);

    if (MB_OS_CAN_BLOCK && mb_os_sem_wait(ack->done, wait_ms)) {
        MB_BUS_LOCK(bus);
        MB_CONFIG_ASSERT(ack->completed);
        err = ack->status;
        if (out_delivered != NULL) {
            *out_delivered = ack->delivered;
        }
        ack_release_locked(ack);
        MB_BUS_UNLOCK(bus);
        return err;
    }

    /* 超时（或平台不能阻塞）：尽量撤销还躺在队列里的那条消息，
     * 免得调用方已经往下走了、回调却又突然跑起来。 */
    MB_BUS_LOCK(bus);
    if (ack->completed) {
        /* 就在超时与加锁之间完成了：以真实结果为准，不误报超时 */
        err = ack->status;
        if (out_delivered != NULL) {
            *out_delivered = ack->delivered;
        }
    } else if (ack->dispatching) {
        /* pump 已经取走、正在跑回调，撤销已来不及。这是很窄的竞态窗口：
         * 调用方拿到 MB_ERR_TIMEOUT，但回调可能仍会执行。 */
        err = MB_ERR_TIMEOUT;
    } else {
        /* 还躺在队列里：当场摘掉并释放，保证它**永远不会**被投递，
         * 也把主题的空位立刻还回去（pump 可能根本没在跑）。 */
        mb_owned_message_t *queued = queue_remove_by_ack_locked(bus, ack);

        err = MB_ERR_TIMEOUT;
        if (queued != NULL) {
            ack->canceled = true;
            bus->stats.async_canceled++;
            owned_message_discard_locked(queued, MB_ERR_TIMEOUT);
            entries_release_empty_locked(bus);
        }
    }
    ack_release_locked(ack);
    MB_BUS_UNLOCK(bus);
    return err;
}

/* -------------------------------------------------------------------------
 * 小节：非阻塞推送 + 完成回调（notify）
 *
 * 与 await 的区别：await 会**阻塞调用线程**，notify 只登记一个通知就返回，
 * 结果（成功或超时）由 mb_bus_pump() 在 pump 线程上通过回调交回。
 *
 * 通知的归属：
 *   - 发布时创建，持有原始请求的一份深拷贝（回调里可读、可重试）；
 *   - 也持有 ack 的一份引用，用来接收投递结果 / 撤销队列里的消息；
 *   - 结果确定后仍留在链表里，等 pump 线程来取并回调；
 *   - 回调返回后释放 ack、节点引用、请求拷贝和通知对象本身。
 *
 * 超时不会自己「响」：库不创建定时器，超时是在下一次 mb_bus_pump() 里
 * 检查 start_ms + timeout_ms 后触发的。所以 pump 必须持续被驱动。
 * ---------------------------------------------------------------------- */

#define NOTIFY_SERVICE_BUDGET 16

/** 通知是否已经可以回调；会在锁内处理超时与撤销。 */
static bool notify_due_locked(mb_bus_t *bus, mb_async_notify_t *n)
{
    if (n->resolved) {
        return true;
    }
    if (n->timeout_ms == MB_WAIT_FOREVER) {
        return false;
    }
    if ((uint32_t)(mb_os_time_ms() - n->start_ms) < n->timeout_ms) {
        return false;
    }

    /* 超时：把还在队列里的消息当场摘掉，之后它一定不会被投递。 */
    if (n->ack != NULL && !n->ack->completed) {
        mb_owned_message_t *queued = queue_remove_by_ack_locked(bus, n->ack);

        if (queued != NULL) {
            queued->ack = NULL; /* 消息不再持有 ack，改由通知统一释放 */
            bus->stats.async_canceled++;
            ack_finish_locked(n->ack, MB_ERR_TIMEOUT, 0);
            mb_owned_message_free(queued);
            entries_release_empty_locked(bus);
        } else if (!n->ack->dispatching) {
            /* 防御：没入队也没有在投递，结果却还没确定 —— 直接按超时结束。
             * 不调用 ack_finish_locked()：此时没有「消息持有的那份引用」，
             * 调用它会把通知自己那份也释放掉。 */
            n->resolved = true;
            n->status = MB_ERR_TIMEOUT;
            n->delivered = 0;
        }
        /* 若正在 dispatching：单 pump 线程下 service 不会在分发中途运行，
         * 理论上到不了这里；真到了就等投递完成后的下一次 service。 */
    }
    return n->resolved;
}

/** 锁内：从通知链表里取走一个已经可以回调的通知（保持发布顺序）。 */
static mb_async_notify_t *notify_take_due_locked(mb_bus_t *bus)
{
    mb_async_notify_t **prev = &bus->async_notifies;

    while (*prev != NULL) {
        mb_async_notify_t *n = *prev;

        if (!notify_due_locked(bus, n)) {
            prev = &n->next;
            continue;
        }

        *prev = n->next;
        if (bus->async_notify_tail == n) {
            bus->async_notify_tail = NULL;
            if (bus->async_notifies != NULL) {
                mb_async_notify_t *tail = bus->async_notifies;

                while (tail->next != NULL) {
                    tail = tail->next;
                }
                bus->async_notify_tail = tail;
            }
        }
        n->next = NULL;
        return n;
    }
    return NULL;
}

/** 在 pump 线程上把已就绪的通知逐个回调；期间不持锁。 */
static void mb_async_notify_service(mb_bus_t *bus)
{
    int budget = NOTIFY_SERVICE_BUDGET;

    while (budget-- > 0) {
        mb_async_notify_t *n;
        mb_async_result_t result;

        MB_BUS_LOCK(bus);
        if (bus->destroying) {
            MB_BUS_UNLOCK(bus);
            return;
        }
        n = notify_take_due_locked(bus);
        MB_BUS_UNLOCK(bus);

        if (n == NULL) {
            return;
        }

        result.success = (n->status == MB_OK && n->delivered > 0);
        result.status = n->status;
        result.delivered = n->delivered;
        result.elapsed_since_publish_ms = mb_os_time_ms() - n->start_ms;
        result.bus = bus;
        result.node = n->node;
        result.source = (n->request != NULL) ? n->request->source : NULL;
        result.topic = (n->request != NULL) ? n->request->topic : NULL;
        result.payload = (n->request != NULL) ? n->request->payload : NULL;
        result.payload_len = (n->request != NULL) ? n->request->payload_len : 0;
        result.opts = n->opts;
        result.requested_timeout_ms = n->timeout_ms;
        result.handler = n->handler;
        result.user_data = n->user_data;

        /* 用户回调：锁外、pump 线程。这里可以再次调用推送函数发起重试。 */
        n->handler(&result, n->user_data);

        MB_BUS_LOCK(bus);
        if (n->ack != NULL) {
            ack_release_locked(n->ack);
            n->ack = NULL;
        }
        if (n->node != NULL) {
            mb_node_release_locked(bus, n->node);
            n->node = NULL;
        }
        MB_BUS_UNLOCK(bus);

        mb_owned_message_free(n->request);
        mb_os_free(n);
    }
}

mb_err_t mb_bus_publish_async_notify_internal(mb_bus_t *bus,
                                              mb_node_t *node,
                                              const char *source,
                                              const char *topic,
                                              const void *payload,
                                              size_t payload_len,
                                              const mb_publish_opts_t *opts,
                                              uint32_t timeout_ms,
                                              mb_async_done_handler_t handler,
                                              void *user_data)
{
    mb_async_notify_t *n;
    mb_publish_opts_t options;
    uint8_t msg_flags;
    mb_err_t err;

    if (bus == NULL || handler == NULL) {
        return MB_ERR_INVALID_ARG;
    }

    err = mb_publish_prepare(topic, payload, payload_len, opts, &options, &msg_flags);
    if (err != MB_OK) {
        return err;
    }

    /* 与 await 保持一致：通知要求「这条消息一定被消化」，不接受主动覆盖。 */
    if ((options.flags & MB_PUB_FLAG_ASYNC_OVERWRITE) != 0) {
        MB_LOG_WARN("async", "MB_PUB_FLAG_ASYNC_OVERWRITE cannot be combined with "
                             "publish_async_notify on bus '%s'", bus->name);
        return MB_ERR_INVALID_ARG;
    }

    n = (mb_async_notify_t *)mb_os_calloc(1, sizeof(*n));
    if (n == NULL) {
        return MB_ERR_NO_MEMORY;
    }
    n->bus = bus;
    n->opts = options;
    n->handler = handler;
    n->user_data = user_data;
    n->start_ms = mb_os_time_ms();
    n->timeout_ms = timeout_ms;

    n->ack = ack_create(false);
    if (n->ack == NULL) {
        mb_os_free(n);
        return MB_ERR_NO_MEMORY;
    }
    n->ack->notify = n;

    n->request = mb_owned_message_create(topic, payload, payload_len, source, 0, 0, msg_flags);
    if (n->request == NULL) {
        MB_BUS_LOCK(bus);
        ack_release_locked(n->ack);
        MB_BUS_UNLOCK(bus);
        mb_os_free(n);
        return MB_ERR_NO_MEMORY;
    }

    /* 先持有发布者节点，保证回调期间 result->node 不会悬垂 */
    if (node != NULL) {
        MB_BUS_LOCK(bus);
        if (bus->destroying || !node->active) {
            ack_release_locked(n->ack);
            MB_BUS_UNLOCK(bus);
            mb_owned_message_free(n->request);
            mb_os_free(n);
            return MB_ERR_STATE;
        }
        node->refcnt++;
        n->node = node;
        MB_BUS_UNLOCK(bus);
    }

    /* 非阻塞入队：这里绝不等待，失败也交给回调报告。 */
    err = mb_bus_publish_async_ack_internal(bus, source, topic, payload, payload_len,
                                            &options, MB_WAIT_NONE, n->ack);
    if (err == MB_ERR_STATE) {
        /* 总线已在销毁：这条通知没有意义，同步报错，不登记回调。 */
        MB_BUS_LOCK(bus);
        if (n->node != NULL) {
            mb_node_release_locked(bus, n->node);
            n->node = NULL;
        }
        ack_release_locked(n->ack);
        MB_BUS_UNLOCK(bus);
        mb_owned_message_free(n->request);
        mb_os_free(n);
        return MB_ERR_STATE;
    }
    if (err != MB_OK) {
        /* 消息没入队：ack 只有通知自己这一份引用，直接记为失败。
         * 不调用 ack_finish_locked()，原因同上（没有消息那份引用可归还）。 */
        n->resolved = true;
        n->status = err;
        n->delivered = 0;
        n->ack->completed = true;
        n->ack->status = err;
        n->ack->delivered = 0;
    }

    MB_BUS_LOCK(bus);
    n->next = NULL;
    if (bus->async_notify_tail != NULL) {
        bus->async_notify_tail->next = n;
    } else {
        bus->async_notifies = n;
    }
    bus->async_notify_tail = n;
    MB_BUS_UNLOCK(bus);

    MB_LOG_DEBUG("async", "notify queued for '%s' on bus '%s' (timeout=%u ms)",
                 topic, bus->name, (unsigned)timeout_ms);
    return MB_OK;
}

/** 锁内：释放所有待完成通知（总线销毁路径，不触发用户回调）。 */
void mb_async_notify_dispose_locked(mb_bus_t *bus)
{
    mb_async_notify_t *n = bus->async_notifies;

    bus->async_notifies = NULL;
    bus->async_notify_tail = NULL;

    while (n != NULL) {
        mb_async_notify_t *next = n->next;

        if (n->ack != NULL) {
            ack_release_locked(n->ack);
            n->ack = NULL;
        }
        if (n->node != NULL) {
            mb_node_release_locked(bus, n->node);
            n->node = NULL;
        }
        mb_owned_message_free(n->request);
        mb_os_free(n);
        n = next;
    }
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

    if (pool->entries == NULL || pool->entry_count == 0) {
        return 0;
    }
    if (pool->next_entry >= pool->entry_count) {
        pool->next_entry = 0;
    }

    /* 外层循环处理「取出来的消息被撤销、不能投递」的情况：它们不占 batch 名额，
     * 但已经消耗了条目里的一个位置，必须继续取，否则 pump 会在还有积压时提前收工。
     * 内层每轮扫一遍全部条目、每个条目最多取一条，条目空则下一轮自然结束。 */
    for (;;) {
        size_t scanned = 0;
        bool popped_any = false;

        while (scanned < pool->entry_count && n < max) {
            size_t index = pool->next_entry;
            mb_async_entry_t *entry = pool->entries[index];
            mb_owned_message_t *msg;
            bool returned;

            pool->next_entry = (index + 1) % pool->entry_count;
            ++scanned;

            if (entry->count == 0) {
                continue;
            }

            msg = entry->slots[entry->head];
            entry->slots[entry->head] = NULL;
            entry->head = (entry->head + 1u) % ASYNC_DEPTH;
            entry->count--;
            popped_any = true;

            /* 归还空位，唤醒可能正在等的发布者。信号量释放是非阻塞的，
             * 所以可以在锁内做 —— 让发布者尽早开工，不必等我们跑完回调。 */
            returned = mb_os_sem_signal(entry->space);
            MB_CONFIG_ASSERT(returned);
            (void)returned;

            if (msg->ack != NULL) {
                if (msg->ack->canceled) {
                    /* 发布方已经等超时并放弃了这条消息：跳过投递并就地释放。
                     * 发布方可能还没归还它那份回执引用，所以这里只是减少一份。 */
                    owned_message_discard_locked(msg, MB_ERR_TIMEOUT);
                    continue;
                }
                /* 从这里到回调结束之间无法再撤销：发布方此刻超时会看到这个标志，
                 * 并如实返回 MB_ERR_TIMEOUT（回调可能已经执行）。 */
                msg->ack->dispatching = true;
            }
            batch[n++] = msg;
        }

        if (n >= max || !popped_any) {
            break;
        }
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

        /* 先处理完成通知：可能触发成功/超时回调，回调里还能再发布（重试）。
         * 重试的消息会在下面的 take_batch 里被同一次 pump 顺带取走。 */
        mb_async_notify_service(bus);

        MB_BUS_LOCK(bus);
        n = take_batch_locked(bus, batch, MB_CONFIG_DELIVER_BATCH);
        /* 即使本批全是「已撤销」的消息（n == 0），条目可能已经空了，
         * 顺手回收，免得空条目一直占着名额。 */
        entries_release_empty_locked(bus);
        MB_BUS_UNLOCK(bus);

        if (n == 0) {
            break; /* 队列空了：本函数不阻塞，直接返回 */
        }

        /* 锁外逐条投递 —— 走的还是同步投递那一条路，订阅语义完全不变 */
        for (i = 0; i < n; ++i) {
            mb_message_t view;
            mb_async_ack_t *ack;
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

            /* 投递结束（成功或失败）是回执的**唯一**完成点：到这里为止，
             * 所有匹配回调都已经从本线程返回，等待方可以安全地继续执行。 */
            ack = batch[i]->ack;
            if (ack != NULL) {
                batch[i]->ack = NULL;
                MB_BUS_LOCK(bus);
                ack_finish_locked(ack, err, delivered);
                MB_BUS_UNLOCK(bus);
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

    if (bus->async.entries != NULL) {
        for (i = 0; i < bus->async.entry_count; ++i) {
            mb_async_entry_t *entry = bus->async.entries[i];
            uint32_t j;

            /* 队列里还没被 pump 取走的消息也要释放，否则每条都泄漏。
             * 带回执的要先结束掉，唤醒可能还在等的发布方（正常用法下销毁前
             * 必须已经 join 掉等待线程，这里只是不把一个安静的等待者留成挂死）。 */
            for (j = 0; j < ASYNC_DEPTH; ++j) {
                if (entry->slots[j] != NULL) {
                    owned_message_discard_locked(entry->slots[j], MB_ERR_STATE);
                    entry->slots[j] = NULL;
                }
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

    /* 未完成的完成通知：销毁路径不再触发用户回调，直接释放。
     * 必须放在队列消息释放之后 —— 消息会先归还它持有的那份 ack 引用。 */
    mb_async_notify_dispose_locked(bus);
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

mb_err_t mb_bus_publish_async_wait(mb_bus_t *bus,
                                   const char *topic,
                                   const void *payload,
                                   size_t payload_len,
                                   const mb_publish_opts_t *opts,
                                   uint32_t timeout_ms,
                                   size_t *out_delivered)
{
    return mb_bus_publish_async_wait_internal(bus, NULL, topic, payload, payload_len,
                                              opts, timeout_ms, out_delivered);
}

mb_err_t mb_bus_publish_async_notify(mb_bus_t *bus,
                                     const char *topic,
                                     const void *payload,
                                     size_t payload_len,
                                     const mb_publish_opts_t *opts,
                                     uint32_t timeout_ms,
                                     mb_async_done_handler_t on_done,
                                     void *user_data)
{
    return mb_bus_publish_async_notify_internal(bus, NULL, NULL, topic, payload, payload_len,
                                                opts, timeout_ms, on_done, user_data);
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

mb_err_t mb_bus_publish_async_ack_internal(mb_bus_t *bus,
                                           const char *source,
                                           const char *topic,
                                           const void *payload,
                                           size_t payload_len,
                                           const mb_publish_opts_t *opts,
                                           uint32_t timeout_ms,
                                           mb_async_ack_t *ack)
{
    (void)bus;
    (void)source;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    (void)ack;
    return MB_ERR_UNSUPPORTED;
}

mb_err_t mb_bus_publish_async_wait_internal(mb_bus_t *bus,
                                            const char *source,
                                            const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            const mb_publish_opts_t *opts,
                                            uint32_t timeout_ms,
                                            size_t *out_delivered)
{
    (void)bus;
    (void)source;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    if (out_delivered != NULL) {
        *out_delivered = 0;
    }
    return MB_ERR_UNSUPPORTED;
}

mb_err_t mb_bus_publish_async_notify_internal(mb_bus_t *bus,
                                              mb_node_t *node,
                                              const char *source,
                                              const char *topic,
                                              const void *payload,
                                              size_t payload_len,
                                              const mb_publish_opts_t *opts,
                                              uint32_t timeout_ms,
                                              mb_async_done_handler_t handler,
                                              void *user_data)
{
    (void)bus;
    (void)node;
    (void)source;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    (void)handler;
    (void)user_data;
    return MB_ERR_UNSUPPORTED;
}

void mb_async_notify_dispose_locked(mb_bus_t *bus)
{
    (void)bus;
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

mb_err_t mb_bus_publish_async_wait(mb_bus_t *bus, const char *topic, const void *payload,
                                   size_t payload_len, const mb_publish_opts_t *opts,
                                   uint32_t timeout_ms, size_t *out_delivered)
{
    (void)bus;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    if (out_delivered != NULL) {
        *out_delivered = 0;
    }
    return MB_ERR_UNSUPPORTED;
}

mb_err_t mb_bus_publish_async_notify(mb_bus_t *bus, const char *topic, const void *payload,
                                     size_t payload_len, const mb_publish_opts_t *opts,
                                     uint32_t timeout_ms, mb_async_done_handler_t on_done,
                                     void *user_data)
{
    (void)bus;
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)opts;
    (void)timeout_ms;
    (void)on_done;
    (void)user_data;
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
