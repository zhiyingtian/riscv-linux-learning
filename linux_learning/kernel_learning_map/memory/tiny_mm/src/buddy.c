/*
 * tiny-mm / M2: buddy 分配器
 *
 * 这个文件先保持独立，不依赖 M1 的 pmm.c。
 * 你只需要实现 buddy_alloc() 和 buddy_free() 两个核心函数；
 * buddy_init()/buddy_check() 已经搭好。
 */

#include <errno.h>
#include <string.h>

#include "buddy.h"

static int order_for_pages(unsigned long nr_pages, unsigned int *order)
{
	unsigned long n = nr_pages;
	unsigned int o = 0;

	if (nr_pages == 0 || (nr_pages & (nr_pages - 1)) != 0)
		return -EINVAL;

	while (n > 1) {
		n >>= 1;
		o++;
	}

	if (o >= MAX_ORDER)
		return -EINVAL;

	*order = o;
	return 0;
}

int buddy_init(struct buddy *b, struct page *pages, unsigned long nr_pages)
{
	unsigned int max_order;
	unsigned long i;

	if (!b || !pages || nr_pages == 0)
		return -EINVAL;

	if (order_for_pages(nr_pages, &max_order) != 0)
		return -EINVAL;

	memset(b, 0, sizeof(*b));
	memset(pages, 0, nr_pages * sizeof(*pages));

	b->pages = pages;
	b->nr_pages = nr_pages;
	b->max_order = max_order;

	for (i = 0; i < nr_pages; i++) {
		pages[i].order = 0;
		pages[i].is_free = false;
		pages[i].refcount = 0;
		pages[i].buddy_next = NULL;
	}

	/* 初始时整个数组是一个 max_order 的空闲块，块头是 pages[0]。 */
	pages[0].order = max_order;
	pages[0].is_free = true;
	b->free_area[max_order].head = &pages[0];
	b->free_area[max_order].nr_free = 1;
	b->free_count = nr_pages;

	return buddy_check(b);
}

void buddy_exit(struct buddy *b)
{
	if (b)
		memset(b, 0, sizeof(*b));
}

struct page *buddy_alloc(struct buddy *b, unsigned int order)
{
	(void)b;
	(void)order;

	/*
	 * TODO(M2):
	 *   1. 校验 b/order；
	 *   2. 从 order 开始向上找第一个非空 free_area；
	 *   3. 从该 free_area 摘下一个块；
	 *   4. 不断二分：低半继续作为候选，高半挂到低一级 free_area；
	 *   5. 最终低半就是分配结果；
	 *   6. 更新 page 字段和 free_count。
	 */
	return NULL;
}

int buddy_free(struct buddy *b, struct page *page, unsigned int order)
{
	(void)b;
	(void)page;
	(void)order;

	/*
	 * TODO(M2):
	 *   1. 校验 b/page/order、page 对齐、page 状态；
	 *   2. 把 page 标记为空闲块；
	 *   3. 循环找 buddy：page 与 buddy 必须同 order 且 buddy 在空闲链表里；
	 *   4. 合并后继续向上，直到不能合并；
	 *   5. 把最终块挂回对应 free_area，更新 free_count。
	 */
	return -EINVAL;
}

unsigned long buddy_free_count(const struct buddy *b)
{
	return b ? b->free_count : 0;
}

int buddy_check(const struct buddy *b)
{
	unsigned long i;
	unsigned long free_pages = 0;

	if (!b || !b->pages || b->nr_pages == 0)
		return -EINVAL;
	if (b->max_order >= MAX_ORDER)
		return -EINVAL;

	for (i = 0; i <= b->max_order; i++) {
		const struct page *p = b->free_area[i].head;
		unsigned long blocks = 0;

		while (p) {
			unsigned long index;

			if (p < b->pages || p >= b->pages + b->nr_pages)
				return -EINVAL;
			if (!p->is_free || p->order != i || p->refcount != 0)
				return -EINVAL;

			index = (unsigned long)(p - b->pages);
			if ((index & ((1UL << i) - 1)) != 0)
				return -EINVAL;

			blocks++;
			if (blocks > b->nr_pages)
				return -EINVAL;
			p = p->buddy_next;
		}

		if (blocks != b->free_area[i].nr_free)
			return -EINVAL;
		free_pages += blocks * (1UL << i);
	}

	if (free_pages != b->free_count)
		return -EINVAL;

	return 0;
}
