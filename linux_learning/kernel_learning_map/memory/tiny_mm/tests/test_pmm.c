#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/tmm.h"
#include "test_runner.h"

#define TEST_START_PFN 0x100UL
#define TEST_NR_PAGES 64UL
#define TEST_MAX_ORDER 6U	/* 64 页 => max_order 6 */

static int test_init(void)
{
	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	CHECK(pmm_total_count() == TEST_NR_PAGES);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_alloc_free(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	CHECK(page->refcount == 1);
	CHECK(page->is_free == false);
	CHECK(page->order == 0);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(page->refcount == 0);
	CHECK(page->is_free == false);
	CHECK(page->state == PAGE_IN_PCP);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_pfn_page_roundtrip(void)
{
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	for (i = 0; i < TEST_NR_PAGES; i++) {
		unsigned long pfn = TEST_START_PFN + i;
		struct page *page = pfn_to_page(pfn);

		CHECK(page != NULL);
		CHECK(pfn_valid(pfn));
		CHECK(page_to_pfn(page) == pfn);
		CHECK(pfn_to_page(page_to_pfn(page)) == page);
	}

	CHECK(!pfn_valid(TEST_START_PFN - 1));
	CHECK(!pfn_valid(TEST_START_PFN + TEST_NR_PAGES));
	CHECK(pfn_to_page(TEST_START_PFN - 1) == NULL);
	CHECK(pfn_to_page(TEST_START_PFN + TEST_NR_PAGES) == NULL);
	return 0;
}

static int test_paddr_roundtrip(void)
{
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	for (i = 0; i < TEST_NR_PAGES; i++) {
		unsigned long pfn = TEST_START_PFN + i;
		phys_addr_t paddr = pfn_to_phys(pfn);

		CHECK(paddr == ((phys_addr_t)pfn << PAGE_SHIFT));
		CHECK(phys_to_pfn(paddr) == pfn);
		CHECK(phys_to_pfn(paddr + PAGE_SIZE - 1) == pfn);
	}
	return 0;
}

static int test_exhaust(void)
{
	struct page *held[TEST_NR_PAGES];
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	for (i = 0; i < TEST_NR_PAGES; i++) {
		held[i] = pmm_alloc_pages(0);
		CHECK(held[i] != NULL);
	}

	CHECK(pmm_alloc_pages(0) == NULL);
	CHECK(pmm_free_count() == 0);
	CHECK(pmm_check() == 0);

	for (i = 0; i < TEST_NR_PAGES; i++)
		CHECK(pmm_free_pages(held[i], 0) == 0);

	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_double_free(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_pages(page, 0) < 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_wrong_order(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);

	/* 分配 order=0，却按 order=1 释放：必须拒绝 */
	CHECK(pmm_free_pages(page, 1) < 0);
	CHECK(page->is_free == false);
	CHECK(page->refcount == 1);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	return 0;
}

static int test_order_supported(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	page = pmm_alloc_pages(2);
	CHECK(page != NULL);
	CHECK(page->order == 2);
	CHECK(page->refcount == 1);
	CHECK(page->is_free == false);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 4);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 2) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_zone(void)
{
	struct page *page;
	struct zone *zone;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	zone = page_zone(page);
	CHECK(zone != NULL);
	CHECK(zone->start_pfn <= page_to_pfn(page));
	CHECK(page_to_pfn(page) < zone->start_pfn + zone->spanned_pages);
	CHECK(zone->free_count == pmm_free_count());
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_pgdat(void)
{
	struct page *page;
	struct zone *zone;
	struct pglist_data *pgdat;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);

	zone = page_zone(page);
	pgdat = page_pgdat(page);
	CHECK(zone != NULL);
	CHECK(pgdat != NULL);
	CHECK(pgdat == zone->zone_pgdat);
	CHECK(pgdat->nr_zones == 1);
	CHECK(pgdat->node_id == 0);
	CHECK(pgdat->node_start_pfn == TEST_START_PFN);
	CHECK(pgdat->node_spanned_pages == TEST_NR_PAGES);
	CHECK(page_to_nid(page) == 0);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_random_stress(void)
{
	struct page *slots[TEST_NR_PAGES] = { 0 };
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	srand(0x5eed);

	for (i = 0; i < 100000; i++) {
		unsigned long idx = (unsigned long)rand() % TEST_NR_PAGES;

		if (slots[idx]) {
			CHECK(pmm_free_pages(slots[idx], 0) == 0);
			slots[idx] = NULL;
		} else {
			slots[idx] = pmm_alloc_pages(0);
			CHECK(slots[idx] != NULL);
		}
	}

	for (i = 0; i < TEST_NR_PAGES; i++) {
		if (slots[i]) {
			CHECK(pmm_free_pages(slots[i], 0) == 0);
			slots[i] = NULL;
		}
	}

	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_free_order_mismatch(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	page = pmm_alloc_pages(2);
	CHECK(page != NULL);
	CHECK(page->order == 2);

	/* order 2 的块头按 order 0 释放：必须拒绝，且状态不能变 */
	CHECK(pmm_free_pages(page, 0) < 0);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(page->refcount == 1);
	CHECK(page->order == 2);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 4);
	CHECK(pmm_check() == 0);

	/* 非块头、未对齐、空指针也不能释放 */
	CHECK(pmm_free_pages(page + 1, 0) < 0);
	CHECK(pmm_free_pages(page + 1, 2) < 0);
	CHECK(pmm_free_pages(NULL, 0) < 0);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 2) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_high_order_merge(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(2);
	CHECK(page != NULL);

	CHECK(pmm_free_pages(page, 2) == 0);
	/* 唯一的 order-2 块释放后应一路合并回 order-6 */
	CHECK(page->order == TEST_MAX_ORDER);
	CHECK(page->is_free == true);
	CHECK(page->refcount == 0);
	CHECK(page->state == PAGE_IN_BUDDY);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_double_free_high_order(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(1);
	CHECK(page != NULL);

	CHECK(pmm_free_pages(page, 1) == 0);
	CHECK(pmm_free_pages(page, 1) < 0);
	CHECK(page->order == TEST_MAX_ORDER);
	CHECK(page->is_free == true);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_order0_pcp_hit(void)
{
	struct page *page;
	struct page *again;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);

	/* 刚 free 的页在 pcp 头部，下一次 order-0 分配应原样拿回 */
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(page->state == PAGE_IN_PCP);
	again = pmm_alloc_pages(0);
	CHECK(again == page);
	CHECK(again->state == PAGE_ALLOCATED);
	CHECK(again->refcount == 1);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(again, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	return 0;
}

static int test_fragmentation(void)
{
	struct page *held[TEST_NR_PAGES];
	struct page *big;
	unsigned long i;
	unsigned long freed = 0;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	for (i = 0; i < TEST_NR_PAGES; i++) {
		held[i] = pmm_alloc_pages(0);
		CHECK(held[i] != NULL);
	}

	/* 按物理页号奇偶释放：每个空闲页的邻居都还被占着 */
	for (i = 0; i < TEST_NR_PAGES; i++) {
		if ((page_to_pfn(held[i]) & 1UL) == 0) {
			CHECK(pmm_free_pages(held[i], 0) == 0);
			held[i] = NULL;
			freed++;
		}
	}
	CHECK(freed == TEST_NR_PAGES / 2);
	CHECK(pmm_free_count() == TEST_NR_PAGES / 2);

	/* 没有连续两页，order-1 必须失败 */
	CHECK(pmm_alloc_pages(1) == NULL);
	CHECK(pmm_check() == 0);

	for (i = 0; i < TEST_NR_PAGES; i++)
		if (held[i])
			CHECK(pmm_free_pages(held[i], 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);

	/* 全部释放后最大块应恢复；pcp 里的缓存页需要先 drain */
	big = pmm_alloc_pages(TEST_MAX_ORDER);
	CHECK(big != NULL);
	CHECK(pmm_free_pages(big, TEST_MAX_ORDER) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_exhaust_high_order(void)
{
	struct page *held[TEST_NR_PAGES / 8];
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	for (i = 0; i < TEST_NR_PAGES / 8; i++) {
		held[i] = pmm_alloc_pages(3);
		CHECK(held[i] != NULL);
	}
	CHECK(pmm_alloc_pages(3) == NULL);
	CHECK(pmm_free_count() == 0);
	CHECK(pmm_check() == 0);

	for (i = 0; i < TEST_NR_PAGES / 8; i++)
		CHECK(pmm_free_pages(held[i], 3) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(held[0]->order == TEST_MAX_ORDER);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_alloc_order_limit(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	CHECK(pmm_alloc_pages(TEST_MAX_ORDER + 1) == NULL);
	CHECK(pmm_alloc_pages(MAX_ORDER) == NULL);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);

	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	return 0;
}

static int test_high_order_drains_pcp(void)
{
	struct page *page;
	struct page *big;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	CHECK(pmm_free_pages(page, 0) == 0);	/* pcp 里留 1 页 */

	big = pmm_alloc_pages(TEST_MAX_ORDER);
	CHECK(big != NULL);			/* 应先 drain 再分配整块 */
	CHECK(pmm_free_count() == 0);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(big, TEST_MAX_ORDER) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

struct test_block {
	struct page *page;
	unsigned int order;
};

static int test_random_stress_all_orders(void)
{
	struct test_block blocks[TEST_NR_PAGES];
	struct page *big;
	unsigned int nr = 0;
	unsigned long i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	srand(0x5eed);

	for (i = 0; i < 100000; i++) {
		if (nr > 0 && (rand() & 1)) {
			unsigned int k = (unsigned int)rand() % nr;

			CHECK(pmm_free_pages(blocks[k].page, blocks[k].order) == 0);
			blocks[k] = blocks[--nr];
		} else {
			unsigned int order = (unsigned int)rand() % (TEST_MAX_ORDER + 1);
			struct page *page = pmm_alloc_pages(order);

			/* 碎片化时高阶失败是允许的，失败不能改状态 */
			if (page) {
				CHECK(page->order == order);
				CHECK(page->state == PAGE_ALLOCATED);
				blocks[nr].page = page;
				blocks[nr].order = order;
				nr++;
			}
		}
		CHECK(pmm_check() == 0);	/* 每次操作后审计 */
	}

	while (nr > 0) {
		nr--;
		CHECK(pmm_free_pages(blocks[nr].page, blocks[nr].order) == 0);
	}
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);

	big = pmm_alloc_pages(TEST_MAX_ORDER);
	CHECK(big != NULL);
	CHECK(pmm_free_pages(big, TEST_MAX_ORDER) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M2c: pmm with buddy + pcp\n");
	printf("---------------------------------\n");

	failures += run_test("init", test_init);
	failures += run_test("alloc/free", test_alloc_free);
	failures += run_test("pfn/page roundtrip", test_pfn_page_roundtrip);
	failures += run_test("pfn/paddr roundtrip", test_paddr_roundtrip);
	failures += run_test("exhaust", test_exhaust);
	failures += run_test("double free", test_double_free);
	failures += run_test("wrong order", test_wrong_order);
	failures += run_test("order supported", test_order_supported);
	failures += run_test("zone", test_zone);
	failures += run_test("pgdat", test_pgdat);
	failures += run_test("random stress", test_random_stress);
	failures += run_test("free order mismatch", test_free_order_mismatch);
	failures += run_test("high order merge", test_high_order_merge);
	failures += run_test("double free high order", test_double_free_high_order);
	failures += run_test("order0 pcp hit", test_order0_pcp_hit);
	failures += run_test("fragmentation", test_fragmentation);
	failures += run_test("exhaust high order", test_exhaust_high_order);
	failures += run_test("order limit", test_alloc_order_limit);
	failures += run_test("high order drains pcp", test_high_order_drains_pcp);
	failures += run_test("random stress all orders", test_random_stress_all_orders);

	printf("---------------------------------\n");
	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
