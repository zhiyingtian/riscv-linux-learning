// SPDX-License-Identifier: GPL-2.0
/*
 * page_refcount.c - 物理页引用计数失衡的错误示范。
 *
 * 模式：
 *   1 = 多一次 get_page，少一次 put_page（泄漏）
 *   2 = 多一次 put_page（页提前释放，后续访问会 UAF）
 *   3 = 正确：一次 alloc_pages + 一次 __free_pages
 *
 * 注意：mode=2 结束后 demo_page 已经被释放，只读 page 元数据用于打印，
 * 不再访问页内容；实际驱动里如果继续用这个 page 指针，就是 use-after-free。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>

static int mode = 1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode,
	"1=get_page leak; 2=extra put_page; 3=correct");

static struct page *demo_page;

static void dump_ref(const char *tag)
{
	if (!demo_page) {
		pr_info("ref_demo: %s: page=NULL\n", tag);
		return;
	}

	pr_info("ref_demo: %s: pfn=%lx refcount=%d\n",
		tag, page_to_pfn(demo_page), page_ref_count(demo_page));
}

static int __init ref_demo_init(void)
{
	pr_info("ref_demo: mode=%d\n", mode);

	demo_page = alloc_pages(GFP_KERNEL, 0);
	if (!demo_page) {
		pr_err("ref_demo: alloc_pages failed\n");
		return -ENOMEM;
	}

	dump_ref("after alloc_pages");

	switch (mode) {
	case 1:
		/*
		 * 错误：多拿了一个引用，却只调用一次 __free_pages。
		 * __free_pages 把 refcount 从 2 减到 1，页不会被释放。
		 */
		get_page(demo_page);
		dump_ref("after get_page");

		__free_pages(demo_page, 0);
		dump_ref("after __free_pages (refcount should be 1 => leaked)");

		/* demo 清理：把多拿的那次引用还回去，避免真的泄漏。 */
		put_page(demo_page);
		demo_page = NULL;
		break;

	case 2:
		/*
		 * 错误：调用者只拥有一个引用，却 put 了两次。
		 * 第一次 2 -> 1，第二次 1 -> 0，页被释放。
		 */
		get_page(demo_page);
		dump_ref("after get_page");

		put_page(demo_page);
		dump_ref("after first put_page");

		put_page(demo_page);
		pr_info("ref_demo: after second put_page: page has been freed\n");
		pr_info("ref_demo: 继续使用 demo_page 指向的内容就是 use-after-free\n");

		demo_page = NULL;
		break;

	case 3:
		__free_pages(demo_page, 0);
		pr_info("ref_demo: correct: one alloc + one free\n");
		demo_page = NULL;
		break;

	default:
		pr_err("ref_demo: invalid mode=%d (expected 1, 2, or 3)\n", mode);
		__free_pages(demo_page, 0);
		demo_page = NULL;
		return -EINVAL;
	}

	pr_info("ref_demo: done\n");
	return 0;
}

static void __exit ref_demo_exit(void)
{
	pr_info("ref_demo: unloaded\n");
}

module_init(ref_demo_init);
module_exit(ref_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Page refcount leak / extra put_page bug demo");
