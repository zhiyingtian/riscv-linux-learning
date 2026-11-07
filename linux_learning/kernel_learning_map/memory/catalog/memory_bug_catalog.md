# Linux 内存管理常见 Bug 清单与实验路线

> **内核**：`runninglinuxkernel_5.15`（RISC-V，`CONFIG_DEBUG_VM=n`、`CONFIG_SLUB_DEBUG=y`、无 KASAN/KFENCE）
> **目的**：先把内存管理里常见的 bug 形态列全，再按“可安全复现 → 需要调试内核 → 危险/需快照”的顺序做 demo。
> **注意**：不是所有 bug 都适合做成默认可跑的模块；double free、错误 order、非法 free、VMA UAF 这类可能直接把 guest 打挂，实验前先保存快照。

---

## 0. 图例

| 标记 | 含义 |
|---|---|
| ✅ 已有 demo | 目录里已有可运行代码和 README |
| 🧪 本批新增 | 这次新增的 lab |
| 📋 待做 | 已在清单里，代码还没写；按优先级排期 |
| ⚠️ 危险 | 可能破坏 guest，必须在快照里做 |

已有实验：

- [buddy_bug_lab](../labs/learn_page_alloc/buddy_bug_lab/)：物理页 double free
- [gfp_context_bug_lab](../labs/learn_page_alloc/gfp_context_bug_lab/)：GFP/原子上下文 bug
- [page_refcount_bug_lab](../labs/learn_page_alloc/page_refcount_bug_lab/)：页引用计数泄漏/多余 put_page（🧪）
- [slab_uaf_bug_lab](../labs/learn_page_alloc/slab_uaf_bug_lab/)：slab UAF/double free/越界写（🧪）
- [patch_exercise_01_vma_merge_uaf.md](../exercises/patch_exercise_01_vma_merge_uaf.md)：VMA merge UAF 真实补丁练习（✅ 文档）

---

## 1. 物理页分配器

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| P01 | double free | 同一个页 `__free_pages()` 两次 | pcp/buddy 损坏、bad page、后续分配崩溃 | `CONFIG_DEBUG_VM`、`bad_page`、`/proc/buddyinfo` | ✅ [buddy_bug_lab](../labs/learn_page_alloc/buddy_bug_lab/) |
| P02 | free 错 order | `alloc_pages(order=2)` 却 `__free_pages(page, 0)` | 页泄漏、buddy 链表错乱、静默损坏 | `CONFIG_DEBUG_VM`、page owner | ⚠️📋 待做 |
| P03 | free 非自己持有的页 | `__free_pages(virt_to_page(stack))`、free 已释放页 | bad page、panic | `CONFIG_DEBUG_VM`、`bad_page` | ⚠️📋 待做 |
| P04 | 页 UAF | 释放后继续用 `struct page *` 或页内容 | 读到新对象、改坏别人数据、延迟 crash | KASAN、`DEBUG_PAGEALLOC`、`PAGE_POISONING` | 🧪 [page_refcount mode=2](../labs/learn_page_alloc/page_refcount_bug_lab/) |
| P05 | refcount 泄漏 | 多 `get_page()`、少 `put_page()` | `MemFree` 慢慢下降，无 warning | `page_ref_count`、page owner | 🧪 [page_refcount mode=1](../labs/learn_page_alloc/page_refcount_bug_lab/) |
| P06 | refcount 下溢/提前释放 | 多 `put_page()`、直接改 `_refcount` | 页提前回 allocator，UAF | `CONFIG_DEBUG_VM`、`page_ref_count` | 🧪 [page_refcount mode=2](../labs/learn_page_alloc/page_refcount_bug_lab/) |
| P07 | 不检查分配失败 | `alloc_pages()` 返回 NULL 直接解引用 | NULL deref/oops | 代码审查、`should_fail_alloc_page()` | 📋 待做（简单） |
| P08 | order 超限 | `order >= MAX_ORDER` | WARN、返回 NULL、调用方没处理 | dmesg、`MAX_ORDER` | 📋 待做 |
| P09 | GFP 上下文错误 | 原子上下文用 `GFP_KERNEL` | `sleeping function called from invalid context` | `CONFIG_DEBUG_ATOMIC_SLEEP` | ✅ [gfp_context_bug_lab](../labs/learn_page_alloc/gfp_context_bug_lab/) |
| P10 | GFP flag 行为误用 | `__GFP_NORETRY`/`NOFS`/`ATOMIC` 语义理解错 | 睡眠、递归回收、保留内存耗尽 | `might_sleep`、`gfp_keypoints.md` | ✅ [gfp_keypoints.md](../labs/learn_page_alloc/gfp_context_bug_lab/gfp_keypoints.md) |
| P11 | pcp/buddy 状态损坏 | double free/错 order/并发 free | 分配失败、页重复分配、崩溃 | `bad_page`、`/proc/pagetypeinfo` | ✅ [buddy_bug_lab](../labs/learn_page_alloc/buddy_bug_lab/) |
| P12 | compound page 引用错误 | 只 put 头页/尾页，`__GFP_COMP` 使用不当 | 尾页 refcount 错、提前释放 | `CONFIG_DEBUG_VM`、`page_ref_count` | 📋 待做 |
| P13 | `split_page()` 误用 | split 后 head/tail 引用计数没理清 | 泄漏或重复释放 | `CONFIG_DEBUG_VM` | 📋 待做 |
| P14 | `alloc_pages_exact`/`free_pages_exact` 不配对 | free 的 size 与 alloc 不一致 | 释放错页/泄漏/损坏 | `CONFIG_DEBUG_VM` | ⚠️📋 待做 |
| P15 | 页泄漏 | 忘记 `__free_pages`/`put_page` | `MemFree` 下降，无 crash | page owner、`/proc/meminfo` | 🧪 [page_refcount](../labs/learn_page_alloc/page_refcount_bug_lab/) |
| P16 | highmem/kmap 不平衡 | 32-bit highmem 下 `kmap`/`kunmap` 不配对 | map count 泄漏、地址耗尽 | `CONFIG_DEBUG_KMAP`（若可用） | 📋 不适用当前 64-bit |
| P17 | page_pool/page_frag 生命周期 | 页归还 pool 后继续使用 | UAF、DMA 读到错误数据 | KASAN、驱动自检 | 📋 待做（网络方向） |

---

## 2. Slab 分配器

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| S01 | slab double free | 同一个对象 `kfree()` 两次 | freelist 损坏、同一对象分给两人 | `slub_debug=F`、KASAN | 🧪 [slab_uaf mode=2](../labs/learn_page_alloc/slab_uaf_bug_lab/) ⚠️ |
| S02 | slab UAF | `kfree()` 后继续读写旧指针 | 读到新对象、静默数据破坏 | KASAN、KFENCE、`slub_debug` | 🧪 [slab_uaf mode=1](../labs/learn_page_alloc/slab_uaf_bug_lab/) |
| S03 | slab 越界写/读 | `kmalloc(32)` 写 64 字节 | 破坏 redzone/相邻对象/元数据 | KASAN、`slub_debug=Z` | 🧪 [slab_uaf mode=3](../labs/learn_page_alloc/slab_uaf_bug_lab/) ⚠️ |
| S04 | kmem_cache double free | cache 对象 free 两次 | freelist 损坏、延迟 crash | `slub_debug`、KASAN | 📋 待做 |
| S05 | cache 销毁时还有对象 | `kmem_cache_destroy()` 时对象仍在使用 | UAF、cache 元数据损坏 | KASAN、`slabinfo` | ⚠️📋 待做 |
| S06 | kfree/vfree 混用 | `kfree(vmalloc_ptr)`、`vfree(kmalloc_ptr)` | 立即 BUG/panic | 代码审查、KASAN | ⚠️📋 待做 |
| S07 | krealloc 失败丢原指针 | `p = krealloc(p, ...)` 失败后 `p=NULL` | 内存泄漏 | 代码审查、kmemleak | 📋 待做 |
| S08 | 分配 size 溢出 | `count * size` 溢出后 `kmalloc` 太小 | 堆溢出、越界写 | KASAN、`check_mul_overflow` | 📋 待做 |
| S09 | slab 泄漏 | 忘记 `kfree` | `slabinfo` 对象数增长、内存下降 | kmemleak、`slabinfo` | 📋 待做 |
| S10 | ctor/dtor 生命周期错 | 对象 free 后仍调用方法/重复构造 | 状态损坏、UAF | KASAN、cache 自检 | 📋 待做 |

---

## 3. vmalloc / vmap / ioremap

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| V01 | vfree double free | `vfree()` 两次 | BUG/panic、vmap 区损坏 | KASAN、`CONFIG_DEBUG_VM` | ⚠️📋 待做 |
| V02 | vfree 非 vmalloc 指针 | 对 kmalloc/stack 指针 `vfree` | 立即 BUG | 代码审查 | ⚠️📋 待做 |
| V03 | vmap/vunmap 不平衡 | 只 map 不 unmap，或提前 unmap | vmalloc 地址泄漏、UAF | `/proc/vmallocinfo` | 📋 待做 |
| V04 | ioremap/iounmap 不平衡 | 驱动卸载漏 unmap | 地址空间泄漏、重复映射 | `/proc/vmallocinfo` | 📋 待做 |
| V05 | vmalloc UAF | `vfree()` 后继续访问 | page fault 或静默复用 | KASAN、`DEBUG_PAGEALLOC` | ⚠️📋 待做 |
| V06 | vmalloc 失败未检查 | `vmalloc()` 返回 NULL 直接写 | NULL deref | 代码审查 | 📋 待做 |
| V07 | vmap 页数/prot 错 | `vmap(pages, count)` 的 count 与实际不符 | 映射越界、权限错 | `CONFIG_DEBUG_VM`、`/proc/vmallocinfo` | ⚠️📋 待做 |

---

## 4. VMA / mmap / GUP

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| M01 | VMA UAF（merge） | merge 后继续用旧 VMA 指针 | 访问已释放 VMA、crash | KASAN、VMA 调试 | ✅ [patch_exercise_01](../exercises/patch_exercise_01_vma_merge_uaf.md) |
| M02 | vm_ops open/close 生命周期 | VMA 关闭后 private data 已释放 | UAF、mmap 回调踩空 | KASAN、`vm_ops` 审查 | 📋 待做 |
| M03 | 模块卸载时还有映射 | module 卸载但用户仍 mmap | UAF/panic | 引用计数、`vm_ops->close` | ⚠️📋 待做 |
| M04 | remap_pfn_range 参数错 | PFN/大小/权限不对 | 映射错页、越界、权限漏洞 | `CONFIG_DEBUG_VM`、用户态验证 | ⚠️📋 待做 |
| M05 | vm_insert_page 引用错 | 插入后提前 put/忘记 put | 页提前释放/泄漏 | `page_ref_count`、KASAN | 📋 待做 |
| M06 | GUP pin 泄漏 | `get_user_pages()` 后忘记 put | 页长期 pin 住、`MemFree` 下降 | `/proc/meminfo`、page owner | 📋 待做 |
| M07 | FOLL_LONGTERM 误用 | 长期 pin 可迁移页/未处理 COW | 数据不一致、迁移失败 | GUP 文档、pin 计数 | 📋 待做 |
| M08 | mmu_notifier 缺失 | 释放页前没 invalidate 二级映射 | 设备访问已释放页 | KASAN、设备 IOMMU | 📋 待做（驱动） |
| M09 | page cache 生命周期 | truncate 后继续用 page | UAF、读到旧数据 | KASAN、page lock | 📋 待做 |
| M10 | mmap 错误路径 | 失败后 VMA/文件引用未清理 | 泄漏、后续 UAF | KASAN、`vm_ops` 审查 | 📋 待做 |

---

## 5. DMA / IOMMU（驱动方向）

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| D01 | `dma_alloc_coherent`/free 不配对 | free 的 size/device 不对 | DMA 内存泄漏、损坏 | DMA API debug、KASAN | 📋 待做 |
| D02 | map/unmap 不平衡 | 只 map 不 unmap | IOMMU 映射泄漏、设备 fault | DMA debug、IOMMU trace | 📋 待做 |
| D03 | DMA 方向/栈内存错误 | 对栈变量做 streaming DMA | 数据损坏、cache 问题 | DMA API debug | 📋 待做 |
| D04 | dma_mmap 的 VMA 生命周期 | VMA 关闭后 DMA buffer 已释放 | UAF、设备写已释放内存 | KASAN、`vm_ops` | ⚠️📋 待做 |
| D05 | DMA mask/GFP_DMA 误用 | 设备寻址范围与分配 zone 不匹配 | 分配失败、设备读写错 | DMA mask 检查、`GFP_DMA` | 📋 待做 |

---

## 6. 用户拷贝 / 边界（安全方向）

| # | Bug | 典型机制 | 现象 | 调试/检测 | Demo |
|---|---|---|---|---|---|
| U01 | copy_from_user 越界 | 长度来自用户且未校验 | 内核堆/栈溢出 | KASAN、`HARDENED_USERCOPY` | 📋 待做 |
| U02 | size 乘法溢出 | `count * size` 溢出 | 分配过小、越界写 | KASAN、`check_mul_overflow` | 📋 待做 |
| U03 | 字符串缺少 NUL | `strncpy_from_user` 后直接 `%s` | 越界读、信息泄漏 | KASAN、代码审查 | 📋 待做 |
| U04 | put_user/get_user 错误处理 | 忽略 fault 返回值 | oops、错误地址暴露 | `access_ok`、代码审查 | 📋 待做 |

---

## 7. 本批新增 demo 怎么跑

### 7.1 页引用计数

```bash
cd linux_learning/kernel_learning_map/memory/labs/learn_page_alloc/page_refcount_bug_lab
make install
```

guest：

```bash
/mnt/learn_page_alloc/page_refcount_bug_lab/guest_run.sh
```

覆盖：P04/P05/P06/P15。默认跑 mode=1/2/3，安全，不会破坏页内容。

### 7.2 Slab UAF

```bash
cd linux_learning/kernel_learning_map/memory/labs/learn_page_alloc/slab_uaf_bug_lab
make install
```

guest：

```bash
/mnt/learn_page_alloc/slab_uaf_bug_lab/guest_run.sh
```

覆盖：S01/S02/S03。默认只跑 mode=1（UAF 别名）和 mode=4（正确对照）；mode=2/3 手动跑，先做快照。

### 7.3 已有实验

```bash
# 物理页 double free
/mnt/learn_page_alloc/buddy_bug_lab/guest_run.sh

# GFP/原子上下文
/mnt/learn_page_alloc/gfp_context_bug_lab/guest_run.sh
```

---

## 8. 建议学习顺序

1. **page_refcount_bug_lab**：先建立“引用计数 = 所有权”的直觉，代价最低。
2. **slab_uaf_bug_lab mode=1/4**：看 slab 对象复用，理解“free 后指针不会变 NULL”。
3. **buddy_bug_lab**：回到物理页，看 double free 如何破坏 pcp/buddy。
4. **gfp_context_bug_lab**：看“允许睡眠”的授权错误，和引用计数是两类问题。
5. **slab_uaf mode=2/3**：在快照里看 double free/越界写；最好加 `slub_debug=FZPU`。
6. **VMA UAF 补丁练习**：从真实补丁理解对象生命周期。
7. **KASAN/KFENCE debug 内核**：UAF/越界想稳定复现，最终要上 KASAN/KFENCE。
8. 再往驱动/DMA/IOMMU 方向扩展。

---

## 9. 为什么不能一次把所有 demo 都做成“默认可跑”

- double free、错 order、非法 free 会直接破坏 allocator，guest 可能卡死；
- UAF/越界写经常是静默的，没有 KASAN/KFENCE 时未必能稳定观察到；
- VMA/DMA 类 bug 需要用户态程序、设备或 IOMMU 配合，代码量和环境要求更高；
- 先做“安全、可重复、有明确预期输出”的 demo，再逐步加危险模式和调试内核。

所以这份清单是**完整索引**，代码按批次落地；每个 lab 都标注了风险和检测手段。

---

## 10. 一页纸

- 物理页：double free、错 order、非法 free、页 UAF、refcount 泄漏/下溢、未检查 NULL。
- Slab：double free、UAF、越界、cache 生命周期、size 溢出。
- vmalloc：vfree 双释放、vmap/vunmap 不平衡、UAF。
- VMA/GUP：merge UAF、vm_ops 生命周期、GUP pin 泄漏、remap 参数错。
- DMA：map/unmap 不平衡、方向/栈内存错、DMA mask 错。
- 用户拷贝：长度未校验、乘法溢出、缺少 NUL。
- 调试工具：`CONFIG_DEBUG_VM`、`DEBUG_PAGEALLOC`、`PAGE_POISONING`、`SLUB_DEBUG`、KASAN/KFENCE、page owner。
- 原则：先安全 demo，再危险模式；先快照，再实验；先检测工具，再猜现场。
