# 内存管理问题链（借 mmap 为主线的总纲）

> **定位**：`memory_half_year_roadmap.md` 的导航层。
> - roadmap 回答「接下来做什么」（M1/M2/M3 + 源码清单 + 时间账本）
> - 本文件回答「我现在在哪个问题」「该学到多深」「什么明确不学」
>
> **主线**：从 `mmap()` 出发，顺着问题往下追，每个节点都是实战单元。
> **原则**：顺序由依赖决定，范围由问题决定。不按文章顺序推进。
> **建立**：2026-10-21

---

## 一、问题链（答完一个标日期，不许跳）

| # | 问题 | 内核落点（5.15） | 状态 | 已有产出 / 验证方式 |
|---|---|---|---|---|
| Q1 | `mmap()` 到底做了什么？ | `ksys_mmap_pgoff` mmap.c:1583 → `vm_mmap_pgoff` util.c:506 → `do_mmap` mmap.c:1404 → `mmap_region` mmap.c:1716 | ✅ 2026-08 | `mmap_source_walk` 阶段 A/B 已答完 |
| Q2 | 首次访问为什么 fault？ | `do_page_fault` fault.c:210 → `handle_mm_fault` memory.c:4773 → `handle_pte_fault` memory.c:4513 | ✅ 2026-08 | walk 阶段 C + E11/E12/E13 |
| Q3 | 匿名页的物理页从哪来？ | `do_anonymous_page` memory.c:3715 → `alloc_pages` → buddy | 🔄 | E12 做过；缺 `alloc_pages`→buddy 这段源码笔记 |
| Q4 | 文件页从哪来？ | `do_fault` memory.c:4274 → `filemap_fault` filemap.c:3037 → `find_get_page` / `readpage` | 🔄 | walk 阶段 D 有答案；`private_file` demo 已写；**M2 未系统做** |
| Q5 | fork 为什么不复制物理页？ | `copy_mm` fork.c:1475 → `dup_mmap` fork.c:651 → `copy_page_range` memory.c:1225 → `copy_present_pte` memory.c:936 | ✅ 实验 + 画图 | `../labs/cow/cow.c`/`../labs/cow/cow.sh` 实跑；`../assets/反向映射.excalidraw` 已画 COW 前后 |
| Q6 | 谁还映射着这个物理页？ | `page_add_new_anon_rmap` rmap.c:1179、`page_add_anon_rmap` rmap.c:1110、`page_referenced` rmap.c:855、`try_to_unmap` | 🔄 | 图已画 `anon_vma`/`anon_vma_chain`/rb_root/same_vma 结构；**reclaim 侧的 `try_to_unmap` 未走** |
| Q7 | 内存不够了怎么回收？ | `kswapd` / `shrink_node` vmscan.c、`do_wp_page` 之外的 swap 路径 | ⬜ | M3 |
| Q8 | 这些结构体本身占内存吗？ | `vm_area_struct`/`anon_vma` 的 `kmem_cache_alloc` → `mm/slub.c` | ⬜ | 顺手摸到即可，不专门开章 |

**状态含义**：✅ 能脱稿讲清 + 有产出 ｜ 🔄 读过/有部分产出但未闭环 ｜ ⬜ 未开始

> **当前快照（2026-10-21）**：Q1✅ Q2✅ Q5✅（实验+图）Q3🔄 Q4🔄 Q6🔄 Q7⬜ Q8⬜
> **下一步就是 Q5/Q6 的源码侧文字化**（图已有，缺 `copy_page_range → copy_present_pte → do_wp_page` 的行号 walk 笔记）。

---

## 二、三档范围（写下来是为了给自己拒绝的理由）

### 档 1 · 强关联（必然遇到，必须学）

顺着 mmap 走一定会撞上的，全部按实战单元处理：

- VMA 建立与合并（`mmap_region` / `vma_merge`）
- 页表与缺页分流（`handle_pte_fault` 的三种分支）
- 物理页分配（`do_anonymous_page` → buddy）
- page cache（`filemap_fault`、`address_space`、readahead）
- fork / COW（`copy_page_range`、`do_wp_page`）
- 反向映射（`anon_vma`、`page_add_*`、`try_to_unmap`）
- 回收 / swap（`kswapd`、水位线、LRU）

### 档 2 · 可钩到（撞到顺手看 30 分钟，不专门开章）

- slab：VMA / anon_vma / task_struct 本身就是 `kmem_cache` 对象
- zone / watermark：任何 `alloc_pages` 都会撞上 GFP 与 min/low/high
- writeback：脏文件页回收时必然路过
- page migration：`try_to_unmap` 的另一个用户

### 档 3 · 明确不碰（第一轮拒绝，写在这里防膨胀）

| 不学 | 原因 |
|---|---|
| memblock | 启动期内容，part1/part2 已覆盖，不再深挖 |
| vmalloc / percpu | mmap 这条路摸不到 |
| NUMA / mempolicy | 用户态 mmap 不涉及；M4 后看方向再定 |
| huge page / THP | 同上 |
| compaction | 只在高阶分配失败时出现，非主干 |
| memory cgroup | 容器方向，独立分支 |
| KSM / zswap / zram | 优化分支，不是主干 |
| 内存热插拔 / DAMON | 独立子系统 |

> 触碰到档 3 内容时：**记录到"待办岛屿"清单，不在当前会话展开。**

---

## 三、停止条件（每个节点学到哪一层）

| 层 | 标准 | 时间预算 | 适用范围 |
|---|---|---|---|
| **L1 主干** | 能画出这条链、说出每个函数职责 | 2–4 小时 | **所有节点默认** |
| **L2 细节** | 能答 3 个引导问题 + 有 1 个实验/patch 证据 | +1–2 天 | **只给 M1/M2/M3 三个核心节点** |
| **L3 深挖** | 能分析真实补丁、讲清版本演进 | 2–3 天 | **第一轮全期只做 3 次** |

**L3 配额账本**（做完划掉）：

- [x] ① VMA merge 的 use-after-free（`../exercises/patch_exercise_01_vma_merge_uaf.md`，2026-08-20）
- [ ] ② 待定（建议：fork/rmap 相关的真实修复补丁 → `patch_exercise_02`）
- [ ] ③ 待定（建议：page cache 相关，M2 期间 → `patch_exercise_03`）

> **超配额即停**：第 4 个 L3 出现时，说明在钻牛角尖，回到 L1 推进主线。

---

## 四、使用方式

1. **每次会话开始**：看一眼问题链，确认「我现在在 Q几」，只做这个问题的内容。
2. **每次会话结束**：更新状态与日期；写下「下一个具体动作」（一句话）。
3. **每周检查一次**：有没有越界到档 3？有没有超出 L3 配额？
4. **答完一个 Q**：在状态下写日期 + 记一行「这次学到了什么原来不懂的」。

### 反膨胀三红线

| 红线 | 说明 |
|---|---|
| 不动 `boot/`、`interrupt/`、`scheduler/` | 空目录是未来的债，不是现在的任务 |
| 不重读 `bin的技术小屋` mm5–mm9（5792 行） | 用 E16/E17 验收代替重读 |
| 一次只开一条主线 | M1 期间不开新主题（哪怕看到有趣的文章） |
