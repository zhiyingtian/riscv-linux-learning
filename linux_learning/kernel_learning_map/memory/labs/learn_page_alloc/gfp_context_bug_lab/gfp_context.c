// SPDX-License-Identifier: GPL-2.0
/*
 * gfp_context.c - 在原子上下文里用 GFP_KERNEL 分配物理页的错误示范。
 *
 * 预期 dmesg：
 *   BUG: sleeping function called from invalid context at mm/page_alloc.c:5156
 *   in_atomic(): 1, irqs_disabled(): 1 ...
 *   Call Trace:
 *     __might_resched
 *     prepare_alloc_pages
 *     __alloc_pages
 *     gfp_demo_init
 *
 * 为什么一次 order-0 分配也会报：
 *   __alloc_pages() 在进入快路径之前会调用 prepare_alloc_pages()，
 *   后者执行 might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM)。
 *   GFP_KERNEL 含 __GFP_DIRECT_RECLAIM，于是即使页从 pcp 快路径拿到、
 *   这次并没有真的睡眠，内核也会在当前上下文不是可睡眠上下文时报警。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>

static int mode = 1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode,
	"1=atomic+GFP_KERNEL（bug）; 2=atomic+GFP_ATOMIC（正确）; 3=process+GFP_KERNEL（对照）");

static DEFINE_SPINLOCK(demo_lock);

static void gfp_demo_dump(const char *tag, gfp_t gfp, struct page *page)
{
	if (!page) {
		pr_warn("gfp_demo: %s: gfp=%#x allocation failed\n", tag, (unsigned int)gfp);
		return;
	}

	pr_info("gfp_demo: %s: gfp=%#x pfn=%lx refcount=%d\n",
		tag, (unsigned int)gfp, page_to_pfn(page), page_ref_count(page));
}

/*
 * 先在本 CPU 的 pcp 里放一页，尽量让后面那次“原子上下文分配”走 pcp
 * 快路径。这样 demo 只会触发 might_sleep 警告，不会真的进入可能睡眠的
 * 慢路径（直接回收）而把 guest 挂住。
 *
 * 注意：这是为了让实验可控，不是修复 bug。真实驱动不应该靠“pcp 里刚好
 * 有页”来掩盖在原子上下文用 GFP_KERNEL 的错误。
 */
static void gfp_demo_warm_pcp(void)
{
	struct page *page = alloc_pages(GFP_KERNEL, 0);

	if (page)
		__free_pages(page, 0);
}

static struct page *gfp_demo_alloc(gfp_t gfp, const char *tag)
{
	struct page *page;

	pr_info("gfp_demo: %s: about to alloc_pages(%#x, 0)\n", tag, (unsigned int)gfp);
	page = alloc_pages(gfp, 0);
	gfp_demo_dump(tag, gfp, page);
	return page;
}

static int __init gfp_demo_init(void)
{
	struct page *page = NULL;
	unsigned long flags;

	pr_info("gfp_demo: mode=%d, in_atomic()=%d, irqs_disabled()=%d\n",
		mode, in_atomic(), irqs_disabled());

	switch (mode) {
	case 1:
		/* 错误：拿 spinlock 后仍用可能睡眠的 GFP_KERNEL。 */
		gfp_demo_warm_pcp();
		spin_lock_irqsave(&demo_lock, flags);
		page = gfp_demo_alloc(GFP_KERNEL, "atomic/GFP_KERNEL");
		spin_unlock_irqrestore(&demo_lock, flags);
		break;

	case 2:
		/* 正确：原子上下文用 GFP_ATOMIC（不允许直接回收）。 */
		spin_lock_irqsave(&demo_lock, flags);
		page = gfp_demo_alloc(GFP_ATOMIC, "atomic/GFP_ATOMIC");
		spin_unlock_irqrestore(&demo_lock, flags);
		break;

	case 3:
		/* 对照：进程上下文用 GFP_KERNEL，允许睡眠。 */
		page = gfp_demo_alloc(GFP_KERNEL, "process/GFP_KERNEL");
		break;

	default:
		pr_err("gfp_demo: invalid mode=%d (expected 1, 2, or 3)\n", mode);
		return -EINVAL;
	}

	if (page)
		__free_pages(page, 0);

	pr_info("gfp_demo: done\n");
	return 0;
}

static void __exit gfp_demo_exit(void)
{
	pr_info("gfp_demo: unloaded\n");
}

module_init(gfp_demo_init);
module_exit(gfp_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GFP/atomic-context page allocation bug demo");
