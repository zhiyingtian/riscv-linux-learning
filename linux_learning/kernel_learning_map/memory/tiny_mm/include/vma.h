#ifndef TINY_MM_VMA_H
#define TINY_MM_VMA_H

#include <stdbool.h>
#include <stdint.h>

#include "pgtable.h"

/*
 * tiny-mm / M4: VMA + 缺页
 *
 * M3 只做“vaddr → PTE → page”的页表 walk，但它不回答
 * “这一段 vaddr 到底该不该映射、以什么权限映射”。
 * M4 在 M3 上面加一层 VMA，用惰性映射串起 mmap → 缺页 → PTE：
 *
 *   mm_mmap()     只建 VMA，不分配页、不写 PTE；
 *   mm_access()   第一次访问触发缺页，才分配匿名页并写 PTE。
 *
 * 当前已完成：数据结构与 API 声明。
 * TODO(M4)：find_vma、mm_mmap/mm_munmap/mm_mprotect/mm_access。
 */

/*
 * VMA flags：故意和 pgtable.h 的 MM_PROT_* 同值，
 * 这样 vm_flags 可以直接传给 mm_map()/mm_protect()，
 * 而 mm_access() 的 access（MM_ACCESS_*）可以直接 & 在 vm_flags 上判权限。
 */
#define VM_READ   MM_PROT_READ
#define VM_WRITE  MM_PROT_WRITE
#define VM_EXEC   MM_PROT_EXEC

/*
 * struct vm_area_struct：一段虚拟地址区间的描述。
 *
 * vm_start 是闭区间起点，vm_end 是开区间终点（Linux 语义）。
 * M4 只做匿名页：vm_pgoff 固定 0，没有 vm_file/vm_ops。
 */
struct vm_area_struct {
	struct mm_struct *vm_mm;
	unsigned long vm_start;		/* 包含 */
	unsigned long vm_end;		/* 不包含 */
	unsigned long vm_flags;		/* VM_READ/WRITE/EXEC */
	unsigned long vm_pgoff;		/* M5 文件页用；M4 固定 0 */
	struct vm_area_struct *vm_next;
};

/*
 * VMA 查找。
 *
 * 返回第一个 vm_end > addr 的 VMA；不保证包含 addr（Linux find_vma 语义）。
 * 空链表或 addr 在所有 VMA 之后返回 NULL。
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);

/*
 * 建立 / 拆除 / 改权限。
 *
 * mm_mmap    ：建匿名 VMA；相邻且兼容的 VMA 合并；重叠返回 -EEXIST。
 * mm_munmap  ：清 PTE、归还匿名页、拆分/删除 VMA。
 * mm_mprotect：改 VMA 权限，并同步已映射的 PTE。
 *
 * 返回 0 成功，负数错误：
 *   -EINVAL: 参数非法/地址未对齐/越界
 *   -EEXIST: mmap 与现有 VMA 重叠
 *   -ENOMEM: 页表页或匿名页分配失败
 */
int mm_mmap(struct mm_struct *mm, unsigned long addr, unsigned long len,
	    unsigned int prot);
int mm_munmap(struct mm_struct *mm, unsigned long addr, unsigned long len);
int mm_mprotect(struct mm_struct *mm, unsigned long addr, unsigned long len,
		unsigned int prot);

/*
 * 访问 vaddr：find_vma → 权限检查 → 缺页。
 *
 * 返回 addr 对应的 struct page；未映射/越权/缺页失败返回 NULL。
 * access 取 MM_ACCESS_READ/WRITE/EXEC。
 */
struct page *mm_access(struct mm_struct *mm, unsigned long addr,
		       unsigned int access);

#endif /* TINY_MM_VMA_H */
