# learn_page_alloc —— 用观测学 buddy 分配路径

> **目标**：用 ftrace 把 `alloc_zeroed_user_highpage_movable → __alloc_pages → rmqueue → pcp` 这条链抓成真实事件，验证 `phy_mem_allocate.c` 里的框架。
> **范围**：只做**快路径**（`get_page_from_freelist` 以内）。slowpath / reclaim / OOM 属于 Q7（M3），本实验不碰。
> **前置**：`phy_mem_allocate.c`（框架）、`../../plans/memory_question_chain.md` 的 Q3。
>
> ## ⚠️ 版本前提（先读）
>
> 本机宿主内核是 **7.0**，而源码学习目标是 **5.15**。两者在 page allocator 上已经分叉：
>
> | | 5.15（你学的源码） | 7.0（本机实际运行的） |
> |---|---|---|
> | 入口符号 | `__alloc_pages` | `__alloc_pages_noprof`（6.8+ profiling hooks） |
> | pcp 分层 | `rmqueue_pcplist → __rmqueue_pcplist` | 已内联/重构，**不可跟踪** |
> | 初始化 | `prep_new_page` | 已改名/内联 |
> | tracepoint 字段 | `order/migratetype/gfp_flags` | **相同**，参数类结论通用 |
>
> **结论**：
> - **参数类实验（实验 1）跨版本有效** —— order / migratetype / gfp_flags 的定义和打印格式没变。
> - **调用链实验（实验 2/3）在 7.0 上只能看骨架** —— 看不到 `rmqueue_pcplist / prep_new_page`。想逐行对照 5.15，需在 qemu 里跑 `runninglinuxkernel_5.15`。
> - 7.0 的匿名页 gfp 多了 `__GFP_COMP`（5.15 的 `GFP_HIGHUSER_MOVABLE` 不含），别拿它当 5.15 的结论。

---

## 文件

| 文件 | 作用 |
|---|---|
| `trigger_alloc.c` | 触发匿名缺页分配（逐页写），打印 `MARK` 时间线 |
| `trace_events.sh` | 抓 `mm_page_alloc` tracepoint，统计 order / migratetype / gfp_flags |
| `trace_graph.sh` | 抓 `__alloc_pages` 的 function_graph 调用链 |

---

## 实验 1：抓一次真实分配（tracepoint）

```bash
sudo ./trace_events.sh 64
```

**实测参考**（2026-10-25，宿主 7.0，8 页触发）：

```
事件数:10  =  8 次匿名缺页 + 2 次杂项
8 条  order=0  migratetype=1  gfp_flags=GFP_HIGHUSER_MOVABLE|__GFP_ZERO|__GFP_COMP
2 条  order=0  migratetype=0  gfp_flags=GFP_NOWAIT        ← 进程启动/清理,不是缺页
```

**预期现象**（对照 `phy_mem_allocate.c` 的框架）：

| 观测项 | 预期值 | 对应框架的哪一句 |
|---|---|---|
| 事件数 | ≈ N + 少量杂项 | `do_anonymous_page` 每页调用一次 |
| `order=` | `0` | `alloc_page_vma` 宏传入 order=0 |
| `migratetype=` | `1`（Movable） | `GFP_HIGHUSER_MOVABLE` |
| `gfp_flags=` | 含 `MOVABLE` + `__GFP_ZERO` | `alloc_zeroed_user_highpage_movable` |

**引导问题**

- **P1**：如果 `migratetype` 是 `0`（Unmovable）而不是 `1`，说明什么？（提示：是不是走了 `GFP_KERNEL` 路径？）
- **P2**：为什么 `/proc/vmstat` 里的 `pgalloc_movable` 一直是 0，但这里 migratetype 明明是 1？两个 "movable" 是一回事吗？
- **P3**：用 `/proc/pagetypeinfo`（需 root）看 MIGRATE_MOVABLE 的 order-0 空闲块数，跑程序前后各看一次，差值说明什么？

---

## 实验 2：抓内部调用链（function_graph）

```bash
sudo ./trace_graph.sh 8
```

**预期调用链**（和 `phy_mem_allocate.c` 对照）：

```
__alloc_pages()
  prepare_alloc_pages()
  get_page_from_freelist()
    zone_watermark_fast()
    rmqueue()
      rmqueue_pcplist()
        __rmqueue_pcplist()
          rmqueue_bulk()      ← 首次 pcp 为空时从 buddy 批量补充
  prep_new_page()
```

**引导问题**

- **P4**：`rmqueue_bulk` 出现了吗？如果出现，说明 pcp 空时的行为是什么？和你框架里写的"返回 NULL 后继续从 freelist 分配"一致吗？
- **P5**：`__rmqueue` / `__rmqueue_smallest` 出现几次？为什么 order=0 时一次都不该出现？
- **P6**：`__alloc_pages_slowpath` 出现几次？如果出现，说明机器当时内存紧张——换一台空闲机器再跑对比。
- **P7**：`prep_new_page` 在 `rmqueue` 之后、返回之前——它对 GFP_ZERO 的请求做了什么？（提示：找 `prep_new_page` 里的 `want_init_on_alloc` / `__GFP_ZERO` 分支）

---

## 实验 3：PCP 的批量行为（进阶）

同一页数跑多次，观察 `rmqueue_bulk` 的**调用次数 vs 分配页数**：

```bash
sudo ./trace_graph.sh 32 2>&1 | grep -E "rmqueue_bulk|prep_new_page"
```

**引导问题**

- **P8**：32 页分配，`rmqueue_bulk` 大概被调几次？`pcp->batch` 的值是多少（在 `__rmqueue_pcplist` 里 `READ_ONCE(pcp->batch)`）？为什么 batch 要 `>> order`？
- **P9**：第二次运行同样的程序，`rmqueue_bulk` 次数会变少吗？为什么（pcp 里还有上次剩下的页）？

---

## 实验 4：高阶分配（观察，不需要自己触发）

系统上本来就有 order>3 的分配（THP、slab 大对象、网络缓冲区）。用 `--all` 模式抓全系统：

```bash
sudo ./trace_events.sh --all 5
```

**实测参考**（2026-10-25，宿主 7.0，采集 5 秒内 26429 条）：

```
order 分布:  26152 个 order=0 / 137 order=3 / 123 order=2 / 14 order=1
             1 个 order=9(THP) / 1 个 order=5 / 1 个 order=4
migratetype: 25108 Movable / 1260 Unmovable / 61 Reclaimable / 0 HighAtomic
gfp 榜首:    GFP_HIGHUSER_MOVABLE|__GFP_ZERO|__GFP_COMP  19392 次
```

**引导问题**

- **P10**：你能抓到 `order>=4` 的事件吗？它们的 `migratetype` 是什么？有没有出现过 `3`（HighAtomic）？
- **P11**：`MIGRATE_HIGHATOMIC` 为什么只服务高阶？（提示：order≤3 在 `rmqueue` 里 `goto out` 就返回了）
- **P12**：既然 order=4/5/9 都出现了，为什么 HighAtomic 一次都没出现？它在什么情况下才会被用到？

---

## 答案填写区（先自己跑完再写）

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

---

## 已确认需要修正的框架细节（跑完后逐条验证）

| # | `phy_mem_allocate.c` 原描述 | 5.15 实际 |
|---|---|---|
| 1 | "rmqueue_pcplist 返回 NULL 后继续从 freelist 分配" | `__rmqueue_pcplist` 内部先用 `rmqueue_bulk` 从 buddy 批量补充；补充失败才返回 NULL，`rmqueue` 直接 `goto out`，**不走** `__rmqueue` |
| 2 | `if (order > 0 && alloc_flags & ALLOC_HARDER)` 是普通分支 | 只在 **order > 3** 时可达（order≤3 已被 `pcp_allowed_order` 的 `goto out` 拦截） |
| 3 | `/proc/vmstat` 的 `pgalloc_movable` 可验证 movable | `pgalloc_*` 按 **zone** 统计，`pgalloc_movable` = `ZONE_MOVABLE`（通常为 0），与 `MIGRATE_MOVABLE` 无关 |

---

## 源码锚点（5.15）

```
include/linux/highmem.h:166        alloc_zeroed_user_highpage_movable
include/linux/gfp.h:599/605        alloc_pages_vma(!NUMA 退化) / alloc_page_vma
mm/page_alloc.c:5131               prepare_alloc_pages
mm/page_alloc.c:5343               __alloc_pages
mm/page_alloc.c                   get_page_from_freelist(retry: / try_this_zone:)
mm/page_alloc.c:3602               __rmqueue_pcplist(含 rmqueue_bulk)
mm/page_alloc.c:3642               rmqueue_pcplist
mm/page_alloc.c:2423               prep_new_page
include/trace/events/kmem.h        TRACE_EVENT(mm_page_alloc)
```
