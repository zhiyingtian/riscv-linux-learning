#include <stdio.h>
#include <stdlib.h>

#include "pcp.h"
#include "test_runner_pcp.h"

#define TEST_NR_PAGES 256
#define TEST_HIGH 8
#define TEST_BATCH 4

static struct page pages[TEST_NR_PAGES];
static struct buddy b;
static struct pcp p;

static int setup_pcp(void)
{
	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	CHECK(pcp_init(&p, &b, TEST_HIGH, TEST_BATCH) == 0);
	return 0;
}

static int test_init(void)
{
	CHECK(setup_pcp() == 0);
	CHECK(p.count == 0);
	CHECK(p.high == TEST_HIGH);
	CHECK(p.batch == TEST_BATCH);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_refill(void)
{
	struct page *page;

	CHECK(setup_pcp() == 0);
	page = pcp_alloc_pages(&p, 0);
	CHECK(page != NULL);
	CHECK(page->order == 0);
	CHECK(page->refcount == 1);
	CHECK(page->is_free == false);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(p.nr_put == TEST_BATCH);
	CHECK(p.count == TEST_BATCH - 1);
	CHECK(b.free_count == TEST_NR_PAGES - TEST_BATCH);
	CHECK(p.alloc_refills == 1);
	CHECK(pcp_check(&p) == 0);

	CHECK(pcp_free_pages(&p, page, 0) == 0);
	CHECK(p.nr_put == TEST_BATCH + 1);
	CHECK(pcp_drain_all(&p) == TEST_BATCH);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_hit(void)
{
	struct page *a;
	struct page *c;

	CHECK(setup_pcp() == 0);
	a = pcp_alloc_pages(&p, 0);
	c = pcp_alloc_pages(&p, 0);
	CHECK(a != NULL);
	CHECK(c != NULL);
	CHECK(a != c);
	CHECK(a->refcount == 1);
	CHECK(a->state == PAGE_ALLOCATED);
	CHECK(c->refcount == 1);
	CHECK(c->state == PAGE_ALLOCATED);
	CHECK(p.nr_put == TEST_BATCH);
	CHECK(p.count == TEST_BATCH - 2);
	CHECK(b.free_count == TEST_NR_PAGES - TEST_BATCH);
	CHECK(p.alloc_refills == 1);
	CHECK(p.alloc_hits >= 2);
	CHECK(pcp_check(&p) == 0);

	CHECK(pcp_free_pages(&p, a, 0) == 0);
	CHECK(pcp_free_pages(&p, c, 0) == 0);
	CHECK(pcp_drain_all(&p) == TEST_BATCH);
	CHECK(b.free_count == TEST_NR_PAGES);
	return 0;
}

static int test_free_cache(void)
{
	struct page *page;

	CHECK(setup_pcp() == 0);
	page = pcp_alloc_pages(&p, 0);
	CHECK(page != NULL);
	CHECK(b.free_count == TEST_NR_PAGES - TEST_BATCH);

	CHECK(pcp_free_pages(&p, page, 0) == 0);
	CHECK(page->is_free == false);
	CHECK(page->refcount == 0);
	CHECK(page->state == PAGE_IN_PCP);
	CHECK(p.nr_put == TEST_BATCH + 1);
	CHECK(p.count == TEST_BATCH);
	CHECK(b.free_count == TEST_NR_PAGES - TEST_BATCH);

	/* 同一个 page 不能重复放进 pcp */
	CHECK(pcp_free_pages(&p, page, 0) < 0);
	CHECK(pcp_drain_all(&p) == TEST_BATCH);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_high_drain(void)
{
	struct page *ptrs[TEST_HIGH];
	int i;

	CHECK(setup_pcp() == 0);
	for (i = 0; i < TEST_HIGH; i++) {
		ptrs[i] = pcp_alloc_pages(&p, 0);
		CHECK(ptrs[i] != NULL);
	}

	CHECK(p.count == 0);
	CHECK(b.free_count == TEST_NR_PAGES - TEST_HIGH);

	for (i = 0; i < TEST_HIGH; i++)
		CHECK(pcp_free_pages(&p, ptrs[i], 0) == 0);

	/* count 达到 high 后必须批量 drain，不能让 pcp 无限增长 */
	CHECK(p.count < p.high);
	CHECK(b.free_count > TEST_NR_PAGES - TEST_HIGH);
	CHECK(p.drain_count >= TEST_BATCH);
	CHECK(pcp_check(&p) == 0);

	CHECK(pcp_drain_all(&p) > 0);
	CHECK(p.count == 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_bypass_order(void)
{
	struct page *page;

	CHECK(setup_pcp() == 0);
	page = pcp_alloc_pages(&p, 1);
	CHECK(page != NULL);
	CHECK(page->order == 1);
	CHECK(page->refcount == 1);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(p.count == 0);
	CHECK(p.nr_put == 0);
	CHECK(p.alloc_refills == 0);
	CHECK(b.free_count == TEST_NR_PAGES - 2);
	CHECK(pcp_check(&p) == 0);

	CHECK(pcp_free_pages(&p, page, 1) == 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_drain(void)
{
	struct page *ptrs[4];
	unsigned int drained;
	int i;

	CHECK(setup_pcp() == 0);
	for (i = 0; i < 4; i++) {
		ptrs[i] = pcp_alloc_pages(&p, 0);
		CHECK(ptrs[i] != NULL);
	}
	CHECK(p.count == 0);
	CHECK(b.free_count == TEST_NR_PAGES - 4);

	for (i = 0; i < 4; i++)
		CHECK(pcp_free_pages(&p, ptrs[i], 0) == 0);
	CHECK(p.count == 4);
	CHECK(b.free_count == TEST_NR_PAGES - 4);

	drained = pcp_drain(&p, 2);
	CHECK(drained == 2);
	CHECK(p.count == 2);
	CHECK(b.free_count == TEST_NR_PAGES - 2);
	CHECK(pcp_check(&p) == 0);

	CHECK(pcp_drain_all(&p) == 2);
	CHECK(b.free_count == TEST_NR_PAGES);
	for (i = 0; i < 4; i++)
		CHECK(ptrs[i]->state == PAGE_IN_BUDDY);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

static int test_invalid(void)
{
	struct buddy bad;
	struct page *bad_page;

	CHECK(pcp_init(NULL, &b, TEST_HIGH, TEST_BATCH) < 0);
	CHECK(pcp_init(&p, NULL, TEST_HIGH, TEST_BATCH) < 0);

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	CHECK(pcp_init(&p, &b, 0, TEST_BATCH) < 0);
	CHECK(pcp_init(&p, &b, TEST_BATCH, TEST_HIGH) < 0);
	CHECK(pcp_init(&p, &b, TEST_HIGH, 1) == 0);

	CHECK(pcp_alloc_pages(&p, MAX_ORDER) == NULL);
	CHECK(pcp_free_pages(&p, NULL, 0) < 0);

	/* 未分配的 page 不能进 pcp */
	CHECK(pcp_free_pages(&p, &pages[0], 0) < 0);

	/* state 不是 PAGE_ALLOCATED 的页也不能进 pcp */
	bad_page = buddy_alloc(&b, 0);
	CHECK(bad_page != NULL);
	bad_page->state = PAGE_IN_PCP;
	CHECK(pcp_free_pages(&p, bad_page, 0) < 0);
	bad_page->state = PAGE_ALLOCATED;
	CHECK(pcp_free_pages(&p, bad_page, 0) == 0);
	CHECK(pcp_drain_all(&p) == 1);

	CHECK(buddy_init(&bad, pages, TEST_NR_PAGES) == 0);
	(void)bad;
	return 0;
}

static int test_random_stress(void)
{
	struct page *slots[TEST_NR_PAGES] = { 0 };
	unsigned long allocs = 0;
	int i;

	CHECK(setup_pcp() == 0);
	srand(2024);

	for (i = 0; i < 20000; i++) {
		unsigned long index = (unsigned long)rand() % TEST_NR_PAGES;

		if (!slots[index]) {
			struct page *page = pcp_alloc_pages(&p, 0);

			if (page) {
				unsigned long pidx = (unsigned long)(page - pages);

				CHECK(pidx < TEST_NR_PAGES);
				slots[pidx] = page;
				allocs++;
			}
		} else {
			CHECK(pcp_free_pages(&p, slots[index], 0) == 0);
			slots[index] = NULL;
		}

		if ((i % 257) == 0)
			CHECK(pcp_check(&p) == 0);
	}

	CHECK(allocs > 0);
	for (i = 0; i < TEST_NR_PAGES; i++) {
		if (slots[i])
			CHECK(pcp_free_pages(&p, slots[i], 0) == 0);
	}

	CHECK(pcp_drain_all(&p) > 0);
	CHECK(p.count == 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(pcp_check(&p) == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M2b: per-CPU page cache\n");
	printf("---------------------------------\n");
	failures += run_test("init", test_init);
	failures += run_test("refill", test_refill);
	failures += run_test("pcp hit", test_hit);
	failures += run_test("free to pcp", test_free_cache);
	failures += run_test("high drain", test_high_drain);
	failures += run_test("order > 0 bypass", test_bypass_order);
	failures += run_test("explicit drain", test_drain);
	failures += run_test("invalid", test_invalid);
	failures += run_test("random stress", test_random_stress);
	printf("---------------------------------\n");

	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}

	printf("all tests passed\n");
	return 0;
}
