// SPDX-License-Identifier: GPL-2.0
/*
 * page_pool.c - 一个极简的 DMA 页池驱动，用来演示物理页生命周期 bug。
 *
 * 驱动在加载时分配 nr_blocks 个 order-0 页，其中 block[victim] 被标记为
 * "cached"。卸载时先 trim 掉 cached 页，再释放整个页池。
 *
 * 编译/运行方法见同目录 README.md。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>

#define POOL_BLOCKS_DEFAULT	8192
#define POOL_VICTIM_DEFAULT	37

struct pool_block {
	struct page *page;
	bool cached;
};

static struct pool_block *pool;
static int nr_blocks = POOL_BLOCKS_DEFAULT;
static int victim = POOL_VICTIM_DEFAULT;

module_param(nr_blocks, int, 0444);
MODULE_PARM_DESC(nr_blocks, "页池块数（每块 1 个 4KB 页）");
module_param(victim, int, 0444);
MODULE_PARM_DESC(victim, "被标记为 cached 的块号");

static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "1=release 时每 1024 个块打印一次进度");

/* 打印一个块当前在 struct page 里能看到的状态，方便和 dmesg 的 pfn 对照。 */
static void pool_dump_block(const char *tag, int i)
{
	struct page *page = pool[i].page;

	if (!page) {
		pr_info("page_pool: %s: block=%d page=NULL (already freed)\n",
			tag, i);
		return;
	}

	pr_info("page_pool: %s: block=%d pfn=%lx refcount=%d flags=%lx buddy=%d\n",
		tag, i, page_to_pfn(page), page_ref_count(page),
		page->flags, PageBuddy(page) ? 1 : 0);
}

static void pool_trim_cached(void)
{
	int i;

	printk("page_pool: trim cached block %d\n", victim);
	for (i = 0; i < nr_blocks; i++) {
		if (!pool[i].cached || !pool[i].page)
			continue;
		pr_info("page_pool: trim cached block %d pfn=%lx, page=%px\n",
			i, page_to_pfn(pool[i].page), pool[i].page);
		pool_dump_block("trim before", i);
		__free_pages(pool[i].page, 0);
		pool_dump_block("trim after", i);
	}
}

static void pool_release_all(void)
{
	int i;

	pr_info("page_pool: release %d blocks\n", nr_blocks);
	for (i = 0; i < nr_blocks; i++) {
		if (!pool[i].page)
			continue;
		if (i == victim)
			pool_dump_block("release before", i);
		else if (verbose && !(i & 1023))
			pr_info("page_pool: release progress %d/%d\n", i, nr_blocks);

		__free_pages(pool[i].page, 0);

		if (i == victim)
			pool_dump_block("release after", i);
	}
	pool_dump_block("release done", victim);
	kfree(pool);
	pool = NULL;
}

static int __init page_pool_init(void)
{
	int i;

	if (nr_blocks <= 0 || nr_blocks > 65536) {
		pr_err("page_pool: invalid nr_blocks=%d\n", nr_blocks);
		return -EINVAL;
	}
	if (victim < 0 || victim >= nr_blocks)
		victim = 0;

	pool = kcalloc(nr_blocks, sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;

	for (i = 0; i < nr_blocks; i++) {
		pool[i].page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
		if (!pool[i].page) {
			pr_err("page_pool: alloc failed at block %d\n", i);
			while (i--)
				__free_pages(pool[i].page, 0);
			kfree(pool);
			pool = NULL;
			return -ENOMEM;
		}
	}

	pool[victim].cached = true;
	pr_info("page_pool: pool ready, %d pages (%d KiB), cached block %d pfn=%lx, page=%px\n",
		nr_blocks, nr_blocks * 4, victim,
		page_to_pfn(pool[victim].page), pool[victim].page);
	pool_dump_block("alloc", victim);
	return 0;
}

static void __exit page_pool_exit(void)
{
	pool_trim_cached();
	pool_release_all();
}

module_init(page_pool_init);
module_exit(page_pool_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA page pool used for a physical page lifetime debugging lab");
