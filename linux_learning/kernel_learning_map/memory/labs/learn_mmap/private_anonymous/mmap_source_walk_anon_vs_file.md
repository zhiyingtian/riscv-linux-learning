# mmap 源码之旅:匿名映射 vs 文件映射

> 环境:Linux 5.15 源码树(本机 `~/workspace/riscv/runninglinuxkernel_5.15`)
> 目标:把"用户态 mmap() → 内核 VMA → 缺页 fault → 物理页就位"整条路在源码里走通,并对比**匿名映射**和**文件映射**两条分支
> 前置:part1(RISC-V MMU)、mm11/mm12(mmap 原理+源码)、E18 验收链
> 方法:每个阶段"读源码 → 答引导问题 → 下一阶段",答案要点在文末

---

## 全程地图(5.15 行号锚点)

```text
【阶段 A】系统调用入口                     mm/mmap.c
  mmap() → ksys_mmap_pgoff (1583)
         → vm_mmap_pgoff  (1624, 加/解锁 mmap_lock)
         → do_mmap        (1404, 参数校验 + 找地址)

【阶段 B】VMA 建立与"匿名/文件"分叉          mm/mmap.c
  do_mmap → mmap_region   (1716)
         ├─ 匿名:  vma_set_anonymous()          (无文件对象)
         └─ 文件:  call_mmap(file, vma)
                   → generic_file_mmap  (filemap.c:3380)
                   → vma->vm_ops = &generic_file_vm_ops (filemap.c:3372)
  (两次 vma_merge 尝试:vma_merge at 1158)

【阶段 C】首次访问 → 缺页异常
  硬件发现 PTE 不存在
   → do_page_fault        (arch/riscv/mm/fault.c:210)
   → handle_mm_fault      (fault.c:323 → mm/memory.c)
   → handle_pte_fault     (mm/memory.c:4513)
       ├─ PTE 空 + 匿名 VMA → do_anonymous_page (3715)   ← 匿名分支
       └─ PTE 空 + 文件 VMA → do_fault (4274)            ← 文件分支
           → __do_fault (3831) → vma->vm_ops->fault
           → filemap_fault (filemap.c:3037)

【阶段 D】filemap_fault 内部(只有文件映射有)
  find_get_page(mapping, index)   ← page cache 查找
  ├─ 命中:直接用,finish_fault 建 PTE
  └─ miss: do_sync_mmap_readahead → mapping->a_ops->readpage → 读盘
```

---

## 阶段 A:系统调用入口(读 mm/mmap.c 1583–1624、1404–1440)

### 阅读任务

1. 打开 `mm/mmap.c:1583` `ksys_mmap_pgoff()`,看它先做什么检查?
2. 往下看到 `vm_mmap_pgoff()`(mm/util.c),注意 `mmap_lock` 的加锁/解锁。
3. 打开 `mm/mmap.c:1404` `do_mmap()`,只看开头 ~60 行:它校验了什么?

### 引导问题

- QA1:`ksys_mmap_pgoff` 里 `pgoff` 和 `len` 的检查逻辑是什么?为什么 offset 必须页对齐?(提示:PAGE_ALIGN)
- QA2:`do_mmap` 里 `file == NULL` 意味着什么?`MAP_ANONYMOUS` 对应的 `vm_flags` 分支在哪设置?(提示:找 `VM_SHARED | VM_MAYSHARE`)
- QA3:返回的映射地址是哪里算出来的?(提示:找 `get_unmapped_area`)
- QA4:整条 syscall 路径拿的是什么锁、什么时候释放?(提示:`mmap_lock` 读锁/写锁)

---

## 阶段 B:VMA 建立与分叉(读 mm/mmap.c 1716–1900)

### 阅读任务

1. `mmap_region()` 开头:`find_vma_intersection()` 在查什么?
2. 第一次 `vma_merge()`:什么时候可以"直接扩展已有 VMA"而不用新建?
3. 分配新 vma 后,找**匿名 vs 文件**的分叉点:
   - 匿名:找 `vma_set_anonymous(vma)`(可以 grep 它的定义,看设置了哪些 flag)
   - 文件:找 `call_mmap(file, vma)` → 顺着到 `filemap.c:3380 generic_file_mmap()`,看它给 `vma->vm_ops` 赋了什么
4. 看第二次 `vma_merge()` 成功时的处理(5.15 是 `vm_area_free(vma)` + goto out——和之前那个真实补丁的对比点!)

### 引导问题

- QB1:匿名 VMA 的 `vm_ops` 是什么?文件 VMA 的 `vm_ops` 是什么?这个字段对后续 fault 起什么作用?
- QB2:`VM_SHARED` 和 `MAP_PRIVATE` 在 VMA 上如何体现?文件映射里 `VM_SHARED` 与 `VM_MAYSHARE` 的区别?
- QB3:5.15 第二次 vma_merge 成功时,代码做了什么?(对比补丁练习 #1 的 bug)

---

## 阶段 C:缺页异常分流(读 arch/riscv/mm/fault.c:210–330 + mm/memory.c:4513–4570)

### 阅读任务

1. RISC-V 入口 `do_page_fault()`:如何区分内核态/用户态 fault?(提示:`user_mode(regs)`)
2. 进入 `handle_mm_fault` → `__handle_mm_fault` → `handle_pte_fault`(4513)
3. 重点看 `handle_pte_fault` 里的**分流 if**(4560–4570):
   - `pte_none()` 且 `vma_is_anonymous(vma)` → `do_anonymous_page`
   - `pte_none()` 且文件 → `do_fault`
   - 写保护 → `do_wp_page`

### 引导问题

- QC1:`handle_pte_fault` 靠什么字段判断"匿名还是文件"?答完把 mm12 学的 `vma_is_anonymous` 找出来。
- QC2:匿名页 fault 和文件页 fault 的**物理页来源**分别是什么?(对照 E12 表格:现造 vs page cache)
- QC3:文件映射为什么需要 `do_fault_around`(4176 附近)?这和 readahead 有什么关系?

---

## 阶段 D:filemap_fault 内部(读 mm/filemap.c:3037–3150)

### 阅读任务

1. `filemap_fault()` 开头怎么在 page cache 里找页?(提示:`find_get_page(mapping, index)`)
2. miss 时走哪条路?(提示:`do_sync_mmap_readahead` → `mapping->a_ops->readpage`)
3. 找到页后,`finish_fault()` / `do_set_pte` 干什么?

### 引导问题

- QD1:page cache 的"键"是什么?(提示:`address_space + index`,规划阶段7)
- QD2:为什么文件映射第一次读会 major fault,匿名页写却不会?(你已经用 perf 验证过了,现在找源码依据)
- QD3:`readpage` 回调是谁?普通文件系统用的是哪个?(提示:ext4 → `ext4_readpage`)

---

## 对比表(读完四阶段后填写)

| 维度 | 匿名映射 | 文件映射 |
|---|---|---|
| mmap 时 vm_ops | ? | generic_file_vm_ops |
| 映射内容来源 | 清零/未定义 | 文件数据 |
| fault 入口函数 | do_anonymous_page | do_fault → filemap_fault |
| 物理页从哪来 | 现分配(alloc) | page cache(可能读盘) |
| 首次访问 fault 类型 | minor | 可能 major |
| MAP_PRIVATE 写 | ?(COW 变体) | do_cow_fault → 复制后改 |
| MAP_SHARED 写 | 无此语义 | do_shared_fault → 写回 page cache |
| 回收优先级 | 需 swap | clean 直接回收 |

---

## 答案要点(先自己走完再翻)

- **QA1**:`PAGE_ALIGN(len)` 检查长度是否溢出页对齐边界;`pgoff` 和 `len` 相加不能溢出(unsigned long)。offset 页对齐是因为内核按页管理映射,非页对齐的 file offset 会破坏 pfn 换算。
- **QA2**:`file == NULL` 即匿名映射(`MAP_ANONYMOUS`);do_mmap 里匿名映射会设置 `VM_SHARED`(若 MAP_SHARED)/`VM_MAYSHARE`,并通过 `vma_set_anonymous()` 打标记。
- **QA3**:`get_unmapped_area()`(mmap.c 里 `arch_get_unmapped_area` 等)在 TASK_SIZE 范围内找一段无冲突的地址,考虑 ASLR、栈生长方向。
- **QA4**:`vm_mmap_pgoff`(mm/util.c)拿 `mmap_lock` **写锁**(`down_write`),`mmap_region` 中途可能降级为读锁(5.15 有 `mmap_write_downgrade`),结束时 `up_write` 释放。
- **QB1**:匿名 VMA 的 `vm_ops` 为 NULL(或匿名专用),文件 VMA 是 `generic_file_vm_ops`(其中 `.fault = filemap_fault`)。fault 时 `vm_ops->fault` 就是"文件怎么把页弄进来"的钩子。
- **QB2**:`VM_SHARED` 表示共享映射(MAP_SHARED),文件页写回 page cache;`VM_MAYSHARE` 表示"允许以后变共享"(mremap 等)。私有映射两标志都无。
- **QB3**:5.15 里第二次 merge 成功后:减少 map_count、`vm_area_free(vma)` 释放新建的 vma、goto out——正确处理;对比补丁练习 #1(新版漏接返回值导致 UAF),正好看出"refactor 引入回归"的典型模式。
- **QC1**:`vma_is_anonymous(vma)`——判断 `vm_ops == NULL`(匿名映射没有 vm_ops)。
- **QC2**:匿名=新分配清零页(alloc_zeroed_user_highpage);文件=page cache 页,miss 则读盘。
- **QC3**:`do_fault_around` 一次 fault 顺带映射附近页,配合 `filemap_fault` 里的 `do_sync_mmap_readahead` 预读,提升顺序读性能——文件映射的"readahead"在 mmap 路径上的体现。
- **QD1**:`struct address_space *mapping + pgoff`(文件页:inode 的 i_mapping;`find_get_page(mapping, index)`)。
- **QD2**:匿名页 fault 分配的是"现成的空页",不碰磁盘 → minor;文件映射 miss 要等磁盘 IO → major。你在 E11 看到的 major-faults=0 就是"全程没读盘"的源码依据。
- **QD3**:`mapping->a_ops->readpage`;ext4 等文件系统实现为 `ext4_readpage`(通过 `struct address_space_operations`)。

---

## 进阶衔接

- 走完这条链,阶段 6(fork/COW/rmap)的入口就清楚了:fork → `dup_mmap` → `copy_page_range`(mm/memory.c)把 PTE 复制并置只读;写 → `do_wp_page`。你 E14 实验验证的正是它。
- 真实补丁练习 #1(`../../../exercises/patch_exercise_01_vma_merge_uaf.md`)现在读起来会轻松得多——bug 就在你刚走过的 `mmap_region` 路径上。
