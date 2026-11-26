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
 * 类型风格尽量贴近 Linux：
 *   - pgd_t / pte_t 是独立的类型；
 *   - 用 pgd_val()/pte_val() 取原始值；
 *   - 用 __pgd()/__pte() 构造；
 *   - 用 pfn_pgd()/pfn_pte() 编码 PFN + flags。
 *
 * 当前已完成：mm_create/mm_destroy、pgtable_walk、mm_map/mm_unmap/mm_protect/
 * mm_translate、TLB flush 计数，以及 Linux 风格 pgd_t/pte_t 编码辅助函数。
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
#define PTE_PFN_MASK (~((pteval_t)PAGE_SIZE - 1))

/* PTE/PGD 标志位；PFN 存在高位。 */
#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_READ    (1UL << 3)
#define PTE_EXEC    (1UL << 4)

/* 两级页表里，PGD entry 也只需要 present 位。 */
#define PGD_PRESENT PTE_PRESENT

/* mm_map/mm_protect 的保护标志。 */
#define MM_PROT_READ  0x1u
#define MM_PROT_WRITE 0x2u
#define MM_PROT_EXEC  0x4u

/* mm_translate 的访问类型。 */
#define MM_ACCESS_READ  MM_PROT_READ
#define MM_ACCESS_WRITE MM_PROT_WRITE
#define MM_ACCESS_EXEC  MM_PROT_EXEC

/*
 * Linux 的 pgd_t/pte_t 不是裸整数，而是带类型的包装。
 * 这样编译器能区分“页表项”和普通整数，避免随手混用。
 */
typedef uint64_t pgdval_t;
typedef uint64_t pteval_t;

typedef struct {
	pgdval_t pgd;
} pgd_t;

typedef struct {
	pteval_t pte;
} pte_t;

static inline pgdval_t pgd_val(pgd_t pgd)
{
	return pgd.pgd;
}

static inline pteval_t pte_val(pte_t pte)
{
	return pte.pte;
}

static inline pgd_t __pgd(pgdval_t val)
{
	return (pgd_t){ .pgd = val };
}

static inline pte_t __pte(pteval_t val)
{
	return (pte_t){ .pte = val };
}

/*
 * PGD entry：
 *   [ PTE 页的 PFN ][ present ]
 */
static inline bool pgd_none(pgd_t pgd)
{
	return pgd_val(pgd) == 0;
}

static inline bool pgd_present(pgd_t pgd)
{
	return (pgd_val(pgd) & PGD_PRESENT) != 0;
}

static inline unsigned long pgd_pfn(pgd_t pgd)
{
	return (unsigned long)(pgd_val(pgd) >> PTE_PFN_SHIFT);
}

static inline pgd_t pfn_pgd(unsigned long pfn, unsigned long flags)
{
	return __pgd(((pgdval_t)pfn << PTE_PFN_SHIFT) | flags);
}

/*
 * PTE entry：
 *   [ 数据页的 PFN ][ present ][ write ][ user ][ read ][ exec ]
 */
static inline bool pte_none(pte_t pte)
{
	return pte_val(pte) == 0;
}

static inline bool pte_present(pte_t pte)
{
	return (pte_val(pte) & PTE_PRESENT) != 0;
}

static inline bool pte_write(pte_t pte)
{
	return (pte_val(pte) & PTE_WRITE) != 0;
}

static inline bool pte_read(pte_t pte)
{
	return (pte_val(pte) & PTE_READ) != 0;
}

static inline bool pte_exec(pte_t pte)
{
	return (pte_val(pte) & PTE_EXEC) != 0;
}

static inline bool pte_user(pte_t pte)
{
	return (pte_val(pte) & PTE_USER) != 0;
}

static inline unsigned long pte_pfn(pte_t pte)
{
	return (unsigned long)(pte_val(pte) >> PTE_PFN_SHIFT);
}

static inline pte_t pfn_pte(unsigned long pfn, unsigned long flags)
{
	return __pte(((pteval_t)pfn << PTE_PFN_SHIFT) | flags);
}

static inline pte_t pte_mkwrite(pte_t pte)
{
	return __pte(pte_val(pte) | PTE_WRITE);
}

static inline pte_t pte_wrprotect(pte_t pte)
{
	return __pte(pte_val(pte) & ~(pteval_t)PTE_WRITE);
}

static inline pte_t pte_mkuser(pte_t pte)
{
	return __pte(pte_val(pte) | PTE_USER);
}

static inline pte_t pte_modify(pte_t pte, unsigned long flags)
{
	return __pte((pte_val(pte) & PTE_PFN_MASK) | flags);
}

/*
 * 一个页表页的内容。
 *
 * PGD 页和 PTE 页是两种不同的页表页：
 *   - struct pgd_page：每个 entry 指向一个 PTE 页；
 *   - struct pte_page：每个 entry 指向一个数据页。
 */
struct pgd_page {
	pgd_t entries[PGD_ENTRIES];
};

struct pte_page {
	pte_t entries[PTE_ENTRIES];
};

/*
 * M3 的最小 mm。
 *
 * M4 会在这里继续加入 VMA 链表、mmap_lock 等字段。
 * 当前只关心页表根和 TLB 统计。
 */
struct vm_area_struct;

struct mm_struct {
	struct page *pgd_page;		/* PGD 页本身，来自 pmm */
	struct pgd_page *pgd;		/* PGD 页内容 */
	unsigned int page_table_pages;	/* PGD + 已分配的 PTE 页数 */
	unsigned long tlb_flush_count;	/* TLB flush 次数模拟 */

	struct vm_area_struct *vma_list; /* M4: 按 vm_start 有序的 VMA 链表头 */
};

#define PGD_INDEX(vaddr) \
	(((vaddr) >> PGD_SHIFT) & (PGD_ENTRIES - 1))
#define PTE_INDEX(vaddr) \
	(((vaddr) >> PTE_SHIFT) & (PTE_ENTRIES - 1))

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
