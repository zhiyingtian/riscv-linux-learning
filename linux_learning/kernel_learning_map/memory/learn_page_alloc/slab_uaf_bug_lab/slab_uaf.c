// SPDX-License-Identifier: GPL-2.0
/*
 * slab_uaf.c - slab use-after-free / double-free / out-of-bounds 的错误示范。
 *
 * 模式：
 *   1 = UAF 别名：kfree 后继续通过旧指针读，观察新分配是否复用同一对象
 *   2 = double free（危险，可能破坏 freelist / 触发 SLUB 报错）
 *   3 = slab 越界写（危险，可能破坏相邻对象）
 *   4 = 正确：alloc + use + free
 *
 * mode=1 是默认模式，相对安全；mode=2/3 建议在可恢复快照里做。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>

static int mode = 1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode,
	"1=UAF alias (safe-ish); 2=double free (danger); 3=OOB write (danger); 4=correct");

#define OBJ_SIZE 64

static void *stale;
static void *fresh;

static int __init slab_demo_init(void)
{
	pr_info("slab_demo: mode=%d\n", mode);

	switch (mode) {
	case 1:
		/*
		 * 错误：kfree(stale) 之后，stale 是悬空指针；这里继续读它。
		 * 然后 kmalloc 一个同尺寸对象，观察 allocator 是否复用同一地址。
		 */
		stale = kmalloc(OBJ_SIZE, GFP_KERNEL);
		if (!stale) {
			pr_err("slab_demo: kmalloc stale failed\n");
			return -ENOMEM;
		}

		memset(stale, 0x41, OBJ_SIZE);
		pr_info("slab_demo: stale=%px (allocated, filled 0x41)\n", stale);
		kfree(stale);
		pr_info("slab_demo: kfree(stale) done\n");

		fresh = kmalloc(OBJ_SIZE, GFP_KERNEL);
		if (!fresh) {
			pr_err("slab_demo: kmalloc fresh failed\n");
			return 0;
		}

		pr_info("slab_demo: fresh=%px\n", fresh);

		memset(fresh, 0x42, OBJ_SIZE);

		/*
		 * UAF 读：stale 已经被释放；如果 fresh == stale，说明新对象
		 * 正好复用了旧地址，旧指针现在指向新对象。
		 */
		pr_info("slab_demo: alias=%d stale[0]=0x%02x fresh[0]=0x%02x\n",
			stale == fresh,
			((u8 *)stale)[0],
			((u8 *)fresh)[0]);

		if (stale == fresh)
			pr_info("slab_demo: stale 和 fresh 是同一地址：旧指针变成新对象的别名\n");
		else
			pr_info("slab_demo: 地址不同，但 stale 仍然悬空；读到的是已释放对象的内存\n");

		kfree(fresh);
		fresh = NULL;
		stale = NULL;
		break;

	case 2:
		/*
		 * 危险：同一个对象 kfree 两次。
		 * 没有 slub_debug/KASAN 时可能不会立刻报错，而是破坏 freelist。
		 */
		stale = kmalloc(OBJ_SIZE, GFP_KERNEL);
		if (!stale) {
			pr_err("slab_demo: kmalloc failed\n");
			return -ENOMEM;
		}

		pr_info("slab_demo: obj=%px, free once\n", stale);
		kfree(stale);
		pr_info("slab_demo: free twice (this may corrupt freelist)\n");
		kfree(stale);
		stale = NULL;
		break;

	case 3:
		/*
		 * 危险：只申请 32 字节，却写 64 字节。
		 * SLUB 的 redzone/KASAN 可以抓到；当前内核默认没有开。
		 */
		stale = kmalloc(32, GFP_KERNEL);
		if (!stale) {
			pr_err("slab_demo: kmalloc failed\n");
			return -ENOMEM;
		}

		pr_info("slab_demo: obj=%px, kmalloc(32), writing 64 bytes\n", stale);
		memset(stale, 0x41, 64);
		kfree(stale);
		stale = NULL;
		break;

	case 4:
		stale = kmalloc(OBJ_SIZE, GFP_KERNEL);
		if (!stale) {
			pr_err("slab_demo: kmalloc failed\n");
			return -ENOMEM;
		}

		memset(stale, 0x41, OBJ_SIZE);
		kfree(stale);
		stale = NULL;
		pr_info("slab_demo: correct alloc + free\n");
		break;

	default:
		pr_err("slab_demo: invalid mode=%d (expected 1..4)\n", mode);
		return -EINVAL;
	}

	pr_info("slab_demo: done\n");
	return 0;
}

static void __exit slab_demo_exit(void)
{
	pr_info("slab_demo: unloaded\n");
}

module_init(slab_demo_init);
module_exit(slab_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Slab UAF / double-free / OOB bug demo");
