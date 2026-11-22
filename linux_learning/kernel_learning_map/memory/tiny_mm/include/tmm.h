#ifndef TINY_MM_H
#define TINY_MM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * tiny-mm: 物理地址模型 + 物理页分配
 *
 * M1：bitmap 版 order-0 分配器。
 * M2c：pmm 改用 buddy + pcp，支持 order 0..MAX_ORDER-1。
 * M2d：zone 增加 min/low/high 水位与回收标记。
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

enum page_state {
	PAGE_ALLOCATED,
	PAGE_IN_BUDDY,
	PAGE_IN_PCP,
};

/*
 * M2d: zone 水位。
 *
 *   WMARK_MIN  : 硬预留，普通分配不能低于它；
 *   WMARK_LOW  : 低于它时标记“需要回收”；
 *   WMARK_HIGH : 回到它以上时清除回收标记。
 */
enum zone_watermark {
	WMARK_MIN,
	WMARK_LOW,
	WMARK_HIGH,
	NR_WMARK,
};

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
	enum page_state state;	/* 页状态：已分配/在 buddy 空闲链表/在 pcp 链表 */
	void *pgtable;		/* M3: 页表页内容；普通数据页为 NULL */
};

struct pglist_data;

struct zone {
	const char *name;
	struct pglist_data *zone_pgdat;	/* 属于哪个 node */
	unsigned long start_pfn;
	unsigned long spanned_pages;
	unsigned long free_count;	/* buddy + pcp 的总空闲页数 */

	/* M2d: min/low/high 三个水位，索引是 enum zone_watermark。 */
	unsigned long watermark[NR_WMARK];
	bool reclaim_needed;		/* 是否已标记需要回收 */
	unsigned long reclaim_wakeups;	/* 从健康进入 low 的次数 */
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
 * nr_pages 是本次模拟管理的页数；M2c 要求它是 2 的幂。
 */
int pmm_init(unsigned long start_pfn, unsigned long nr_pages);
void pmm_exit(void);

/*
 * 分配/释放。
 *
 * M2c：order 0 走 pcp，order > 0 直接走 buddy。
 * 失败返回 NULL，不崩溃。
 *
 * pmm_free_pages 返回 0 成功，负数错误：
 *   -EINVAL: page 为空/越界/已 free/order 不匹配/state 不对
 *
 * 注意：order 0 free 后页进入 pcp，此时 is_free=false、
 * refcount=0、state=PAGE_IN_PCP，而不是回到 buddy 的 free_area。
 */
struct page *pmm_alloc_pages(unsigned int order);
int pmm_free_pages(struct page *page, unsigned int order);

/*
 * M2d: 带分配标志的版本。
 *
 * 普通分配（flags=0）会受 WMARK_MIN 水位限制；
 * PMM_ALLOC_HARD 允许使用 min 预留，类似 Linux 的 ALLOC_HARDER/原子分配，
 * 用于耗尽测试、最大块分配以及不能睡眠的分配路径。
 */
#define PMM_ALLOC_HARD 0x1u

struct page *pmm_alloc_pages_flags(unsigned int order, unsigned int flags);

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

/*
 * M2d: 水位与回收标记。
 *
 * pmm_watermark_free() 只返回 buddy 空闲页，不包含 pcp 缓存页；
 * 水位判断应该基于这个值。
 */
bool pmm_watermark_ok(unsigned int order, enum zone_watermark mark);
unsigned long pmm_watermark_free(void);
bool pmm_reclaim_needed(void);
unsigned long pmm_reclaim_wakeups(void);

#endif /* TINY_MM_H */
