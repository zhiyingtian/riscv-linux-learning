# 内存管理 Capstone 项目：tiny-mm

> 目标：不要再做零散 demo，而是用**一个自己写的小型内存管理器**，把 `mm_struct → VMA → 页表 → PTE → page/PFN → zone/pg_data → rmap` 这条链路从头实现一遍。
>
> 本文是最初的 6-milestone 方案；review 后的 9 阶段版本见：[tiny_mm_learning_framework.md](tiny_mm_learning_framework.md)
>
> M0/M1 实验脚手架：[README.md](../README.md)
>
> 定位：用户态 C 项目，不是内核模块。原因：可以安全地注入 double free/UAF/错 order，能用 ASan/UBSan 抓问题，能自动跑测试；结构命名和 Linux 5.15 对齐，学完再回看内核源码。

---

## 0. 先选项目：三种经典路线

| 项目 | 覆盖范围 | 优点 | 局限 |
|---|---|---|---|
| **CS:APP Malloc Lab** | 堆分配器：free list、split/coalesce、对齐、碎片 | 经典、代码量小、测试强 | 不覆盖 VMA/页表/物理页/rmap |
| **xv6-riscv labs** | `kalloc`、页表、`mmap`、COW、缺页 | 真实 OS、RISC-V、有课程测试 | 结构简化，没有 Linux 的 zone/NUMA/rmap 体系 |
| **tiny-mm（本文推荐）** | 物理页、buddy/pcp、zone、VMA、页表、缺页、rmap、COW、slab | 直接对应你学的 Linux 结构和 9 个转换问题 | 需要自己写测试/脚手架，工作量大于单个 lab |

**建议**：

- 想体验“经典 OS 课程项目”→ 做 **xv6-riscv** 的 `mmap` + COW lab；
- 想真正把 Linux 这套结构串起来→ 做 **tiny-mm**；
- 只补堆分配器→ 做 **Malloc Lab**。

下面按 tiny-mm 展开。

---

## 1. 项目目标

写一个用户态程序 `tiny_mm`，支持：

```c
/* 物理页 */
struct page *alloc_pages(gfp_t gfp, int order);
void free_pages(struct page *page, int order);

/* 虚拟地址空间 */
struct mm_struct *mm_create(void);
void *mm_mmap(struct mm_struct *mm, size_t len, int prot, int flags);
int mm_munmap(struct mm_struct *mm, void *addr, size_t len);

/* 模拟一次用户访问：可能触发缺页 */
int mm_access(struct mm_struct *mm, void *addr, int write);

/* slab */
void *kmalloc(size_t size);
void kfree(void *ptr);
```

最终验收：

- 能跑一个多进程/多 VMA 的 workload；
- 所有页都能正确回收，`free_pages` 计数归零；
- 故意注入的 bug 能被测试/断言抓住；
- `make test` 全部通过，ASan/UBSan 无报错。

---

## 2. 推荐目录

```text
tiny_mm/
├── Makefile
├── README.md
├── include/
│   └── tmm.h          # struct page/zone/mm/vma/pte 和 API
├── src/
│   ├── pmm.c          # 物理页：PFN/paddr、zone、buddy、pcp
│   ├── vmm.c          # mm/VMA、页表、缺页、mmap/munmap
│   ├── rmap.c         # page→VMA 反向映射、COW
│   ├── slab.c         # 简单 slab 分配器
│   └── main.c         # 场景/CLI
└── tests/
    ├── test_pmm.c
    ├── test_vmm.c
    ├── test_rmap.c
    ├── test_slab.c
    └── test_integration.c
```

---

## 3. 六个 milestone

### M0：脚手架与测试框架（0.5 天）

- `Makefile`：`-Wall -Wextra -Werror -fsanitize=address,undefined`；
- `tests/` 使用简单 assert 宏，输出 PASS/FAIL；
- `make test` 跑全部测试；
- 统一日志：`LOG("pfn=%lu ref=%d")`。

**验收**：空测试能跑；ASan 能抓住一个故意越界。

---

### M1：物理页 + PFN/paddr + zone（2~3 天）

实现：

```c
struct page {
    int refcount;
    int order;
    int is_free;
    unsigned long flags;   /* node/zone 编码，可简化 */
    void *data;            /* 模拟页内容 */
};

struct zone {
    unsigned long start_pfn;
    unsigned long spanned_pages;
    struct free_area free_area[MAX_ORDER];
};

unsigned long page_to_pfn(struct page *page);
struct page *pfn_to_page(unsigned long pfn);
phys_addr_t pfn_to_paddr(unsigned long pfn);
unsigned long paddr_to_pfn(phys_addr_t paddr);
struct zone *page_zone(struct page *page);
```

先不要写 buddy，先用 bitmap 或简单 free list 实现 order-0 分配；M2 再换成 buddy。

必须通过的测试：

1. `alloc_pages(0)` 返回的 page 的 `refcount == 1`；
2. `page_to_pfn(pfn_to_page(pfn)) == pfn`；
3. `pfn_to_paddr(paddr_to_pfn(pa))` 按页对齐后等于 `pa & PAGE_MASK`；
4. 分配 N 页、全部释放后 `free_count == 总页数`；
5. 故意 double free，测试必须报错（自己写 `assert(!page->is_free)`）；
6. 故意 free 错 order，测试必须报错；
7. 分配失败返回 NULL，不崩溃。

**对应 Linux**：

- `struct page`：`include/linux/mm_types.h:70`
- `page_to_pfn/pfn_to_page`：`include/asm-generic/memory_model.h:18`
- `PFN_PHYS/PHYS_PFN`：`include/linux/pfn.h:21`
- `page_zone/page_zonenum`：`include/linux/mm.h:1539` / `:1128`
- `zone` 字段：`include/linux/mmzone.h:490`

**练习的坑**：

- PFN 是页号，不是字节地址；
- page 数组索引与 paddr 的偏移；
- double free、free 错 order；
- 分配失败路径；
- 释放后 page 元数据仍可读，但内容不应再使用。

---

### M2：buddy + pcp（3~4 天）

实现：

- `free_area[MAX_ORDER]` 空闲链表；
- 分配时按 order 找，找不到就 split；
- 释放时尝试与 buddy 合并；
- 每个 zone 一个简化 pcp 缓存（order-0 批量）。

必须通过的测试：

1. 随机 `alloc_pages(order)` / `free_pages(page, order)` 10 万次，最终 `free_count` 回到初始值；
2. 分配 order=2 后 free order=0，断言必须报错；
3. 连续分配/释放后，最大空闲块能合并回预期 order；
4. pcp 命中/下刷计数正确；
5. 模拟“内存压力”：小页占满后高阶分配失败，释放后恢复。

**对应 Linux**：

- `struct free_area`：`include/linux/mmzone.h:97`
- `zone->free_area[]`：`include/linux/mmzone.h:608`
- `struct per_cpu_pages`：`include/linux/mmzone.h:360`
- `__alloc_pages()` / `free_pages()`：`mm/page_alloc.c`

**练习的坑**：

- buddy 合并时算错 buddy pfn；
- free 错 order 导致链表错乱；
- pcp 批量下刷与计数；
- 高阶分配失败的回退；
- double free 不立刻崩，但会静默破坏 freelist。

---

### M3：VMA + 页表 + 缺页（4~5 天）

实现：

```c
struct vm_area_struct {
    unsigned long vm_start, vm_end;
    int prot;
    int flags;
    struct vm_area_struct *next;
    struct mm_struct *vm_mm;
    struct file *vm_file;       /* 可先只做匿名 */
    unsigned long vm_pgoff;
};

struct mm_struct {
    struct vm_area_struct *mmap;
    pte_t *pgd;                 /* 可用两级页表简化 */
    ...
};
```

先做两级页表（PGD → PTE）即可，不必一上来五级。

必须通过的测试：

1. `mm_mmap` 建 VMA，`mm_access` 首次访问触发缺页，PTE 从 none 变 present；
2. `vaddr → VMA` 用 `find_vma` 语义（第一个 `vm_end > addr`，再检查 `vm_start`）；
3. `VMA + vaddr → page` 通过页表遍历，`pte_pfn` → `pfn_to_page`；
4. `mm_munmap` 后 VMA 删除、PTE 清除、页 refcount 归还；
5. 访问未映射地址触发 SIGSEGV 模拟（返回错误）；
6. 两个 VMA 相邻/重叠的边界测试；
7. VMA 合并/拆分至少做一个简单版本。

**对应 Linux**：

- `find_vma()`：`mm/mmap.c:2273`
- `vma_lookup()`：`include/linux/mm.h:2713`
- `handle_mm_fault()`：`mm/memory.c:4773`
- `__handle_mm_fault()`：`mm/memory.c:4617`
- `do_anonymous_page()`：`mm/memory.c:3715`
- `vm_normal_page()`：`mm/memory.c:605`
- `pte_pfn/pte_page/pfn_pte`：`arch/riscv/include/asm/pgtable.h:252`

**练习的坑**：

- VMA 存在 ≠ 页已分配；
- `find_vma` 返回的 VMA 不一定包含 addr；
- PTE 可能是 special/zero page，不能直接 `pte_page`；
- `munmap` 要清 PTE、TLB（模拟）、put_page；
- 多级页表中间页的分配/释放。

---

### M4：rmap + COW + fork（4~5 天）

实现：

- 每个 page 记录 `mapping`（匿名页用 `anon_vma` 简化版）和 `index`；
- `rmap_walk(page)` 回调所有 `(vma, vaddr)`；
- `fork` 复制 VMA/页表，页表只读，refcount++；
- 写时缺页 → 分配新页、复制内容、改 PTE、减旧页 refcount。

必须通过的测试：

1. 一个 page 被两个 VMA 映射，`rmap_walk` 能访问到两个 `(vma, addr)`；
2. `page -> VMA -> vaddr` 用 `page->index + vm_pgoff` 反算正确；
3. `fork` 后父子共享物理页，refcount=2；
4. 父/子写时 COW，各自拿到新页，旧页 refcount 减 1；
5. `munmap` 一个映射后，page 仍被另一个 VMA 映射，不能释放；
6. 最后一个映射消失后，page 才回收。

**对应 Linux**：

- `vma_address()`：`mm/internal.h:389`
- `page_address_in_vma()`：`mm/rmap.c:709`
- `rmap_walk()`：`mm/rmap.c:2363`
- `anon_vma` / `anon_vma_chain`：`include/linux/rmap.h:29` / `:77`
- `do_wp_page()` / COW：`mm/memory.c`
- `page_add_anon_rmap()`：`mm/rmap.c`

**练习的坑**：

- `page->index` 不是 vaddr；
- 一个 page 多个 VMA，不能假设唯一；
- `_refcount` 和 `_mapcount` 分离；
- COW 后旧页何时能释放；
- rmap 遍历的锁和一致性。

---

### M5：slab 分配器（3~4 天）

实现：

- 用 buddy 申请 slab page；
- 固定大小对象 cache（8/16/32/64/128）；
- `kmalloc/kfree`，空闲对象链表；
- 可选 redzone/poison（自己模拟）。

必须通过的测试：

1. `kmalloc` 返回对齐地址，`kfree` 后对象可复用；
2. UAF：free 后写入旧指针，测试必须检测（poison/redzone 或断言）；
3. double free 检测；
4. 越界写检测（redzone）；
5. 10 万次随机 alloc/free，无泄漏、无崩溃；
6. slab page 用完后能归还 buddy。

**对应 Linux**：

- `mm/slub.c`
- `include/linux/slab.h`
- `kmem_cache` / `kmalloc_caches`

**练习的坑**：

- 对象大小与对齐；
- freelist 被 UAF 写坏；
- double free 把同一对象分给两个用户；
- slab page 归还时机。

---

### M6：内存压力 + 综合场景（3~4 天）

实现/模拟：

- 物理页耗尽时：直接回收失败 → 返回 NULL/OOM；
- GFP 语义模拟：`GFP_KERNEL` 允许睡眠，`GFP_ATOMIC` 不允许；
- 原子上下文分配 `GFP_KERNEL` → 断言/警告；
- 内存压力下 mmap/COW/rmap 组合 workload。

综合验收：

```text
1. 创建 3 个 mm；
2. 每个 mm 建 100 个 VMA；
3. 随机访问/写触发缺页；
4. fork 一个 mm，制造 COW；
5. munmap 一部分；
6. 释放所有 mm；
7. 检查：所有 page refcount=0、buddy 空闲页数回到初始、slab 无泄漏；
8. ASan/UBSan 无报错。
```

---

## 4. 建议时间安排

按每天 2 小时、周末 4~6 小时：

| 周 | 内容 | 产出 |
|---|---|---|
| 第 1 周 | M0 + M1 | 物理页/PFN/paddr/zone 测试通过 |
| 第 2 周 | M2 | buddy + pcp 随机压力测试通过 |
| 第 3 周 | M3 | VMA + 两级页表 + 缺页 |
| 第 4 周 | M4 | rmap + COW + fork |
| 第 5 周 | M5 | slab + UAF/double free 检测 |
| 第 6 周 | M6 | 综合 workload + 总结 |

如果只有周末：把每个 milestone 拆成两周，总共 10~12 周。

---

## 5. 规则：避免又变成“看代码”

1. **先写测试，再写实现**：每个 milestone 先列 5 个断言。
2. **每实现一个模块，先故意注入一个 bug**：double free、错 order、少 put、stale VMA，确认测试能抓住。
3. **不复制 Linux 源码**：只对照函数语义，用自己的数据结构实现。
4. **每完成一个 milestone 写 5 行笔记**：我踩了哪个坑、测试怎么抓住的。
5. **最后再做一次“闭卷验收”**：不看笔记，从 `mmap` 到 `page` 画一遍链路。

---

## 6. 和 Linux 源码的对应关系

| tiny-mm 模块 | Linux 对应 |
|---|---|
| `pmm.c` 物理页/PFN/paddr | `include/asm-generic/memory_model.h`、`include/linux/pfn.h` |
| `pmm.c` zone/buddy/pcp | `include/linux/mmzone.h`、`mm/page_alloc.c` |
| `vmm.c` mm/VMA | `include/linux/mm_types.h`、`mm/mmap.c` |
| `vmm.c` 页表/缺页 | `mm/memory.c`、`arch/riscv/include/asm/pgtable.h` |
| `rmap.c` 反向映射/COW | `mm/rmap.c`、`mm/memory.c` |
| `slab.c` | `mm/slub.c`、`include/linux/slab.h` |
| 测试/注入 bug | 之前四个 lab + `../../catalog/memory_bug_catalog.md` |

---

## 7. 第一个具体任务（今天就能开始）

不要一开始写完整项目。先做 **M1 的最小闭环**：

```text
1. 定义 struct page / struct zone；
2. 实现 page_to_pfn / pfn_to_page / PFN_PHYS / PHYS_PFN；
3. 用 bitmap 实现 order-0 alloc/free；
4. 写 5 个测试：
   - alloc 后 refcount=1
   - pfn/page 往返
   - pfn/paddr 往返
   - alloc N + free N 后 free_count 恢复
   - double free 触发断言
5. 跑 ASan/UBSan。
```

完成标准：`make test` 全绿，且故意把 `free_pages` 改成不检查 `is_free` 时，double free 测试会失败——说明测试真的在保护你。

---

## 8. 下一步

如果认可这个方向，下一步不是继续看文档，而是：

1. 建 `tiny_mm/` 目录和 `Makefile`；
2. 写 `include/tmm.h`；
3. 先只实现 M1，跑通 5 个测试；
4. 再进入 M2。

项目不需要一次写完；每个 milestone 都是一个可验收的小闭环。
