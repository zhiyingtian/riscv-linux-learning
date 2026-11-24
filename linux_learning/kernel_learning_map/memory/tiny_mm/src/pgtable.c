#include "pgtable.h"
#include "tmm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * tiny-mm / M3: 两级页表
 *
 * 已完成的脚手架部分：
 *   - mm_create()：从 pmm 分配 PGD 页，并分配页表存储；
 *   - mm_destroy()：释放所有 PTE 页和 PGD 页；
 *   - 统计/自检 getter。
 *
 * TODO(M3):
 *   - pgtable_walk()：按 vaddr 找 PTE，create 时按需分配 PTE 页；
 *   - mm_map()/mm_unmap()/mm_protect()/mm_translate()；
 *   - 修改 PTE 后的 TLB flush 计数。
 */

static struct page *pgtable_alloc_page(void)
{
	struct page *page = pmm_alloc_pages(0);

	if (!page)
		return NULL;

	page->pgtable = calloc(1, PAGE_SIZE);
	if (!page->pgtable) {
		pmm_free_pages(page, 0);
		return NULL;
	}

	return page;
}

static void pgtable_free_page(struct page *page)
{
	if (!page)
		return;

	free(page->pgtable);
	page->pgtable = NULL;
	pmm_free_pages(page, 0);
}

struct mm_struct *mm_create(void)
{
	struct mm_struct *mm = calloc(1, sizeof(*mm));

	if (!mm)
		return NULL;

	mm->pgd_page = pgtable_alloc_page();
	if (!mm->pgd_page) {
		free(mm);
		return NULL;
	}

	mm->pgd = (struct pgd_page *)mm->pgd_page->pgtable;
	mm->page_table_pages = 1;
	mm->tlb_flush_count = 0;

	return mm;
}

void mm_destroy(struct mm_struct *mm)
{
	unsigned int i;

	if (!mm)
		return;

	if (mm->pgd) {
		for (i = 0; i < PGD_ENTRIES; i++) {
			pgd_t entry = mm->pgd->entries[i];
			struct page *pte_page;

			if (!pgd_present(entry))
				continue;

			pte_page = pfn_to_page(pgd_pfn(entry));
			if (!pte_page)
				continue;

			pgtable_free_page(pte_page);
			mm->pgd->entries[i] = __pgd(0);
			if (mm->page_table_pages > 0)
				mm->page_table_pages--;
		}
	}

	pgtable_free_page(mm->pgd_page);
	free(mm);
}

/*
 * M3: 按 vaddr 找 PTE。
 *
 * create=false：只查找；PTE 页不存在时返回 NULL。
 * create=true ：PTE 页不存在时，从 pmm 分配一页并写入 PGD entry。
 *
 * 注意：create=true 创建的是“PTE 页”，不是数据页。
 */
static pte_t *pgtable_walk(struct mm_struct *mm, unsigned long vaddr,
			   bool create)
{
	unsigned long pgd_index;
	pgd_t pgd_entry;
	struct page *pte_page;
	struct pte_page *pt;

	if (!mm || !mm->pgd || !mm->pgd_page)
		return NULL;

	if ((vaddr & (PAGE_SIZE - 1)) != 0)
		return NULL;
	if (vaddr >= VADDR_LIMIT)
		return NULL;

	pgd_index = PGD_INDEX(vaddr);
	pgd_entry = mm->pgd->entries[pgd_index];

	/*
	PGD 页
	entries[pgd_index]  → 指向一个 PTE 页（PFN + flags）
	PTE 页
	entries[pte_index]  → 指向真正的数据页（PFN + flags）
	*/

	if (!pgd_none(pgd_entry)) {
		if (!pgd_present(pgd_entry))
			// pgd_entry存在，但present为0，表示无效，直接返回
			return NULL;

		pte_page = pfn_to_page(pgd_pfn(pgd_entry));
		if (!pte_page || !pte_page->pgtable)
			return NULL;

		pt = pte_page->pgtable;
		return &pt->entries[PTE_INDEX(vaddr)];
	}

	if (!create)
		return NULL;

	pte_page = pgtable_alloc_page();
	if (!pte_page)
		return NULL;

	mm->pgd->entries[pgd_index] =
		pfn_pgd(page_to_pfn(pte_page), PGD_PRESENT);
	mm->page_table_pages++;

	pt = pte_page->pgtable;
	return &pt->entries[PTE_INDEX(vaddr)];
}

int mm_map(struct mm_struct *mm, unsigned long vaddr, struct page *page,
	   unsigned int prot)
{
	pte_t *pte;
	unsigned long flags;

	if (!mm || !page)
		return -EINVAL;

	if ((vaddr & (PAGE_SIZE - 1)) != 0)
		return -EINVAL;

	if (vaddr >= VADDR_LIMIT)
		return -EINVAL;

	if (page->state != PAGE_ALLOCATED)
		return -EINVAL;

	pte = pgtable_walk(mm, vaddr, true);
	if (!pte)
		return -ENOMEM;

	/* 同一个 vaddr 不能重复映射。 */
	if (pte_present(*pte))
		return -EEXIST;

	flags = PTE_PRESENT | PTE_USER;
	if (prot & MM_PROT_WRITE)
		flags |= PTE_WRITE;

	*pte = pfn_pte(page_to_pfn(page), flags);
	mm->tlb_flush_count++;

	return 0;
}

int mm_unmap(struct mm_struct *mm, unsigned long vaddr)
{
	pte_t *pte = pgtable_walk(mm, vaddr, false);

	(void)pte;

	/* 留空：M3 的实现练习。 */
	return -ENOSYS;
}

int mm_protect(struct mm_struct *mm, unsigned long vaddr, unsigned int prot)
{
	pte_t *pte = pgtable_walk(mm, vaddr, false);

	(void)prot;
	(void)pte;

	/* 留空：M3 的实现练习。 */
	return -ENOSYS;
}

struct page *mm_translate(struct mm_struct *mm, unsigned long vaddr,
			  unsigned int access)
{
	pte_t *pte = pgtable_walk(mm, vaddr, false);

	(void)access;
	(void)pte;

	/* 留空：M3 的实现练习。 */
	return NULL;
}

unsigned long mm_tlb_flush_count(const struct mm_struct *mm)
{
	return mm ? mm->tlb_flush_count : 0;
}

unsigned int mm_page_table_pages(const struct mm_struct *mm)
{
	return mm ? mm->page_table_pages : 0;
}

int mm_check(const struct mm_struct *mm)
{
	if (!mm || !mm->pgd_page || !mm->pgd)
		return -EINVAL;
	if (mm->pgd_page->pgtable != (void *)mm->pgd)
		return -EINVAL;

	return 0;
}
