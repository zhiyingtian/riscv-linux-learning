#include <stdio.h>

#include "../include/tmm.h"
#include "test_runner.h"

#define TEST_START_PFN 0x100UL
#define TEST_NR_PAGES 64UL

static struct zone *test_zone(void)
{
	return page_zone(pfn_to_page(TEST_START_PFN));
}

static int test_init(void)
{
	struct zone *zone;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	zone = test_zone();
	CHECK(zone != NULL);
	CHECK(zone->watermark[WMARK_MIN] > 0);
	CHECK(zone->watermark[WMARK_MIN] < zone->watermark[WMARK_LOW]);
	CHECK(zone->watermark[WMARK_LOW] < zone->watermark[WMARK_HIGH]);
	CHECK(zone->watermark[WMARK_HIGH] <= pmm_total_count());
	CHECK(zone->reclaim_needed == false);
	CHECK(zone->reclaim_wakeups == 0);
	CHECK(pmm_watermark_free() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_watermark_order(void)
{
	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	/* 初始 64 页，低水位 10：order=0 需要 11 页，可以通过。 */
	CHECK(pmm_watermark_ok(0, WMARK_LOW) == true);
	/* order=6 需要整个 64 页，再加水印后不满足。 */
	CHECK(pmm_watermark_ok(6, WMARK_LOW) == false);
	CHECK(pmm_watermark_ok(11, WMARK_LOW) == false);
	return 0;
}

static int test_pcp_not_counted(void)
{
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	page = pmm_alloc_pages(0);
	CHECK(page != NULL);

	/*
	 * order-0 alloc 会 refill 4 页：1 页返回，3 页留在 pcp。
	 * 水位只看 buddy：60 页；总空闲仍是 63 页。
	 */
	CHECK(pmm_watermark_free() == 60);
	CHECK(pmm_free_count() == 63);
	CHECK(pmm_watermark_free() < pmm_free_count());
	CHECK(pmm_check() == 0);

	/* free 后 page 回到 pcp，buddy 仍然是 60 页。 */
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_watermark_free() == 60);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_hard_uses_reserve(void)
{
	struct page *held[TEST_NR_PAGES];
	struct page *page;
	struct zone *zone;
	unsigned int n = 0;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	zone = test_zone();
	CHECK(zone != NULL);

	while (pmm_watermark_free() > zone->watermark[WMARK_MIN]) {
		CHECK(n < TEST_NR_PAGES);
		held[n] = pmm_alloc_pages(0);
		CHECK(held[n] != NULL);
		n++;
	}

	/* 普通分配不能动 min 预留，hard 分配可以。 */
	CHECK(pmm_alloc_pages(0) == NULL);
	page = pmm_alloc_pages_flags(0, PMM_ALLOC_HARD);
	CHECK(page != NULL);
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_low_marks_reclaim(void)
{
	struct page *held[TEST_NR_PAGES];
	struct zone *zone;
	unsigned int n = 0;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	zone = test_zone();
	CHECK(zone != NULL);

	while (pmm_watermark_free() >= zone->watermark[WMARK_LOW]) {
		CHECK(n < TEST_NR_PAGES);
		held[n] = pmm_alloc_pages(0);
		CHECK(held[n] != NULL);
		n++;
	}

	CHECK(pmm_watermark_free() < zone->watermark[WMARK_LOW]);
	CHECK(pmm_reclaim_needed() == true);
	CHECK(pmm_reclaim_wakeups() == 1);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_high_clears_reclaim(void)
{
	struct page *held[TEST_NR_PAGES];
	struct zone *zone;
	unsigned int n = 0;
	unsigned int i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	zone = test_zone();
	CHECK(zone != NULL);

	while (pmm_watermark_free() >= zone->watermark[WMARK_LOW]) {
		CHECK(n < TEST_NR_PAGES);
		held[n] = pmm_alloc_pages(0);
		CHECK(held[n] != NULL);
		n++;
	}
	CHECK(pmm_reclaim_needed() == true);

	for (i = 0; i < n; i++)
		CHECK(pmm_free_pages(held[i], 0) == 0);

	CHECK(pmm_watermark_free() >= zone->watermark[WMARK_HIGH]);
	CHECK(pmm_reclaim_needed() == false);
	CHECK(pmm_reclaim_wakeups() == 1);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_min_blocks_alloc(void)
{
	struct page *held[TEST_NR_PAGES];
	struct zone *zone;
	unsigned int n = 0;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	zone = test_zone();
	CHECK(zone != NULL);

	while (pmm_watermark_free() > zone->watermark[WMARK_MIN]) {
		CHECK(n < TEST_NR_PAGES);
		held[n] = pmm_alloc_pages(0);
		CHECK(held[n] != NULL);
		n++;
	}

	CHECK(pmm_watermark_free() <= zone->watermark[WMARK_MIN]);
	CHECK(pmm_alloc_pages(0) == NULL);
	CHECK(pmm_check() == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M2d: watermark + reclaim mark\n");
	printf("---------------------------------\n");

	failures += run_test("init", test_init);
	failures += run_test("watermark order", test_watermark_order);
	failures += run_test("pcp not counted", test_pcp_not_counted);
	failures += run_test("hard uses reserve", test_hard_uses_reserve);
	failures += run_test("low marks reclaim", test_low_marks_reclaim);
	failures += run_test("high clears reclaim", test_high_clears_reclaim);
	failures += run_test("min blocks alloc", test_min_blocks_alloc);

	printf("---------------------------------\n");
	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}

	printf("all tests passed\n");
	return 0;
}
