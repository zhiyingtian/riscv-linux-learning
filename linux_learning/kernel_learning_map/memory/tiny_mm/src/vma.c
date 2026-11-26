#include "vma.h"
#include "tmm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

/*
 * tiny-mm / M4: VMA + 缺页
 *
 * 已完成的脚手架部分：
 *   - 数据结构与 API 声明（include/vma.h）。
 *
 * TODO(M4):
 *   - find_vma()：在有序链表上找第一个 vm_end > addr；
 *   - mm_mmap()：对齐/溢出检查、VMA 建立、相邻 VMA 合并、重叠检查；
 *   - mm_access()：find_vma → 权限检查 → 缺页分配匿名页并写 PTE；
 *   - mm_munmap()：清 PTE、归还匿名页、拆分/删除 VMA；
 *   - mm_mprotect()：改 VMA 权限并同步已映射的 PTE。
 *
 * 注意：mm_access() 缺页分配的匿名页归 mm 所有，因此 mm_destroy()
 * 除了释放页表页，还要释放这些匿名页（对应 M3 的“数据页属于调用者”的改变）。
 */

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	(void)mm;
	(void)addr;

	/* 留空：M4 的实现练习。 */
	return NULL;
}

int mm_mmap(struct mm_struct *mm, unsigned long addr, unsigned long len,
	    unsigned int prot)
{
	(void)mm;
	(void)addr;
	(void)len;
	(void)prot;

	/* 留空：M4 的实现练习。 */
	return -ENOSYS;
}

int mm_munmap(struct mm_struct *mm, unsigned long addr, unsigned long len)
{
	(void)mm;
	(void)addr;
	(void)len;

	/* 留空：M4 的实现练习。 */
	return -ENOSYS;
}

int mm_mprotect(struct mm_struct *mm, unsigned long addr, unsigned long len,
		unsigned int prot)
{
	(void)mm;
	(void)addr;
	(void)len;
	(void)prot;

	/* 留空：M4 的实现练习。 */
	return -ENOSYS;
}

struct page *mm_access(struct mm_struct *mm, unsigned long addr,
		       unsigned int access)
{
	(void)mm;
	(void)addr;
	(void)access;

	/* 留空：M4 的实现练习。 */
	return NULL;
}
