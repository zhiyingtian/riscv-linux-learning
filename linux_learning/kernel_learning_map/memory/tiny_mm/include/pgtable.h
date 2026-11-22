#ifndef TINY_MM_PGTABLE_H
#define TINY_MM_PGTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "tmm.h"

/*
 * tiny-mm / M3: 两级页表 + TLB 计数模拟
 *
 * 虚拟地址布局（30 位）：
 *
 *   [ PGD index : 9 位 ][ PTE index : 9 位 ][ page offset : 12 位 ]
 *     31      21          20      12          11             0
 *
 * PGD 和 PTE 页都来自 pmm 的 order-0 分配；它们本身也是物理页。
 *
 * TODO(M3):
 *   pgtable.c 里保留了 walk/map/unmap/protect/translate 的骨架，
 *   当前只完成 mm_create()/mm_destroy()，其余函数返回错误。
 */

#define PTE_INDEX_BITS 9
#define PGD_INDEX_BITS 9
#define PTE_SHIFT PAGE_SHIFT
#define PGD_SHIFT (PTE_SHIFT + PTE_INDEX_BITS)
#define VADDR_BITS (PGD_SHIFT + PGD_INDEX_BITS)

#define PTE_ENTRIES (1U << PTE_INDEX_BITS)
#define PGD_ENTRIES (1U << PGD_INDEX_BITS)
#define VADDR_LIMIT (1UL << VADDR_BITS)

#define PTE_PFN_SHIFT PAGE_SHIFT

/* PTE 标志位；PFN 存在高位。 */
#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)

/* mm_map/mm_protect 的保护标志。 */
#define MM_PROT_READ  0x1u
#define MM_PROT_WRITE 0x2u
#define MM_PROT_EXEC  0x4u

/* mm_translate 的访问类型。 */
#define MM_ACCESS_READ  MM_PROT_READ
#define MM_ACCESS_WRITE MM_PROT_WRITE
#define MM_ACCESS_EXEC  MM_PROT_EXEC

typedef uint64_t pte_t;

/*
 * 一个页表页的内容。
 *
 * PGD 和 PTE 都使用这个结构；PGD entry 和 PTE entry 的编码相同：
 *   [ PFN ][ flags ]
 */
struct pte_page {
	pte_t entries[PTE_ENTRIES];
};

/*
 * M3 的最小 mm。
 *
 * M4 会在这里继续加入 VMA 链表、mmap_lock 等字段。
 * 当前只关心页表根和 TLB 统计。
 */
struct mm_struct {
	struct page *pgd_page;		/* PGD 页本身，来自 pmm */
	struct pte_page *pgd;		/* PGD 页内容 */
	unsigned int page_table_pages;	/* PGD + 已分配的 PTE 页数 */
	unsigned long tlb_flush_count;	/* TLB flush 次数模拟 */
};

#define PGD_INDEX(vaddr) \
	(((vaddr) >> PGD_SHIFT) & (PGD_ENTRIES - 1))
#define PTE_INDEX(vaddr) \
	(((vaddr) >> PTE_SHIFT) & (PTE_ENTRIES - 1))

static inline unsigned long pte_pfn(pte_t pte)
{
	return (unsigned long)(pte >> PTE_PFN_SHIFT);
}

static inline pte_t pfn_pte(unsigned long pfn, unsigned long flags)
{
	return ((pte_t)pfn << PTE_PFN_SHIFT) | flags;
}

static inline bool pte_present(pte_t pte)
{
	return (pte & PTE_PRESENT) != 0;
}

/*
 * 生命周期。
 */
struct mm_struct *mm_create(void);
void mm_destroy(struct mm_struct *mm);

/*
 * 页表操作。
 *
 * 返回 0 成功，负数错误：
 *   -EINVAL: 参数非法/地址未对齐/越界
 *   -EEXIST: 该 vaddr 已经映射
 *   -ENOENT: 该 vaddr 没有映射
 *   -ENOMEM: 页表页分配失败
 *   -ENOSYS: M3 脚手架尚未实现
 *
 * mm_unmap() 只清除 PTE，不释放数据页；数据页由调用者管理。
 */
int mm_map(struct mm_struct *mm, unsigned long vaddr, struct page *page,
	   unsigned int prot);
int mm_unmap(struct mm_struct *mm, unsigned long vaddr);
int mm_protect(struct mm_struct *mm, unsigned long vaddr, unsigned int prot);
struct page *mm_translate(struct mm_struct *mm, unsigned long vaddr,
			  unsigned int access);

/*
 * 统计与自检。
 */
unsigned long mm_tlb_flush_count(const struct mm_struct *mm);
unsigned int mm_page_table_pages(const struct mm_struct *mm);
int mm_check(const struct mm_struct *mm);

#endif /* TINY_MM_PGTABLE_H */
