/*
 * tiny-mm / M2b: per-CPU page cache
 *
 * 核心 TODO：
 *   - pcp_alloc_pages()
 *   - pcp_free_pages()
 *   - pcp_drain()
 *
 * pcp_init()/pcp_check() 已经实现。
 */

#include <errno.h>
#include <string.h>

#include "pcp.h"

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
	(void)p;
	(void)order;

	/*
	 * TODO(M2b):
	 *   order > 0：直接 buddy_alloc()。
	 *   order == 0：
	 *     1. pcp 链表非空：摘头，返回；
	 *     2. pcp 为空：从 buddy 批量 refill，--alloc_refills，
	 *        再把其中一页返回；
	 *     3. buddy 也拿不到：--alloc_misses，返回 NULL。
	 */
	return NULL;
}

int pcp_free_pages(struct pcp *p, struct page *page, unsigned int order)
{
	(void)p;
	(void)page;
	(void)order;

	/*
	 * TODO(M2b):
	 *   order > 0：直接 buddy_free()。
	 *   order == 0：
	 *     1. 校验 page 状态；
	 *     2. 挂到 pcp 链表头，count++；
	 *     3. count >= high 时 drain。
	 */
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
	return 0;
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
		if (!page->is_free || page->refcount != 0 || page->order != 0)
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
