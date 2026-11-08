#ifndef TINY_MM_H
#define TINY_MM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * tiny-mm / M1: 物理地址模型
 *
 * 这里刻意使用 Linux 风格的命名，但实现是用户态模拟。
 * 先不要看 Linux 源码抄实现：先按注释和测试把语义想清楚。
 */

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define MAX_ORDER 11
#define MAX_ZONES 2
#define MAX_NUMNODES 1

typedef uint64_t phys_addr_t;

/*
 * struct page：物理页的元数据，不是页内容。
 *
 * M1 只需要这些字段；M2 之后会加 free_area/buddy 相关字段，
 * M6 会加 PageSlab/compound 相关字段。
 */
struct page {
	unsigned long flags;	/* 保留：M1 不用，M2+ 可放 zone/order 状态 */
	unsigned int order;	/* 当前页的 order；free 时用于检查 free 错 order */
	struct page *buddy_next;	/* M2: free_area 空闲链表指针，仅空闲块头使用 */
	int refcount;		/* 引用计数：alloc 后 1，free 后 0 */
	bool is_free;		/* 是否在 allocator 的空闲集合里 */
	unsigned int zone_id;	/* 属于哪个 zone（M1 只有一个 zone） */
};

struct pglist_data;

struct zone {
	const char *name;
	struct pglist_data *zone_pgdat;	/* 属于哪个 node */
	unsigned long start_pfn;
	unsigned long spanned_pages;
	unsigned long free_count;
};

/*
 * M1 只有一个 node（模拟 CONFIG_NUMA=n）。
 * Linux 的 pg_data_t 里 node_zones[] 可以有多个 zone；这里先用 1 个。
 */
struct pglist_data {
	struct zone node_zones[MAX_ZONES];
	int nr_zones;
	unsigned long node_start_pfn;
	unsigned long node_spanned_pages;
	int node_id;
};

/*
 * 初始化/销毁。
 *
 * start_pfn 模拟物理内存起始 PFN（不是一定从 0 开始）。
 * nr_pages 是本次模拟管理的页数。
 */
int pmm_init(unsigned long start_pfn, unsigned long nr_pages);
void pmm_exit(void);

/*
 * 分配/释放。
 *
 * M1 只支持 order=0；order>0 返回 NULL（M2 再扩展）。
 * 失败返回 NULL，不崩溃。
 *
 * pmm_free_pages 返回 0 成功，负数错误：
 *   -EINVAL: page 为空/越界/已 free/order 不匹配
 */
struct page *pmm_alloc_pages(unsigned int order);
int pmm_free_pages(struct page *page, unsigned int order);

/*
 * 地址转换：M1 的核心。
 */
unsigned long page_to_pfn(const struct page *page);
struct page *pfn_to_page(unsigned long pfn);
bool pfn_valid(unsigned long pfn);
phys_addr_t pfn_to_phys(unsigned long pfn);
unsigned long phys_to_pfn(phys_addr_t paddr);

/*
 * zone 查询。
 */
struct zone *page_zone(const struct page *page);
struct pglist_data *page_pgdat(const struct page *page);
int page_to_nid(const struct page *page);

/*
 * 统计与自检。
 */
unsigned long pmm_free_count(void);
unsigned long pmm_total_count(void);
int pmm_check(void);

#endif /* TINY_MM_H */
