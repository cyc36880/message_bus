/**
 * @file mb_subscription.c
 * @brief 订阅对象的引用计数、挂载与摘除。
 *
 * 订阅表按 seq 升序排列，这一点是投递算法正确性的前提：
 * 投递时用「游标 = 已检查过的最大 seq」分批扫描，只有在有序表上
 * 才能保证每条消息对每个订阅者恰好处理一次。
 */
#include "mb_internal.h"

/** 确保订阅表至少能容纳 needed 个元素。调用者必须持锁。 */
static bool ensure_sub_capacity_locked(mb_bus_t *bus, size_t needed)
{
    mb_subscription_t **grown;
    size_t capacity;

    if (bus->sub_capacity >= needed) {
        return true;
    }

    capacity = (bus->sub_capacity > 0) ? bus->sub_capacity : 4;
    while (capacity < needed) {
        capacity *= 2;
    }

    /* 不用 realloc：很多 RTOS 的堆实现没有它 */
    grown = (mb_subscription_t **)mb_os_malloc(capacity * sizeof(*grown));
    if (grown == NULL) {
        MB_LOG_ERROR("sub", "failed to grow subscription table: %u entries requested", (unsigned)capacity);
        return false;
    }
    if (bus->sub_count > 0) {
        memcpy(grown, bus->subs, bus->sub_count * sizeof(*grown));
    }
    mb_os_free(bus->subs);
    bus->subs = grown;
    bus->sub_capacity = capacity;
    return true;
}

bool mb_bus_add_subscription_locked(mb_bus_t *bus, mb_subscription_t *sub)
{
    if (!ensure_sub_capacity_locked(bus, bus->sub_count + 1)) {
        return false;
    }
    /* 新订阅的 seq 最大，直接追加即可维持升序 */
    bus->subs[bus->sub_count++] = sub;
    if (bus->sub_count > bus->stats.peak_subscriptions) {
        bus->stats.peak_subscriptions = bus->sub_count;
    }
    return true;
}

void mb_subscription_detach_locked(mb_bus_t *bus, mb_subscription_t *sub)
{
    size_t index;

    for (index = 0; index < bus->sub_count; ++index) {
        if (bus->subs[index] == sub) {
            break;
        }
    }
    if (index == bus->sub_count) {
        MB_CONFIG_ASSERT(false); /* 订阅不在总线的表中，属于内部错误 */
        return;
    }

    /* 保序删除：把后面的元素整体前移，维持 seq 升序 */
    if (index + 1 < bus->sub_count) {
        memmove(&bus->subs[index], &bus->subs[index + 1],
                (bus->sub_count - index - 1) * sizeof(bus->subs[0]));
    }
    bus->sub_count--;
    sub->active = false;
}

void mb_subscription_retain_locked(mb_subscription_t *sub)
{
    sub->refcnt++;
}

void mb_subscription_release_locked(mb_bus_t *bus, mb_subscription_t *sub)
{
    MB_CONFIG_ASSERT(bus == sub->bus);
    MB_CONFIG_ASSERT(sub->refcnt > 0);

    sub->refcnt--;
    if (sub->refcnt > 0) {
        return;
    }

    /* 引用归零只可能发生在已摘除之后（在途回调必然持有一份引用） */
    MB_CONFIG_ASSERT(!sub->active);

    mb_os_free(sub->filter);
    mb_os_free(sub->source_filter);
    mb_os_free(sub);
}
