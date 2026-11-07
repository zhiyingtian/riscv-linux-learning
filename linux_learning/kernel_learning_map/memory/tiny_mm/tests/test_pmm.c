#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tmm.h"
#include "test_runner.h"

#define TEST_START_PFN 0x100UL
#define TEST_NR_PAGES 64UL

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
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(page->refcount == 0);
	CHECK(page->is_free == true);
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

	/* 边界：越界要返回 NULL */
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

		/* 非页对齐地址向下取整到同一个 PFN */
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
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	/* 正确的 order 仍然可以释放 */
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	return 0;
}

static int test_order_unsupported(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	/* 先确认 order=0 能工作，再确认 order>0 被拒绝 */
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	CHECK(pmm_alloc_pages(1) == NULL);
	CHECK(pmm_free_count() == TEST_NR_PAGES - 1);
	CHECK(pmm_check() == 0);

	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
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

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M1: physical page model\n");
	printf("---------------------------------\n");

	failures += run_test("init", test_init);
	failures += run_test("alloc/free", test_alloc_free);
	failures += run_test("pfn/page roundtrip", test_pfn_page_roundtrip);
	failures += run_test("pfn/paddr roundtrip", test_paddr_roundtrip);
	failures += run_test("exhaust", test_exhaust);
	failures += run_test("double free", test_double_free);
	failures += run_test("wrong order", test_wrong_order);
	failures += run_test("order unsupported", test_order_unsupported);
	failures += run_test("zone", test_zone);
	failures += run_test("pgdat", test_pgdat);
	failures += run_test("random stress", test_random_stress);

	printf("---------------------------------\n");
	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
