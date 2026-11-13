/*
 * tiny-mm / M2: buddy 分配器
 *
 * 这个文件先保持独立，不依赖 M1 的 pmm.c。
 * 你只需要实现 buddy_alloc() 和 buddy_free() 两个核心函数；
 * buddy_init()/buddy_check() 已经搭好。
 */

#include <errno.h>
#include <string.h>
#include "../include/buddy.h"

static int page_to_pfn_m2(const struct page *page, const struct buddy *b)
{
	return (int)(page - b->pages);
}

static struct page *pfn_to_page_m2(int pfn, const struct buddy *b)
{
	return &b->pages[pfn];
}

static int find_buddy_pfn_m2(struct page *page, unsigned int order, const struct buddy *b)
{
	return page_to_pfn_m2(page, b) ^ (1UL << order);
}

static struct page *find_buddy_m2(struct page *page, unsigned int order, const struct buddy *b)
{
	return pfn_to_page_m2(find_buddy_pfn_m2(page, order, b), b);
}


static int order_for_pages(unsigned long nr_pages, unsigned int *order)
{
	unsigned long n = nr_pages;
	unsigned int o = 0;
	// nr_pages 必须是 2 的幂
	// 只有nr_pages是2的幂时，nr_pages & (nr_pages - 1) 才等于0
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
	/*
	 * TODO(M2):
	 *   1. 校验 b/order；
	 *   2. 从 order 开始向上找第一个非空 free_area；
	 *   3. 从该 free_area 摘下一个块；
	 *   4. 不断二分：低半继续作为候选，高半挂到低一级 free_area；
	 *   5. 最终低半就是分配结果；
	 *   6. 更新 page 字段和 free_count。
	 */

	if(!b || order > b->max_order)
		return NULL;

	for(unsigned int o = order; o <= b->max_order; o ++)
	{
		if(b->free_area[o].head != NULL && b->free_area[o].nr_free > 0)
		{
			
			// 当前order的free area不满足所申请的pages，向上查找，如找到则拆分处理
			while(o > order)
			{
				struct page *p = b->free_area[o].head;
				// 将当前order的free area的头部空闲块摘下，更新当前order free area的head，指向p的下一个空闲块
				b->free_area[o].head = p->buddy_next;
				b->free_area[o].nr_free --;

				// 将当前块拆分为两个order-1的块，低半部分继续作为候选，高半部分挂到低一级的free area

				// 1.计算高半部分buddy的地址
				// 2.初始化此buddy的order、is_free、refcount
				// 3.将高半部分挂到低一级的free area
				// 4.更新当前块的order
				// 5.o减1，继续循环，直到o等于order
				struct page *buddy = find_buddy_m2(p, o - 1, b);
				buddy->order = o - 1;
				buddy->is_free = true;
				buddy->refcount = 0;
				
				// 姑且先把拆封后的page挂到低一级free area的头部（此时并free area链表并没有按照地址顺序排列）
				p->buddy_next = buddy;
				buddy->buddy_next = b->free_area[o - 1].head;
				b->free_area[o - 1].head = p;
				b->free_area[o - 1].nr_free += 2;
				b->split_count ++;

				// 更新当前块的order
				p->order = o - 1;
				// 更新当前order free area的head，指向p的下一个空闲块、
				// 这部分操作必须放到操作完p之q前，否则p更新的是buddy_next，导致链表断裂
				
				o --;
			}
			// 如果当前order等于请求的order，则直接分配该块
			if(o == order)
			{
				// 指针p指向伙伴系统在当前order下的free area中的第一个空闲块
				struct page *p = b->free_area[o].head;
				b->free_area[o].head = p->buddy_next;
				b->free_area[o].nr_free --;
				b->free_count -= (1UL << order);
				p->is_free = false;
				p->refcount = 1;
				return p;
			}
		}
	}

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
	if(!b || !page || order > b->max_order)
		return -EINVAL;
	if(page < b->pages || page >= b->pages + b->nr_pages)
		return -EINVAL;
	if(page->order != order || page->is_free || page->refcount != 1)
		return -EINVAL;

	page->is_free = true;
	page->refcount = 0;
	b->free_count += (1UL << order);

	struct page *buddy;
	while(order < b->max_order)
	{
		buddy = find_buddy_m2(page, order, b);
		// 判断buddy page是否在buddy系统的pages范围内，是否是空闲块，是否与page同order
		if(buddy < b->pages || buddy >= b->pages + b->nr_pages)
			break;

		// 如果buddy不在空闲链表中，无法合并，直接挂回当前order的free area
		if(buddy->order == order)
		{
			if(buddy->is_free == false )
			{
				// 无法继续合并，将page挂回当前order的free area

				// 在buddy page前插入page
				struct page *prev = NULL;
				struct page *curr = b->free_area[order].head;
				if(curr == NULL)
				{
					// 空闲的free area list
					b->free_area[order].head = page;
					page->buddy_next = NULL;
					b->free_area[order].nr_free ++;
					return 0;
				}
				while(curr)
				{
					if(curr == buddy)
					{
						if(prev)
						{
							// prev != NULL，说明buddy不是free area链表的头部，将page插入到buddy前面
							prev->buddy_next = page;
							page->buddy_next = buddy;
						}
						else
						{
							// prev == NULL， 说明buddy是free area链表的头部，将page插入到buddy前面
							page->buddy_next = buddy;
							b->free_area[order].head = page;
						}
						b->free_area[order].nr_free ++;
						break;
					}
					prev = curr;
					curr = curr->buddy_next;
				}
			}
			else
			{
				// buddy是空闲块，可以合并
				struct page *prev = NULL;
				struct page *curr = b->free_area[order].head;

				while(curr)
				{
					if(find_buddy_m2(page, order, b) == buddy)
					{
						if(prev)
						{
							// prev != NULL，说明buddy不是free area链表的头部，将page和buddy从free area链表中摘下
							prev->buddy_next = buddy->buddy_next;
						}
						else
						{
							// prev == NULL， 说明buddy是free area链表的头部，将buddy摘下，buddy->buddy_next作为free area的头部
							b->free_area[order].head = buddy->buddy_next;
						}
						// order++，继续向上合并
						b->free_area[order].nr_free --;
						b->merge_count ++;
						page->order ++;
						order ++;
						break;
					}
					prev = curr;
					curr = curr->buddy_next;
				}
			}
		}

	}
	if(order == b->max_order)
	{
		// 如果已经合并到最大order，则将page挂回当前order的free area
		page->buddy_next = b->free_area[order].head;
		b->free_area[order].head = page;
		b->free_area[order].nr_free ++;
	}

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
			unsigned long index;

			// 判断 p 是否在 pages 范围内
			if (p < b->pages || p >= b->pages + b->nr_pages)
				return -EINVAL;
			// 判断 p 是否是空闲块头，p 的 order 是否与 free_area[i] 的 order 匹配，p 的 refcount 是否为 0
			if (!p->is_free || p->order != i || p->refcount != 0)
				return -EINVAL;

			index = (unsigned long)(p - b->pages);
			// 判断 p 是否按 2^i 对齐
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
