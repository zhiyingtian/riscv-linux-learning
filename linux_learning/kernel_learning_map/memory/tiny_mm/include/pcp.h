#ifndef TINY_MM_PCP_H
#define TINY_MM_PCP_H

#include <stdbool.h>
#include <stddef.h>

#include "buddy.h"

/*
 * tiny-mm / M2b: per-CPU page cache（pcp）
 *
 * 这里只模拟一个 CPU、一个 zone、order 0 的缓存：
 *   - order 0 分配/释放优先走 pcp 链表；
 *   - pcp 空了：从 buddy 批量 refill（batch 个页）；
 *   - pcp 满了：批量 drain 回 buddy；
 *   - order > 0 直接绕过 pcp，走 buddy。
 *
 * pcp 页不在 buddy 的 free_area 里，所以它们对 buddy 来说仍是“已分配”：
 *   - pcp 链表用 page->buddy_next 串起来；
 *   - pcp 里的 page：is_free=false、refcount=0、order=0、state=PAGE_IN_PCP；
 *   - 从 pcp 取出后：is_free=false、refcount=1、order=0、state=PAGE_ALLOCATED。
 */
struct pcp {
	struct buddy *buddy;	/* 下层 buddy，不拥有 */
	struct page *list;	/* order 0 的 pcp 链表头 */
	unsigned int count;	/* pcp 缓存页数，不是块数 */
	unsigned int high;	/* 高于等于 high 触发 drain */
	unsigned int batch;	/* 每次 refill/drain 的批大小 */

	unsigned long alloc_hits;	/* 从 pcp 命中的次数 */
	unsigned long alloc_refills;	/* pcp 为空、refill 的次数 */
	unsigned long alloc_misses;	/* buddy 也拿不到页的次数 */
	unsigned long nr_put;	/* 累计放进 pcp 的页数（refill + free） */
	unsigned long drain_count;	/* 从 pcp 还给 buddy 的页数 */
};

/*
 * high/batch 要求：
 *   - high > 0；
 *   - batch > 0；
 *   - batch <= high；
 *   - high <= buddy->nr_pages。
 * 成功返回 0，失败返回 -EINVAL。
 */
int pcp_init(struct pcp *p, struct buddy *b,
	     unsigned int high, unsigned int batch);

/* 清空 pcp 元数据；不会把缓存页还给 buddy。 */
void pcp_exit(struct pcp *p);

/*
 * 分配 2^order 页。
 *
 * order == 0：优先 pcp；pcp 空则从 buddy refill。
 * order > 0：直接 buddy_alloc()，不经过 pcp。
 *
 * 成功返回块首页；失败返回 NULL。order > 0 时返回 buddy 的已分配页。
 */
struct page *pcp_alloc_pages(struct pcp *p, unsigned int order);

/*
 * 释放从 pcp_alloc_pages() 拿到的页。
 *
 * order == 0：放进 pcp；达到 high 后批量 drain 回 buddy。
 * order > 0：直接 buddy_free()。
 *
 * 成功返回 0，失败返回 -EINVAL。
 */
int pcp_free_pages(struct pcp *p, struct page *page, unsigned int order);

/*
 * 主动从 pcp 还 nr 页给 buddy，返回实际还的页数。
 * 只处理 order 0；pcp 里的页都是 order 0。
 */
unsigned int pcp_drain(struct pcp *p, unsigned int nr);

/* 把 pcp 里的页全部还给 buddy，返回实际还的页数。 */
unsigned int pcp_drain_all(struct pcp *p);

/*
 * 自检：
 *   - count 与链表长度一致；
 *   - 每个缓存页在 buddy->pages 范围内，且 is_free/refcount/order/state 正确；
 *   - buddy_check() 通过。
 */
int pcp_check(const struct pcp *p);

#endif /* TINY_MM_PCP_H */
