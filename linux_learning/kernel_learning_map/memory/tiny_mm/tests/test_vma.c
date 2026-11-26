#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/tmm.h"
#include "../include/pgtable.h"
#include "../include/vma.h"
#include "test_runner.h"

#define TEST_START_PFN 0x100UL
#define TEST_NR_PAGES 64UL

/*
 * M4 测试专用清理：CHECK 失败时先销毁 mm，避免 VMA/页表/匿名页泄漏。
 * 匿名页归 mm 所有，随 mm_destroy() 一起回收。
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

/* VMA flags 与 MM_PROT_* 同值，空链表 find_vma 返回 NULL。 */
static int test_flags_and_empty(void)
{
	struct mm_struct *mm;

	CHECK(VM_READ == MM_PROT_READ);
	CHECK(VM_WRITE == MM_PROT_WRITE);
	CHECK(VM_EXEC == MM_PROT_EXEC);

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);
	CHECK(find_vma(mm, 0) == NULL);
	CHECK(find_vma(mm, 0x400000UL) == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* mmap 后首次访问缺页，PTE 建立；重复访问返回同一页。 */
static int test_mmap_fault(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, 3 * PAGE_SIZE,
		      VM_READ | VM_WRITE) == 0);

	page = mm_access(mm, 0x400000UL, MM_ACCESS_READ);
	CHECK(page != NULL);
	CHECK(page->state == PAGE_ALLOCATED);
	CHECK(page->refcount == 1);

	/* 同一 vaddr 再访问，不重复分配，返回同一页。 */
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_READ) == page);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_WRITE) == page);

	/* 缺页后物理页被占用了 1 页（页表页另算）。 */
	CHECK(pmm_free_count() == before - 1);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* 访问没有 VMA 覆盖的地址返回 NULL。 */
static int test_unmapped_access(void)
{
	struct mm_struct *mm;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, PAGE_SIZE, VM_READ) == 0);
	/* VMA 范围之外、以及空洞地址，都返回 NULL。 */
	CHECK(mm_access(mm, 0x401000UL, MM_ACCESS_READ) == NULL);
	CHECK(mm_access(mm, 0x3FF000UL, MM_ACCESS_READ) == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* 只读 VMA 上写访问返回 NULL，读访问正常。 */
static int test_readonly_vma(void)
{
	struct mm_struct *mm;
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, PAGE_SIZE, VM_READ) == 0);
	page = mm_access(mm, 0x400000UL, MM_ACCESS_READ);
	CHECK(page != NULL);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_WRITE) == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* munmap 后 VMA 消失、PTE 清空、匿名页归还。 */
static int test_munmap(void)
{
	struct mm_struct *mm;
	struct page *page;
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, 2 * PAGE_SIZE, VM_READ | VM_WRITE) == 0);
	page = mm_access(mm, 0x400000UL, MM_ACCESS_READ);
	CHECK(page != NULL);
	CHECK(pmm_free_count() == before - 1);

	CHECK(mm_munmap(mm, 0x400000UL, 2 * PAGE_SIZE) == 0);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_READ) == NULL);
	CHECK(find_vma(mm, 0x400000UL) == NULL);
	CHECK(pmm_free_count() == before);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* 两个相邻 VMA，find_vma 语义正确（返回 vm_end > addr 的第一个）。 */
static int test_find_vma_adjacent(void)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, PAGE_SIZE, VM_READ) == 0);
	CHECK(mm_mmap(mm, 0x401000UL, PAGE_SIZE, VM_WRITE) == 0);

	vma = find_vma(mm, 0x400800UL);
	CHECK(vma != NULL);
	CHECK(vma->vm_start == 0x400000UL);

	vma = find_vma(mm, 0x401800UL);
	CHECK(vma != NULL);
	CHECK(vma->vm_start == 0x401000UL);

	vma = find_vma(mm, 0x402000UL);
	CHECK(vma == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* 相邻且 flags 兼容的两个 VMA 合并成一个。 */
static int test_vma_merge(void)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, PAGE_SIZE, VM_READ | VM_WRITE) == 0);
	CHECK(mm_mmap(mm, 0x401000UL, PAGE_SIZE, VM_READ | VM_WRITE) == 0);

	vma = find_vma(mm, 0x400000UL);
	CHECK(vma != NULL);
	CHECK(vma->vm_start == 0x400000UL);
	CHECK(vma->vm_end == 0x402000UL);	/* 合并成一个 2 页 VMA */
	CHECK(vma->vm_next == NULL);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* mprotect 后权限生效。 */
static int test_mprotect(void)
{
	struct mm_struct *mm;
	struct page *page;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, PAGE_SIZE, VM_READ) == 0);
	page = mm_access(mm, 0x400000UL, MM_ACCESS_READ);
	CHECK(page != NULL);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_WRITE) == NULL);

	CHECK(mm_mprotect(mm, 0x400000UL, PAGE_SIZE, VM_READ | VM_WRITE) == 0);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_WRITE) == page);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == TEST_NR_PAGES);
	CHECK(pmm_check() == 0);
	return 0;
}

/* mm_destroy 后匿名页、页表页全部归还。 */
static int test_destroy_releases_all(void)
{
	struct mm_struct *mm;
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	CHECK(mm_mmap(mm, 0x400000UL, 4 * PAGE_SIZE, VM_READ | VM_WRITE) == 0);
	CHECK(mm_access(mm, 0x400000UL, MM_ACCESS_READ) != NULL);
	CHECK(mm_access(mm, 0x401000UL, MM_ACCESS_READ) != NULL);
	CHECK(mm_access(mm, 0x402000UL, MM_ACCESS_READ) != NULL);
	CHECK(mm_access(mm, 0x403000UL, MM_ACCESS_READ) != NULL);
	CHECK(pmm_free_count() == before - 4);

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == before);
	CHECK(pmm_check() == 0);
	return 0;
}

/* 随机 mmap/munmap/access 后无泄漏。 */
static int test_random_stress(void)
{
	struct mm_struct *mm;
	unsigned long i;
	unsigned long before;

	CHECK(pmm_init(TEST_START_PFN, TEST_NR_PAGES) == 0);
	before = pmm_free_count();
	mm = mm_create();
	g_mm = mm;
	CHECK(mm != NULL);

	srand(0x4eed);
	for (i = 0; i < 10000; i++) {
		unsigned long vaddr =
			0x400000UL + ((unsigned long)rand() % 32) * PAGE_SIZE;

		switch (rand() % 3) {
		case 0:
			(void)mm_mmap(mm, vaddr, PAGE_SIZE,
				      VM_READ | VM_WRITE);
			break;
		case 1:
			(void)mm_access(mm, vaddr, MM_ACCESS_READ);
			break;
		default:
			(void)mm_munmap(mm, vaddr, PAGE_SIZE);
			break;
		}
	}

	mm_destroy(mm);
	g_mm = NULL;
	CHECK(pmm_free_count() == before);
	CHECK(pmm_check() == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("tiny-mm / M4: VMA + demand paging\n");
	printf("---------------------------------\n");

	failures += run_test("flags & empty find_vma", test_flags_and_empty);
	failures += run_test("mmap fault", test_mmap_fault);
	failures += run_test("unmapped access", test_unmapped_access);
	failures += run_test("readonly vma", test_readonly_vma);
	failures += run_test("munmap", test_munmap);
	failures += run_test("find_vma adjacent", test_find_vma_adjacent);
	failures += run_test("vma merge", test_vma_merge);
	failures += run_test("mprotect", test_mprotect);
	failures += run_test("destroy releases all", test_destroy_releases_all);
	failures += run_test("random stress", test_random_stress);

	printf("---------------------------------\n");
	if (failures) {
		printf("%d test(s) failed\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
