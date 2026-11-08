#ifndef TINY_MM_BUDDY_H
#define TINY_MM_BUDDY_H

#include <stdbool.h>
#include <stddef.h>

#include "tmm.h"

/*
 * tiny-mm / M2: buddy 分配器
 *
 * M2 先做一个独立的 buddy 模块，不改 M1 的 pmm.c：
 *   - 页元数据仍用 struct page；
 *   - 空闲页通过 page->buddy_next 串成链表；
 *   - 每个 order 一个 free_area。
 *
 * 这样可以在不破坏 M1 测试的前提下，单独验证 split/merge。
 * M2b 再把 pmm_alloc_pages() 切到这个 buddy 上。
 */

struct free_area {
	struct page *head;	/* 空闲块链表头；块大小 = 2^order 页 */
	unsigned long nr_free;	/* 空闲块个数，不是页数 */
};

struct buddy {
	struct page *pages;	/* 调用者提供的 page 数组 */
	unsigned long nr_pages;	/* 页数，必须是 2 的幂 */
	unsigned int max_order;	/* nr_pages = 2^max_order */
	struct free_area free_area[MAX_ORDER];
	unsigned long free_count;	/* 空闲页总数，不是空闲块数 */
	unsigned long split_count;	/* 发生过多少次 split，便于观察 */
	unsigned long merge_count;	/* 发生过多少次 merge，便于观察 */
};

/*
 * 初始化。
 *
 * 要求：
 *   - pages 非空；
 *   - nr_pages 是 2 的幂；
 *   - 2^max_order < MAX_ORDER。
 *
 * 初始状态：整个 pages[0..nr_pages) 是一个 max_order 的空闲块。
 * 成功返回 0，失败返回 -EINVAL。
 */
int buddy_init(struct buddy *b, struct page *pages, unsigned long nr_pages);

/* 清空 buddy 元数据；不释放调用者的 pages 数组。 */
void buddy_exit(struct buddy *b);

/*
 * 分配 2^order 个连续页。
 *
 * 成功：返回块首页，首页 refcount=1、is_free=false、order=order。
 * 失败：返回 NULL，不改变状态。
 */
struct page *buddy_alloc(struct buddy *b, unsigned int order);

/*
 * 释放从 page 开始的 2^order 个连续页。
 *
 * 成功返回 0；失败返回 -EINVAL：
 *   - page 为空/越界/未按 2^order 对齐；
 *   - order 越界；
 *   - page 已 free、refcount != 1 或 order 不匹配。
 *
 * 释放后要尝试与 buddy 合并，直到不能再合并。
 */
int buddy_free(struct buddy *b, struct page *page, unsigned int order);

/* 当前空闲页总数。 */
unsigned long buddy_free_count(const struct buddy *b);

/*
 * 自检：
 *   - 每个 free_area 的链表节点都在 pages 范围内；
 *   - 节点 order、is_free、refcount 正确；
 *   - nr_free 与链表长度一致；
 *   - free_count 与所有空闲块页数之和一致。
 *
 * 但链表重叠/循环只能抓到一部分；测试仍要显式检查分配结果。
 */
int buddy_check(const struct buddy *b);

#endif /* TINY_MM_BUDDY_H */
