/*
 * alloc_bug_demo.c —— 物理页分配的三种典型 bug,用于调式练习
 *
 * 用法(guest 内):
 *   insmod alloc_bug_demo.ko bug=1     # 原子上下文用 GFP_KERNEL(间歇性)
 *   insmod alloc_bug_demo.ko bug=2     # double free(稳定报错)
 *   insmod alloc_bug_demo.ko bug=3 leak_pages=256   # 泄漏(完全静默)
 *   rmmod alloc_bug_demo
 *
 * 注意:bug=2 会让 buddy 报 Bad page state,但内核会拒绝第二次释放,
 *       不会真正破坏元数据;重启 qemu 可完全复位。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static int bug = 1;
module_param(bug, int, 0644);
MODULE_PARM_DESC(bug, "1=atomic GFP_KERNEL, 2=double free, 3=page leak");

static int leak_pages = 256;
module_param(leak_pages, int, 0644);
MODULE_PARM_DESC(leak_pages, "bug=3 时泄漏的页数(默认 256 = 1MB)");

static DEFINE_SPINLOCK(demo_lock);

/*
 * bug=1:自旋锁保护的原子上下文里使用 GFP_KERNEL。
 * GFP_KERNEL 允许直接回收,可能睡眠。
 * 内存充足时分配走快路径、不睡眠 → 可能什么都不报(间歇性 bug)。
 * 只有进入 slowpath(内存压力)才会触发 "sleeping function called from
 * invalid context"。
 */
static void demo_bug1(void)
{
	struct page *page;
	unsigned long flags;

	spin_lock_irqsave(&demo_lock, flags);
	page = alloc_pages(GFP_KERNEL, 0);
	spin_unlock_irqrestore(&demo_lock, flags);

	if (page)
		__free_pages(page, 0);
}

/*
 * bug=2:把 slab 页当 buddy 页释放。
 * free_pages_prepare() → check_free_page() 检查 PAGE_FLAGS_CHECK_AT_FREE,
 * 其中包含 PG_slab → bad_page() 打印 "BUG: Bad page state" 并拒绝释放。
 * 稳定可见,且不会 panic(返回 false 后不进入 buddy)。
 */
static void demo_bug2(void)
{
	void *p = kzalloc(PAGE_SIZE, GFP_KERNEL);
	struct page *page;

	if (!p) {
		pr_err("alloc_bug_demo: kzalloc failed\n");
		return;
	}
	page = virt_to_page(p);
	pr_info("alloc_bug_demo: slab page pfn=%lx, free it as a buddy page\n",
		page_to_pfn(page));
	__free_pages(page, 0);	/* BUG:PageSlab,bad_page 后拒绝释放 */
	kfree(p);		/* 内核拒绝后 p 仍然有效 */
}

/*
 * bug=4:double free —— 只在 CONFIG_DEBUG_VM=y 时可见。
 * 无 DEBUG_VM 时:第二次 put_page_testzero() 的 VM_BUG_ON_PAGE 是空操作,
 * refcount 从 0 减到 -1,order=0 的 while 不执行 → 完全静默。
 */
static void demo_bug4(void)
{
	struct page *page = alloc_pages(GFP_KERNEL, 0);

	if (!page) {
		pr_err("alloc_bug_demo: alloc failed\n");
		return;
	}
	pr_info("alloc_bug_demo: alloc pfn=%lx, now free twice\n",
		page_to_pfn(page));
	__free_pages(page, 0);
	__free_pages(page, 0);
}

/*
 * bug=3:分配后不保存指针、不释放。
 * 完全静默,只能通过 /proc/meminfo、/proc/buddyinfo 看到变化。
 * rmmod 也不会回收,直到重启。
 */
static void demo_bug3(void)
{
	int i;

	pr_info("alloc_bug_demo: leaking up to %d pages\n", leak_pages);
	for (i = 0; i < leak_pages; i++) {
		struct page *page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);

		if (!page) {
			pr_warn("alloc_bug_demo: alloc failed at %d\n", i);
			break;
		}
		/* 故意不保存指针,也不 __free_pages */
	}
	pr_info("alloc_bug_demo: leaked %d pages\n", i);
}

static int __init demo_init(void)
{
	pr_info("alloc_bug_demo: bug=%d\n", bug);

	switch (bug) {
	case 1:
		demo_bug1();
		break;
	case 2:
		demo_bug2();
		break;
	case 3:
		demo_bug3();
		break;
	case 4:
		demo_bug4();
		break;
	default:
		pr_warn("alloc_bug_demo: unknown bug=%d\n", bug);
		break;
	}

	pr_info("alloc_bug_demo: init done\n");
	return 0;
}

static void __exit demo_exit(void)
{
	pr_info("alloc_bug_demo: exit (bug=3 泄漏的页不会在这里回收)\n");
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("buddy allocator bug demo for debugging practice");
