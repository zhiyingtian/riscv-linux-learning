#include "tmm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * M1 的物理内存模拟：
 *
 *   g_pmm.pages[]   : 连续的 struct page 数组，索引 0 对应 start_pfn
 *   g_pmm.bitmap[]  : 1 = 已分配，0 = 空闲
 *
 * 真正的 Linux 用 mem_map/vmemmap + buddy + pcp；
 * M1 先用最简单的 bitmap，把 page/PFN/paddr/zone/refcount 语义跑通。
 */
static struct pmm {
	struct page *pages;
	unsigned char *bitmap;
	unsigned long start_pfn;
	unsigned long nr_pages;
	unsigned long free_count;
	struct pglist_data node;
} g_pmm;

int pmm_init(unsigned long start_pfn, unsigned long nr_pages)
{
	unsigned long i;

	if (!nr_pages)
		return -EINVAL;

	memset(&g_pmm, 0, sizeof(g_pmm));

	g_pmm.pages = calloc(nr_pages, sizeof(*g_pmm.pages));
	g_pmm.bitmap = calloc(nr_pages, sizeof(*g_pmm.bitmap));
	if (!g_pmm.pages || !g_pmm.bitmap) {
		free(g_pmm.pages);
		free(g_pmm.bitmap);
		memset(&g_pmm, 0, sizeof(g_pmm));
		return -ENOMEM;
	}

	g_pmm.start_pfn = start_pfn;
	g_pmm.nr_pages = nr_pages;
	g_pmm.free_count = nr_pages;

	g_pmm.node.nr_zones = 1;
	g_pmm.node.node_start_pfn = start_pfn;
	g_pmm.node.node_spanned_pages = nr_pages;
	g_pmm.node.node_id = 0;

	g_pmm.node.node_zones[0].name = "ZONE_NORMAL";
	g_pmm.node.node_zones[0].zone_pgdat = &g_pmm.node;
	g_pmm.node.node_zones[0].start_pfn = start_pfn;
	g_pmm.node.node_zones[0].spanned_pages = nr_pages;
	g_pmm.node.node_zones[0].free_count = nr_pages;

	for (i = 0; i < nr_pages; i++) {
		g_pmm.pages[i].order = 0;
		g_pmm.pages[i].refcount = 0;
		g_pmm.pages[i].is_free = true;
		g_pmm.pages[i].zone_id = 0;
	}

	return pmm_check();
}

void pmm_exit(void)
{
	free(g_pmm.pages);
	free(g_pmm.bitmap);
	memset(&g_pmm, 0, sizeof(g_pmm));
}

/*
 * TODO(M1): 实现 page -> PFN。
 *
 * 提示：
 *   - g_pmm.pages 的索引 0 对应 g_pmm.start_pfn；
 *   - 先检查 page 是否在 [pages, pages + nr_pages) 内；
 *   - 越界返回 0 或专门错误值？本项目约定：调用者先用 pfn_to_page 校验，
 *     page_to_pfn 只处理合法 page；测试不会传非法 page。
 */
unsigned long page_to_pfn(const struct page *page)
{
	(void)page;

	if(page < g_pmm.pages || page >= g_pmm.pages + g_pmm.nr_pages)
		return 0;

	return page - g_pmm.pages + g_pmm.start_pfn;
}

/*
 * TODO(M1): 实现 PFN -> page。
 *
 * 提示：
 *   - pfn < start_pfn 或 >= start_pfn + nr_pages 时返回 NULL；
 *   - 返回 &g_pmm.pages[pfn - start_pfn]。
 */
struct page *pfn_to_page(unsigned long pfn)
{
	if(pfn < g_pmm.start_pfn || pfn >= g_pmm.start_pfn + g_pmm.nr_pages)
		return NULL;
	return &g_pmm.pages[pfn - g_pmm.start_pfn];
}

bool pfn_valid(unsigned long pfn)
{
	return pfn >= g_pmm.start_pfn &&
	       pfn - g_pmm.start_pfn < g_pmm.nr_pages;
}

/*
 * TODO(M1): PFN -> 物理字节地址。
 *
 * paddr = pfn << PAGE_SHIFT
 */
phys_addr_t pfn_to_phys(unsigned long pfn)
{
	return pfn << PAGE_SHIFT;
}

/*
 * TODO(M1): 物理字节地址 -> PFN。
 *
 * pfn = paddr >> PAGE_SHIFT
 */
unsigned long phys_to_pfn(phys_addr_t paddr)
{
	return paddr >> PAGE_SHIFT;
}

/*
 * TODO(M1): 分配 order 个连续页。
 *
 * M1 只支持 order == 0：
 *   - order != 0 直接返回 NULL；
 *   - 扫描 bitmap 找第一个 0；
 *   - 置 bitmap[i] = 1；
 *   - page->is_free = false；
 *   - page->order = 0；
 *   - page->refcount = 1；
 *   - g_pmm.free_count--；
 *   - g_pmm.node.node_zones[0].free_count--；
 *   - 返回 &g_pmm.pages[i]。
 *
 * 注意：不要用 malloc 返回页；页来自 g_pmm.pages 数组。
 */
struct page *pmm_alloc_pages(unsigned int order)
{
	if(order != 0)
		return NULL;
	for (unsigned i = 0; i < g_pmm.nr_pages; i ++) {
		if(g_pmm.bitmap[i] == 0) {
			g_pmm.bitmap[i] = 1;
			g_pmm.pages[i].is_free = false;
			g_pmm.pages[i].order = 0;
			g_pmm.pages[i].refcount = 1;
			g_pmm.free_count --;
			g_pmm.node.node_zones[0].free_count --;
			return &g_pmm.pages[i];
		}
	}
	// no page avail.
	return NULL;
}

/*
 * TODO(M1): 释放页。
 *
 * 返回 0 成功，-EINVAL 失败。必须检查：
 *   - page 非空且在 pages 数组范围内；
 *   - order == 0；
 *   - page->is_free == false；
 *   - page->order == 0；
 *   - page->refcount == 1；
 * 成功时：
 *   - bitmap[i] = 0；
 *   - page->is_free = true；
 *   - page->refcount = 0；
 *   - page->order = 0；
 *   - g_pmm.free_count++；
 *   - g_pmm.node.node_zones[0].free_count++。
 *
 * 如果第二次 free 同一个 page，必须返回错误，不能重复 free_count++。
 */
int pmm_free_pages(struct page *page, unsigned int order)
{
	(void)page;
	(void)order;
	if(!page || page < g_pmm.pages || page >= g_pmm.pages + g_pmm.nr_pages || page->refcount == 0 || order != 0 || \
		page->is_free == true || page->order != 0)
		return -EINVAL;
	g_pmm.bitmap[page - g_pmm.pages] = 0;
	page->is_free = true;
	page->refcount = 0;
	page->order = 0;
	g_pmm.free_count ++;
	g_pmm.node.node_zones[0].free_count ++;

	return 0;
}

struct zone *page_zone(const struct page *page)
{
	if (!page || page < g_pmm.pages || page >= g_pmm.pages + g_pmm.nr_pages)
		return NULL;
	return &g_pmm.node.node_zones[page->zone_id];
}

struct pglist_data *page_pgdat(const struct page *page)
{
	struct zone *zone = page_zone(page);

	return zone ? zone->zone_pgdat : NULL;
}

int page_to_nid(const struct page *page)
{
	/* M1 只有一个 node；Linux 里从 page->flags 的 NODES_* 位段取。 */
	return page ? g_pmm.node.node_id : -1;
}

unsigned long pmm_free_count(void)
{
	return g_pmm.free_count;
}

unsigned long pmm_total_count(void)
{
	return g_pmm.nr_pages;
}

/*
 * 自检：所有 page/bitmap/free_count 必须一致。
 *
 * 这个函数已经实现，用来帮你抓“状态不同步”的 bug。
 */
int pmm_check(void)
{
	unsigned long i;
	unsigned long free_count = 0;

	if (!g_pmm.pages || !g_pmm.bitmap)
		return -EINVAL;

	for (i = 0; i < g_pmm.nr_pages; i++) {
		struct page *page = &g_pmm.pages[i];
		bool allocated = g_pmm.bitmap[i] != 0;

		if (allocated) {
			if (page->is_free)
				return -EINVAL;
			if (page->refcount != 1)
				return -EINVAL;
			if (page->order != 0)
				return -EINVAL;
		} else {
			if (!page->is_free)
				return -EINVAL;
			if (page->refcount != 0)
				return -EINVAL;
			if (page->order != 0)
				return -EINVAL;
			free_count++;
		}
	}

	if (free_count != g_pmm.free_count)
		return -EINVAL;
	if (g_pmm.node.node_zones[0].free_count != free_count)
		return -EINVAL;

	return 0;
}
