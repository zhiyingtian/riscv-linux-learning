# Linux Memory 框架学习规划

本文档面向当前阶段：已经学习完 RISC-V MMU 启动流程，准备继续研究 Linux Memory 框架，并为后续学习 Scheduler、Interrupt，以及未来转向高端存储或 HPC 方向打基础。

建议学习顺序：

```text
RISC-V MMU 启动
    -> Linux Memory 框架
        -> Scheduler
            -> Interrupt / Exception / Softirq
                -> Block / Filesystem / Network / Driver / HPC Runtime
```

选择先学 Memory 是非常合理的。因为 Linux 内核很多核心问题最终都会落到内存管理上：

- 进程为什么能拥有独立地址空间
- 缺页异常为什么能按需分配物理页
- 文件 IO 为什么绕不开 page cache
- 高性能 IO 为什么关心 pin page、DMA、NUMA、huge page
- Scheduler 为什么需要理解 task、mm、stack、context
- Interrupt 为什么最终也要考虑内存分配上下文、原子上下文和缓存一致性

---

## 一、学习目标

学习 Memory 框架不要只停留在“哪个函数调用哪个函数”。目标应该是建立三层理解：

### 1. 知其然：Linux Memory 在做什么

必须能回答：

- Linux 如何描述物理内存
- Linux 如何分配和释放物理页
- Linux 如何描述一个进程的虚拟地址空间
- Linux 如何处理缺页异常
- Linux 如何把文件数据缓存到内存
- Linux 如何在内存压力下回收页面
- Linux 如何支持高性能场景下的大页、NUMA、DMA、page pin

### 2. 知其所以然：为什么这样设计

必须能回答：

- 为什么需要 `struct page`
- 为什么需要 zone/node，而不是一个全局 freelist
- 为什么启动早期使用 `memblock`，后期切换到 buddy allocator
- 为什么物理内存分配和虚拟地址空间管理要分层
- 为什么 Linux 不在 `mmap()` 时立刻分配所有物理页
- 为什么缺页异常是正常路径，而不是错误路径
- 为什么 page cache 是 Linux IO 性能的核心
- 为什么内存回收要区分 anonymous page 和 file page
- 为什么高性能系统会特别关注 NUMA、huge page、TLB、cache locality

### 3. 能迁移：为高端存储和 HPC 打基础

Memory 学完后，应该能自然理解这些方向：

- 高端存储：page cache、direct IO、bio、DMA、zero-copy、writeback、reclaim、memory pressure
- HPC：NUMA、huge page、TLB miss、page migration、memory locality、pin memory、RDMA
- 内核性能分析：page fault、kswapd、OOM、slab、vmstat、perf、tracepoint

---

## 二、总览：Linux Memory 为什么会这样设计

Linux Memory 框架本质上是在解决四组矛盾。

### 1. 物理内存有限，但进程希望看到很大的连续空间

用户进程看到的是虚拟地址空间。这个空间可以很大、连续、私有，但底层物理内存可能：

- 容量有限
- 不连续
- 有设备保留区域
- 有 DMA 限制
- 有 NUMA 距离差异
- 有热插拔和持久内存等复杂场景

所以 Linux 必须把“虚拟地址空间”和“物理页管理”分开。

对应源码主线：

- `struct mm_struct`
- `struct vm_area_struct`
- `struct page`
- `struct zone`
- `pg_data_t`

### 2. 物理页需要高效分配，但也要控制碎片

内核频繁分配和释放页面。如果只用简单链表，很难同时满足：

- 快速分配
- 连续页分配
- 减少碎片
- 支持不同迁移类型
- 支持 DMA、Normal、Movable 等区域

所以 Linux 使用 buddy allocator 管理物理页，并在其上构建 slab/slub 管理小对象。

对应源码主线：

- `mm/page_alloc.c`
- `mm/slub.c`
- `include/linux/mmzone.h`

### 3. 进程需要隔离，但数据又要共享

每个进程有独立地址空间，但很多内容可以共享：

- 共享库代码段
- 文件映射
- fork 后的 copy-on-write 页面
- page cache 中的文件页

所以 Linux 使用 VMA 描述虚拟区间，用页表描述实际映射，用 refcount/mapcount/rmap 跟踪页面关系。

对应源码主线：

- `mm/mmap.c`
- `mm/memory.c`
- `mm/rmap.c`
- `kernel/fork.c`

### 4. 内存要尽量用满，但不能真的耗尽

空闲内存如果闲着不用，是浪费。所以 Linux 会尽量把空闲内存用于 page cache、slab cache 等缓存。

但当内存压力出现时，又必须能回收：

- 干净 file page 可以直接丢弃
- 脏 file page 需要 writeback 后回收
- anonymous page 可能需要 swap
- 不可回收页面会增加 OOM 风险

所以 Linux Memory 不只是分配器，还是一个动态平衡系统。

对应源码主线：

- `mm/filemap.c`
- `mm/vmscan.c`
- `mm/swap.c`
- `mm/page-writeback.c`
- `mm/oom_kill.c`

---

## 三、推荐学习顺序

下面是建议的第一轮学习路径。第一轮目标不是读完所有 `mm/` 文件，而是打通主干。

```text
阶段 0：衔接 RISC-V MMU 启动
阶段 1：核心数据结构
阶段 2：早期内存管理 memblock
阶段 3：物理页分配 buddy allocator
阶段 4：进程地址空间 VMA
阶段 5：缺页异常和页表建立
阶段 6：fork / COW / reverse mapping
阶段 7：page cache 和文件映射
阶段 8：内存回收、swap、OOM
阶段 9：slab/slub、vmalloc、percpu
阶段 10：NUMA、huge page、migration、compaction
```

---

## 四、阶段 0：从 RISC-V MMU 启动接到通用 Memory

### 学习目标

理解启动阶段页表建好后，Linux 如何进入通用内存管理框架。

你已经掌握了：

- `head.S`
- `setup_vm()`
- `relocate`
- `setup_vm_final()`
- `swapper_pg_dir`
- 线性映射
- fixmap
- RISC-V Sv39 页表

接下来要看的是：

```text
start_kernel()
    -> setup_arch()
        -> paging_init()
            -> setup_vm_final()
            -> sparse_init()
            -> zone_sizes_init()
                -> free_area_init()
    -> mm_init()
    -> mem_init()
        -> memblock_free_all()
```

### 源码入口

- `init/main.c`
- `arch/riscv/kernel/setup.c`
- `arch/riscv/mm/init.c`
- `mm/mm_init.c`
- `mm/page_alloc.c`
- `mm/sparse.c`

### 必须掌握

- `setup_vm_final()` 建立最终线性映射后，通用 MM 仍然没有完全可用
- `sparse_init()` 用于初始化 `struct page` 相关的内存模型
- `free_area_init()` 初始化 zone、node、buddy allocator 的基础结构
- `memblock_free_all()` 之后，大部分可用内存才正式交给 buddy allocator

### 为什么这样设计

启动早期不能一上来就用完整内存管理器，因为完整内存管理器本身需要内存来初始化：

- 页表还没完全稳定
- `struct page` 数组还没建立好
- zone/node 还没初始化
- buddy freelist 还不存在

所以 Linux 先用简单可靠的 `memblock` 过渡，等基础设施建好后，再把内存移交给 buddy allocator。

### 检验问题

- 为什么 `setup_vm_final()` 之后还需要 `free_area_init()`？
- 为什么 `memblock` 不能作为长期物理页分配器？
- `memblock_free_all()` 的语义是什么？
- RISC-V 的 `paging_init()` 和通用 `free_area_init()` 是如何连接的？

---

## 五、阶段 1：核心数据结构

### 学习目标

先把 Memory 的核心对象关系建立起来。第一轮不要急着进入复杂算法。

### 源码入口

- `include/linux/mm_types.h`
- `include/linux/mmzone.h`
- `include/linux/mm.h`
- `include/linux/page-flags.h`
- `include/linux/gfp.h`

### 必须掌握的结构体

| 结构体 | 作用 |
|---|---|
| `struct page` | 描述一个物理页帧 |
| `struct zone` | 描述一类物理内存区域 |
| `pg_data_t` | 描述一个 NUMA node 的内存管理状态 |
| `struct mm_struct` | 描述一个进程的整个用户地址空间 |
| `struct vm_area_struct` | 描述进程地址空间中的一段连续虚拟区间 |
| `struct address_space` | 文件 page cache 的核心对象 |
| `struct anon_vma` | anonymous memory 的反向映射辅助结构 |

### 必须掌握的概念

- PFN 和 `struct page` 的关系
- `pfn_to_page()` / `page_to_pfn()`
- page flags
- page refcount 和 mapcount
- zone watermark
- GFP flags
- VMA flags
- file-backed mapping 和 anonymous mapping

### 为什么这样设计

Linux 不能只记录“某个物理页是否空闲”。它还必须知道这个页当前扮演什么角色：

- buddy 空闲页
- page cache 页
- anonymous 页
- slab 页
- page table 页
- compound page 的 head/tail 页
- device memory 页

所以 `struct page` 被设计成一个复用程度很高的结构体。它不是“简单页描述符”，而是 Linux 物理页生命周期的统一入口。

### 检验问题

- 为什么每个物理页都需要一个 `struct page`？
- 为什么 `struct page` 里面有很多 union？
- `struct page` 如何同时服务 page cache、slab、buddy？
- `mm_struct` 和 `vm_area_struct` 的关系是什么？
- VMA 是否等于页表映射？

---

## 六、阶段 2：早期内存管理 memblock

### 学习目标

理解启动早期 Linux 如何记录可用内存和保留内存。

### 源码入口

- `mm/memblock.c`
- `include/linux/memblock.h`
- `arch/riscv/mm/init.c`
- `drivers/of/fdt.c`

### 必须掌握

- `memblock.memory`
- `memblock.reserved`
- `memblock_add()`
- `memblock_reserve()`
- `memblock_phys_alloc()`
- `memblock_free_all()`
- DTB reserved-memory 如何进入 memblock
- kernel image、initrd、dtb 为什么要 reserve

### 为什么这样设计

早期启动只需要解决两个问题：

- 哪些物理内存可以用
- 哪些物理内存不能碰

此时用复杂分配器不合适。`memblock` 用 region 数组描述物理内存范围，简单、稳定、适合启动阶段。

### 检验问题

- `memblock.memory` 和 `memblock.reserved` 有什么区别？
- 为什么内核镜像本身要 reserve？
- DTB 什么时候从物理地址变成虚拟地址访问？
- `memblock` 分配出去的内存最后如何进入 buddy 体系？

---

## 七、阶段 3：物理页分配 Buddy Allocator

### 学习目标

理解 Linux 如何分配和释放物理页。

### 源码入口

- `mm/page_alloc.c`
- `include/linux/gfp.h`
- `include/linux/mmzone.h`
- `Documentation/admin-guide/mm/concepts.rst`

### 阅读主线

```text
alloc_pages()
    -> __alloc_pages()
        -> get_page_from_freelist()
            -> rmqueue()
                -> __rmqueue()
                    -> __rmqueue_smallest()

__free_pages()
    -> free_unref_page()
        -> free_unref_page_commit()
            -> __free_one_page()
```

### 必须掌握

- order 的含义
- buddy 合并与拆分
- zone 的 watermark
- per-cpu page list
- GFP flags 如何影响分配行为
- `ZONE_DMA` / `ZONE_DMA32` / `ZONE_NORMAL` / `ZONE_MOVABLE`
- migratetype
- direct reclaim 和 compaction 何时介入

### 为什么这样设计

物理页分配器要同时满足两个目标：

- 普通小页分配要快
- 高阶连续页分配要尽量可行

buddy allocator 用 order 管理连续页块，用拆分和合并减少外部碎片。per-cpu page list 则减少频繁分配释放时的锁竞争。

### 检验问题

- 为什么 buddy allocator 使用 2 的幂次大小？
- `order=0` 和 `order=9` 的分配难度有什么区别？
- watermark 为什么分为 min、low、high？
- 为什么有些分配允许 reclaim，有些不能睡眠？
- `GFP_KERNEL` 和 `GFP_ATOMIC` 的本质差别是什么？

---

## 八、阶段 4：进程虚拟地址空间 VMA

### 学习目标

理解一个进程的用户态虚拟地址空间如何被描述和管理。

### 源码入口

- `mm/mmap.c`
- `include/linux/mm_types.h`
- `include/linux/mm.h`
- `kernel/fork.c`

### 阅读主线

```text
sys_mmap()
    -> ksys_mmap_pgoff()
        -> vm_mmap_pgoff()
            -> do_mmap()
                -> mmap_region()
                    -> vma_merge()
                    -> vma_link()

exit()
    -> exit_mmap()

fork()
    -> dup_mm()
        -> dup_mmap()
```

### 必须掌握

- `mm_struct` 表示整个用户地址空间
- `vm_area_struct` 表示一段连续虚拟区间
- VMA 使用链表和红黑树管理
- VMA 只描述权限和范围，不等于已经分配物理页
- `mmap()` 通常只是建立 VMA，不马上分配物理页
- stack/heap/file mapping/shared mapping 的 VMA 差异

### 为什么这样设计

如果 `mmap()` 时立刻分配所有物理页，会造成巨大浪费。很多映射可能永远不会被访问。

Linux 采用 lazy allocation：

- `mmap()` 先建立 VMA
- 首次访问触发 page fault
- page fault 时再分配物理页或建立文件页映射

这就是虚拟内存的核心价值：把“承诺的地址空间”和“实际占用的物理页”分离。

### 检验问题

- VMA 和 PTE 是什么关系？
- 为什么一个 VMA 可以覆盖很多页表项？
- 为什么 `mmap()` 成功不代表物理内存已经分配？
- `find_vma()` 为什么是 page fault 的关键步骤？
- VMA merge 的意义是什么？

---

## 九、阶段 5：缺页异常和页表建立

### 学习目标

把 RISC-V exception、VMA、page allocator、页表建立串起来。

### 源码入口

- `arch/riscv/mm/fault.c`
- `mm/memory.c`
- `include/linux/mm.h`
- `arch/riscv/include/asm/pgtable.h`

### 阅读主线

```text
RISC-V page fault
    -> do_page_fault()
        -> find_vma()
        -> handle_mm_fault()
            -> __handle_mm_fault()
                -> handle_pte_fault()
                    -> do_anonymous_page()
                    -> do_fault()
                    -> do_wp_page()
```

### 必须掌握

- instruction/load/store page fault 的区别
- page fault 不一定是错误，很多时候是正常分配路径
- anonymous page fault
- file-backed page fault
- write-protect page fault
- copy-on-write
- zero page
- PTE 权限位和 VMA 权限的关系
- page table page 本身也需要分配

### 为什么这样设计

缺页异常是 Linux 延迟分配的执行点。

设计思想是：

- 地址空间先用 VMA 描述
- 访问时由硬件发现 PTE 不存在或权限不匹配
- CPU 进入异常
- 内核根据 VMA 决定这是合法访问还是非法访问
- 合法访问就补齐页表和物理页
- 非法访问就发送 `SIGSEGV`

这让 Linux 能够支持 demand paging、COW、file mapping、swap in 等机制。

### 检验问题

- 为什么 page fault 是虚拟内存系统的核心，而不是异常情况？
- `do_page_fault()` 如何区分合法缺页和非法访问？
- `handle_mm_fault()` 最终如何分配物理页？
- COW 的写保护页为什么会触发 page fault？
- `do_anonymous_page()` 和 `do_fault()` 分别处理什么？

---

## 十、阶段 6：fork、COW 和反向映射

### 学习目标

理解进程复制时如何避免复制所有物理页，以及 Linux 如何从物理页找到映射它的 VMA/PTE。

### 源码入口

- `kernel/fork.c`
- `mm/memory.c`
- `mm/rmap.c`
- `include/linux/rmap.h`

### 阅读主线

```text
fork()
    -> copy_process()
        -> copy_mm()
            -> dup_mm()
                -> dup_mmap()
                    -> copy_page_range()

write fault
    -> do_wp_page()
        -> wp_page_copy()
```

### 必须掌握

- fork 后父子进程共享只读页面
- 写入时触发 COW
- `copy_page_range()` 复制页表，不一定复制物理页
- page refcount 和 mapcount 的差异
- rmap 用于页面回收、迁移、COW 等场景

### 为什么这样设计

如果 fork 时复制整个进程地址空间，代价会非常高，尤其是 shell、server、database、HPC runtime 这类程序。

COW 的设计是：

- fork 时共享物理页
- 父子 PTE 都改成只读
- 谁先写，谁触发 write fault
- fault path 再复制物理页

这把一次巨大的复制成本变成了按需复制。

### 检验问题

- fork 为什么不立即复制所有用户页？
- COW 为什么要依赖 PTE write-protect？
- refcount 和 mapcount 分别回答什么问题？
- rmap 为什么对 reclaim 和 migration 很重要？

---

## 十一、阶段 7：Page Cache 和文件映射

### 学习目标

理解 Linux IO 为什么绕不开内存管理。

### 源码入口

- `mm/filemap.c`
- `mm/readahead.c`
- `mm/page-writeback.c`
- `mm/truncate.c`
- `include/linux/fs.h`
- `include/linux/pagemap.h`

### 阅读主线

```text
read()
    -> vfs_read()
        -> filemap_read()
            -> page cache lookup
            -> cache miss -> readpage/readahead

mmap file fault
    -> do_fault()
        -> filemap_fault()

writeback
    -> balance_dirty_pages()
    -> write_cache_pages()
```

### 必须掌握

- page cache 的 key 是 `address_space + index`
- 文件页和物理页通过 `struct page` 关联
- clean page 可以直接回收
- dirty page 必须 writeback 后才能回收
- readahead 提升顺序读性能
- buffered IO 与 direct IO 的差异
- mmap 文件访问和 read/write 都可能走 page cache

### 为什么这样设计

磁盘和网络存储比内存慢很多。Linux 使用 page cache 把文件数据缓存在内存中：

- 读文件时避免重复访问设备
- 写文件时先写内存，再异步 writeback
- mmap 文件时可以直接通过 page fault 建立文件页映射

对于高端存储方向，page cache 是理解 buffered IO、writeback、reclaim、direct IO 性能差异的核心。

### 检验问题

- 为什么 `free` 看到的 cache 很大不是坏事？
- page cache 和 VMA 是什么关系？
- file-backed page fault 如何找到文件页？
- dirty page 为什么不能直接释放？
- direct IO 为什么要绕过 page cache？

---

## 十二、阶段 8：内存回收、Swap 和 OOM

### 学习目标

理解 Linux 如何在内存压力下保持系统运行。

### 源码入口

- `mm/vmscan.c`
- `mm/swap.c`
- `mm/swap_state.c`
- `mm/swapfile.c`
- `mm/page-writeback.c`
- `mm/oom_kill.c`
- `Documentation/admin-guide/mm/concepts.rst`

### 阅读主线

```text
allocation slowpath
    -> __alloc_pages_slowpath()
        -> __alloc_pages_direct_reclaim()
            -> try_to_free_pages()
                -> shrink_node()
                    -> shrink_lruvec()
                        -> shrink_list()
                            -> shrink_page_list()

background reclaim
    -> kswapd()
        -> balance_pgdat()
```

### 必须掌握

- active/inactive LRU
- anonymous LRU 和 file LRU
- kswapd
- direct reclaim
- page reclaim
- slab reclaim
- swap out / swap in
- dirty page writeback
- OOM killer
- memory pressure 和 allocation stall

### 为什么这样设计

Linux 会尽量使用空闲内存做缓存，所以系统运行一段时间后 free memory 变少是正常现象。

真正关键的是：

- 是否有足够 reclaimable memory
- 回收成本是否过高
- 是否发生 direct reclaim stall
- dirty page 是否来不及 writeback
- anonymous page 是否需要 swap
- 是否走到 OOM

对高端存储系统来说，内存回收会直接影响 IO 延迟。对 HPC 来说，reclaim、swap、NUMA miss 都可能造成明显性能抖动。

### 检验问题

- 为什么 Linux 要区分 file page 和 anonymous page？
- clean file page 为什么最容易回收？
- dirty file page 回收为什么会牵涉 writeback？
- kswapd 和 direct reclaim 有什么区别？
- 什么情况下会触发 OOM？
- 为什么存储系统中 direct reclaim 可能导致长尾延迟？

---

## 十三、阶段 9：Slab/Slub、Vmalloc 和 Percpu

### 学习目标

理解内核对象和内核虚拟地址空间如何管理。

### 源码入口

- `mm/slub.c`
- `mm/slab_common.c`
- `mm/vmalloc.c`
- `mm/percpu.c`
- `include/linux/slab.h`
- `include/linux/vmalloc.h`

### 必须掌握

- page allocator 分配页，slub 分配小对象
- `kmalloc()` 和 `vmalloc()` 的区别
- `kmalloc()` 物理连续，`vmalloc()` 虚拟连续
- slab cache 的对象复用
- percpu 变量减少锁竞争
- 内核栈、task_struct、vma、inode 等对象与 slab 的关系

### 为什么这样设计

很多内核对象远小于一页。如果每个对象都用 page allocator 分配，会造成巨大浪费。

Slub 在 page allocator 之上管理小对象：

- 减少内存碎片
- 提升分配释放速度
- 便于对象缓存和调试

`vmalloc()` 则解决“大块虚拟连续但物理不连续”的需求，常用于模块、某些大缓冲区、ioremap 辅助场景。

### 检验问题

- 为什么有了 page allocator 还需要 slub？
- `kmalloc(8KB)` 和 `vmalloc(8KB)` 的语义差异是什么？
- 为什么 `vmalloc()` 得到的地址不能随便用于 DMA？
- percpu allocator 为什么能降低并发开销？

---

## 十四、阶段 10：NUMA、Huge Page、Migration 和 Compaction

### 学习目标

为高端存储和 HPC 建立性能视角。

### 源码入口

- `mm/mempolicy.c`
- `mm/hugetlb.c`
- `mm/huge_memory.c`
- `mm/khugepaged.c`
- `mm/migrate.c`
- `mm/compaction.c`
- `Documentation/vm/numa.rst`
- `Documentation/admin-guide/mm/hugetlbpage.rst`
- `Documentation/admin-guide/mm/transhuge.rst`

### 必须掌握

- NUMA node 和内存访问距离
- memory policy
- huge page 减少 TLB miss
- THP 和 HugeTLB 的区别
- memory compaction 为什么服务高阶页分配
- page migration 为什么需要 rmap
- page pin 为什么会影响 migration 和 reclaim

### 为什么这样设计

在普通应用里，内存访问可以抽象成“读写地址”。但在高性能系统里，内存访问本身就是性能瓶颈：

- 访问远端 NUMA node 会增加延迟
- 小页会增加 TLB 压力
- 内存碎片会导致大页分配失败
- page pin 会阻碍 reclaim、migration、compaction
- RDMA、NVMe、GPU、DAX 等场景都会放大这些问题

这也是 HPC 和高端存储岗位非常看重 Linux Memory 基础的原因。

### 检验问题

- 为什么 HPC 程序关心 NUMA binding？
- huge page 为什么能提升性能？
- THP 为什么有时提升性能，有时造成延迟抖动？
- page migration 为什么需要知道哪些进程映射了这个 page？
- 长期 pin page 为什么会伤害内存回收和迁移？

---

## 十五、第一轮必须掌握的源码清单

第一轮优先读这些，不要一开始陷入所有子系统。

| 顺序 | 文件 | 重点 |
|---|---|---|
| 1 | `arch/riscv/mm/init.c` | RISC-V 如何接入通用 MM |
| 2 | `include/linux/mm_types.h` | `struct page`、`mm_struct`、`vm_area_struct` |
| 3 | `include/linux/mmzone.h` | zone、node、free_area、watermark |
| 4 | `mm/memblock.c` | 早期物理内存管理 |
| 5 | `mm/page_alloc.c` | buddy allocator |
| 6 | `mm/mmap.c` | VMA 管理 |
| 7 | `arch/riscv/mm/fault.c` | RISC-V page fault 入口 |
| 8 | `mm/memory.c` | page fault、COW、页表建立 |
| 9 | `kernel/fork.c` | fork 和地址空间复制 |
| 10 | `mm/rmap.c` | reverse mapping |
| 11 | `mm/filemap.c` | page cache |
| 12 | `mm/vmscan.c` | reclaim |
| 13 | `mm/swap.c` | LRU 和 swap 辅助逻辑 |
| 14 | `mm/page-writeback.c` | dirty page writeback |
| 15 | `mm/oom_kill.c` | OOM |

---

## 十六、第二轮扩展清单

第一轮主干打通后，再看这些模块。

| 模块 | 文件 | 学习目的 |
|---|---|---|
| Slub | `mm/slub.c` | 小对象分配 |
| Vmalloc | `mm/vmalloc.c` | 内核虚拟连续映射 |
| Percpu | `mm/percpu.c` | per-cpu 内存 |
| Compaction | `mm/compaction.c` | 内存碎片整理 |
| Migration | `mm/migrate.c` | 页面迁移 |
| THP | `mm/huge_memory.c` | 透明大页 |
| HugeTLB | `mm/hugetlb.c` | 显式大页 |
| Memcg | `mm/memcontrol.c` | cgroup 内存控制 |
| KSM | `mm/ksm.c` | 相同页面合并 |
| DAMON | `mm/damon/` | 内存访问监控 |

---

## 十七、建议实验

源码阅读最好配合实验，否则容易停留在抽象层。

### 1. 观察启动内存布局

建议打开或关注：

- `memblock=debug`
- `mminit_loglevel=4`
- `CONFIG_DEBUG_VM`
- `CONFIG_PROC_VMCORE` 可暂时不必关注

重点观察：

- memblock memory/reserved
- zone 初始化
- `free_area_init()`
- `mem_init()`
- `/proc/meminfo`
- `/proc/zoneinfo`
- `/proc/vmstat`

### 2. 观察 page fault

写一个用户态程序：

- `mmap()` 一大段 anonymous memory
- 只 `mmap()` 不访问
- 逐页写入
- 观察 minor fault 增长

可观察：

- `perf stat -e page-faults`
- `/proc/<pid>/stat`
- `/proc/<pid>/maps`
- `/proc/<pid>/smaps`

### 3. 观察 page cache

实验：

- 读取一个大文件
- 再次读取同一个文件
- 对比耗时和 `/proc/meminfo` 中 Cached
- 使用 `drop_caches` 后再次读取

重点理解：

- page cache 为什么提升读性能
- cache 占用内存为什么不是泄漏
- drop cache 为什么通常不是线上性能优化手段

### 4. 观察 reclaim

实验：

- 运行内存压力程序
- 观察 `kswapd`
- 观察 `/proc/vmstat`
- 观察 direct reclaim 相关计数

重点理解：

- 内存压力不是只看 free memory
- reclaim 会带来延迟
- dirty writeback 会影响回收速度

### 5. 观察 NUMA 和 huge page

如果后续环境支持 NUMA：

- `numactl --hardware`
- `numactl --membind`
- `numastat`
- transparent hugepage 开关
- hugetlbfs

重点理解：

- 本地内存和远端内存访问差异
- huge page 对 TLB 的影响
- THP 对延迟的影响

---

## 十八、和 Scheduler、Interrupt 的衔接

你计划按 `Memory -> Scheduler -> Interrupt` 学，这是一个很扎实的顺序。

### Memory 到 Scheduler

学完 Memory 后，看 Scheduler 时会更容易理解：

- `task_struct` 和内核栈如何分配
- `mm_struct` 和 `active_mm` 的意义
- 线程切换为什么涉及地址空间切换
- kernel thread 为什么没有用户地址空间
- NUMA balancing 为什么横跨 scheduler 和 memory
- page fault 为什么会导致进程阻塞和调度
- reclaim 中为什么会发生 sleep

建议衔接源码：

- `include/linux/sched.h`
- `kernel/fork.c`
- `kernel/sched/core.c`
- `arch/riscv/kernel/process.c`
- `arch/riscv/mm/context.c`

### Scheduler 到 Interrupt

学完 Scheduler 后，再看 Interrupt 会更容易理解：

- 中断上下文为什么不能随便睡眠
- `GFP_ATOMIC` 为什么存在
- softirq/tasklet/workqueue 为什么要分层
- IO completion 如何唤醒阻塞任务
- timer interrupt 和 scheduler tick 的关系
- page fault、syscall、external interrupt 的上下文差异

建议衔接源码：

- `arch/riscv/kernel/entry.S`
- `arch/riscv/kernel/traps.c`
- `kernel/irq/`
- `kernel/softirq.c`
- `kernel/time/`

---

## 十九、面向高端存储和 HPC 的重点能力

### 高端存储方向重点

优先关注：

- page cache
- direct IO
- writeback
- dirty throttling
- reclaim latency
- memory pressure
- DMA mapping
- pin user page
- bio 和 block layer
- NVMe queue
- zero-copy

Memory 中最相关模块：

- `mm/filemap.c`
- `mm/page-writeback.c`
- `mm/vmscan.c`
- `mm/gup.c`
- `mm/memory.c`
- `mm/page_alloc.c`

后续可衔接：

- `block/`
- `drivers/nvme/`
- `fs/iomap/`
- `fs/direct-io.c`

### HPC 方向重点

优先关注：

- NUMA
- huge page
- TLB
- page fault overhead
- memory bandwidth
- memory locality
- page migration
- CPU affinity
- scheduler affinity
- RDMA pin memory

Memory 中最相关模块：

- `mm/mempolicy.c`
- `mm/huge_memory.c`
- `mm/hugetlb.c`
- `mm/migrate.c`
- `mm/compaction.c`
- `mm/gup.c`
- `mm/page_alloc.c`

后续可衔接：

- `kernel/sched/fair.c`
- `kernel/sched/topology.c`
- `drivers/infiniband/`
- `lib/iov_iter.c`

---

## 二十、阶段性验收标准

### 第一阶段验收：内存初始化

你应该能画出：

```text
DTB memory 信息
    -> memblock
        -> sparse_init / struct page
            -> free_area_init / zone
                -> memblock_free_all
                    -> buddy allocator
```

### 第二阶段验收：一次匿名页访问

你应该能讲清：

```text
mmap anonymous memory
    -> 建立 VMA
        -> 首次写入
            -> page fault
                -> find_vma
                    -> handle_mm_fault
                        -> do_anonymous_page
                            -> alloc page
                                -> set PTE
```

### 第三阶段验收：一次 fork 后写入

你应该能讲清：

```text
fork
    -> copy mm
        -> copy page table
            -> PTE write-protect
                -> 子进程/父进程写入
                    -> write fault
                        -> COW
                            -> 新 page
                                -> 更新 PTE
```

### 第四阶段验收：一次文件读取

你应该能讲清：

```text
read file
    -> page cache lookup
        -> cache hit: copy to user
        -> cache miss: submit IO
            -> page becomes uptodate
                -> copy to user
```

### 第五阶段验收：一次内存压力回收

你应该能讲清：

```text
alloc_pages slowpath
    -> direct reclaim / wakeup kswapd
        -> scan LRU
            -> reclaim clean file page
            -> writeback dirty file page
            -> swap anonymous page
            -> OOM if no progress
```

---

## 二十一、推荐阅读方式

每个模块按这个顺序学：

1. 先看数据结构
2. 再看初始化
3. 再看正常路径
4. 再看慢路径
5. 最后看异常路径和性能优化

每次读源码都问四个问题：

- 这个模块在解决什么矛盾？
- 核心状态保存在哪里？
- 快路径是什么？
- 慢路径为什么必须存在？

不要一开始追所有 corner case。Linux Memory 的难点不是某一个函数长，而是不同机制互相牵连。第一轮目标是建立地图，第二轮再深挖细节。

---

## 二十二、建议学习节奏

### 第 1 周：Memory 初始化和核心数据结构

- `arch/riscv/mm/init.c`
- `include/linux/mm_types.h`
- `include/linux/mmzone.h`
- `mm/memblock.c`
- `mm/page_alloc.c` 初始化部分

产出：

- 画出 `memblock -> struct page -> zone -> buddy` 图
- 整理 `struct page` 字段用途

### 第 2 周：Buddy Allocator 和 GFP

- `mm/page_alloc.c`
- `include/linux/gfp.h`
- `/proc/zoneinfo`
- `/proc/buddyinfo`

产出：

- 画出 `__alloc_pages()` 快路径和慢路径
- 解释 `GFP_KERNEL`、`GFP_ATOMIC`、`__GFP_RECLAIM`

### 第 3 周：VMA 和 Page Fault

- `mm/mmap.c`
- `arch/riscv/mm/fault.c`
- `mm/memory.c`

产出：

- 画出 `mmap -> VMA -> page fault -> PTE` 流程
- 解释 anonymous fault、file fault、write-protect fault

### 第 4 周：fork、COW、rmap

- `kernel/fork.c`
- `mm/memory.c`
- `mm/rmap.c`

产出：

- 画出 fork COW 流程
- 解释 refcount、mapcount、rmap 的区别

### 第 5 周：Page Cache 和 Writeback

- `mm/filemap.c`
- `mm/readahead.c`
- `mm/page-writeback.c`
- `mm/truncate.c`

产出：

- 画出 read file 的 page cache 路径
- 解释 dirty page writeback 和 reclaim 的关系

### 第 6 周：Reclaim、Swap、OOM

- `mm/vmscan.c`
- `mm/swap.c`
- `mm/swap_state.c`
- `mm/oom_kill.c`

产出：

- 画出 direct reclaim 和 kswapd 路径
- 解释 file/anon LRU、active/inactive LRU

### 第 7 周：Slub、Vmalloc、Percpu

- `mm/slub.c`
- `mm/slab_common.c`
- `mm/vmalloc.c`
- `mm/percpu.c`

产出：

- 解释 `kmalloc()`、`alloc_pages()`、`vmalloc()` 的差异
- 解释 slab cache 为什么适合内核对象

### 第 8 周：NUMA、Huge Page、Migration

- `mm/mempolicy.c`
- `mm/huge_memory.c`
- `mm/hugetlb.c`
- `mm/migrate.c`
- `mm/compaction.c`

产出：

- 解释 NUMA locality
- 解释 THP/HugeTLB 差异
- 解释 compaction 和 migration 为什么服务高阶页分配

---

## 二十三、最终目标图

学完第一轮后，建议能从任意一个现象反推 Memory 框架路径。

例如：

```text
现象：程序第一次访问 malloc 出来的内存变慢
解释：malloc/brk/mmap 只是建立虚拟空间，首次访问触发 page fault，内核分配物理页并建立 PTE
```

```text
现象：机器 free memory 很少，但性能正常
解释：空闲内存被 page cache/slab 使用，必要时可回收，不等于内存泄漏
```

```text
现象：存储服务出现 IO 长尾延迟
解释：可能与 direct reclaim、dirty writeback、page cache pressure、slab pressure、NUMA miss、page allocation stall 有关
```

```text
现象：HPC 程序换 NUMA 绑定后性能变化明显
解释：内存访问延迟和带宽受 NUMA locality 影响，调度和内存分配策略共同决定性能
```

真正掌握 Linux Memory，不是记住 `mm/` 目录下所有函数，而是能建立这张因果图：

```text
虚拟地址
    -> VMA
        -> page fault
            -> page allocator
                -> struct page / zone / node
                    -> page cache / anon / slab
                        -> reclaim / writeback / swap / OOM
                            -> performance / latency / throughput
```

这张图就是后续学习 Scheduler、Interrupt、高端存储、HPC 的地基。

