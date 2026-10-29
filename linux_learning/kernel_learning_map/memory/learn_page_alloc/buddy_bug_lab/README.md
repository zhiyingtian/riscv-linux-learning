# 物理页分配 Debug 实验：页池的 double free

> **内核**：`runninglinuxkernel_5.15`（Linux 5.15.0+，RISC-V，`-O0`，`CONFIG_DEBUG_VM=n`）
> **平台**：QEMU `virt`，1 GiB，SMP=2
> **目标**：给你一条真实的 `BUG: Bad page state ... nonzero _refcount`，从现象反推物理页的生命周期错误。
>
> 关键点速查：[double_free_keypoints.md](double_free_keypoints.md)

---

## 0. 为什么做这个实验

学完 buddy 的拆分/合并、`GFP` 标志、pcp 之后，最容易写错的其实不是算法，而是**页的生命周期**：

- 同一个 `struct page *` 释放两次；
- 释放后指针没清空，错误路径和正常路径各释放一次；
- 分配 order=2，释放时 order 写错；
- 在原子上下文用 `GFP_KERNEL`；
- 分配失败不检查，或者失败路径不回滚。

这些 bug 的共同点是：**报错的地方往往不是写错的地方**。这个实验只放一个 bug——页池里的一次 double free——但它会延迟到 pcp 批量回收时才被内核发现。你要做的，就是把这根链条接起来。

---

## 1. 场景

`page_pool.c` 是一个极简的“DMA 页池”驱动：

```text
insmod
  page_pool_init()
    kcalloc(pool, nr_blocks)
    for i in 0..nr_blocks-1:
        pool[i].page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0)
    pool[37].cached = true          // 其中一个块标记为 cached

rmmod
  page_pool_exit()
    pool_trim_cached()              // 先释放 cached 块
    pool_release_all()              // 再释放整个页池
```

默认 `nr_blocks=8192`（8192 × 4KB = 32 MiB），`victim=37`。`pool_trim_cached()` 释放了 block 37，但**没有把 `pool[37].page` 置空**；`pool_release_all()` 又按数组释放了一遍。

于是同一个 `struct page` 被 `__free_pages(page, 0)` 了两次。

> 模块里加了 `pool_dump_block()` 调试打印：默认只打印 victim 的 `pfn/refcount/flags/buddy`；
> `insmod page_pool.ko verbose=1` 会额外在 release 时每 1024 个块打印一次进度。

---

## 2. 5.15 里这条路径长什么样

先自己对照源码读一遍（行号以你的树为准）：

```text
pool_trim_cached()
  __free_pages(page, 0)                       mm/page_alloc.c:5451
    put_page_testzero(page)                   include/linux/mm.h:745
      page_ref_dec_and_test(): 1 -> 0         // 第一次，成功
    free_the_page(page, 0)                    mm/page_alloc.c:702
      free_unref_page()                       mm/page_alloc.c:3390
        free_unref_page_prepare()
          free_pcp_prepare()                  mm/page_alloc.c:1408
            free_pages_prepare(..., check_free=false)
        free_unref_page_commit()              mm/page_alloc.c:3366
          list_add(&page->lru, &pcp->lists[...])
          pcp->count++

pool_release_all()
  __free_pages(page, 0)                       // 同一个 page
    put_page_testzero(page)
      page_ref_dec_and_test(): 0 -> -1        // 第二次，返回 false
    // order == 0，else if (!PageHead(page)) 的 while 一次都不执行
    // 页仍然躺在 pcp 链表里，refcount 已经是 -1

  ... 继续释放其余 8191 个页 ...
    pcp->count >= pcp->high
    free_pcppages_bulk()                      mm/page_alloc.c:1442
      bulkfree_pcp_prepare()                  mm/page_alloc.c:1416
        check_free_page()                     mm/page_alloc.c:1203
          page_expected_state(): refcount != 0
          bad_page(page, "nonzero _refcount") mm/page_alloc.c:621
```

关键点：

- `CONFIG_DEBUG_VM=n` 时，`put_page_testzero()` 里的
  `VM_BUG_ON_PAGE(page_ref_count(page) == 0, page)` 被编译掉，第二次释放不会当场 BUG；
- order-0 的 pcp 释放路径 `free_pcp_prepare()` 把 `check_free` 传成 `false`，
  所以**释放时不检查**；
- 检查被推迟到 pcp 批量倒回 buddy 时（`free_pcppages_bulk` → `bulkfree_pcp_prepare` → `check_free_page`）；
- 触发这个“延迟检查”的，是后面那 8191 次释放把 pcp 撑到 `high`。

---

## 3. 快速开始

### 3.1 host 编译并同步到 9p 共享目录

```bash
cd linux_learning/kernel_learning_map/memory/learn_page_alloc/buddy_bug_lab
make install
```

`make install` 会：

1. 用你的 5.15 树交叉编译 `page_pool.ko`；
2. 复制到 `runninglinuxkernel_5.15/kmodules/learn_page_alloc/buddy_bug_lab/`。

> 如果目录不同，用 `make install SHARED=/path/to/share` 覆盖。
> 只编译不复制：`make`。

### 3.2 启动 guest

```bash
cd ~/workspace/riscv/runninglinuxkernel_5.15
./run_rlk_riscv.sh run
# 登录 benshushu / 123
su root
```

9p 共享一般已经在 `/mnt`；没有的话：

```bash
mount -t 9p -o trans=virtio kmod_mount /mnt
```

### 3.3 触发

```bash
cd /mnt/learn_page_alloc/buddy_bug_lab

# 方式 A：一键脚本
./guest_run.sh

# 方式 B：手动，方便做实验
grep MemFree /proc/meminfo
insmod page_pool.ko
dmesg | tail -2          # 记下 pfn
rmmod page_pool
dmesg | tail -30
```

> `bad_page()` 有“60 条/分钟”的抑制窗口；如果重复跑很多次后看不到警告，等一分钟或重启 guest。

---

## 4. 实测现象

以下是我在 1 GiB / SMP=2 的 guest 里实测的输出（pfn 和 MemFree 每次会变）：

```text
# insmod page_pool.ko
[    5.848796] page_pool: pool ready, 8192 pages (32768 KiB), cached block 37 pfn=84667
[    5.849428] page_pool: alloc: block=37 pfn=84667 refcount=1 flags=0 buddy=0

# grep MemFree /proc/meminfo
MemFree:          965040 kB

# rmmod page_pool
[    5.984964] page_pool: trim cached block 37 pfn=84667
[    5.985177] page_pool: trim before: block=37 pfn=84667 refcount=1 flags=0 buddy=0
[    5.985432] page_pool: trim after: block=37 pfn=84667 refcount=0 flags=0 buddy=0
[    5.985701] page_pool: release 8192 blocks
[    5.985958] page_pool: release before: block=37 pfn=84667 refcount=0 flags=0 buddy=0
[    5.986202] page_pool: release after: block=37 pfn=84667 refcount=-1 flags=0 buddy=0
[    5.987927] BUG: Bad page state in process rmmod  pfn:84667
[    5.988475] page:(____ptrval____) refcount:-1 mapcount:0 mapping:0000000000000000 index:0x0 pfn:0x84667
[    5.991592] page dumped because: nonzero _refcount
[    6.035482] page_pool: release done: block=37 pfn=84667 refcount=-1 flags=0 buddy=0
```

`pool_dump_block()` 打印的是 `struct page` 里的真实字段：

| 字段 | 含义 |
|---|---|
| `refcount` | `page->_refcount`，正常的调用者引用是 1 |
| `flags` | `page->flags`；`0` 表示没有 `PG_slab`/`PG_buddy` 等 |
| `buddy` | `PageBuddy(page)`；`0` 说明页不在 buddy 的 `free_area[]` 里 |

`trim after` 时 `refcount=0` 但 `buddy=0`，说明页在 **pcp** 而不是 buddy；
`release after` 时 `refcount=-1`，说明第二次 free 只把引用计数减了 1，并没有真正回收；
`release done` 仍然是 `-1`，说明它已经被 pcp 倒回时的检查丢弃了。

配套的 `/proc/zoneinfo`（同一台 guest）：

```text
        low      1248
        high     1497
        managed  250545
              high:  624          <- 每个 CPU 的 pcp->high
```

`release 8192 blocks` 远超 pcp 的 `high=624`，所以释放过程中一定会把 pcp 倒回 buddy，也就一定会检查到那个 refcount=-1 的页。

修复后（`pool[i].page = NULL`，见第 8 节）同一套流程：

```text
[    8.009355] page_pool: pool ready, 8192 pages (32768 KiB), cached block 37 pfn=866eb
[    8.141407] page_pool: trim cached block 37 pfn=866eb
[    8.141883] page_pool: release 8192 blocks
# 没有 Bad page state
```

---

## 5. 问题链（先自己答，答案在第 7 节）

> 建议顺序：先只看第 4 节的 dmesg，合上第 7 节，用源码和 `/proc` 回答；卡住再往下看。

**Q1.** `refcount:-1` 是谁减的？为什么是 `-1` 而不是 `0`？

**Q2.** 第二次 `__free_pages()` 为什么没有当场报错？内核是在哪一行、由哪次操作触发的检查？

**Q3.** 为什么 Bad page state 的 `pfn:846f9` 和 `trim cached block 37 pfn=846f9` 完全相同？这说明什么？

**Q4.** `flags: 0x0(zone=0)` 说明这个页当时在哪里？为什么没有 `PG_buddy`？

**Q5.** `Call Trace` 只打印了 `dump_backtrace` 一帧。除了栈，你还能用哪些证据定位？（至少说出 3 种）

**Q6.** 第二次 free 时，`mm_page_free` tracepoint 为什么没有记录？这个现象对排查意味着什么？

**Q7.** 如果在 `rmmod` 之前刚好有一次同 CPU 的 order-0 分配，警告可能完全消失。为什么？“没有警告”能说明没有 bug 吗？

**Q8.** 正确的修复是什么？改完以后用什么指标验证？

**Q9.** `pool_trim_cached()` 和 `pool_release_all()` 之间的所有权约定应该怎么写，才能避免同类错误？

**Q10.** 为什么 `bad_page()` 只 taint 内核、不 panic？如果 `panic_on_warn=1` 会怎样？

---

## 6. 调试工具箱

### 6.1 读 dmesg

```text
pfn:846f9                       <- 物理页号，和模块打印的 pfn 对照
refcount:-1                     <- 被多减了一次
flags: 0x0(zone=0)              <- zone=0；没有 PG_buddy/PG_slab 等标志
page dumped because: nonzero _refcount
Comm: rmmod                     <- 当前进程；说明检查发生在 rmmod 的释放路径
```

`refcount`、`mapcount`、`mapping`、`flags` 是 `dump_page()` 从 `struct page` 直接打印的。`pfn` 是最有用的索引：用它去找“谁分配了它、谁释放了它”。

### 6.2 /proc

```bash
grep -E 'MemFree|MemAvailable' /proc/meminfo
cat /proc/buddyinfo
grep -E 'low|high|managed' /proc/zoneinfo
cat /proc/pagetypeinfo        # 需要 root，看 MIGRATE_* 的分布
```

- `MemFree`：页池 32 MiB 的进出；double free 只丢 1 页（4KB），在 MB 级波动里看不出来，所以**不要只靠 MemFree 判断**。
- `buddyinfo`：pcp 倒回 buddy 后，order-0 的页会尝试合并成高阶；看的是趋势，不是精确账本。
- `zoneinfo`：本实验里 pcp 的 `high=624`，解释为什么 8192 次释放一定会触发批量回收。

### 6.3 tracepoint：能看到第一次，看不到第二次

```bash
TR=/sys/kernel/tracing
mount -t tracefs tracefs $TR 2>/dev/null || true
: > $TR/trace
echo 1 > $TR/events/kmem/mm_page_free/enable

insmod page_pool.ko
rmmod page_pool

grep 846f9 $TR/trace
```

你会看到 victim 被 `mm_page_free` 记录**一次**（trim 的那次），第二次 free 没有事件——因为 `mm_page_free` 在 `free_pages_prepare()` 里，而第二次 `__free_pages()` 在 `put_page_testzero()` 就返回了，根本没走到 `free_the_page()`。

> 结论：tracepoint 能帮你确认“第一次释放”，但不能证明“没有第二次释放”。refcount 的变化要靠 gdb 或源码推理。

### 6.4 gdb：在 `bad_page` 停下

用 debug 模式启动（注意：`run debug` 会加 `-s -S`，并且把 SMP 设为单 CPU）：

```bash
./run_rlk_riscv.sh run debug
```

另一个终端：

```bash
gdb -q vmlinux
(gdb) target remote :1234
(gdb) break bad_page
(gdb) commands
> silent
> printf "bad_page: refcount=%d flags=%lx\n", page->_refcount.counter, page->flags
> bt 16
> continue
> end
(gdb) continue
```

然后照常登录、`insmod`、`rmmod`。gdb 会在 `bad_page()` 停下。`bt` 至少能看到内核侧的回收路径：

```text
bad_page
check_free_page
bulkfree_pcp_prepare
free_pcppages_bulk
free_unref_page_commit
free_unref_page
__free_pages
```

> 模块自己的帧（`pool_release_all` / `page_pool_exit`）在 gdb 里可能只显示地址，
> 因为模块符号不在 `vmlinux` 里；要解析它们得用 `add-symbol-file page_pool.ko <text地址>`
> （地址从 `/sys/module/page_pool/sections/.text` 取）。先用 pfn + 源码对照就够了。
>
> 实测 dmesg 的 `Call Trace` 只打印到 `dump_backtrace`（这个 `-O0` RISC-V 构建的栈回溯不完整），
> gdb 的 `bt` 更有用。

想看 refcount 的两次递减，最直接的是在 `__free_pages` 上打断点、用模块打印的 pfn 对照
（该函数参数是 `page` 和 `order`）；或者临时打开 `CONFIG_DEBUG_VM`，让第二次 free 当场 `BUG()`。

### 6.5 源码锚点（5.15，行号以你的树为准）

| 位置 | 作用 |
|---|---|
| `mm/page_alloc.c:5451` | `__free_pages()`：refcount 减到 0 才真正回收 |
| `include/linux/mm.h:745` | `put_page_testzero()`：`VM_BUG_ON_PAGE(refcount==0)`（`DEBUG_VM=n` 时编译掉） |
| `mm/page_alloc.c:702` | `free_the_page()`：order ≤ 3 走 pcp，否则直接 `__free_pages_ok()` |
| `mm/page_alloc.c:3390` | `free_unref_page()`：pcp 释放入口 |
| `mm/page_alloc.c:3312` | `free_unref_page_prepare()` → `free_pcp_prepare()` |
| `mm/page_alloc.c:1408` | `free_pcp_prepare()`：`DEBUG_VM=n` 时 `check_free=false` |
| `mm/page_alloc.c:3366` | `free_unref_page_commit()`：入 pcp 链表，`count >= high` 时批量倒回 |
| `mm/page_alloc.c:1442` | `free_pcppages_bulk()`：批量倒回 buddy |
| `mm/page_alloc.c:1416` | `bulkfree_pcp_prepare()` → `check_free_page()` |
| `mm/page_alloc.c:1203` | `check_free_page()`：检查 `page_expected_state()` |
| `mm/page_alloc.c:1157` | `page_expected_state()`：`_mapcount`、`mapping`、`_refcount`、flags |
| `mm/page_alloc.c:621` | `bad_page()`：打印并 `add_taint()`，不是 `BUG()` |
| `mm/page_alloc.c:2388` | `post_alloc_hook()` → `set_page_refcounted()` |
| `mm/internal.h:92` | `set_page_refcounted()`：`set_page_count(page, 1)`，会把 -1 直接“洗白” |
| `mm/page_alloc.c:6810` | `zone_highsize()`：pcp `high` 的来源（水位线 / CPU 数） |
| `mm/page_alloc.c:1058` | `__free_one_page()`：pcp 倒回后真正进 buddy、合并伙伴 |

---

## 7. 参考答案

### Q1

`pool_trim_cached()` 第一次 `__free_pages(page, 0)` 把 refcount 从 1 减到 0，页进入 pcp 链表。`pool_release_all()` 又对同一个页调用一次：

```c
// include/linux/mm.h:745
static inline int put_page_testzero(struct page *page)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);  // DEBUG_VM=n：空操作
	return page_ref_dec_and_test(page);               // 0 -> -1，返回 false
}
```

`VM_BUG_ON_PAGE()` 在 `CONFIG_DEBUG_VM=n` 时被编译掉，所以没有当场 BUG；原子减还是执行了，`_refcount` 变成 -1，`put_page_testzero()` 返回 false。

### Q2

第二次 `__free_pages()`：

```c
if (put_page_testzero(page))          // false
	free_the_page(page, order);   // 不执行
else if (!PageHead(page))
	while (order-- > 0)           // order=0，一次都不执行
		free_the_page(page + (1 << order), order);
```

所以第二次释放**什么都没做**，只是把 refcount 减成了 -1，页仍然在 pcp 链表里。

检查发生在之后某次释放把 `pcp->count` 撑到 `high` 时：

```text
free_unref_page_commit()
  pcp->count += 1 << order;
  if (pcp->count >= high)
      free_pcppages_bulk()
        bulkfree_pcp_prepare(page)
          check_free_page(page)
            page_expected_state(page, PAGE_FLAGS_CHECK_AT_FREE)
              page_ref_count(page) != 0  ->  false
            check_free_page_bad(page)
              bad_page(page, "nonzero _refcount")
```

`free_pcp_prepare()` 在 `DEBUG_VM=n` 时把 `check_free` 传成 `false`，所以第一次入 pcp 时不做检查；检查被推迟到批量倒回时。这是“延迟发现”的根源。

### Q3

同一个 pfn。`trim` 第一次释放，`release` 第二次释放；Bad page state 是第二次释放留下的 -1 在 pcp 倒回时被发现。pfn 是串联“模块日志”和“内核报告”的唯一可靠线索。

### Q4

页在 **pcp 链表**里，不在 buddy 里。`PG_buddy` 只在 `__free_one_page()` 把页插入 `free_area[]` 时由 `set_buddy_order()` 设置；pcp 是 buddy 之上的每 CPU 缓存，pcp 页没有 `PG_buddy`。`flags: 0x0(zone=0)` 里的 `zone=0` 是 `ZONE_DMA32`（这台 RISC-V guest 只有 DMA32 zone）。

### Q5

- 模块打印的 pfn 与 Bad page state 的 pfn 对照；
- 源码：`trim` 和 `release` 都持有 `pool[i].page`，且 `trim` 后没清空；
- gdb：`break bad_page` / `break __free_pages if page == ...`，看 refcount 和 `bt`；
- tracepoint：`mm_page_free` 只能看到第一次；
- `/proc/zoneinfo` 的 pcp `high`，解释为什么批量倒回会发生；
- 必要时开 `CONFIG_DEBUG_VM` / `PAGE_OWNER` 获取更多信息。

### Q6

`mm_page_free` tracepoint 在 `free_pages_prepare()` 开头。第二次 `__free_pages()` 在 `put_page_testzero()` 就返回了，没有调用 `free_the_page()`，自然不会进 `free_pages_prepare()`。所以 tracepoint 里只有第一次释放；**refcount 被减成 -1 这件事对 tracepoint 完全不可见**。

### Q7

`pcp` 是 LIFO：刚 trim 掉的 victim 在链表头，下一次同 CPU 的 order-0 分配很可能正好把它取走。取出时 `prep_new_page()` → `post_alloc_hook()` → `set_page_refcounted()`：

```c
// mm/internal.h:92
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	VM_BUG_ON_PAGE(page_ref_count(page), page);  // DEBUG_VM=n：空操作
	set_page_count(page, 1);                     // -1 直接被改成 1
}
```

refcount 被“洗白”成 1，页有了新主人；之后再 `release`，1 → 0，检查通过，警告消失。**double free 仍然发生了**，只是被一次分配掩盖了。所以“没有警告”不能证明没有 bug，这类 bug 才会在线上时隐时现。

### Q8

在 `trim` 释放后把指针置空（或清 `cached` 并让 release 跳过）：

```c
static void pool_trim_cached(void)
{
	int i;

	for (i = 0; i < nr_blocks; i++) {
		if (!pool[i].cached)
			continue;
		pr_info("page_pool: trim cached block %d pfn=%lx\n",
			i, page_to_pfn(pool[i].page));
		__free_pages(pool[i].page, 0);
		pool[i].page = NULL;     /* 关键：释放后使指针失效 */
	}
}
```

`pool_release_all()` 里已经有 `if (!pool[i].page) continue;`，所以不用再改。验证：

- `dmesg` 无 `Bad page state`；
- `MemFree` 基本回到基线（几 MB 波动正常）；
- 连续 `insmod/rmmod` 10 次无警告；
- `/proc/buddyinfo` 与基线同量级。

### Q9

一句话：**一次分配对应一次释放，释放后指针必须失效；错误路径和正常路径不能同时拥有同一个页。**

工程上可以：

- 释放后 `page = NULL`（本实验采用）；
- 或者用 `freed` 标志 + `WARN_ON` 防重入；
- 或者让 `trim` 只清缓存标记，不真正释放，所有权统一留给 `release`；
- 错误路径用 `goto err_free` 集中回滚，避免“释放了但没清”。

### Q10

`bad_page()` 的实现是 `pr_alert()` + `dump_page()` + `print_modules()` + `dump_stack()` + `add_taint(TAINT_BAD_PAGE)`，没有 `BUG()`，所以只污染内核、不 panic。`panic_on_warn=1` 只对 `WARN()` 生效，`bad_page()` 不是 `WARN()`，不会因为它 panic；但内核已经被 taint，后续行为不再可信。想让它在 `bad_page()` 时直接 panic，用 `panic_on_taint=0x20`（bit 5 = `TAINT_BAD_PAGE`）。taint 与 panic 的完整区别见 [double_free_keypoints.md](double_free_keypoints.md)。

---

## 8. 修复与验证

修改 `page_pool.c` 的 `pool_trim_cached()`，在 `__free_pages()` 后加一行：

```c
pool[i].page = NULL;
```

重新 `make install`，guest 里重跑：

```bash
rmmod page_pool 2>/dev/null || true
dmesg -C
insmod page_pool.ko
rmmod page_pool
dmesg | tail -10
```

预期：只有 `pool ready` / `trim cached` / `release`，没有 `Bad page state`。

如果保留调试打印，修复后 `trim after` 和 `release done` 会打印
`page=NULL (already freed)`——这是预期的，说明指针已经被主动失效，release 会跳过它。

> 注意：`dmesg -C` 只是清环形缓冲区，不会清掉内核 taint；taint 会一直保留到重启，这是正常的。

---

## 9. 延伸实验（选做，做完对 pcp 的理解会更深）

1. **让 victim 晚一点释放**：`insmod page_pool.ko victim=8000`。
   victim 在 release 靠后的位置才被第二次释放；在这之前，它很可能已经被 pcp 倒回 buddy（refcount=0，检查通过），第二次 free 只是把它变成 -1 的 `PageBuddy` 页。**警告可能完全不出现**，或者换了时机才出现——这正好说明“检测结果依赖释放顺序”。

2. **让页池小于 pcp high**：`insmod page_pool.ko nr_blocks=100`。
   release 不会触发批量倒回，警告不会在 rmmod 时出现；之后随便制造一些页释放（或等系统活动），看它什么时候冒出来。

3. **把 `DEBUG_VM` 打开**（需要重编内核）：
   `put_page_testzero()` 里的 `VM_BUG_ON_PAGE(refcount == 0)` 会在第二次 free 当场 `BUG()`，你会得到即时崩溃和调用栈；代价是内核更大、更慢。对比“延迟发现”和“即时发现”的取舍。

4. **`CONFIG_PAGE_OWNER=y` + `page_owner=on` 启动**：
   `dump_page()` 会打印这个页的分配栈，直接告诉你“谁分配的”。这是生产环境定位页泄漏/双重释放的常用手段（有开销）。

5. **故意 UAF**：trim 后不置空，继续对 `page_address(pool[i].page)` 写数据；在页被重新分配后，观察谁的数据被改坏。配合 `CONFIG_PAGE_POISONING`/`INIT_ON_FREE`/KASAN 更容易看到。

6. **order 写错**：把块改成 `alloc_pages(..., 2)`，释放仍写 `__free_pages(page, 0)`。不会立刻报错：头页以 order-0 进入 buddy，3 个尾页永久泄漏；`MemFree` 会少 3 页/块。用 `page->private`/`PageBuddy` 在 gdb 里验证。

---

## 10. 物理页分配 bug 地图（背下来）

| 类别 | 典型写法 | 现象 | 5.15 关键路径 | 首选手段 |
|---|---|---|---|---|
| 上下文/GFP | 自旋锁/中断里 `GFP_KERNEL` | `sleeping function called from invalid context`；内存紧张才出现 | `__alloc_pages_slowpath` → direct reclaim | 压力下复现；`DEBUG_ATOMIC_SLEEP` |
| 释放次数 | 同一页 `__free_pages()` 两次 | 延迟 `Bad page state ... nonzero _refcount`；页被重分配则静默 | `put_page_testzero`；pcp 延迟 `check_free_page` | pfn 对照；gdb watch refcount |
| 释放阶数 | order=2 分配、order=0 释放 | 静默泄漏；或 buddy 重叠/坏页 | `__free_pages(page, order)` 契约 | `MemFree`/`buddyinfo`；`page->private` |
| 释放对象 | slab 页当 buddy 页 free；或把 `alloc_pages()` 的页 `kfree()` | `PG_slab` Bad page；反方向在 5.15 里 `VM_BUG_ON_PAGE(!PageCompound)` 被编译掉，可能静默只释放头页 | `check_free_page`；`free_nonslab_page` | `flags`/`PG_slab`；调用栈；`page->private` |
| UAF | free 后继续用；页还被映射/引用就 free | 静默数据损坏、随机崩溃 | refcount / `get_page` / `put_page` | KASAN / `PAGE_OWNER` / poison；gdb watchpoint |
| 泄漏 | 错误路径忘记释放/不回滚 | `MemFree` 下降、`buddyinfo` 变化 | alloc/free 配对 | `/proc/meminfo`、`buddyinfo`、`page_owner` |
| 引用计数 | 少 `put` / 多 `put` | 页永久占用 / 提前释放 | `_refcount`、`_mapcount` | gdb `page->_refcount` |
| 空指针 | 不检查 `alloc_pages()` 返回值 | NULL deref / oops | `alloc_pages()` 可返回 NULL | 栈；低内存注入 |
| 迁移类型 | 长期 pin 住 unmovable 页 | 碎片、高阶分配失败 | `MIGRATE_*` / fallback | `/proc/pagetypeinfo` |

---

## 11. 一页纸

- `alloc_pages(order)` ↔ `__free_pages(page, order)`，**一次分配一次释放**；
- 释放后指针必须失效（`page = NULL`），错误路径和正常路径不能都以为自己是 owner；
- pcp 页是“已释放但还没回 buddy”的页，**没有 `PG_buddy`**，检查被延迟；
- `bad_page` 是证据不是根因：它告诉你 pfn 和 `refcount`，根因在更早的代码；
- 没有警告 ≠ 没有 bug：一次及时的分配就能把 `-1` 洗成 `1`；
- 先用 pfn 把“模块日志”和“内核报告”对上，再回源码找第二次 `__free_pages()`。

---

## 文件

| 文件 | 作用 |
|---|---|
| `page_pool.c` | 有 bug 的页池模块（double free） |
| `Makefile` | 交叉编译 + `make install` 到 9p 共享目录 |
| `guest_run.sh` | guest 内一键触发并打印 dmesg |
| `README.md` | 本文 |
| `double_free_keypoints.md` | 这个 bug 的关键点整理：状态机、pcp 延迟检查、taint/panic、gdb 与调试技巧 |
