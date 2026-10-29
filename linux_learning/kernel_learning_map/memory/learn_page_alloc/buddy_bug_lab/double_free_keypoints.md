# 页池 double free：关键点整理

> 配套实验：[README.md](README.md)
> 模块：[page_pool.c](page_pool.c)
> 内核：Linux 5.15.0+（RISC-V，`-O0`，`CONFIG_DEBUG_VM=n`，`CONFIG_DEBUG_INFO=y`）
> 平台：QEMU `virt`，1 GiB，SMP=2

---

## 0. 一句话

同一个 `struct page` 被 `__free_pages(page, 0)` 释放了两次：

- 第一次：`refcount 1 -> 0`，页进入 **pcp**（per-CPU page list）；
- 第二次：`refcount 0 -> -1`，`put_page_testzero()` 返回 false，**第二次 free 什么都没做**；
- 之后其余 8191 次释放把 pcp 撑到 `high`，pcp 批量归还 buddy 时 `check_free_page()` 才发现 `_refcount != 0`，`bad_page()` 报 `nonzero _refcount` 并 taint 内核。

**报错的地方不是写错的地方**：根因在 `pool_trim_cached()` 释放后没有把 `pool[37].page` 置空。

---

## 1. 场景与时间线

```text
insmod
  page_pool_init()
    kcalloc(pool, nr_blocks)
    for i in 0..nr_blocks-1:
        pool[i].page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0)
    pool[37].cached = true                 // refcount = 1
    pool[37].page 仍然有效

rmmod
  page_pool_exit()
    pool_trim_cached()
      __free_pages(pool[37].page, 0)        // 第一次：1 -> 0，进 pcp
      // 没有 pool[37].page = NULL  ← BUG

    pool_release_all()
      for i in 0..8191:
        __free_pages(pool[i].page, 0)
        // i=37 时第二次释放 victim：0 -> -1，返回 false
        // 其余 8191 次释放把 pcp 撑到 high，触发批量归还 buddy
```

关键顺序：victim 在 `release` 的**早期**（i=37）就被第二次释放，但此时 pcp 还没满；真正报错发生在后面释放其他块、pcp 批量归还 buddy 的时候。

---

## 2. 状态机

| 时刻 | 操作 | `_refcount` | 页在哪里 | `PG_buddy` | 内核是否检查 |
|---|---|---|---:|---|---|
| `alloc` 后 | `alloc_pages()` | 1 | 已分配 | 0 | — |
| `trim` 第一次 free | `__free_pages()` | 0 | **pcp** | 0 | 否（`check_free=false`） |
| `release` 第二次 free | `__free_pages()` | **-1** | 仍在 pcp | 0 | 否（没走到 `free_the_page`） |
| pcp 批量归还 buddy | `free_pcppages_bulk()` | -1 | pcp → 检查 | 0 | **是**，`check_free_page()` → `bad_page()` |
| `bad_page` 后 | `continue` | -1 | 被丢弃，不回 buddy | 0 | 已记录 |

核心状态变化只有一条：

```text
1 --(trim)--> 0 --(release 第二次)--> -1 --(pcp drain)--> bad_page
```

`-1` 不是“又释放成功了一次”，而是**引用计数被多减了一次**。第二次 `__free_pages()` 没有把页重新挂到任何链表，页仍然躺在第一次 free 放进去的 pcp 链表里。

---

## 3. 五个关键机制

### 3.1 `put_page_testzero()` 决定第二次 free “什么都不做”

[`mm/page_alloc.c:5451`](../../../../../../runninglinuxkernel_5.15/mm/page_alloc.c#L5451)（准确行号以你的树为准，下同）：

```c
void __free_pages(struct page *page, unsigned int order)
{
	if (put_page_testzero(page))
		free_the_page(page, order);
	else if (!PageHead(page))
		while (order-- > 0)
			free_the_page(page + (1 << order), order);
}
```

[`include/linux/mm.h:745`](../../../../../../runninglinuxkernel_5.15/include/linux/mm.h#L745)：

```c
static inline int put_page_testzero(struct page *page)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);
	return page_ref_dec_and_test(page);
}
```

第二次调用时：

1. `VM_BUG_ON_PAGE(refcount == 0)` 在 `CONFIG_DEBUG_VM=n` 时是空操作；
2. `page_ref_dec_and_test()` 把 0 减成 -1，返回 `false`；
3. `__free_pages()` 走 `else if (!PageHead(page))`；
4. `order == 0`，`while (order-- > 0)` 一次都不执行；
5. 所以**没有调用 `free_the_page()`**，没有重新入 pcp，也没有任何页状态检查。

注意：`else if` 不是错误处理。它是为**高阶页的 speculative reference** 准备的：`put_page()` 可能只释放非 compound 分配的第一页，剩下的尾页由 `__free_pages()` 补释放。所以“`put_page_testzero()` 返回 false”在 API 语义里不一定是 bug，函数不能简单地在这里报错。

### 3.2 `CONFIG_DEBUG_VM` 把“当场断言”编译掉

`VM_BUG_ON_PAGE(refcount == 0)` 就是为 double free 准备的立即检查，但只有 `CONFIG_DEBUG_VM=y` 时才展开成：

```c
dump_page(page, "VM_BUG_ON_PAGE(...)");
BUG();
```

默认 `CONFIG_DEBUG_VM=n`，整条语句被编译掉。于是第二次 free 不会 BUG，只会静默地把 `_refcount` 减成 -1。

打开 `DEBUG_VM` 后，第二次 free 会在 `put_page_testzero()` 里当场 `BUG()`（oops，不是 `bad_page`），而不是等到 pcp 批量归还 buddy。这是“即时发现”和“延迟发现”的分水岭。

### 3.3 pcp 是“已释放、但还没回 buddy”的中间态

`free_the_page()`（`mm/page_alloc.c:702`）：

```c
static inline void free_the_page(struct page *page, unsigned int order)
{
	if (pcp_allowed_order(order))        /* order <= 3 */
		free_unref_page(page, order);   /* 走 pcp */
	else
		__free_pages_ok(page, order, FPI_NONE);
}
```

第一次 free 的调用链：

```text
__free_pages()
  put_page_testzero(): 1 -> 0
  free_the_page()
    free_unref_page()
      free_unref_page_prepare()
        free_pcp_prepare()               mm/page_alloc.c:1408
          free_pages_prepare(..., check_free=false)
      free_unref_page_commit()           mm/page_alloc.c:3366
        list_add(&page->lru, &pcp->lists[...])
        pcp->count++
```

pcp 页的特点：

- 已经“释放”，但**不在 buddy 的 `free_area[]` 里**；
- 因此**没有 `PG_buddy`**（`PageBuddy(page) == 0`）；
- 真正回到 buddy 是在 `free_pcppages_bulk()` 批量归还 buddy 的时候。

本实验里 `/proc/zoneinfo` 的 `high: 624`（每个 CPU 的 `pcp->high`），而 `release` 要释放 8192 个页，所以**一定会触发批量归还 buddy**，也就一定会检查到 refcount=-1 的 victim。

### 3.4 检查被推迟到 pcp 批量归还 buddy

非 `DEBUG_VM` 版本（`mm/page_alloc.c:1408` / `:1416`）：

```c
static bool free_pcp_prepare(struct page *page, unsigned int order)
{
	...
	return free_pages_prepare(page, order, false, FPI_NONE);  /* check_free=false */
}

static bool bulkfree_pcp_prepare(struct page *page)
{
	return check_free_page(page);   /* 批量归还 buddy 时才做完整校验 */
}
```

`free_pcppages_bulk()`（`mm/page_alloc.c:1442`）：

```c
page = list_last_entry(list, struct page, lru);
list_del(&page->lru);
...
if (bulkfree_pcp_prepare(page))
	continue;              /* 坏页：丢掉，不再回 buddy */
```

`check_free_page()`（`mm/page_alloc.c:1203`）→ `page_expected_state()`（`:1157`）检查：

- `_mapcount != -1`；
- `page->mapping != NULL`；
- `_refcount != 0`；        ← 本实验命中这一条
- `memcg_data != 0`（`CONFIG_MEMCG`）；
- `flags & PAGE_FLAGS_CHECK_AT_FREE`。

任何一条不满足，就 `bad_page(page, reason)`（`mm/page_alloc.c:621`）：

```c
pr_alert("BUG: Bad page state in process %s  pfn:%05lx\n", ...);
dump_page(page, reason);
print_modules();
dump_stack();

page_mapcount_reset(page);   /* remove PageBuddy */
add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
```

所以 dmesg 里看到的是：

```text
BUG: Bad page state in process rmmod  pfn:84667
page dumped because: nonzero _refcount
```

### 3.5 重新分配会把 `-1` “洗白”

如果 pcp 批量归还 buddy 之前，victim 被同 CPU 的一次 order-0 分配取走：

```text
prep_new_page()
  post_alloc_hook()                 mm/page_alloc.c:2388
    set_page_refcounted()           mm/internal.h:92
      set_page_count(page, 1)       /* -1 -> 1 */
```

refcount 被直接改成 1，页有了新主人；之后即使再释放，检查也会通过。**double free 仍然发生了，只是没有留下可见证据。**

这就是“没有警告 ≠ 没有 bug”的原因，也是这类 bug 在线上时隐时现的原因。

---

## 4. 为什么内核要这样设计

### 4.1 每次 free 都做完整检查，所有正常释放都会跟着变慢

`__free_pages()` 每秒可能被调用几十万到几百万次。`page_expected_state()` 要读 `_mapcount`、`mapping`、`_refcount`、`memcg_data`、`flags` 多个字段，可能跨多条 cacheline；如果每次 free 都查一遍，等于给每一次正常释放都加一道固定开销，极少数 bug 的检查成本要由所有正常 workload 来承担。

内核的取舍：

- **最频繁的释放路径只做最便宜的 refcount 原子操作**；
- **完整的页状态校验放到批量慢路径**；
- **更重的检查用编译期开关显式打开**。

### 4.2 pcp 批量归还 buddy 时，顺带做校验

这里的“批量归还 buddy”指 `free_pcppages_bulk()` 把 pcp 链表里的一批页一次性还给伙伴系统（buddy）的 `free_area[]`，不是每 free 一页就单独归还一次。

- 页已经在 pcp 里攒成一批，不在 CPU 最热的单页 free 路径上；
- 一次归还一批，`check_free_page()` 的成本摊到这批页上，而不是每页单独查；
- 校验发生在拿 `zone->lock` 之前，`bad_page()` 的 `dump_page`/`dump_stack` 不在最紧的临界区里；
- 所有 order-0 释放最终都经过这里，覆盖率高。

### 4.3 调试检查是 opt-in，不是运行时安全边界

`CONFIG_DEBUG_VM` 的 Kconfig 原话是：

> “Enable this to turn on extended checks in the virtual-memory system that may impact performance.”

除此之外还有 `CONFIG_DEBUG_PAGEALLOC`、KASAN、`PAGE_OWNER`、`PAGE_POISONING` 等更重的工具。生产内核默认不付这个成本；需要抓 bug 时再开。

### 4.4 可用性优先：抓到也只 taint，不默认 panic

- `bad_page()` 自带“60 条/分钟”的限流，说明它设计上准备继续跑；
- 一个驱动 bug 或用户可触发的 double free，不应该让整台服务器直接挂掉；
- 想 fail-fast 的人可以显式开 `panic_on_taint=0x20`。

### 4.5 API 契约：调用者必须持有引用

`__free_pages()` 不是校验器，它假设调用者持有页的最后一个引用。重复释放是调用者的逻辑错误；内核用 debug 断言（`VM_BUG_ON_PAGE`）而不是生产路径上的分支来抓它。

---

## 5. 内核报了什么，没报什么

### 5.1 报了什么

- `pfn`：和模块打印的 pfn 完全一致，是串联“模块日志”和“内核报告”的唯一可靠索引；
- `refcount:-1`、`mapcount`、`mapping`、`flags`：`dump_page()` 直接从 `struct page` 读；
- `page dumped because: nonzero _refcount`；
- `Tainted: B`（`TAINT_BAD_PAGE`，bit 5，值 32）；
- `/proc/sys/kernel/tainted` 读出来是 32。

### 5.2 没报什么

- **没有 panic**：`bad_page()` 不调用 `panic()`；
- **没有 `BUG()`**：日志里的 `"BUG: Bad page state"` 只是字符串，不是 `BUG()` 宏；
- **没有 `WARN()`**：所以 `panic_on_warn=1` 对它无效；
- **没有当场报**：第二次 free 在 `put_page_testzero()` 就返回了，检查发生在 pcp 批量归还 buddy。

### 5.3 taint vs panic

| 维度 | taint | panic |
|---|---|---|
| 本质 | 状态位（bitmask） | 终止动作 |
| 是否停机 | 否 | 是 |
| 持续性 | 只增不减，重启才清 | 立即终止，可能按 `panic_timeout` 重启 |
| 目的 | 标记“内核不再干净”，影响 bug 报告可信度 | 防止继续运行造成更大破坏，或触发 kdump |
| 触发 | `add_taint()` | `panic()` |
| 升级方式 | `panic_on_taint=<bitmask>` | `panic_on_oops` / `panic_on_warn` 等策略 |

想让本实验在 `bad_page()` 时 panic：

```text
panic_on_taint=0x20
```

`0x20` = 32 = `1 << TAINT_BAD_PAGE`。`panic_on_warn=1` 不会生效，因为 `bad_page()` 不是 `WARN()`。

---

## 6. 调试路径（按可靠性排序）

### 6.1 pfn 对照（最便宜）

模块打印：

```text
page_pool: trim before: block=37 pfn=84667 refcount=1 ...
page_pool: trim after:  block=37 pfn=84667 refcount=0 ...
page_pool: release before: block=37 pfn=84667 refcount=0 ...
page_pool: release after:  block=37 pfn=84667 refcount=-1 ...
```

内核报告：

```text
BUG: Bad page state in process rmmod  pfn:84667
page dumped because: nonzero _refcount
```

同一个 pfn + refcount 变化，足以把两次 free 串起来。

### 6.2 printk 标记：放在调用之前

gdb 断在 `__free_pages` 入口时，CPU 还没执行函数体；只有**调用之前**已经执行的 printk 才会出现在 dmesg/串口。

```c
pr_info("page_pool: [%s] about to free block=%d pfn=%lx\n",
        __func__, i, page_to_pfn(pool[i].page));
__free_pages(pool[i].page, 0);
```

断点停住时看到 `[pool_trim_cached]` 还是 `[pool_release_all]`，就知道当前是哪条路径。

注意 `%p` 的坑：

```c
pr_info("page=%p\n", page);    /* 32 位哈希，不是地址 */
pr_info("page=%px\n", page);   /* 原始地址，调试用 */
```

`%p` 从 4.15 起默认做指针哈希（siphash + 每次启动的随机 key），所以 dmesg 里的 `page=00000000xxxxxxxx` 和 gdb 里的 `0xffff...` 对不上是正常的；`pfn` 是用真实指针算的，不受影响。

### 6.3 gdb：在 `bad_page` 停下

```bash
./run_rlk_riscv.sh run debug    # -s -S
```

另一个终端：

```bash
gdb-multiarch -q vmlinux
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

然后 guest 里 `insmod` / `rmmod`。`bt` 至少能看到内核侧：

```text
bad_page
check_free_page
bulkfree_pcp_prepare
free_pcppages_bulk
free_unref_page_commit
free_unref_page
__free_pages
```

模块自己的帧（`pool_release_all` / `page_pool_exit`）不在 `vmlinux` 里，可能只显示地址；要解析得用 `add-symbol-file`（见 6.4）。

### 6.4 gdb 行断点：只停在 `pool_trim_cached()` 的那次 free

源码不用改，`page_pool.c` 第 64 行就是 `pool_trim_cached()` 里的 `__free_pages()`。

```bash
# QEMU 带 -s；guest 里先 insmod
insmod /mnt/learn_page_alloc/buddy_bug_lab/page_pool.ko

# 取模块运行时地址
cat /sys/module/page_pool/sections/.text
# 例如 0xffffffffc0000000
```

host gdb：

```gdb
add-symbol-file /home/alan/workspace/riscv/runninglinuxkernel_5.15/kmodules/learn_page_alloc/buddy_bug_lab/page_pool.ko 0xffffffffc0000000

info line page_pool.c:64
break page_pool.c:64
```

`info line` 应输出：

```text
Line 64 ... starts at address 0xffffffffc000049a <pool_trim_cached+192>
```

然后 guest 里 `rmmod page_pool`，断点命中。

注意：

- 模块是 `-g -O0` 编译的，行号精确；
- 只断 `page_pool.c:64` 就只会命中 `pool_trim_cached()`；`release` 路径在 `page_pool.c:82`，不会误停；
- 每次重新 `insmod`，模块地址可能变，要 `remove-symbol-file page_pool.ko` 后重新 `add-symbol-file`；
- 这版 gdb 对 `.bss`/`.sdata` 静态变量的 `-s` 重定位不生效，`p pool[i].page` 可能读错；可在断点处用 `i` + `/sys/module/page_pool/sections/.bss` 手工取，或 `stepi` 到 `jalr` 前看 `$a0`；
- `bt` 的 `page_pool_exit` 帧也可能只显示地址（`.exit.text` 的符号重定位问题），不影响“停在 64 行”这个事实。

### 6.5 tracepoint：只能看到第一次

```bash
TR=/sys/kernel/tracing
mount -t tracefs tracefs $TR 2>/dev/null || true
: > $TR/trace
echo 1 > $TR/events/kmem/mm_page_free/enable

insmod page_pool.ko
rmmod page_pool

grep 84667 $TR/trace
```

`mm_page_free` 在 `free_pages_prepare()` 里；第二次 `__free_pages()` 在 `put_page_testzero()` 就返回了，根本没走到 `free_the_page()`，所以 tracepoint 里只有第一次释放。

> tracepoint 能确认“第一次释放”，但不能证明“没有第二次释放”。

### 6.6 `/proc/zoneinfo`：解释为什么一定会批量归还 buddy

```bash
grep -E 'high|managed' /proc/zoneinfo
```

本实验里每个 CPU 的 `pcp->high` 是 624；`release` 释放 8192 个页，远超 high，所以一定会触发 `free_pcppages_bulk()`，也就一定会检查到 victim。

---

## 7. 修复与验证

修复：释放后让指针失效。

```c
static void pool_trim_cached(void)
{
	int i;

	for (i = 0; i < nr_blocks; i++) {
		if (!pool[i].cached || !pool[i].page)
			continue;
		pr_info("page_pool: trim cached block %d pfn=%lx\n",
			i, page_to_pfn(pool[i].page));
		__free_pages(pool[i].page, 0);
		pool[i].page = NULL;     /* 关键：释放后使指针失效 */
	}
}
```

`pool_release_all()` 里已有：

```c
if (!pool[i].page)
	continue;
```

所以 release 会自动跳过 victim。

验证：

- dmesg 无 `Bad page state`；
- `MemFree` 基本回到基线（几 MB 波动正常）；
- 连续 `insmod/rmmod` 10 次无警告；
- `/proc/buddyinfo` 与基线同量级。

> `dmesg -C` 只清环形缓冲区，不会清掉内核 taint；taint 会保留到重启。

---

## 8. 配置矩阵

| 配置 | 检测时机 | 现象 | 成本 |
|---|---|---|---|
| `CONFIG_DEBUG_VM=n`（本实验） | pcp 批量归还 buddy | `bad_page()` + `Tainted: B` | 低 |
| `CONFIG_DEBUG_VM=y` | 第二次 free 当场 | `VM_BUG_ON_PAGE` → `BUG()`/oops | 高 |
| `CONFIG_DEBUG_PAGEALLOC` | free 时立即检查 + unmap | 更早发现，可能 oops | 很高 |
| KASAN | 访问/释放时 | UAF/OOB 报告 | 很高 |
| `CONFIG_PAGE_OWNER` | `dump_page()` 时 | 打印分配栈 | 高 |
| `CONFIG_PAGE_POISONING` | free 时填 0xaa | UAF 更容易暴露 | 中 |
| `panic_on_taint=0x20` | `add_taint()` 时 | `bad_page()` 直接 panic | 策略 |
| `no_hash_pointers` | 启动参数 | `%p` 打印原始地址 | 调试用 |

---

## 9. 源码锚点（5.15）

| 位置 | 作用 |
|---|---|
| `mm/page_alloc.c:5451` | `__free_pages()`：refcount 减到 0 才真正回收 |
| `include/linux/mm.h:745` | `put_page_testzero()`：`VM_BUG_ON_PAGE(refcount==0)`（`DEBUG_VM=n` 时编译掉） |
| `mm/page_alloc.c:702` | `free_the_page()`：order ≤ 3 走 pcp |
| `mm/page_alloc.c:3390` | `free_unref_page()`：pcp 释放入口 |
| `mm/page_alloc.c:3312` | `free_unref_page_prepare()` |
| `mm/page_alloc.c:3366` | `free_unref_page_commit()`：入 pcp，`count >= high` 时批量归还 buddy |
| `mm/page_alloc.c:1408` | `free_pcp_prepare()`：`DEBUG_VM=n` 时 `check_free=false` |
| `mm/page_alloc.c:1442` | `free_pcppages_bulk()`：批量归还 buddy |
| `mm/page_alloc.c:1416` | `bulkfree_pcp_prepare()` → `check_free_page()` |
| `mm/page_alloc.c:1203` | `check_free_page()` |
| `mm/page_alloc.c:1157` | `page_expected_state()`：`_mapcount`、`mapping`、`_refcount`、flags |
| `mm/page_alloc.c:621` | `bad_page()`：打印 + `add_taint()`，不是 `BUG()` |
| `mm/page_alloc.c:2388` | `post_alloc_hook()` |
| `mm/internal.h:92` | `set_page_refcounted()`：`set_page_count(page, 1)`，会把 -1 洗白 |
| `mm/page_alloc.c:6810` | `zone_highsize()`：pcp `high` 的来源 |
| `mm/page_alloc.c:1058` | `__free_one_page()`：pcp 批量归还 buddy 后真正进 buddy、合并伙伴 |

---

## 10. 一页纸

- 一次 `alloc_pages(order)` 对应一次 `__free_pages(page, order)`；
- 释放后必须让指针失效（`pool[i].page = NULL`），错误路径和正常路径不能都以为自己是 owner；
- pcp 页是“已释放但还没回 buddy”的页，**没有 `PG_buddy`**；
- 第二次 free 只把 `_refcount` 从 0 减到 -1，**不会重新入链表**；
- `CONFIG_DEBUG_VM=n` 时 `VM_BUG_ON_PAGE` 被编译掉，所以不当场报；
- 检查在 pcp 批量归还 buddy 时做：`free_pcppages_bulk()` → `bulkfree_pcp_prepare()` → `check_free_page()` → `bad_page()`；
- `bad_page()` 只 taint（`Tainted: B`），不 panic；`panic_on_warn=1` 管不到它，`panic_on_taint=0x20` 才会；
- `%p` 是哈希不是地址，`%px` 才是原始地址；`pfn` 始终真实；
- 没有警告 ≠ 没有 bug：一次及时分配就能把 -1 洗成 1；
- 先用 pfn 把“模块日志”和“内核报告”对上，再回源码找第二次 `__free_pages()`。
