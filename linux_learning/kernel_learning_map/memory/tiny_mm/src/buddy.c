/*
 * tiny-mm / M2a: buddy allocator
 *
 * 这个文件保持独立，不依赖 M1 的 pmm.c。
 * M2a 的核心是 split/merge，以及 free_area 链表和 free_count 的同步。
 */

#include <errno.h>
#include <string.h>

#include "buddy.h"

static unsigned long page_index(const struct buddy *b, const struct page *page)
{
	return (unsigned long)(page - b->pages);
}

static bool block_valid(const struct buddy *b, const struct page *page,
			unsigned int order)
{
	unsigned long index;

	if (!b || !b->pages || !page)
		return false;
	if (page < b->pages || page >= b->pages + b->nr_pages)
		return false;

	index = page_index(b, page);
	if ((index & ((1UL << order) - 1)) != 0)
		return false;

	return index + (1UL << order) <= b->nr_pages;
}

static void free_list_push(struct buddy *b, struct page *page,
			   unsigned int order)
{
	page->order = order;
	page->is_free = true;
	page->refcount = 0;
	page->buddy_next = b->free_area[order].head;
	b->free_area[order].head = page;
	b->free_area[order].nr_free++;
}

static bool free_list_contains(const struct buddy *b, const struct page *page,
			       unsigned int order)
{
	const struct page *p;

	for (p = b->free_area[order].head; p; p = p->buddy_next) {
		if (p == page)
			return true;
	}
	return false;
}

static int free_list_remove(struct buddy *b, struct page *page,
			    unsigned int order)
{
	struct page **link = &b->free_area[order].head;

	while (*link && *link != page)
		link = &(*link)->buddy_next;

	if (!*link)
		return -EINVAL;

	*link = page->buddy_next;
	page->buddy_next = NULL;
	b->free_area[order].nr_free--;
	return 0;
}

/*
 * 合并 low 和 high 两个同 order 的块。
 * 合并后低地址块 low 变成 order+1 的空闲块头；
 * 高地址块 high 变成块内部页，元数据必须清理干净。
 */
static void merge_page_pair(struct page *low, struct page *high)
{
	low->order++;
	low->is_free = true;
	low->refcount = 0;
	low->buddy_next = NULL;

	high->order = 0;
	high->is_free = false;
	high->refcount = 0;
	high->buddy_next = NULL;
}

static int order_for_pages(unsigned long nr_pages, unsigned int *order)
{
	unsigned long n = nr_pages;
	unsigned int o = 0;

	/* nr_pages 必须是 2 的幂 */
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
	unsigned int o;

	if (!b || order > b->max_order)
		return NULL;

	for (o = order; o <= b->max_order; o++) {
		struct page *page = b->free_area[o].head;

		if (!page || b->free_area[o].nr_free == 0)
			continue;

		/* 如果 free_area 里的块头不合法，先不要改状态。 */
		if (!block_valid(b, page, o))
			return NULL;

		b->free_area[o].head = page->buddy_next;
		b->free_area[o].nr_free--;

		/*
		 * 只把高半挂到低一级 free_area；
		 * 低半 page 保持为候选，不进入任何 free list。
		 */
		while (o > order) {
			struct page *buddy = page + (1UL << (o - 1));

			free_list_push(b, buddy, o - 1);
			page->order = o - 1;
			b->split_count++;
			o--;
		}

		page->order = order;
		page->is_free = false;
		page->refcount = 1;
		page->buddy_next = NULL;
		b->free_count -= (1UL << order);
		return page;
	}

	return NULL;
}

int buddy_free(struct buddy *b, struct page *page, unsigned int order)
{
	unsigned long index;
	unsigned int cur;

	if (!b || !page || order > b->max_order)
		return -EINVAL;
	if (!block_valid(b, page, order))
		return -EINVAL;
	if (page->order != order || page->is_free || page->refcount != 1)
		return -EINVAL;

	/*
	 * 预检整条合并链：如果某个 buddy 标记为 free，但实际不在
	 * free_area 里，就直接返回 -EINVAL，不修改 page/free_count。
	 */
	index = page_index(b, page);
	for (cur = order; cur < b->max_order; cur++) {
		unsigned long buddy_index = index ^ (1UL << cur);
		struct page *buddy;

		if (buddy_index >= b->nr_pages)
			break;

		buddy = &b->pages[buddy_index];
		if (buddy->order != cur || !buddy->is_free)
			break;
		if (!free_list_contains(b, buddy, cur))
			return -EINVAL;

		if (buddy_index < index)
			index = buddy_index;
	}

	page->is_free = true;
	page->refcount = 0;
	b->free_count += (1UL << order);

	index = page_index(b, page);
	cur = order;
	while (cur < b->max_order) {
		unsigned long buddy_index = index ^ (1UL << cur);
		struct page *buddy;
		struct page *low;
		struct page *high;

		if (buddy_index >= b->nr_pages)
			break;

		buddy = &b->pages[buddy_index];
		if (buddy->order != cur || !buddy->is_free)
			break;

		/* 预检已保证 buddy 在链表里；这里移除失败说明状态被破坏。 */
		if (free_list_remove(b, buddy, cur) != 0)
			return -EINVAL;

		if (buddy_index < index) {
			low = buddy;
			high = &b->pages[index];
			index = buddy_index;
		} else {
			low = &b->pages[index];
			high = buddy;
		}

		merge_page_pair(low, high);
		page = low;
		cur++;
		b->merge_count++;
	}

	free_list_push(b, page, cur);
	return 0;
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
			if (!block_valid(b, p, i))
				return -EINVAL;
			if (!p->is_free || p->order != i || p->refcount != 0)
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
