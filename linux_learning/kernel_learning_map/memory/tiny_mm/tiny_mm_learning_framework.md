# tiny-mm 学习框架（Review 版）

> 配套项目说明：[memory_capstone_tiny_mm.md](memory_capstone_tiny_mm.md)
> 核心概念底稿：[内存管理核心数据结构与地址转换.md](../notes/内存管理核心数据结构与地址转换.md)
> 目标：用一个小项目把物理页、页表、VMA、rmap、COW、slab、页缓存/回收串起来，而不是继续堆零散 demo。
>
> M0/M1 实验脚手架：[README.md](README.md)

---

## 0. 对原 6 个 milestone 的 Review

原来的 M1–M6 主干是对的，但作为“从物理页到虚拟内存再到回收”的完整项目，有几块关键知识被压缩或漏掉了：

| 原任务 | 遗漏/压缩 | 影响 | 处理 |
|---|---|---|---|
| M1 物理页 | 没有页表页、TLB、`pfn_valid` 边界、page flags；也没有 `pg_data_t`/node 概念 | 只会做页分配，不会做地址翻译，zone→node 关系缺失 | 加 pg_data，拆出 M3 |
| M2 buddy/pcp | 没有 watermarks、migratetype、fragmentation | 不懂“为什么分配会失败/碎片化” | 加进 M2 |
| M3 VMA/页表 | 只有匿名页和简单两级页表；没有 `mprotect/mremap`、`vm_ops`、`mmap_lock` | 不会处理真实 VMA 生命周期 | 扩展 M4 |
| M4 rmap/COW | 只做匿名 rmap；没有文件页 `i_mmap`、`mm_users/mm_count`、`exit_mmap` | 不理解共享页和 mm 销毁 | 扩展 M5 |
| M5 slab | 没有 page 类型/compound/tail page | 分不清 `PageSlab/PageBuddy/PageTable` | 扩展 M6 |
| M6 内存压力 | 只有“分配失败”；没有 LRU/kswapd/direct reclaim/swap/OOM/page cache | 不懂内存压力下系统如何活下来 | 拆出 M7（可选） |
| 全部 | 没有 GUP/pin、mmu_notifier、vmalloc/highmem/DMA | 驱动/KVM/网络方向会断层 | M8（可选） |
| 全部 | 没有 observability、计数器、最终验收 | 项目结束无法证明“我真的掌握了” | M9 |

所以框架从 6 个核心 milestone 扩展为：

```text
M0 工具链/测试
M1 物理地址模型
M2 buddy + pcp + 分配策略
M3 页表 + TLB
M4 VMA + 缺页
M5 rmap + COW + fork
M6 slab + page 类型
-------------------------- 核心闭环
M7 page cache + 回收/swap/OOM    （可选进阶）
M8 GUP/pin + vmalloc/DMA         （可选进阶）
M9 综合 capstone + observability （最终验收）
```

---

## 1. 总体学习框架

| 阶段 | 主题 | 核心问题 | 建议时间 | 必须通过 |
|---|---|---|---|---|
| M0 | 工具链 | 怎么编译、跑测试、抓内存错误？ | 0.5 天 | ASan 能抓一次越界；`make test` 可跑 |
| M1 | 物理地址模型 | page/PFN/paddr/zone 怎么互转？ | 2–3 天 | 分配/释放/双释放/错 order 测试 |
| M2 | buddy/pcp | 多 order 怎么分配、合并、缓存？ | 3–5 天 | 10 万次随机压力；碎片/水位测试 |
| M3 | 页表/TLB | vaddr 怎么翻译成 pfn？页表页谁分配？ | 3–4 天 | 多级 walk、缺页建表、TLB 失效 |
| M4 | VMA/缺页 | 一段 vaddr 的语义是谁定义的？ | 4–5 天 | mmap/munmap/mprotect、匿名缺页、VMA 合并 |
| M5 | rmap/COW | 一个 page 被多个 VMA 映射怎么办？ | 4–5 天 | rmap 遍历、fork、COW、refcount/mapcount |
| M6 | slab/page 类型 | 页怎么变成对象？复合页怎么管？ | 3–4 天 | slab UAF/double free、compound/tail |
| M7 | 回收/页缓存 | 内存不够时谁被回收？ | 5–7 天 | LRU、kswapd/direct reclaim、swap/OOM 模拟 |
| M8 | GUP/DMA/vmalloc | 内核怎么 pin 用户页/映射设备内存？ | 4–6 天 | pin/unpin、mmu_notifier、vmalloc/DMA |
| M9 | 综合 | 全链路能不能跑通并解释？ | 3–4 天 | 综合 workload + 闭卷讲解 |

> 只做核心闭环：M0–M6，约 4–6 周（每天 2h + 周末）。
> 加进阶：M7/M8，约 8–10 周。
> 不建议一上来做 M7；先把 M0–M6 的“物理页→页表→VMA→rmap”跑通。

---

## 2. 每个阶段的知识点与验收

### M0：工具链与测试框架（0.5 天）

**知识点**

- `PAGE_SHIFT/PAGE_SIZE`、`phys_addr_t`、`pfn` 类型；
- C 结构体布局、指针与数组索引；
- `-Wall -Wextra -Werror`、ASan/UBSan；
- 断言/测试框架、失败信息、随机压力测试；
- “先写失败测试，再写实现”的节奏。

**实现**

```text
tiny_mm/
├── Makefile
├── include/tmm.h
├── src/pmm.c
├── tests/test_pmm.c
└── tests/test_runner.h
```

**验收**

- `make` 通过；
- `make test` 能执行测试并报告 PASS/FAIL；
- 故意写一个 `int a[1]; a[1]=1;`，ASan 必须报错；
- 删掉一个断言，测试必须失败（证明测试有效）。

---

### M1：物理地址模型（2–3 天）

**知识点**

- `struct page` 是物理页元数据，不是页内容；
- `page_to_pfn/pfn_to_page`；
- `PFN_PHYS/PHYS_PFN`、`PFN_DOWN/UP/ALIGN`；
- `pfn_valid` 与内存空洞；
- `struct zone` 的 `zone_start_pfn/spanned_pages/free_count`；
- `struct pglist_data`（单 node 简化版）、`node_zones[]`、`zone->zone_pgdat`、`page_pgdat`、`page_to_nid`；
- page refcount 初值 1，free 后回 0；
- double free、free 错 order、分配失败、泄漏计数。

**实现**

- `pmm_init(start_pfn, nr_pages)` 建 page 数组 + bitmap；
- `pmm_alloc_pages(order)`：M1 只支持 order=0，其他返回 NULL；
- `pmm_free_pages(page, order)`：检查 `page->is_free`、`page->order`；
- `page_to_pfn/pfn_to_page`、`pfn_to_phys/phys_to_pfn`；
- 单 node 的 `struct pglist_data`，把 zone 挂到 `node_zones[0]`；
- `page_pgdat`/`page_to_nid`；
- `pmm_free_count()`、`pmm_check()`。

**验收测试**

1. 分配后 `refcount == 1`、`is_free == 0`；
2. `page_to_pfn(pfn_to_page(pfn)) == pfn`；
3. `phys_to_pfn(pfn_to_phys(pfn)) == pfn`；
4. 分配全部页后下一次分配返回 NULL；
5. 全部释放后 `free_count == 初始值`；
6. double free 返回错误，且 `free_count` 不重复增加；
7. free 错 order 返回错误，不改状态；
8. `pfn_to_page` 对越界 pfn 返回 NULL；
9. `page_pgdat(page)->node_zones[0]` 与 `page_zone(page)` 一致，`zone->zone_pgdat` 回指 node；
10. 随机压力：order-0 分配/释放 10 万次，最终计数归零；
11. ASan/UBSan 无报错。

**对应 Linux**

- `struct page`：`include/linux/mm_types.h:70`
- `page_to_pfn/pfn_to_page`：`include/asm-generic/memory_model.h:18`
- `PFN_PHYS/PHYS_PFN`：`include/linux/pfn.h:21`
- `pfn_valid`：`include/linux/mmzone.h:1469`
- `page_zone`：`include/linux/mm.h:1539`
- `page_pgdat`：`include/linux/mm.h:1544`
- `struct pglist_data`：`include/linux/mmzone.h:800`
- `zone->zone_pgdat`：`include/linux/mmzone.h:513`
- `zone` 范围：`include/linux/mmzone.h:532`

**主动注入的 bug**

- free 时不检查 `is_free` → double free 测试应失败；
- free 时用错 order → 错 order 测试应失败；
- 分配时忘记 `refcount=1` → 引用计数测试失败；
- 释放时忘记 `free_count++` → 泄漏测试失败。

---

### M2：buddy + pcp + 分配策略（3–5 天）

**知识点**

- `free_area[order]`、buddy 的 split/merge；
- buddy pfn 计算：`buddy = pfn ^ (1 << order)`；
- pcp（per-CPU page list）批量缓存；
- `zone` watermarks：min/low/high；
- migratetype（UNMOVABLE/RECLAIMABLE/MOVABLE）与 pageblock；
- 碎片化：为什么高阶分配会失败；
- GFP flag 的简化模拟：`GFP_KERNEL/GFP_ATOMIC/__GFP_ZERO/__GFP_MOVABLE`。

**实现**

- 替换 M1 的 bitmap 为 buddy；
- `pmm_alloc_pages(order)` 支持 0..MAX_ORDER-1；
- split 到目标 order，free 时尝试合并；
- 简化 pcp：order-0 批量缓存；
- 统计：`free_pages[order]`、`alloc_fail`、`pcp_hit/pcp_drain`。

**验收**

1. 随机 alloc/free 10 万次，所有 order，最终空闲计数/最大块恢复；
2. 分配 order=2 后 free order=0 → 返回错误（或断言）；
3. 分配 order=2 后 free order=2 → 正确合并；
4. 构造“交替分配”制造碎片，验证高阶分配失败并返回 NULL；
5. pcp 计数：批量分配/释放后 `pcp_hit+pcp_miss+pcp_drain` 自洽；
6. 水位模拟：低于 min 时分配失败/触发回收标记；
7. 全部释放后 `pmm_check()` 通过。

**对应 Linux**

- `free_area`：`include/linux/mmzone.h:97`
- `zone->free_area[]`：`include/linux/mmzone.h:608`
- `per_cpu_pages`：`include/linux/mmzone.h:360`
- `__alloc_pages()` / `free_pages()`：`mm/page_alloc.c`
- GFP flags：`include/linux/gfp.h`

**主动注入的 bug**

- buddy 计算写成 `pfn + (1<<order)` → 合并错误；
- free 时不检查 order → freelist 损坏；
- pcp 下刷时漏页 → 泄漏；
- 分配失败不返回 NULL → 崩溃。

---

### M3：页表 + TLB（3–4 天）

**知识点**

- `pgd → p4d → pud → pmd → pte`；可以先做两级，再做四级；
- `pte_t`：PFN + present/write/user 位；
- `pte_pfn/pfn_pte/pte_page`；
- 页表页本身也是物理页，需要 alloc/free，并计入 `PageTable`；
- `mm->pgd`、`pgd_offset`、`pte_offset_map`；
- PTE 修改：`set_pte_at`、`ptep_modify_prot`；
- TLB 与 PTE 修改：本地 `flush_tlb_*`、跨 CPU TLB shootdown；
- `vm_normal_page` 与 special/PFNMAP/zero page 的区别。

**实现**

- 两级页表：PGD 指向 PTE 页；
- `walk(mm, vaddr, create)`：按需分配页表页；
- `map_page(mm, vaddr, page, prot)` / `unmap_page`；
- `translate(mm, vaddr)` 返回 page；
- `tlb_flush(vaddr/range)` 计数模拟。

**验收**

1. `map_page` 后 `translate` 返回同一个 page；
2. 未映射地址 `translate` 返回 NULL；
3. 两级 walk 的中间页表页从 pmm 分配，释放 mm 时归还；
4. 改写 PTE 后 TLB flush 计数增加；
5. 访问权限：只读页写访问返回错误；
6. 随机 map/unmap 10 万次，页表页无泄漏；
7. ASan 无越界。

**对应 Linux**

- `pgd_offset`：`include/linux/pgtable.h:122`
- `pte_offset_map`：`include/linux/pgtable.h:96`
- `pte_pfn/pte_page/pfn_pte`：`arch/riscv/include/asm/pgtable.h:252`
- `handle_mm_fault`：`mm/memory.c:4773`
- `vm_normal_page`：`mm/memory.c:605`

**主动注入的 bug**

- PTE 存 page 指针而不是 PFN → 结构错；
- 忘记分配中间页表页 → NULL 解引用；
- 修改 PTE 不 flush TLB → 读到旧映射；
- 释放 mm 时忘记释放页表页 → 泄漏。

---

### M4：VMA + 缺页（4–5 天）

**知识点**

- `mm_struct`：VMA 链表/树、`mm_users/mm_count`、`mmap_lock`；
- VMA 字段：`vm_start/vm_end/vm_flags/vm_pgoff/vm_file/vm_ops`；
- VMA 的链表 + 红黑树两种索引，以及 `vmacache` 的概念；
- `find_vma` 语义：第一个 `vm_end > addr`，不保证包含 addr；
- `mmap/munmap/mprotect/mremap`，以及 `mmap` 的地址对齐/溢出检查；
- 部分 `munmap` 的 VMA 拆分；VMA 合并的条件（地址相邻、flags/file/pgoff 兼容）；
- 缺页类型：读/写/执行、权限不足、栈增长 `VM_GROWSDOWN`、SIGSEGV/SIGBUS；
- `mmap_lock` 读写锁与锁顺序；
- `vm_ops->fault/open/close`（可先只做匿名）。

**实现**

- `mm_create/mm_destroy`；
- VMA 有序链表 + `find_vma`；
- `mm_mmap` 建立 VMA；
- `mm_access` 查 VMA → walk PTE → 缺页分配；
- `mm_munmap` 清 PTE、put_page、删 VMA；
- `mm_mprotect` 改权限；
- `mm_remap`（可选）。

**验收**

1. `mmap` 后首次访问缺页，PTE 建立；
2. 访问未映射地址返回错误；
3. 只读 VMA 写访问返回错误；
4. `munmap` 后 VMA 消失、PTE 清空、page refcount 归还；
5. 两个 VMA 相邻，`find_vma` 返回正确；
6. VMA 合并：两个可合并 VMA 合并后 `find_vma` 仍正确；
7. `mprotect` 后权限生效；
8. `mm_destroy` 后所有页/页表页回收；
9. 随机 mmap/munmap/access 压力测试。

**对应 Linux**

- `struct mm_struct`：`include/linux/mm_types.h:402`
- `struct vm_area_struct`：`include/linux/mm_types.h:319`
- `find_vma`：`mm/mmap.c:2273`
- `vma_lookup`：`include/linux/mm.h:2713`
- `handle_mm_fault`：`mm/memory.c:4773`
- `do_anonymous_page`：`mm/memory.c:3715`

**主动注入的 bug**

- `find_vma` 只检查 `vm_end` 不检查 `vm_start` → 空洞误命中；
- `munmap` 忘记 put_page → 泄漏；
- `mmap` 重叠不检查 → VMA 链表损坏；
- `mprotect` 不检查权限 → 越权访问。

---

### M5：rmap + COW + fork（4–5 天）

**知识点**

- `_refcount`（总引用） vs `_mapcount`（用户 PTE 映射数）；
- `page->mapping`：文件页 `address_space`，匿名页 `anon_vma` + 标志位；
- `page->index` 与 `vm_pgoff` 反算 vaddr；
- 匿名 rmap：`anon_vma`/`anon_vma_chain` 区间树；
- 文件 rmap：`address_space->i_mmap` 区间树；
- `rmap_walk`、`page_vma_mapped_walk`；
- `fork`/`dup_mmap`、COW、`do_wp_page`；
- `mm_users/mm_count` 与 mm 销毁。

**实现**

- page 上记录 `mapping/index`；
- 每个 VMA 一个简化 `anon_vma` 映射链表；
- `rmap_walk(page)` 遍历所有 `(vma, addr)`；
- `mm_fork` 复制 VMA/PTE，页表只读、page refcount++；
- 写缺页触发 COW；
- `mm_destroy` 逐 VMA 释放页。

**验收**

1. 一个 page 被两个 VMA 映射，`rmap_walk` 找到两个；
2. `page + VMA → vaddr` 与 `vma_address` 公式一致；
3. `fork` 后共享页 `refcount=2`；
4. 子进程写触发 COW，新旧页内容独立；
5. `munmap` 一个映射后 page 仍被另一个映射，不释放；
6. 最后一个映射消失后 page 才回 buddy；
7. `page->_mapcount` 与实际 PTE 数一致；
8. `mm_users/mm_count` 生命周期正确。

**对应 Linux**

- `vma_address`：`mm/internal.h:389`
- `page_address_in_vma`：`mm/rmap.c:709`
- `rmap_walk`：`mm/rmap.c:2363`
- `anon_vma`：`include/linux/rmap.h:29`
- `do_wp_page`：`mm/memory.c`
- `page_add_anon_rmap`：`mm/rmap.c`

**主动注入的 bug**

- 少一次 `get_page` → COW 后提前释放；
- 多一次 `put_page` → 页被复用；
- `page->index` 用 vaddr 代替 → rmap 反算地址错；
- `mm_users` 减错 → mm 提前释放；
- rmap 遍历不加锁 → 并发损坏。

---

### M6：slab + page 类型（3–4 天）

**知识点**

- slab 页：`PageSlab`、`slab_cache`、freelist；
- 对象大小/对齐/ctor/dtor；
- `kmalloc/kfree`、`kmem_cache`、per-CPU slab 缓存；
- slab 的 UAF/double free/越界；
- compound page、tail page、`compound_head`、`PageTail`；
- `PageBuddy`/`PageTable`/`PageSlab` 区别；
- 页表页/compound 页的 refcount 规则。

**实现**

- 用 buddy 分配 slab page；
- 固定 size cache（8/16/32/64/128）；
- `kmalloc/kfree`；
- 可选 redzone/poison；
- 识别 `PageSlab` 并拒绝 `pmm_free_pages` 直接释放。

**验收**

1. `kmalloc` 对齐、`kfree` 后可复用；
2. slab page 用完后归还 buddy；
3. UAF 写旧指针被 redzone/poison 检测；
4. double free 检测；
5. 越界写检测；
6. 随机 10 万次 alloc/free 无泄漏；
7. compound page 的 head/tail 换算正确；
8. `PageSlab/PageTable/PageBuddy` 状态不混。

**对应 Linux**

- `mm/slub.c`
- `include/linux/slab.h`
- `include/linux/page-flags.h`
- `include/linux/mm_types.h`（compound tail）

**主动注入的 bug**

- `kfree` 不检查对象状态 → double free；
- slab page 归还 buddy 时还有对象 → UAF；
- compound tail 当 head 用 → refcount 错；
- 越界写破坏下一个对象。

---

### M7：page cache + 回收/swap/OOM（可选，5–7 天）

**知识点**

- `address_space`、`xarray/i_pages`、`page->index`；
- 文件映射 VMA、`vm_ops->fault`、readahead；
- dirty page / writeback / truncate / `msync`；
- LRU（active/inactive）、kswapd、direct reclaim；
- watermarks、`GFP_KERNEL/GFP_ATOMIC` 在回收中的区别；
- swap entry、swap cache；
- OOM killer 简化模拟；
- overcommit。

**实现**

- 文件页对象 + page cache 查找/插入/删除；
- 文件 VMA 缺页从 page cache 取页；
- LRU 链表 + 简化回收；
- 内存压力下触发 reclaim/swap；
- 统计 `pgscan/pgsteal/allocstall`。

**验收**

1. 同一文件页被两个 mm 映射，只有一份 page；
2. page cache 命中不重复分配；
3. 内存压力下回收 clean page，保留 dirty/被引用页；
4. 回收后 rmap 正确清理 PTE；
5. swap 简化版：换出/换入后内容一致；
6. OOM 选择最大 mm（简化）并回收。

**对应 Linux**

- `mm/filemap.c`、`mm/readahead.c`
- `mm/vmscan.c`、`mm/swap.c`、`mm/swapfile.c`
- `mm/oom_kill.c`

**主动注入的 bug**

- page cache 中 page 被提前释放；
- 回收时忘记清 PTE/TLB；
- dirty page 被错误回收；
- swap entry 与 page 映射错。

---

### M8：GUP/pin + vmalloc/DMA（可选，4–6 天）

**知识点**

- `get_user_pages`/`pin_user_pages`、FOLL_GET/FOLL_PIN/FOLL_LONGTERM；
- `follow_page`/`follow_page_pte` 与 `vm_normal_page`；
- pin 计数与 `_refcount` 的关系；
- `mmu_notifier`：释放页前 invalidate 二级映射；
- `vmalloc/vmap/ioremap`；
- highmem/kmap（可只做概念）；
- DMA zone/coherent/streaming（可选）。

**验收**

1. pin 后 page refcount 增加，unpin 后恢复；
2. pin 的页不能迁移/回收（简化）；
3. mm 销毁时 mmu_notifier 清理设备映射；
4. vmalloc 区映射/释放无泄漏；
5. DMA zone 分配限制正确。

---

### M9：综合 capstone + observability（3–4 天）

**知识点**

- 多 mm、多 VMA、fork/COW、page cache、回收的组合；
- 计数器/统计：`free_pages`、`pgfault`、`pgalloc`、`pgfree`、`rmap_walk_count`；
- 调试：日志、断言、ASan、失败注入；
- Linux 观测工具：`/proc/meminfo`、`/proc/buddyinfo`、`/proc/pagetypeinfo`、`/proc/vmstat`、`/proc/slabinfo`、`page_owner`、`tracepoints`；
- 性能：页表 walk 次数、rmap 遍历次数；
- 闭卷讲清 9 个转换问题。

**最终验收**

```text
1. 创建 3 个 mm；
2. 每个 mm 建 100 个 VMA；
3. 随机访问/写触发缺页；
4. fork 一个 mm，制造 COW；
5. munmap 一部分；
6. 释放所有 mm；
7. 检查：
   - 所有 page refcount=0；
   - buddy 空闲页数回到初始；
   - 页表页全部归还；
   - slab 无泄漏；
   - ASan/UBSan 无报错；
8. 输出统计：缺页数、rmap 遍历数、COW 次数、回收次数。
```

---

## 3. 如何验收才不是“自我感觉良好”

每个阶段都必须有**可执行的验收**，不能只看代码：

1. **测试通过**：`make test` 全绿；
2. **注入 bug 测试**：故意删掉一个检查，测试必须失败；
3. **随机压力**：10 万次 alloc/free/map/unmap；
4. **不变量检查**：`pmm_check()`、`mm_check()`、`slab_check()`；
5. **统计自洽**：alloc/free、map/unmap、refcount 计数对得上；
6. **ASan/UBSan**：无越界/泄漏；
7. **闭卷讲解**：不看笔记画出当前阶段链路。

建议每个阶段结束写一个 `STAGE_REPORT.md`：

```text
我实现了什么：
我踩了什么坑：
测试怎么抓住的：
和 Linux 源码哪一段对应：
还没懂的问题：
```

---

## 4. 建议节奏

### 方案 A：核心闭环（推荐先做）

```text
第 1 周：M0 + M1
第 2 周：M2
第 3 周：M3
第 4 周：M4
第 5 周：M5
第 6 周：M6 + 综合小验收
```

每天 2h，周末 4–6h；如果只有周末，时间 ×2。

### 方案 B：完整版

```text
M0–M6（6 周）
→ M7（2 周）
→ M8（1–2 周）
→ M9（1 周）
```

不要为了“做完”跳过 M5 的 COW/rmap；那是整个项目最有价值的部分。

---

## 5. 和已有资料的对应

| 阶段 | 先读 | 做完后对照 |
|---|---|---|
| M0 | 本文件 | `memory_capstone_tiny_mm.md` |
| M1 | [内存管理核心数据结构与地址转换.md](../notes/内存管理核心数据结构与地址转换.md) 第 3.4–3.6、4.5–4.8 | [page_refcount_bug_lab](../labs/learn_page_alloc/page_refcount_bug_lab/) |
| M2 | `gfp_keypoints.md`、`buddy_bug_lab` | `/proc/buddyinfo`、`/proc/pagetypeinfo` |
| M3 | 第 3.3、4.4、4.7 | [gfp_context_bug_lab](../labs/learn_page_alloc/gfp_context_bug_lab/)、页表 walk 实验 |
| M4 | 第 3.1–3.2、4.1、4.4 | [learn_mmap](../labs/learn_mmap/) |
| M5 | 第 3.4、4.2–4.3 | [patch_exercise_01_vma_merge_uaf.md](../exercises/patch_exercise_01_vma_merge_uaf.md) |
| M6 | [slab_uaf_bug_lab](../labs/learn_page_alloc/slab_uaf_bug_lab/)、[memory_bug_catalog.md](../catalog/memory_bug_catalog.md) | `/proc/slabinfo` |
| M7 | [memory_half_year_roadmap.md](../plans/memory_half_year_roadmap.md) 的 page cache/reclaim 阶段 | `/proc/vmstat`、`/proc/meminfo` |
| M8 | GUP/DMA 文档（后续补） | `pin_user_pages` 实验 |
| M9 | 全部 | 闭卷画图 |

---

## 6. 明确不做的部分（避免项目失控）

第一版 tiny-mm 不强求：

- 真正的多核并发/锁竞争（先单线程，后期加简单锁）；
- NUMA 真实拓扑（只做单 node）；
- memcg/cgroup 记账；
- 内存热插拔；
- THP 拆分/合并；
- KSM；
- 真实 swap 设备；
- 真实硬件 MMU/TLB（用户态模拟）。

这些放到 M7/M8 或后续项目；先把核心闭环跑通。

---

## 7. 一句话总结

> **M0–M2 让你理解“物理页怎么来”，M3–M4 让你理解“虚拟地址怎么变成页”，M5 让你理解“一个页怎么被多人共享”，M6 让你理解“页怎么变成对象”，M7–M8 让你理解“内存不够/设备要 pin 时怎么办”，M9 把它们串成一条完整链路。**
