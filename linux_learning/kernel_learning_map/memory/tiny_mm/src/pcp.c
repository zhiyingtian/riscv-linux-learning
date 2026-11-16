/*
 * tiny-mm / M2b: per-CPU page cache
 *
 * 核心:
 *   - pcp_alloc_pages()
 *   - pcp_free_pages()
 *   - pcp_drain()
 *
 * pcp_init()/pcp_check() 已经实现。
 */

#include <errno.h>
#include <string.h>

#include "pcp.h"
#include "tmm.h"

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
	/*
	 * M2b:
	 *   order > 0：直接 buddy_alloc()。
	 *   order == 0：
	 *     1. pcp 链表非空：摘头，返回；
	 *     2. pcp 为空：从 buddy 批量 refill，--alloc_refills，
	 *        再把其中一页返回；
	 *     3. buddy 也拿不到：--alloc_misses，返回 NULL。
	 */
	
	if (!p || !p->buddy || !p->buddy->pages || order > p->buddy->max_order)
		return NULL;
	if (order > 0)
	{
		return buddy_alloc(p->buddy, order);
	}

	// pcp 链表非空，直接摘头返回
	if (p->list) {
		struct page *page = p->list;
		p->list = page->buddy_next;
		page->buddy_next = NULL;
		page->refcount = 1;
		page->state = PAGE_ALLOCATED;
		p->count--;
		p->alloc_hits++;
		return page;
	}
	// pcp 链表为空，从 buddy 批量 refill
	else if (p->buddy->free_count > 0)
	{
		unsigned int refill_count = p->batch < p->buddy->free_count ? p->batch : p->buddy->free_count;
		if(refill_count > 0)
			p->alloc_refills++;
		for (unsigned int i = 0; i < refill_count; )
		{
			struct page * page = buddy_alloc(p->buddy, 0);

			if(i == 0 && !page)
			{
				p->alloc_misses ++;
				return NULL;
			}

			if (page)
			{
				page->buddy_next = p->list;
				p->list = page;
				page->refcount = 0;
				page->state = PAGE_IN_PCP;
				p->count++;
				p->free_count++;
				i++;
			}
			else
				break;
		}
		if (p->count > 0)
		{
			struct page *page = p->list;
			p->list = page->buddy_next;
			page->buddy_next = NULL;
			page->refcount = 1;
			page->state = PAGE_ALLOCATED;
			p->count--;
			p->alloc_hits++;
			return page;
		}
		else
		{
			p->alloc_misses++;
			return NULL;
		}
	}
	p->alloc_misses++;
	return NULL;
}

int pcp_free_pages(struct pcp *p, struct page *page, unsigned int order)
{
	/*
	 * M2b:
	 *   order > 0：直接 buddy_free()。
	 *   order == 0：
	 *     1. 校验 page 状态；
	 *     2. 挂到 pcp 链表头，count++；
	 *     3. count >= high 时 drain。
	 */

	if (!p || !p->buddy || !p->buddy->pages || !page)
		return -EINVAL;

	if (page->state != PAGE_ALLOCATED)
		return -EINVAL;

	if (order > 0)
	{
		return buddy_free(p->buddy, page, order);
	}
	else
	{
		if (page->is_free == false && page->refcount == 1 && page->order == 0)
		{
			page->state = PAGE_IN_PCP;
			page->refcount = 0;
			page->buddy_next = p->list;
			p->list = page;
			p->count++;
			p->free_count++;
			if (p->count >= p->high)
				pcp_drain(p, p->batch);
			return 0;
		}
		else
		{
			return -EINVAL;
		}
	}


	return -EINVAL;
}

unsigned int pcp_drain(struct pcp *p, unsigned int nr)
{
	(void)p;
	(void)nr;

	/*
	 * TODO(M2b):
	 *   从 pcp 链表摘 nr 页，逐页恢复成“已分配”状态后调用 buddy_free()。
	 *   返回实际 drain 的页数，并更新 drain_count。
	 */
	if (!p || !p->buddy || !p->buddy->pages)
		return 0;
	if (nr == 0 || p->count == 0)
		return 0;

	unsigned int drained = 0;
	while (drained < nr && p->list)
	{
		struct page *page = p->list;
		p->list = page->buddy_next;
		page->buddy_next = NULL;
		// 改变page state状态，以便buddy_free()可以正确释放
		page->state = PAGE_ALLOCATED;
		page->refcount = 1;
		if (buddy_free(p->buddy, page, 0) == 0)
		{
			drained++;
			p->count--;
			p->drain_count++;
		}
		else
		{
			// 如果释放失败，恢复page状态为PAGE_IN_PCP，并将其重新挂回pcp链表
			page->state = PAGE_IN_PCP;
			page->is_free = false;
			page->refcount = 0;
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
