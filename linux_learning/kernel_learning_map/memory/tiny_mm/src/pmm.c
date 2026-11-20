#include "tmm.h"
#include "buddy.h"
#include "pcp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * tiny-mm / M2c: 把 M1 的 bitmap 物理页分配器切到 buddy + pcp。
 *
 * M2c 的目标：
 *   - pmm_init() 初始化 buddy 和 pcp；
 *   - pmm_alloc_pages(order) 分配 0..MAX_ORDER-1 页；
 *   - pmm_free_pages(page, order) 释放并尝试 merge；
 *   - order 0 走 pcp，order > 0 走 buddy；
 *   - pmm_check() 同时检查 buddy 和 pcp。
 *
 * 注意：
 *   - buddy 要求 nr_pages 是 2 的幂；
 *   - pcp 里的页不进入 buddy 的 free_area，但计入 pmm 的总空闲页数。
 */

#define PMM_PCP_HIGH 8
#define PMM_PCP_BATCH 4

static struct pmm {
	struct page *pages;
	unsigned long start_pfn;
	unsigned long nr_pages;

	/* 对上层来说“可用空闲页” = buddy 空闲 + pcp 缓存。 */
	unsigned long free_count;

	struct pglist_data node;
	struct buddy buddy;
	struct pcp pcp;
} g_pmm;

static unsigned long pmm_total_free(void)
{
	return g_pmm.buddy.free_count + g_pmm.pcp.count;
}

int pmm_init(unsigned long start_pfn, unsigned long nr_pages)
{
	unsigned long i;

	if (!nr_pages)
		return -EINVAL;

	memset(&g_pmm, 0, sizeof(g_pmm));

	g_pmm.pages = calloc(nr_pages, sizeof(*g_pmm.pages));
	if (!g_pmm.pages)
		return -ENOMEM;

	g_pmm.start_pfn = start_pfn;
	g_pmm.nr_pages = nr_pages;

	if (buddy_init(&g_pmm.buddy, g_pmm.pages, nr_pages) != 0)
		goto fail;

	/* 初始化 zone 元数据。 */
	g_pmm.node.nr_zones = 1;
	g_pmm.node.node_start_pfn = start_pfn;
	g_pmm.node.node_spanned_pages = nr_pages;
	g_pmm.node.node_id = 0;

	g_pmm.node.node_zones[0].name = "ZONE_NORMAL";
	g_pmm.node.node_zones[0].zone_pgdat = &g_pmm.node;
	g_pmm.node.node_zones[0].start_pfn = start_pfn;
	g_pmm.node.node_zones[0].spanned_pages = nr_pages;

	for (i = 0; i < nr_pages; i++)
		g_pmm.pages[i].zone_id = 0;

	if (pcp_init(&g_pmm.pcp, &g_pmm.buddy,
		     PMM_PCP_HIGH, PMM_PCP_BATCH) != 0)
		goto fail;

	g_pmm.free_count = pmm_total_free();
	g_pmm.node.node_zones[0].free_count = g_pmm.free_count;

	return pmm_check();

fail:
	free(g_pmm.pages);
	memset(&g_pmm, 0, sizeof(g_pmm));
	return -EINVAL;
}

void pmm_exit(void)
{
	if (g_pmm.pcp.buddy)
		pcp_drain_all(&g_pmm.pcp);

	pcp_exit(&g_pmm.pcp);
	buddy_exit(&g_pmm.buddy);
	free(g_pmm.pages);
	memset(&g_pmm, 0, sizeof(g_pmm));
}

unsigned long page_to_pfn(const struct page *page)
{
	if (!page || page < g_pmm.pages ||
	    page >= g_pmm.pages + g_pmm.nr_pages)
		return 0;

	return (unsigned long)(page - g_pmm.pages) + g_pmm.start_pfn;
}

struct page *pfn_to_page(unsigned long pfn)
{
	if (pfn < g_pmm.start_pfn ||
	    pfn - g_pmm.start_pfn >= g_pmm.nr_pages)
		return NULL;

	return &g_pmm.pages[pfn - g_pmm.start_pfn];
}

bool pfn_valid(unsigned long pfn)
{
	return pfn >= g_pmm.start_pfn &&
	       pfn - g_pmm.start_pfn < g_pmm.nr_pages;
}

phys_addr_t pfn_to_phys(unsigned long pfn)
{
	return (phys_addr_t)pfn << PAGE_SHIFT;
}

unsigned long phys_to_pfn(phys_addr_t paddr)
{
	return (unsigned long)(paddr >> PAGE_SHIFT);
}

struct page *pmm_alloc_pages(unsigned int order)
{
	/*
	 * M2c:
	 *   1. order 0 调用 pcp_alloc_pages(&g_pmm.pcp, 0)；
	 *   2. order > 0 调用 buddy_alloc(&g_pmm.buddy, order)；
	 *   3. 成功后更新 g_pmm.free_count 和 zone.free_count；
	 *   4. 失败返回 NULL，不修改状态。
	 */

	struct page *page;

	if (order == 0) {
		page = pcp_alloc_pages(&g_pmm.pcp, order);
		if (!page)
			return NULL;
	} else {
		page = buddy_alloc(&g_pmm.buddy, order);

		/*
		 * 高阶分配失败时，pcp 里的 order-0 缓存页会挡住合并。
		 * 先把 pcp 还回 buddy 再试一次；drain 只搬页，
		 * buddy.free_count + pcp.count 不变，所以 free_count 不用改。
		 */
		if (!page && g_pmm.pcp.count > 0) {
			pcp_drain_all(&g_pmm.pcp);
			page = buddy_alloc(&g_pmm.buddy, order);
		}
		if (!page)
			return NULL;
	}
	g_pmm.free_count -= 1UL << order;
	g_pmm.node.node_zones[0].free_count -= 1UL << order;

	return page;
}

int pmm_free_pages(struct page *page, unsigned int order)
{
	/*
	 * M2c:
	 *   1. 校验 page 在 pages 范围内；
	 *   2. order 0 调用 pcp_free_pages(&g_pmm.pcp, page, 0)；
	 *   3. order > 0 调用 buddy_free(&g_pmm.buddy, page, order)；
	 *   4. 成功后更新 g_pmm.free_count 和 zone.free_count；
	 *   5. 失败返回 -EINVAL。
	 */

	if (!page || page < g_pmm.pages || page >= g_pmm.pages + g_pmm.nr_pages)
		return -EINVAL;
	if (order == 0)
	{
		if (pcp_free_pages(&g_pmm.pcp, page, order))
			return -EINVAL;
	}
	else
	{
		if (buddy_free(&g_pmm.buddy, page, order))
			return -EINVAL;
	}

	g_pmm.free_count += 1UL << order;
	g_pmm.node.node_zones[0].free_count += 1UL << order;

	return 0;
}

struct zone *page_zone(const struct page *page)
{
	if (!page || page < g_pmm.pages ||
	    page >= g_pmm.pages + g_pmm.nr_pages)
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

int pmm_check(void)
{
	unsigned long free_count;

	if (!g_pmm.pages || !g_pmm.buddy.pages || !g_pmm.pcp.buddy)
		return -EINVAL;
	if (g_pmm.pcp.buddy != &g_pmm.buddy)
		return -EINVAL;
	if (buddy_check(&g_pmm.buddy) != 0)
		return -EINVAL;
	if (pcp_check(&g_pmm.pcp) != 0)
		return -EINVAL;

	free_count = pmm_total_free();
	if (free_count != g_pmm.free_count)
		return -EINVAL;
	if (g_pmm.node.node_zones[0].free_count != free_count)
		return -EINVAL;

	return 0;
}
