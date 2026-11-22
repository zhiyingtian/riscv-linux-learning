# M2 学习复盘：从 buddy 到水位

> 适用范围：M2a buddy、M2b pcp、M2c pmm 集成、M2d 水位/回收标记。
>
> 当前结果：M2a 11/11、M2b 9/9、M2c 20/20、M2d 7/7。
>
> 这份文档不是 API 手册，而是**把 M2 的主线、关键不变量、踩过的坑和自测问题串起来**，
> 用于隔一段时间后快速恢复记忆。

---

## 0. 一句话主线

```text
buddy 管“连续块”，pcp 管“order-0 热缓存”，pmm 把两者接起来，
watermark 决定“还能不能继续分配、要不要回收”。
```

如果只能记住五件事：

1. `free_area[order]` 是**块链表**，`nr_free` 是**块数**；`free_count` 是**页数**。
2. 分配是“向上找大块 → split”；释放是“找 buddy → merge 到低地址”。
3. pcp 缓存页**不在** buddy 的 `free_area` 里，所以状态必须是
   `is_free=false / refcount=0 / PAGE_IN_PCP`。
4. `pmm.free_count = buddy.free_count + pcp.count`，
   但水位只看 `buddy.free_count`。
5. 普通分配保留 `WMARK_MIN`；`PMM_ALLOC_HARD` 才能使用 min 预留。

---

## 1. 全景图

```text
                    pmm_alloc_pages(order)
                              │
              ┌───────────────┴────────────────┐
              │                                │
          order == 0                       order > 0
              │                                │
              ▼                                ▼
        ┌──────────┐                    ┌──────────────┐
        │   pcp    │                    │ drain pcp    │
        │ 命中/refill│                   │ (若有缓存页) │
        └────┬─────┘                    └──────┬───────┘
             │                                 │
             └──────────────┬──────────────────┘
                            ▼
                    ┌──────────────┐
                    │    buddy     │
                    │ free_area[]  │
                    │ split/merge  │
                    └──────────────┘

释放方向相反：

order 0  → pcp（超过 high 批量 drain 回 buddy）
order >0 → buddy（尝试与 buddy 合并）
```

水位在 pmm 层判断：

```text
buddy.free_count >= watermark[mark] + (1 << order)  → 水位 OK
```

注意：pcp 不参与水位，但参与 `pmm.free_count`。

---

## 2. 四层职责

| 层 | 主要职责 | 关键不变量 | 验收 |
|---|---|---|---|
| M2a buddy | 按 order 管理连续块，split/merge | `free_count` 与各 `free_area[].nr_free` 一致 | 11/11 |
| M2b pcp | 缓存 order-0 页，refill/high drain | `pcp.count` 等于链表长度；缓存页状态正确 | 9/9 |
| M2c pmm | 把 buddy + pcp 接到 page/zone | `pmm.free_count = buddy.free_count + pcp.count` | 20/20 |
| M2d watermark | min/low/high、回收标记、硬预留 | 水位只看 buddy；low/high 滞回；wakeup 只加一次 | 7/7 |

一个常见的错误是“每层单独看都对，接起来却错”：

- buddy 测试只关心块；
- pcp 测试只关心缓存；
- pmm 测试才会暴露 `free_count`、pcp 挡高阶块、水位和最大 order 的冲突。

所以每次改 pmm，不能只跑新测试，必须跑全量回归。

---

## 3. 数据结构：先把三个计数分开

### 3.1 三个容易混的计数

```text
free_area[o].nr_free   order=o 的空闲“块数”
buddy.free_count       所有空闲“页数”
pcp.count              当前 pcp 缓存“页数”
pmm.free_count         buddy.free_count + pcp.count
```

例子：64 页全部空闲。

```text
buddy.free_count = 64
free_area[6].nr_free = 1      /* 一个 64 页块 */
pcp.count = 0
pmm.free_count = 64
```

拆分一次后：

```text
free_area[5].nr_free = 2
buddy.free_count 仍然是 64
```

**块数不是页数**，这是 buddy 最容易写错的地方之一。

### 3.2 page 的三种位置

| 位置 | `is_free` | `refcount` | `order` | `state` |
|---|---|---|---|---|
| buddy 空闲块头 | true | 0 | 当前 order | `PAGE_IN_BUDDY` |
| pcp 缓存页 | false | 0 | 0 | `PAGE_IN_PCP` |
| 已分配页 | false | 1 | 分配 order | `PAGE_ALLOCATED` |

关键点：

- **pcp 缓存页不是 `is_free=true`**；
- buddy 只认 `free_area` 里的块；
- pcp 页对 buddy 来说像“已分配”，所以不能让 buddy 误以为它在空闲链表里。

### 3.3 pcp 的两个计数

```text
pcp.count    当前缓存页数，必须等于链表长度
pcp.nr_put   累计放进 pcp 的页数，只增不减
```

不能用 `nr_put` 判断 high/drain；它只是统计量。

### 3.4 zone 的水位字段

```c
unsigned long watermark[NR_WMARK];  /* min/low/high */
bool reclaim_needed;
unsigned long reclaim_wakeups;
```

当前测试参数（64 页）：

```text
WMARK_MIN  = 8
WMARK_LOW  = 10
WMARK_HIGH = 12
```

---

## 4. 核心算法

### 4.1 buddy 分配：向上找，向下 split

```text
alloc(order=2)
  → 从 free_area[2] 向上找第一个非空 order
  → 假设找到 order=6 的块
  → 不断把块拆成两半
  → 低半继续作为候选，高半挂到低一级 free_area
  → 直到 order=2，返回低半首页
```

split 地址：

```c
buddy = page + (1UL << (cur - 1));
cur--;
```

拆分时：

- 低半继续；
- 高半入 `free_area[cur]`；
- 两半的 `order` 都更新为 `cur`；
- `split_count++`。

### 4.2 buddy 释放：异或找 buddy，向低地址合并

```c
buddy_index = index ^ (1UL << order);
```

能合并的条件：

- buddy 在范围内；
- buddy 是**同 order** 的空闲块头；
- buddy 在对应的 `free_area[order]` 链表中。

合并后：

- 块头指向低地址的一半；
- `order++`；
- `merge_count++`；
- 继续找更大的 buddy。

`free_count` 加的是**原始释放页数**：

```c
free_count += 1UL << freed_order;
```

不是合并后的大块页数。

### 4.3 pcp：order-0 的批量缓存

```text
alloc order 0:
    pcp 有页 → 命中
    pcp 空   → 从 buddy refill batch 页，再取一页
    buddy 空 → NULL

free order 0:
    放回 pcp
    count >= high → 批量 drain 回 buddy

order > 0:
    绕过 pcp，直接走 buddy
```

当前参数：

```text
PCP_HIGH  = 8
PCP_BATCH = 4
```

refill 4 页后取 1 页，所以 pcp 通常留下 3 页。

### 4.4 pmm：接线和总账

```text
order 0  → pcp
order > 0 → buddy
高阶分配前先 drain pcp
free_count = buddy.free_count + pcp.count
```

为什么高阶分配前要 drain pcp？

因为 pcp 里的页虽然“空闲”，但不在 buddy 的 `free_area` 中，
buddy 不能把它们当作连续块的一部分。drain 只是把页从 pcp 搬回 buddy：

```text
buddy.free_count ↑
pcp.count ↓
pmm.free_count 不变
```

所以 drain 本身不需要改 `pmm.free_count`。

### 4.5 水位：门槛 + 滞回

公式：

```text
buddy.free_count >= watermark[mark] + (1 << order)
```

为什么加 `(1 << order)`：

- 水位是“分配后还要保留的页”；
- 当前这次分配也要占用页；
- 所以必须同时满足“本次需求 + 水位”。

状态机：

```text
free < low          → reclaim_needed = true，第一次进入时 wakeups++
low <= free < high  → 保持原状态（滞回）
free >= high        → reclaim_needed = false
```

`reclaim_wakeups` 只在 `false -> true` 时加一，不能在每次 alloc/free 都加。

### 4.6 普通分配 vs hard 分配

```c
pmm_alloc_pages(order);                              /* 普通，保留 min */
pmm_alloc_pages_flags(order, PMM_ALLOC_HARD);        /* 可使用 min */
```

为什么需要 hard 分配：

```text
普通分配要求保留 WMARK_MIN = 8
order=6 需要 64 页
min + 64 = 72 > 64
```

所以普通 `pmm_alloc_pages(6)` 在 64 页内存上**永远无法成功**。
hard 分配相当于 Linux 的 `ALLOC_HARDER` / 原子路径，可以使用预留。

这不是“测试作弊”，而是把“普通分配”和“不能失败/原子分配”两种语义分开。

---

## 5. 为什么这样设计

### Q1：为什么 `free_count` 包含 pcp，但水位不包含？

- `free_count` 是“系统还有多少可用空闲页”，pcp 里的页可以被 order-0 分配命中，所以算；
- 水位判断的是“buddy 还能不能满足带 order 的连续分配”，pcp 页不在 `free_area`，不能算。

一句话：

```text
总账包含 pcp；水位只看 buddy。
```

### Q2：为什么 pcp 页的 `is_free` 是 false？

如果设成 true，`buddy_free()` 会以为它是一个 buddy 空闲块头，去 `free_area` 里找它；
但它实际上只在 pcp 链表里，结果就是“状态说空闲，链表找不到”，释放失败或破坏状态。

pcp 用 `state=PAGE_IN_PCP` 表示位置，不用 `is_free`。

### Q3：为什么高阶分配失败后不能只看“总空闲页”？

总空闲页可能很多，但都在 pcp 或碎片里：

```text
buddy 空闲页数够不够？  → 决定能否组成连续块
pcp 里有多少页？        → 可以先 drain 回 buddy
是否有足够大的 order？  → 决定 split 是否成功
```

所以 pmm 的检查顺序是：

```text
drain pcp
→ 检查水位
→ buddy 分配
```

### Q4：为什么水位检查要放在分配之前？

因为要保证分配后仍然保留 min/low：

```text
分配前：free >= mark + need
分配后：free >= mark
```

如果先分配再检查，可能已经吃掉了预留页。

### Q5：为什么 low 和 high 要分开？

如果只有一个阈值：

```text
free 刚好在边界抖动 → 反复进入/退出回收
```

low/high 形成滞回区：

```text
free < low   → 标记回收
free >= high → 才清除
中间保持原状态
```

这样避免回收状态抖动。

---

## 6. 踩过的坑

这些是 M2 里真实出现过的 bug，不只是理论。

### 6.1 buddy 合并方向错误

现象：merge 循环不收敛，甚至死循环。

原因：合并后块头没有指向低地址的一半。

修复：

```text
合并后 page 指向低地址块，并更新 order
```

### 6.2 buddy 的 order 不同还继续合并

现象：order 一直不前进。

原因：buddy 不是同 order 的空闲块头，代码仍然继续 merge。

修复：

```text
不同 order / 不在 free_area → 停止 merge
把 page 挂回当前 order 的 free_area
```

### 6.3 pcp 页用 `is_free=true`

现象：`buddy_free()` 认为 buddy 空闲，但链表里找不到。

修复：

```text
pcp 缓存页：is_free=false, refcount=0, state=PAGE_IN_PCP
```

### 6.4 pcp 计数和链表不同步

现象：`pcp_check()` 失败。

修复：

- 统一用 `pcp_pop()` 摘页；
- 每次摘页都 `count--`；
- refill/high drain 后同步 count。

### 6.5 pmm 双重扣减 buddy.free_count

错误写法：

```c
page = buddy_alloc(&g_pmm.buddy, order);
g_pmm.buddy.free_count -= 1UL << order;  /* 错：buddy_alloc 已经扣过 */
```

正确写法：

```c
page = buddy_alloc(&g_pmm.buddy, order);
g_pmm.free_count -= 1UL << order;        /* 只维护 pmm 总账 */
```

记住：**buddy 内部计数由 buddy 自己维护，pmm 只维护总账。**

### 6.6 用 bool 和水位比较

错误写法：

```c
if (pmm_watermark_ok(order, WMARK_MIN) < zone.watermark[WMARK_MIN])
    return NULL;
```

`pmm_watermark_ok()` 返回 `bool`，不是空闲页数。

正确：

```c
if (!pmm_watermark_ok(order, WMARK_MIN))
    return NULL;
```

### 6.7 水位 min 直接打断 M2c

现象：M2c 的 `exhaust`、`fragmentation`、`exhaust high order`、
`high order drains pcp`、`random stress all orders` 失败。

原因：

```text
普通分配强制保留 min
→ 不能分配全部 64 页
→ order=6 需要 min + 64 页，更不可能
```

修复：

```text
普通分配保留 min
hard 分配使用 PMM_ALLOC_HARD 绕过 min
```

### 6.8 水位检查在 pcp drain 之前

现象：pcp 有页，但高阶分配提前被水位拒绝。

修复：

```text
order > 0 先 drain pcp
再检查水位
再分配
```

### 6.9 drain 改变 buddy 空闲页后没有更新回收标记

现象：`pmm_check()` 发现 `reclaim_needed` 与 `buddy.free_count` 不一致。

修复：

- drain 后立即 `pmm_update_reclaim_state()`；
- 分配失败路径也要更新；
- `pmm_check()` 增加标记一致性检查。

### 6.10 直接写 `reclaim_needed = true`

现象：`reclaim_wakeups` 可能不增加，或者状态转移绕过统计。

修复：

```text
所有 false -> true 都走 pmm_update_reclaim_state()
每次进入 low 只 wakeup 一次
```

---

## 7. 测试地图

| 测试 | 数量 | 主要保护什么 |
|---|---:|---|
| `make test-m2` | 11 | buddy split/merge、double free、错 order、对齐、随机压力 |
| `make test-m2b` | 9 | pcp refill/hit/high drain、状态、order>0 bypass |
| `make test-m2c` | 20 | pmm 接线、总账、高阶、碎片、最大块恢复 |
| `make test-m2d` | 7 | 水位、pcp 排除、hard 预留、回收滞回 |

全量回归：

```bash
make test-all
```

每次改 pmm 后至少跑：

```bash
make test-m2c
make test-m2d
make test-m2
make test-m2b
```

---

## 8. 调试顺序

遇到 pmm 问题时，按这个顺序查：

```text
1. pmm_check() 是否通过？
2. free_count 是否等于 buddy.free_count + pcp.count？
3. buddy_check() / pcp_check() 是否通过？
4. page 的 state/refcount/order 是否匹配当前位置？
5. pcp 是否挡住了高阶分配？先 drain 试试。
6. 水位比较是否用了正确的 free（buddy，不是 total）？
7. 分配失败后，回收标记是否同步？
```

常用命令：

```bash
make test-m2c
make test-m2d
make debug-m2d
```

调试时优先看：

```text
buddy.free_count
pcp.count
free_area[o].nr_free
page->state / refcount / order
zone.reclaim_needed / reclaim_wakeups
```

---

## 9. 白板自测

合上文档，回答这些问题。

### 基础

1. `free_area[3].nr_free == 2` 表示多少页？
   2^3*2=16个页
2. `buddy.free_count` 和 `free_area[o].nr_free` 分别是什么单位？
   buddy是空闲page的数量，nr_free是o阶下的空闲块数量
3. 分配 order=2 时，split 出来的高半挂到哪里？
   挂到order=1的free area上面
4. 释放时 buddy 地址怎么算？
   用异或运算 buddy_index = index ^ (1UL << order)
5. 为什么合并后 `free_count` 加的是原始释放页数？
   buddy统计的是空闲页数量，合并后的增量只有释放的页数
   update: 合并只是改变块的组织方式，释放只归还原始块的大小

### pcp

6. pcp 缓存页的 `is_free/refcount/state` 是什么？
   is_free表示当前页是否被分配
   refcount表示当前页被引用的多少次
   state表示当前页的状态
7. refill 4 页、取 1 页后，`pcp.count` 是多少？
   3
8. pcp count 达到 high 后发生什么？
   一次性排空batch个page到buddy
9.  为什么 order>0 不经过 pcp？
    pcp只缓存了order=1的page，用于不经过buddy的快速分配
10. `pcp.nr_put` 和 `pcp.count` 有什么区别？
    nr_put表示往pcp里面填充了多少次page
    count表示pcp中的page数量

### pmm

11. `pmm.free_count` 的公式是什么？
    buddy.free_count + pcp.count
12. 高阶分配前为什么要 drain pcp？
    为了避免缓存的page会导致page不连续，从而造成系统没有高阶page块而分配失败
    update:可能挡住连续块
13. `buddy_alloc()` 成功后，pmm 还要不要扣 `buddy.free_count`？
    不需要，pmm和buddy各司其职，各自维护自己的状态
14. 分配失败时能不能修改 `free_count`？
    不能
15. `pmm_check()` 检查哪几条关键不变量？
    page是否为空，是否越界，page的状态是否正确

### 水位

16. `pmm_watermark_ok(order, mark)` 的完整公式是什么？
    buddy.free_count >= watermark[mark] + (1UL << order);
17. 为什么水位不包含 pcp？
    水位机制只针对buddy
    update:pcp页不在buddy，不能参与连续块的分配
18. `reclaim_wakeups` 什么时候增加？
    在reclaim_need为false的时候
19. low 和 high 之间为什么要保持原状态？
    避免在阈值之间反复进入/退出而出现抖动
20. 普通分配和 `PMM_ALLOC_HARD` 的区别是什么？
    普通分配会受到水位线的控制，但HARD会无视水位线，只要系统有可分配的空间就能分配成功
    update:普通分配保留min，hard可以分配min的预留page
21. 为什么 64 页内存上普通 order=6 分配不可能成功？
    考虑到水位线，普通分配需要free>water[low]+64，但总共才64页

### 参考答案要点

1. 2 个 order=3 块，即 `2 * 8 = 16` 页。
2. `free_area.nr_free` 是块数；`buddy.free_count` 是页数。
3. `free_area[1]`，order=1；split 时高半入低一级。
4. `buddy_index = index ^ (1UL << order)`。
5. 释放只归还了原始块的大小，合并只是改变块的组织方式。
6. `is_free=false`，`refcount=0`，`state=PAGE_IN_PCP`。
7. 3 页。
8. 批量 drain 回 buddy，`pcp.count` 减少。
9. pcp 只缓存 order-0；高阶需要 buddy 的连续块。
10. `count` 是当前缓存页数；`nr_put` 是累计放入页数。
11. `buddy.free_count + pcp.count`。
12. pcp 页不在 buddy 的 `free_area` 里，可能挡住连续块。
13. 不需要；buddy 内部已经扣过。
14. 不能；失败路径不能改总账。
15. 总账、buddy/pcp 自检、page 状态、水位顺序、回收标记一致性。
16. `buddy.free_count >= watermark[mark] + (1 << order)`。
17. pcp 页不在 `free_area`，不能参与连续块分配。
18. 只在 `reclaim_needed` 从 false 变成 true 时。
19. 避免在阈值附近反复进入/退出回收。
20. 普通分配保留 min；hard 分配可以使用 min 预留。
21. 普通分配要求 `free >= 8 + 64`，但总共只有 64 页。

---

## 10. 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 | 说明 |
|---|---|---|
| `free_area[order]` | `zone.free_area[order].free_list[]` | Linux 还按 migratetype 分链 |
| `buddy_alloc()` | `__rmqueue_smallest()` / `expand()` | 向上找块并 split |
| `buddy_free()` | `__free_one_page()` | 释放并 merge |
| `buddy_index = index ^ ...` | `__find_buddy_pfn()` | 异或找 buddy |
| `pcp` | `per_cpu_pages` | Linux 还有 order/migratetype |
| `pcp_drain()` | `free_pcppages_bulk()` | 批量归还 |
| `pmm_alloc_pages()` | `alloc_pages()` | 入口 |
| `pmm_free_pages()` | `__free_pages()` | 释放 |
| `WMARK_MIN/LOW/HIGH` | `enum zone_watermarks` | 三档水位 |
| `pmm_watermark_ok()` | `zone_watermark_ok()` | 还考虑 alloc_flags 等 |
| `PMM_ALLOC_HARD` | `ALLOC_HARDER` / `GFP_ATOMIC` | 简化模拟 |

tiny-mm 没有模拟：

- migratetype / fallback；
- NUMA；
- reclaim/compaction/OOM；
- watermark_boost；
- per-CPU 并发和锁。

---

## 11. 还没完全弄清楚的问题

1. Linux 的高阶分配失败后，reclaim、compaction、pcp drain 的真实顺序是什么？
2. `watermark_boost` 如何影响碎片化场景下的水位判断？
3. pcp 在真实多核下如何避免竞争？`free_pcppages_bulk()` 的批量策略为什么这样设计？
4. `free_area` 按 migratetype 分链后，fallback 如何影响碎片和分配成功率？
5. `WMARK_MIN` 的预留和 `GFP_ATOMIC` 的关系，在真实 `__alloc_pages()` 中如何体现？

---

## 12. 15 分钟复习清单

- [x] 画出 `pmm_alloc_pages` 的 order 0 / order >0 两条路径
- [x] 画出 buddy split 和 merge 的 64 页例子
- [x] 说出三种 page 状态的字段
- [x] 说出三个计数的区别
- [x] 写出 `pmm_watermark_ok()` 公式
- [x] 解释为什么水位不包含 pcp
- [x] 解释普通分配和 hard 分配
- [x] 说出两个真实踩过的 bug 和修复方式
- [x] 跑一遍 `make test-all`
- [x] 不看文档回答第 9 节的 21 个问题

如果这 10 项都能完成，M2 的主线就已经掌握了。
