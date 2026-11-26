#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/tmm.h"
#include "../include/pgtable.h"
#include "test_runner.h"

#define TEST_START_PFN 0x100UL
#define TEST_NR_PAGES 64UL

/*
 * M3 测试专用清理：CHECK 失败时先销毁 mm，避免页表页存储泄漏。
 * 数据页由 pmm_exit() 统一回收。
 */
static struct mm_struct *g_mm;

#undef CHECK
#define CHECK(cond)                                                           \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,       \
				__LINE__, #cond);                              \
			if (g_mm) {                                            \
				mm_destroy(g_mm);                              \
				g_mm = NULL;                                   \
			}                                                     \
			pmm_exit();                                            \
			return 1;                                              \
		}                                                             \
	} while (0)

static int test_create_destroy(void)
{
	struct mm_struct *mm;
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();

	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);
	CHECK(mm_check(mm) == 0);
	CHECK(mm_page_table_pages(mm) == 1);
	CHECK(pmm_free_count() == before - 1);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == before);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_pte_encoding(void)
{
	pte_t pte = pfn_pte(5, PTE_PRESENT | PTE_WRITE | PTE_USER);
	pte_t ro;
	pte_t modified;
	pgd_t pgd = pfn_pgd(7, PGD_PRESENT);

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);

	CHECK(pte_present(pte));
	CHECK(pte_write(pte));
	CHECK(pte_user(pte));
	CHECK(!pte_none(pte));
	CHECK(pte_pfn(pte) == 5);
	CHECK(pte_val(pte) == ((5UL << PTE_PFN_SHIFT) |
			       PTE_PRESENT | PTE_WRITE | PTE_USER));

	ro = pte_wrprotect(pte);
	CHECK(pte_present(ro));
	CHECK(!pte_write(ro));
	CHECK(pte_pfn(ro) == 5);

	modified = pte_modify(pte, PTE_PRESENT | PTE_USER);
	CHECK(!pte_write(modified));
	CHECK(pte_pfn(modified) == 5);

	CHECK(pte_none(__pte(0)));

	CHECK(pgd_present(pgd));
	CHECK(!pgd_none(pgd));
	CHECK(pgd_pfn(pgd) == 7);
	CHECK(pgd_none(__pgd(0)));

	CHECK(pmm_check() == 0);
	return 0;
}

static int test_unmapped_translate(void)
{
	struct mm_struct *mm;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_translate(mm, 0x400000UL, MM_ACCESS_READ) == NULL);
	CHECK(mm_translate(mm, 0x400000UL, MM_ACCESS_WRITE) == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_map_translate_unmap(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long vaddr = 0x100000UL;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_map(mm, vaddr, page, MM_PROT_READ | MM_PROT_WRITE) == 0);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_READ) == page);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_WRITE) == page);

	CHECK(mm_unmap(mm, vaddr) == 0);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_READ) == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_page_table_pages(void)
{
	struct mm_struct *mm;
	struct page *pages[2];
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	pages[0] = pmm_alloc_pages(0);
	pages[1] = pmm_alloc_pages(0);
	CHECK(pages[0] != NULL && pages[1] != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	/* 同一 PTE 页覆盖 2MB 范围；不同 PGD entry 需要新的 PTE 页。 */
	CHECK(mm_map(mm, 0x100000UL, pages[0], MM_PROT_READ) == 0);
	CHECK(mm_map(mm, 0x101000UL, pages[1], MM_PROT_READ) == 0);
	CHECK(mm_page_table_pages(mm) == 2);
	CHECK(mm_check(mm) == 0);

	CHECK(mm_map(mm, 0x200000UL, pages[0], MM_PROT_READ) == 0);
	CHECK(mm_page_table_pages(mm) == 3);
	CHECK(mm_check(mm) == 0);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(pages[0], 0) == 0);
	CHECK(pmm_free_pages(pages[1], 0) == 0);
	CHECK(pmm_free_count() == before);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_permission(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long vaddr = 0x100000UL;
	unsigned long flushed;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_map(mm, vaddr, page, MM_PROT_READ) == 0);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_READ) == page);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_WRITE) == NULL);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_EXEC) == NULL);

	flushed = mm_tlb_flush_count(mm);
	CHECK(mm_protect(mm, vaddr, MM_PROT_READ | MM_PROT_WRITE) == 0);
	CHECK(mm_tlb_flush_count(mm) > flushed);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_WRITE) == page);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_EXEC) == NULL);

	flushed = mm_tlb_flush_count(mm);
	CHECK(mm_protect(mm, vaddr,
			 MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXEC) == 0);
	CHECK(mm_tlb_flush_count(mm) > flushed);
	CHECK(mm_translate(mm, vaddr, MM_ACCESS_EXEC) == page);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_tlb_flush_count(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long count;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	count = mm_tlb_flush_count(mm);
	CHECK(mm_map(mm, 0x100000UL, page, MM_PROT_READ) == 0);
	CHECK(mm_tlb_flush_count(mm) > count);

	count = mm_tlb_flush_count(mm);
	CHECK(mm_protect(mm, 0x100000UL, MM_PROT_WRITE) == 0);
	CHECK(mm_tlb_flush_count(mm) > count);

	count = mm_tlb_flush_count(mm);
	CHECK(mm_unmap(mm, 0x100000UL) == 0);
	CHECK(mm_tlb_flush_count(mm) > count);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_destroy_releases_page_tables(void)
{
	struct mm_struct *mm;
	struct page *pages[8];
	unsigned long before;
	unsigned int i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	for (i = 0; i < 8; i++) {
		pages[i] = pmm_alloc_pages(0);
		CHECK(pages[i] != NULL);
	}
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	for (i = 0; i < 8; i++)
		CHECK(mm_map(mm, 0x100000UL + i * PAGE_SIZE, pages[i],
			     MM_PROT_READ) == 0);

	CHECK(mm_page_table_pages(mm) == 2);
	mm_destroy(mm);
	g_mm = NULL;
	for (i = 0; i < 8; i++)
		CHECK(pmm_free_pages(pages[i], 0) == 0);
	CHECK(pmm_free_count() == before);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_random_map_unmap(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long vaddr;
	unsigned int i;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	srand(0x3eed);
	for (i = 0; i < 100000; i++) {
		vaddr = 0x100000UL + ((unsigned long)rand() % 64) * PAGE_SIZE;
		if (mm_translate(mm, vaddr, MM_ACCESS_READ)) {
			CHECK(mm_unmap(mm, vaddr) == 0);
		} else {
			CHECK(mm_map(mm, vaddr, page, MM_PROT_READ) == 0);
		}
	}
	CHECK(mm_check(mm) == 0);
	CHECK(pmm_check() == 0);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

static int test_error_paths(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long vaddr = 0x100000UL;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	page = pmm_alloc_pages(0);
	CHECK(page != NULL);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	/* mm_map：非法参数返回 -EINVAL。 */
	CHECK(mm_map(NULL, vaddr, page, MM_PROT_READ) == -EINVAL);
	CHECK(mm_map(mm, vaddr, NULL, MM_PROT_READ) == -EINVAL);
	CHECK(mm_map(mm, 0x100001UL, page, MM_PROT_READ) == -EINVAL);
	CHECK(mm_map(mm, VADDR_LIMIT, page, MM_PROT_READ) == -EINVAL);

	/* 重复映射返回 -EEXIST，且不增加页表页。 */
	CHECK(mm_map(mm, vaddr, page, MM_PROT_READ) == 0);
	CHECK(mm_map(mm, vaddr, page, MM_PROT_READ) == -EEXIST);
	CHECK(mm_page_table_pages(mm) == 2);
	CHECK(mm_check(mm) == 0);

	/* mm_unmap：非法参数 -EINVAL，未映射 -ENOENT。 */
	CHECK(mm_unmap(NULL, vaddr) == -EINVAL);
	CHECK(mm_unmap(mm, 0x100001UL) == -EINVAL);
	CHECK(mm_unmap(mm, VADDR_LIMIT) == -EINVAL);
	CHECK(mm_unmap(mm, 0x100000UL + PAGE_SIZE) == -ENOENT);

	/* mm_protect：非法参数 -EINVAL，未映射 -ENOENT。 */
	CHECK(mm_protect(NULL, vaddr, MM_PROT_READ) == -EINVAL);
	CHECK(mm_protect(mm, 0x100001UL, MM_PROT_READ) == -EINVAL);
	CHECK(mm_protect(mm, 0x100000UL + PAGE_SIZE, MM_PROT_READ) == -ENOENT);

	/* 正常 unmap 后再次 unmap 返回 -ENOENT。 */
	CHECK(mm_unmap(mm, vaddr) == 0);
	CHECK(mm_unmap(mm, vaddr) == -ENOENT);
	CHECK(mm_check(mm) == 0);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_pages(page, 0) == 0);
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M3: two-level page table + TLB\n");
	printf("---------------------------------\n");

	failures += run_test("create/destroy", test_create_destroy);
	failures += run_test("pte encoding", test_pte_encoding);
	failures += run_test("unmapped translate", test_unmapped_translate);
	failures += run_test("map/translate/unmap", test_map_translate_unmap);
	failures += run_test("page table pages", test_page_table_pages);
	failures += run_test("permission", test_permission);
	failures += run_test("tlb flush count", test_tlb_flush_count);
	failures += run_test("destroy releases tables", test_destroy_releases_page_tables);
	failures += run_test("random map/unmap", test_random_map_unmap);
	failures += run_test("error paths", test_error_paths);

	printf("---------------------------------\n");
	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}

	printf("all tests passed\n");
	return 0;
}
