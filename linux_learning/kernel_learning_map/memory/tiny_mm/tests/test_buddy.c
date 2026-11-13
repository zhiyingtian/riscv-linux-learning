#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/buddy.h"
#include "test_runner_buddy.h"

#define TEST_NR_PAGES 256
#define TEST_MAX_ORDER 8

static struct page pages[TEST_NR_PAGES];
static struct buddy b;

static int test_init(void)
{
	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	CHECK(b.nr_pages == TEST_NR_PAGES);
	CHECK(b.max_order == TEST_MAX_ORDER);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(b.free_area[TEST_MAX_ORDER].nr_free == 1);
	CHECK(b.free_area[TEST_MAX_ORDER].head == &pages[0]);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_alloc_order0(void)
{
	struct page *p;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 0);
	CHECK(p != NULL);
	CHECK(p == &pages[0]);
	CHECK(p->order == 0);
	CHECK(p->refcount == 1);
	CHECK(p->is_free == false);
	CHECK(b.free_count == TEST_NR_PAGES - 1);
	CHECK(buddy_check(&b) == 0);

	CHECK(buddy_free(&b, p, 0) == 0);
	CHECK(p->refcount == 0);
	CHECK(p->is_free == true);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_alloc_order2(void)
{
	struct page *p;
	unsigned long index;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 2);
	CHECK(p != NULL);
	index = (unsigned long)(p - pages);
	CHECK((index & 0x3UL) == 0);
	CHECK(p->order == 2);
	CHECK(p->refcount == 1);
	CHECK(b.free_count == TEST_NR_PAGES - 4);
	CHECK(buddy_check(&b) == 0);

	CHECK(buddy_free(&b, p, 2) == 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_split(void)
{
	struct page *p;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 2);
	CHECK(p != NULL);

	/* 从 order 8 一路 split 到 order 2，order 2 应剩一个空闲高半。 */
	CHECK(b.free_area[TEST_MAX_ORDER].nr_free == 0);
	CHECK(b.free_area[2].nr_free == 1);
	CHECK(b.split_count > 0);
	CHECK(buddy_check(&b) == 0);

	CHECK(buddy_free(&b, p, 2) == 0);
	return 0;
}

static int test_merge(void)
{
	struct page *p;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 2);
	CHECK(p != NULL);
	CHECK(buddy_free(&b, p, 2) == 0);

	/* 释放后应一路合并回一个 order 8 的整块。 */
	CHECK(b.free_area[TEST_MAX_ORDER].nr_free == 1);
	CHECK(b.free_area[TEST_MAX_ORDER].head == &pages[0]);
	CHECK(b.free_area[2].nr_free == 0);
	CHECK(b.merge_count > 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_double_free(void)
{
	struct page *p;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 0);
	CHECK(p != NULL);
	CHECK(buddy_free(&b, p, 0) == 0);
	CHECK(buddy_free(&b, p, 0) < 0);
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_wrong_order(void)
{
	struct page *p;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	p = buddy_alloc(&b, 0);
	CHECK(p != NULL);
	CHECK(buddy_free(&b, p, 1) < 0);
	CHECK(p->refcount == 1);
	CHECK(buddy_free(&b, p, 0) == 0);
	return 0;
}

static int test_exhaust(void)
{
	struct page *ptrs[TEST_NR_PAGES] = { 0 };
	unsigned long i;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	for (i = 0; i < TEST_NR_PAGES; i++) {
		ptrs[i] = buddy_alloc(&b, 0);
		CHECK(ptrs[i] != NULL);
	}
	CHECK(buddy_alloc(&b, 0) == NULL);
	CHECK(b.free_count == 0);
	CHECK(buddy_check(&b) == 0);

	for (i = 0; i < TEST_NR_PAGES; i++) {
		CHECK(buddy_free(&b, ptrs[i], 0) == 0);
	}
	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

static int test_invalid(void)
{
	struct buddy bad;

	CHECK(buddy_init(NULL, pages, TEST_NR_PAGES) < 0);
	CHECK(buddy_init(&bad, NULL, TEST_NR_PAGES) < 0);
	CHECK(buddy_init(&bad, pages, 0) < 0);
	CHECK(buddy_init(&bad, pages, 250) < 0);
	CHECK(buddy_init(&bad, pages, 1UL << MAX_ORDER) < 0);

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	CHECK(buddy_alloc(&b, MAX_ORDER) == NULL);
	CHECK(buddy_free(&b, NULL, 0) < 0);
	return 0;
}

static int test_random_stress(void)
{
	struct page *slots[TEST_NR_PAGES] = { 0 };
	unsigned int orders[TEST_NR_PAGES] = { 0 };
	unsigned long allocs = 0;
	int i;

	CHECK(buddy_init(&b, pages, TEST_NR_PAGES) == 0);
	srand(1234);

	for (i = 0; i < 20000; i++) {
		unsigned long index = (unsigned long)rand() % TEST_NR_PAGES;

		if (!slots[index]) {
			unsigned int order = (unsigned int)(rand() % 5);
			struct page *p = buddy_alloc(&b, order);

			if (p) {
				unsigned long pidx = (unsigned long)(p - pages);

				CHECK(pidx + (1UL << order) <= TEST_NR_PAGES);
				slots[pidx] = p;
				orders[pidx] = order;
				allocs++;
			}
		} else {
			CHECK(buddy_free(&b, slots[index], orders[index]) == 0);
			slots[index] = NULL;
			orders[index] = 0;
		}

		if ((i % 257) == 0)
			CHECK(buddy_check(&b) == 0);
	}

	CHECK(allocs > 0);
	for (i = 0; i < TEST_NR_PAGES; i++) {
		if (slots[i])
			CHECK(buddy_free(&b, slots[i], orders[i]) == 0);
	}

	CHECK(b.free_count == TEST_NR_PAGES);
	CHECK(buddy_check(&b) == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M2: buddy allocator\n");
	printf("---------------------------------\n");
	failures += run_test("init", test_init);
	failures += run_test("alloc order 0", test_alloc_order0);
	failures += run_test("alloc order 2", test_alloc_order2);
	failures += run_test("split", test_split);
	failures += run_test("merge", test_merge);
	failures += run_test("double free", test_double_free);
	failures += run_test("wrong order", test_wrong_order);
	failures += run_test("exhaust", test_exhaust);
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
