/*
 * tiny-mm / M2b: per-CPU page cache
 *
 * pcp 在 buddy 上加一层 order-0 缓存：
 *   - alloc order 0：先查 pcp，空了再向 buddy 批量 refill；
 *   - free order 0：放进 pcp，达到 high 后批量 drain 回 buddy；
 *   - alloc/free order > 0：直接走 buddy。
 */

#include <errno.h>
#include <string.h>

#include "pcp.h"
#include "tmm.h"

/*
 * 从 pcp 链表摘一页，并恢复成“已分配页”状态。
 * 返回 NULL 表示 pcp 为空。
 */
static struct page *pcp_pop(struct pcp *p)
{
	struct page *page = p->list;

	if (!page)
		return NULL;

	p->list = page->buddy_next;
	page->buddy_next = NULL;
	page->is_free = false;
	page->refcount = 1;
	page->state = PAGE_ALLOCATED;
	p->count--;
	p->alloc_hits++;

	return page;
}

int pcp_init(struct pcp *p, struct buddy *b,
	     unsigned int high, unsigned int batch)
{
	if (!p || !b || !b->pages || b->nr_pages == 0)
		return -EINVAL;
	if (high == 0 || batch == 0 || batch > high)
		return -EINVAL;
	if (high > b->nr_pages)
		return -EINVAL;

	memset(p, 0, sizeof(*p));
	p->buddy = b;
	p->high = high;
	p->batch = batch;

	return pcp_check(p);
}

void pcp_exit(struct pcp *p)
{
	if (p)
		memset(p, 0, sizeof(*p));
}

struct page *pcp_alloc_pages(struct pcp *p, unsigned int order)
{
	struct page *page;
	unsigned int refill_count;
	unsigned int i;

	if (!p || !p->buddy || !p->buddy->pages ||
	    order > p->buddy->max_order)
		return NULL;

	/* order > 0 绕过 pcp，直接分配。 */
	if (order > 0)
		return buddy_alloc(p->buddy, order);

	/* 先看 pcp 缓存。 */
	page = pcp_pop(p);
	if (page)
		return page;

	if (p->buddy->free_count == 0) {
		p->alloc_misses++;
		return NULL;
	}

	/* pcp 为空：从 buddy 批量 refill。 */
	refill_count = p->batch < p->buddy->free_count ?
		       p->batch : (unsigned int)p->buddy->free_count;
	if (refill_count == 0) {
		p->alloc_misses++;
		return NULL;
	}

	p->alloc_refills++;
	for (i = 0; i < refill_count; i++) {
		page = buddy_alloc(p->buddy, 0);
		if (!page)
			break;

		page->is_free = false;
		page->refcount = 0;
		page->order = 0;
		page->state = PAGE_IN_PCP;
		page->buddy_next = p->list;
		p->list = page;
		p->count++;
		p->nr_put++;
	}

	page = pcp_pop(p);
	if (page)
		return page;

	p->alloc_misses++;
	return NULL;
}

int pcp_free_pages(struct pcp *p, struct page *page, unsigned int order)
{
	if (!p || !p->buddy || !p->buddy->pages || !page)
		return -EINVAL;

	if (order > 0)
		return buddy_free(p->buddy, page, order);

	if (page->is_free || page->refcount != 1 || page->order != 0 ||
	    page->state != PAGE_ALLOCATED)
		return -EINVAL;

	/* 已分配页 → pcp 缓存页。 */
	page->is_free = false;
	page->refcount = 0;
	page->state = PAGE_IN_PCP;
	page->buddy_next = p->list;
	p->list = page;
	p->count++;
	p->nr_put++;

	if (p->count >= p->high)
		pcp_drain(p, p->batch);

	return 0;
}

unsigned int pcp_drain(struct pcp *p, unsigned int nr)
{
	unsigned int drained = 0;

	if (!p || !p->buddy || !p->buddy->pages)
		return 0;
	if (nr == 0 || p->count == 0)
		return 0;

	while (drained < nr && p->list) {
		struct page *page = p->list;

		p->list = page->buddy_next;
		page->buddy_next = NULL;

		/* pcp 缓存页 → 已分配页，再交给 buddy。 */
		page->is_free = false;
		page->refcount = 1;
		page->state = PAGE_ALLOCATED;

		if (buddy_free(p->buddy, page, 0) == 0) {
			drained++;
			p->count--;
			p->drain_count++;
		} else {
			/* 理论上不会发生；失败时把页恢复成 pcp 缓存态。 */
			page->is_free = false;
			page->refcount = 0;
			page->state = PAGE_IN_PCP;
			page->buddy_next = p->list;
			p->list = page;
			break;
		}
	}

	return drained;
}

unsigned int pcp_drain_all(struct pcp *p)
{
	if (!p)
		return 0;
	return pcp_drain(p, p->count);
}

int pcp_check(const struct pcp *p)
{
	const struct page *page;
	unsigned int count = 0;

	if (!p || !p->buddy || !p->buddy->pages)
		return -EINVAL;

	page = p->list;
	while (page) {
		if (page < p->buddy->pages ||
		    page >= p->buddy->pages + p->buddy->nr_pages)
			return -EINVAL;
		if (page->is_free || page->refcount != 0 || page->order != 0)
			return -EINVAL;
		if (page->state != PAGE_IN_PCP)
			return -EINVAL;
		count++;
		if (count > p->buddy->nr_pages)
			return -EINVAL;
		page = page->buddy_next;
	}

	if (count != p->count)
		return -EINVAL;

	return buddy_check(p->buddy);
}
