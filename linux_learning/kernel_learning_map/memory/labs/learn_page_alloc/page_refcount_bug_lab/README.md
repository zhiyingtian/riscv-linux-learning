# 物理页引用计数 Bug 实验：泄漏与多余 put_page

> **内核**：`runninglinuxkernel_5.15`（RISC-V，`CONFIG_DEBUG_VM=n`）
> **目标**：从 `refcount` 角度观察两种常见物理页 bug：
> 1. 多拿引用、少放引用 → 页泄漏；
> 2. 多放引用 → 页提前释放，后续访问 UAF。
> **配套**：`buddy_bug_lab`（double free）、`gfp_context_bug_lab`（GFP/上下文）

---

## 0. 一句话

`alloc_pages()` 返回的页初始 `refcount = 1`；`get_page()` 加一，`put_page()` / `__free_pages()` 减一，减到 0 才真正归还给分配器。

```c
page = alloc_pages(GFP_KERNEL, 0);   /* refcount = 1 */
get_page(page);                      /* refcount = 2 */

__free_pages(page, 0);               /* 2 -> 1，页没有释放 */
/* 少一次 put_page => 页泄漏 */
```

或者：

```c
page = alloc_pages(GFP_KERNEL, 0);   /* refcount = 1 */
get_page(page);                      /* refcount = 2 */
put_page(page);                      /* 2 -> 1 */
put_page(page);                      /* 1 -> 0，页被释放 */
/* 调用者只拥有一个引用，却 put 两次 => 提前释放 */
```

第二种在释放后继续用 `page` 指针或页内容，就是 use-after-free。

---

## 1. 三种模式

| mode | 行为 | 预期 |
|---|---|---|
| `1` | `get_page` 多一次，`__free_pages` 只释放一次 | 打印 `refcount=1`，页泄漏 |
| `2` | `get_page` 一次，`put_page` 两次 | 第二次 `put_page` 后页被释放 |
| `3` | 一次 `alloc_pages` + 一次 `__free_pages` | 正确释放 |

`mode=1` 在打印完之后会补一次 `put_page` 清理，避免 demo 真的把页永久泄漏；真实驱动通常没有这个清理。

---

## 2. 快速开始

```bash
cd linux_learning/kernel_learning_map/memory/labs/learn_page_alloc/page_refcount_bug_lab
make install
```

guest 里以 root 运行：

```bash
/mnt/learn_page_alloc/page_refcount_bug_lab/guest_run.sh
```

手动跑单个模式：

```bash
dmesg -C
insmod page_refcount.ko mode=1
rmmod page_refcount
dmesg | tail -30
```

---

## 3. 预期 dmesg

### 3.1 mode=1：引用计数泄漏

```text
ref_demo: mode=1
ref_demo: after alloc_pages: pfn=... refcount=1
ref_demo: after get_page: pfn=... refcount=2
ref_demo: after __free_pages (refcount should be 1 => leaked): pfn=... refcount=1
ref_demo: done
ref_demo: unloaded
```

关键点：

- `__free_pages()` 不是“无条件把页还给 buddy”；
- 它先做 `put_page_testzero()`：只有 refcount 减到 0 才真正 `free_the_page()`；
- 这里 refcount 是 2，减到 1，所以页仍然被占着；
- 页既不在 buddy，也不在 pcp，也没有任何正常路径会再释放它——这就是泄漏。

### 3.2 mode=2：多余的 put_page

```text
ref_demo: mode=2
ref_demo: after alloc_pages: pfn=... refcount=1
ref_demo: after get_page: pfn=... refcount=2
ref_demo: after first put_page: pfn=... refcount=1
ref_demo: after second put_page: page has been freed
ref_demo: 继续使用 demo_page 指向的内容就是 use-after-free
ref_demo: done
ref_demo: unloaded
```

关键点：

- 第二次 `put_page()` 把 refcount 减到 0，页被释放；
- 如果代码里还有别的指针指向这个 `struct page` 或页内容，它们现在都是悬空的；
- 下一次分配可能立刻复用这个 pfn，旧指针会悄悄指向新对象。

### 3.3 mode=3：正确释放

```text
ref_demo: mode=3
ref_demo: after alloc_pages: pfn=... refcount=1
ref_demo: correct: one alloc + one free
ref_demo: done
ref_demo: unloaded
```

---

## 4. 为什么 refcount bug 很危险

### 4.1 泄漏版

- 单次泄漏可能只有 4 KiB，但循环里泄漏会累积；
- 页不在 buddy/pcp，`/proc/meminfo` 的 `MemFree` 会下降，但 `buddyinfo` 未必能解释；
- 没有 warning、没有 crash，只有“内存慢慢变少”；
- 长期运行后 OOM，现场已经离 bug 很远。

### 4.2 提前释放版

- 释放后页可能被别的路径重新分配；
- 旧指针读写会改到别人的数据（silent corruption）；
- 如果页被还给 buddy 并被拆分/合并，`struct page` 的 `private`、`PageBuddy` 等字段都会变；
- 继续调用 `put_page()` 还可能让 refcount 下溢，触发 `bad_page` 或 use-after-free。

当前内核 `CONFIG_DEBUG_VM=n`，所以很多 `VM_BUG_ON_PAGE()` 不会执行；不要因为“没看到 BUG”就认为引用计数正确。

---

## 5. 怎么调试这类 bug

### 5.1 看 refcount

本 demo 用 `page_ref_count(page)` 打印。真实驱动可以在关键路径临时打印：

```c
pr_info("pfn=%lx refcount=%d\n",
        page_to_pfn(page), page_ref_count(page));
```

注意 `%p` 打印的是哈希后的地址，看 pfn 更可靠；需要真实地址时用 `%px` 或 `no_hash_pointers`。

### 5.2 打开更强的检查

| 配置 | 作用 |
|---|---|
| `CONFIG_DEBUG_VM=y` | 启用 `VM_BUG_ON_PAGE` 等页级检查 |
| `CONFIG_DEBUG_PAGEALLOC=y` | 释放后取消映射，UAF 更容易立刻 fault |
| `CONFIG_PAGE_POISONING=y` | 释放后填充 poison，UAF 读到 0xaa |
| `CONFIG_PAGE_OWNER=y` | 记录页的分配/释放调用栈（`page_owner=on`） |
| `CONFIG_KASAN=y` | slab/page UAF、越界检查（需要重新编译内核） |

当前 `runninglinuxkernel_5.15` 这些基本都没开；想深入体验，建议单独编译一个 debug 内核，而不是改正在用的实验内核。

### 5.3 看 `/proc` 信息

```bash
cat /proc/meminfo | grep -E 'MemFree|MemAvailable'
cat /proc/buddyinfo
cat /proc/pagetypeinfo
```

泄漏时 `MemFree` 下降但 `buddyinfo` 可能不变（页在 pcp 或被人拿着）；提前释放时这些全局计数未必能直接告诉你哪个页错了，需要配合 pfn/调用栈。

---

## 6. 正确做法

1. **谁分配谁释放**：`alloc_pages()` 的引用由分配者负责 `__free_pages()` / `put_page()`。
2. **`get_page()` 必须有配对的 `put_page()`**，通常只在“明确多一个所有者”时使用。
3. **释放后把指针置 NULL**，避免 stale pointer 被再次 put 或访问。
4. **不要直接改 `page->_refcount`**；用 `get_page()` / `put_page()` / `page_ref_*()` API。
5. **共享页用引用计数表达所有权**，而不是靠“我记得这里还有一个用户”。
6. 对可能被并发释放的页，配合锁、`rcu`、`refcount_t` 或上层对象生命周期一起设计。

---

## 7. 思考题

**Q1.** 为什么 `__free_pages()` 在 refcount 为 2 时不会把页还给 buddy？

**Q2.** mode=1 里页泄漏后，`/proc/buddyinfo` 会不会显示这块页？为什么？

**Q3.** mode=2 第二次 `put_page()` 后，页可能去了哪里？pcp 还是 buddy？

**Q4.** 如果第二次 `put_page()` 后马上 `alloc_pages(GFP_KERNEL, 0)`，旧 pfn 有没有可能被复用？这会导致什么？

**Q5.** 为什么“引用计数下溢”比“泄漏”更难调试？

---

## 8. 源码锚点（5.15）

| 位置 | 内容 |
|---|---|
| `mm/page_alloc.c:5451` | `__free_pages()` 的 `put_page_testzero()` 逻辑 |
| `include/linux/mm.h:745` | `put_page_testzero()` |
| `include/linux/page_ref.h` | `page_ref_count()`、`get_page()`、`put_page()` |
| `mm/page_alloc.c:1408` | `free_pcp_prepare()`（页进入 pcp 的路径） |
| `mm/page_alloc.c:1203` | `check_free_page()`（`CONFIG_DEBUG_VM` 相关检查） |

---

## 9. 一页纸

- 页初始 refcount=1；`get_page()` +1，`put_page()`/`__free_pages()` -1，到 0 才释放。
- 多 `get_page()` 少 `put_page()` → 泄漏；多 `put_page()` → 提前释放/UAF。
- 泄漏没有 warning，只有内存慢慢变少；提前释放可能悄悄改坏别人的数据。
- 释放后置空指针，按所有权配对引用，不要手改 `_refcount`。
- `CONFIG_DEBUG_VM` / `DEBUG_PAGEALLOC` / `PAGE_POISONING` / KASAN 能把隐藏问题放大。
