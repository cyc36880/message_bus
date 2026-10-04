/**
 * @file mb_dispatch.c
 * @brief 主题匹配与同步投递。
 *
 * 这是整个库唯一的投递出口。把它单独放在一个文件里，是为了将来若要
 * 改成「队列 + 分发线程」的异步模型，只动这里即可，公开 API 不变。
 *
 * ── 投递算法 ────────────────────────────────────────────────────────────
 * 订阅表按 seq 升序排列。每次投递用一个游标 cursor 记录「已经取走的最大
 * seq」，分批（每批 MB_CONFIG_DELIVER_BATCH 个）取出匹配的订阅：
 *
 *   for (;;) {
 *       在锁内：扫描 seq > cursor 的订阅，挑出匹配的，给它们 +1 引用；
 *       在锁外：逐个调用回调（此时可以安全地增删订阅）；
 *       在锁内：释放引用，更新统计；
 *       若这一批没取满，说明已经扫完，结束。
 *   }
 *
 * 关键细节：cursor 只在**真正取走**一条订阅时才推进。不匹配的订阅推进
 * cursor 只是省去下一轮的重复扫描；而「本批已满、这条留到下一批」时
 * **绝不能**推进，否则超过一个批次宽度（默认 8）之后的所有匹配订阅
 * 都会被永久漏投。
 *
 * 这样做的两个好处：
 *   1. 回调永远在锁外执行 —— 回调内部可以调用任何总线 API；
 *   2. 回调执行期间即使有其它线程取消订阅 / 销毁节点，引用计数也保证
 *      对象不会提前释放，不会出现 use-after-free。
 * ───────────────────────────────────────────────────────────────────────
 */
#include "mb_internal.h"

/** 一条消息（或一条 retained）是否应该投递给某个订阅。锁内调用。 */
static bool subscription_matches(const mb_subscription_t *sub, const char *topic, const char *source)
{
    if (!sub->active) {
        return false;
    }
    if (!mb_topic_match(sub->filter, topic)) {
        return false;
    }
    if (sub->source_filter != NULL) {
        /* 只有带来源的发布才能命中来源过滤 */
        if (source == NULL || !mb_topic_match(sub->source_filter, source)) {
            return false;
        }
    }
    if ((sub->flags & MB_SUB_FLAG_NOLOCAL) != 0) {
        if (source != NULL && sub->node != NULL && strcmp(source, sub->node->name) == 0) {
            return false;
        }
    }
    return true;
}

/** 调用一个回调，并顺带做一次「订阅是否还有效」的检查。 */
static void invoke_handler(mb_subscription_t *sub, const mb_message_t *msg)
{
    bool alive;

    MB_BUS_LOCK(sub->bus);
    alive = sub->active;
    MB_BUS_UNLOCK(sub->bus);

    if (alive) {
        sub->handler(sub, msg, sub->user_data);
    }
}

mb_err_t mb_dispatch_message(mb_bus_t *bus, const mb_message_t *msg, size_t *out_delivered)
{
    /* 递归深度是「每个线程」的属性：同步投递下，回调里再 publish 会递归。 */
    static MB_THREAD_LOCAL int dispatch_depth = 0;

    mb_subscription_t *batch[MB_CONFIG_DELIVER_BATCH];
    uint64_t cursor = 0;
    size_t total_delivered = 0;

    if (out_delivered != NULL) {
        *out_delivered = 0;
    }

#if MB_CONFIG_MAX_DISPATCH_DEPTH > 0
    if (dispatch_depth >= MB_CONFIG_MAX_DISPATCH_DEPTH) {
        MB_BUS_LOCK(bus);
        bus->stats.dropped++;
        MB_BUS_UNLOCK(bus);
        MB_LOG_WARN("bus", "dispatch depth exceeds %d, message on topic '%s' dropped "
                           "(publish loop among callbacks?)",
                    (int)MB_CONFIG_MAX_DISPATCH_DEPTH, msg->topic);
        return MB_ERR_BUSY;
    }
#endif

    dispatch_depth++;

    for (;;) {
        size_t n = 0;
        size_t i;
        bool more = false;

        /* ── 第一步：锁内挑选本批订阅，并各加一次引用 ── */
        MB_BUS_LOCK(bus);
        for (i = 0; i < bus->sub_count; ++i) {
            mb_subscription_t *sub = bus->subs[i];

            if (sub->seq <= cursor) {
                continue;
            }
            if (!subscription_matches(sub, msg->topic, msg->source)) {
                cursor = sub->seq; /* 不匹配的也推进游标，下一轮不必重复扫描 */
                continue;
            }
            if (n == MB_CONFIG_DELIVER_BATCH) {
                /* 本批已满。此处的 cursor 仍停在**上一个已取走**的订阅上，
                 * 因此当前这条留到下一批，不会被跳过。
                 * （若在这里先推进 cursor，这条匹配的订阅就会被永久漏掉。） */
                more = true;
                break;
            }

            cursor = sub->seq;
            mb_subscription_retain_locked(sub);
            sub->node->refcnt++;
            batch[n++] = sub;
        }
        MB_BUS_UNLOCK(bus);

        /* ── 第二步：锁外调用回调 ── */
        for (i = 0; i < n; ++i) {
            invoke_handler(batch[i], msg);
        }
        total_delivered += n;

        /* ── 第三步：锁内归还引用 ── */
        MB_BUS_LOCK(bus);
        for (i = 0; i < n; ++i) {
            mb_subscription_t *sub = batch[i];
            mb_node_t *node = sub->node; /* 必须在 sub 可能被释放前取出 */

            mb_subscription_release_locked(bus, sub);
            mb_node_release_locked(bus, node);
        }
        bus->stats.delivered += n;
        MB_BUS_UNLOCK(bus);

        if (!more) {
            break;
        }
    }

    dispatch_depth--;

    if (out_delivered != NULL) {
        *out_delivered = total_delivered;
    }
    return MB_OK;
}

void mb_dispatch_retained_to_subscription(mb_bus_t *bus, mb_subscription_t *sub)
{
    mb_retained_t *batch[MB_CONFIG_DELIVER_BATCH];
    uint64_t cursor = 0;

    MB_CONFIG_ASSERT(bus != NULL);
    MB_CONFIG_ASSERT(sub != NULL);

    if ((sub->flags & MB_SUB_FLAG_SKIP_RETAINED) != 0) {
        return;
    }

    for (;;) {
        mb_retained_t *entry;
        size_t n = 0;
        size_t i;
        bool more = false;

        /* ── 锁内挑选 ── */
        MB_BUS_LOCK(bus);
        for (entry = bus->retained; entry != NULL; entry = entry->next) {
            if (entry->seq <= cursor) {
                continue;
            }
            if (!entry->active || !subscription_matches(sub, entry->msg->topic, entry->msg->source)) {
                cursor = entry->seq;
                continue;
            }
            if (n == MB_CONFIG_DELIVER_BATCH) {
                /* 同 mb_dispatch_message()：cursor 不推进，留到下一批补发 */
                more = true;
                break;
            }

            cursor = entry->seq;
            entry->refcnt++;
            batch[n++] = entry;
        }
        MB_BUS_UNLOCK(bus);

        /* ── 锁外回调：注意 view 是栈上临时对象，仅本次调用有效 ── */
        for (i = 0; i < n; ++i) {
            mb_message_t view;

            mb_owned_message_view(batch[i]->msg, &view);
            view.flags |= MB_MSG_FLAG_RETAINED;
            invoke_handler(sub, &view);
        }

        /* ── 归还引用 ── */
        MB_BUS_LOCK(bus);
        for (i = 0; i < n; ++i) {
            mb_retained_release_locked(bus, batch[i]);
        }
        MB_BUS_UNLOCK(bus);

        if (!more) {
            break;
        }
    }
}
