# 物理页分配 Debug 练习

> **目标**：三个“故意写错”的物理页使用场景，你通过内核反馈自己定位、修复。
> **方式**：一次只加载一个 bug，先观察再改代码，改完重编译验证。
> **前提**：qemu 里的 5.15 内核已开 `CONFIG_DEBUG_ATOMIC_SLEEP` / `CONFIG_SLUB_DEBUG`。

---

## 准备

宿主编译并同步到 qemu 共享目录（一条命令）：

```bash
cd linux_learning/kernel_learning_map/memory/labs/learn_page_alloc/alloc_bug_demo
make install
```

`make install` = 交叉编译 + 自动复制 `alloc_bug_demo.ko` 到 `kmodules/learn_page_alloc/alloc_bug_demo/`。
改完 bug 后重复这一条即可。

guest 里：

```bash
mount -t 9p -o trans=virtio kmod_mount /mnt
cd /mnt/learn_page_alloc/alloc_bug_demo
```

> 建议顺序：**bug=2 → bug=3 → bug=1 → bug=4（可选）**。前三个开箱即用；bug=4 的 double free 检测依赖 `CONFIG_DEBUG_VM`，当前配置下是静默的（见该节说明）。

---

## bug=2：把 slab 页当 buddy 页释放（稳定可见，先做这个）

```bash
insmod alloc_bug_demo.ko bug=2
dmesg | tail -30
```

**你应该看到**（节选）：

```
alloc_bug_demo: slab page pfn=xxxxx, free it as a buddy page
BUG: Bad page state in process insmod  pfn:xxxxx
page:... refcount:0 mapcount:0 mapping:... index:...
flags: 0x...(...|PG_slab|...)
```

`free_pages_prepare()` 里的 `check_free_page()` 检查 `PAGE_FLAGS_CHECK_AT_FREE`，
发现 `PG_slab` 后调用 `bad_page()` 并返回 false，所以这次释放被拒绝，页不会进 buddy。

**引导问题**

- **P1**：内核靠哪个标志位认出“这不是 buddy 页”？
- **P2**：如果内核不检查、直接按 buddy 页释放，会发生什么？
- **P3**：slab 页和 buddy 页在 `struct page` 上有哪些不同的标志位？

**修复方向**（不写代码，自己改）：slab 分配的对象必须用 `kfree` 归还，不能 `__free_pages`。

---

## bug=3：泄漏（完全静默，只能靠观测发现）

```bash
# 1. 记录基线
grep MemFree /proc/meminfo
cat /proc/buddyinfo

# 2. 触发泄漏
insmod alloc_bug_demo.ko bug=3 leak_pages=256
dmesg | tail -5

# 3. 再看
grep MemFree /proc/meminfo
cat /proc/buddyinfo

# 4. 卸载模块（注意观察是否恢复）
rmmod alloc_bug_demo
grep MemFree /proc/meminfo
```

**引导问题**

- **P4**：`MemFree` 少了多少 KB？和 `leak_pages=256` 对得上吗？（提示：256 页 × 4KB）
- **P5**：`/proc/buddyinfo` 里哪一列变了？为什么是这一列而不是高阶？
- **P6**：`rmmod` 之后内存回来了吗？为什么？
- **P7**：如果分配循环在第 100 页失败，前 99 页该怎么办？现在的代码是怎么处理的？

**修复方向**：保存每次分配的指针，在 `__exit` 里释放；同时处理“部分失败”的回滚。

---

## bug=1：原子上下文用 GFP_KERNEL（间歇性，最难）

```bash
insmod alloc_bug_demo.ko bug=1
dmesg | tail -30
```

**可能的两种结果**：

1. **什么都不报** —— 内存充足，分配走快路径，没有真正睡眠。
2. **报错** —— 进入 slowpath 后真的睡眠了：

```
BUG: sleeping function called from invalid context at ...
in_atomic(): 1, irqs_disabled(): 1, ...
```

**引导问题**

- **P8**：为什么同一个 bug，内存充足时不报、有压力时才报？这对你排查线上问题意味着什么？
- **P9**：`GFP_KERNEL` 比 `GFP_ATOMIC` 多了哪个 `__GFP_*` 位？这个位允许内核做什么？
- **P10**：修复有几种方向？分别影响什么？（改 gfp 标志 / 改锁的持有范围 / 预分配）

**制造内存压力的方法**（可选，别把 qemu 撑死）：

```bash
# 分配大块并触碰，制造压力；Ctrl-C 结束
head -c 512M /dev/zero > /tmp/pressure &
```

---

## bug=4：double free（需要 `CONFIG_DEBUG_VM`，当前默认静默）

```bash
insmod alloc_bug_demo.ko bug=4
dmesg | tail -10
```

**当前配置下你会看到**：只有 `alloc pfn=..., now free twice` 和 `init done`，**没有 Bad page state**。

这不是 bug 没触发，而是检测被编译掉了：

```c
void __free_pages(struct page *page, unsigned int order)
{
	if (put_page_testzero(page))          // 第 1 次:1→0,true,真正释放
		free_the_page(page, order);
	else if (!PageHead(page))
		while (order-- > 0)                // 第 2 次:order=0,一次都不执行
			free_the_page(page + (1 << order), order);
}

static inline int put_page_testzero(struct page *page)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);   // ← 需要 CONFIG_DEBUG_VM
	return page_ref_dec_and_test(page);                 // 0 → -1,返回 false
}
```

**想看它报错**：内核开 `CONFIG_DEBUG_VM=y` 重编。届时 `VM_BUG_ON_PAGE` 会 `BUG()`，
打印 `VM_BUG_ON_PAGE(page_ref_count(page) == 0)` 和调用栈（guest 会 oops，需要重启）。

**引导问题**

- **P11**：为什么同一个“重复释放”，内核有的配置报、有的不报？这对线上排查意味着什么？
- **P12**：`refcount` 被减到 -1 之后，这个页还能被正确释放吗？下次分配会发生什么？
- **P13**：`__free_pages` 里 `else if (!PageHead(page))` 分支是给谁用的？（提示：order>0 的复合页）

---

## 观测指标速查

| 指标 | 看什么 |
|---|---|
| `/proc/meminfo` | `MemFree`、`MemAvailable`（泄漏） |
| `/proc/buddyinfo` | 每列 = order 0..10 的空闲块数（泄漏时 order 0 减少） |
| `/proc/zoneinfo` | `free`、水位线 `min/low/high` |
| `dmesg` | `Bad page state`、`sleeping function`、`page allocation failure` |
| `dmesg \| grep -i warn` | 所有 WARN/BUG |

> 注意：`/proc/buddyinfo` 在低内存时会**自动合并**相邻空闲块，所以泄漏 256 页不一定直接表现为 order0 少 256——先看 `MemFree` 更可靠。

---

## 修复验证清单

- [ ] bug=2 修复后：重复 `insmod/rmmod` 10 次，`dmesg` 无 `Bad page state`
- [ ] bug=3 修复后：`insmod` 后再 `rmmod`，`MemFree` 回到基线（±几 MB 正常）
- [ ] bug=1 修复后：在内存压力下 `insmod`，`dmesg` 无 `sleeping function`

---

## 答案区（先自己跑完再写）

- P1:
- P2:
- P3:
- P4:
- P5:
- P6:
- P7:
- P8:
- P9:
- P10:
- P11:
- P12:
- P13:
