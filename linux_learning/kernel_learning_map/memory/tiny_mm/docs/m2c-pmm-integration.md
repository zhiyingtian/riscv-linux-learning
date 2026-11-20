# M2c：把 pmm 切到 buddy + pcp

> 项目总入口：[../README.md](../README.md)
> 前置：[m2-buddy.md](m2-buddy.md) · [m2b-pcp.md](m2b-pcp.md)
> 状态：**✅ 已完成；`make test-m2c` 20/20 通过。**
>
> 验收测试见 [../tests/test_pmm.c](../tests/test_pmm.c)，阶段报告见 [../reports/M2c.md](../reports/M2c.md)。

## 目标

M1 的 `pmm.c` 用 bitmap 扫描分配 order-0 页；M2a/M2b 已经分别完成 buddy 和 pcp。

M2c 把两者接回 `pmm`：

```text
pmm_alloc_pages(order):
    order == 0 → pcp_alloc_pages()
    order > 0  → buddy_alloc()

pmm_free_pages(page, order):
    order == 0 → pcp_free_pages()
    order > 0  → buddy_free()

pmm_check():
    buddy_check() + pcp_check() + zone 计数一致
```

目标不是重写 buddy/pcp，而是让 M1 的 page/PFN/zone API 继续可用，同时底层换成真正的分配器。

## 数据结构

`src/pmm.c` 的全局状态：

```c
struct pmm {
    struct page *pages;
    unsigned long start_pfn;
    unsigned long nr_pages;
    unsigned long free_count;   /* 对上层可见的空闲页数 */
    struct pglist_data node;
    struct buddy buddy;
    struct pcp pcp;
};
```

### `free_count` 的语义

M2c 中：

```text
pmm.free_count = buddy.free_count + pcp.count
zone.free_count = 同一个值
```

原因：

- pcp 里的页不在 buddy 的 `free_area`；
- 但它们仍可被 order-0 分配命中，所以对上层仍算“可用空闲页”；
- 不能只返回 `buddy.free_count`，否则 pcp 缓存会被误算成已分配。

### order 0 free 后的状态

M2c 改变了 M1 的语义：

| 操作 | `is_free` | `refcount` | `state` |
|---|---|---|---|
| alloc order 0 | false | 1 | `PAGE_ALLOCATED` |
| free order 0 | false | 0 | `PAGE_IN_PCP` |
| alloc order > 0 | false | 1 | `PAGE_ALLOCATED` |
| free order > 0 | true | 0 | `PAGE_IN_BUDDY` |

所以 M1 测试里“free 后 `is_free == true`”的断言，在 M2c 要改成检查 pcp 状态。

## 参数

脚手架固定：

```c
#define PMM_PCP_HIGH 8
#define PMM_PCP_BATCH 4
```

`buddy_init()` 要求 `nr_pages` 是 2 的幂。测试使用 64 页。

## 已实现

- `pmm_init()`：
  - 分配 `pages[]`；
  - `buddy_init()`；
  - 初始化 node/zone；
  - 设置每个 page 的 `zone_id`；
  - `pcp_init()`；
  - 计算 `free_count`。
- `pmm_exit()`：
  - 先把 pcp 里的页 drain 回 buddy；
  - 清理 pcp/buddy；
  - 释放 pages。
- `pmm_check()`：
  - `buddy_check()`；
  - `pcp_check()`；
  - 检查 `pcp.buddy == &buddy`；
  - 检查 `pmm.free_count`、`zone.free_count` 与 `buddy.free_count + pcp.count` 一致。
- 高阶分配失败时先 `pcp_drain_all()` 再重试；drain 只搬页，`free_count` 不变。
- M1 的地址转换、zone/pgdat API 保留。

## 实现要点（已完成）

### 1. `pmm_alloc_pages(order)`

实现步骤：

```text
1. order > MAX_ORDER-1 → NULL
2. order == 0:
       page = pcp_alloc_pages(&g_pmm.pcp, 0)
   else:
       page = buddy_alloc(&g_pmm.buddy, order)
       if page == NULL && pcp.count > 0:
           pcp_drain_all(&g_pmm.pcp)
           page = buddy_alloc(&g_pmm.buddy, order)
3. page == NULL → 返回 NULL（不改 free_count）
4. 更新：
       g_pmm.free_count -= 1UL << order
       zone.free_count = g_pmm.free_count
5. 返回 page
```

注意：`pcp_alloc_pages()` 内部负责 refill 和设置 `PAGE_ALLOCATED`，不要再改状态。

### 2. `pmm_free_pages(page, order)`

实现步骤：

```text
1. page 为空/越界 → -EINVAL
2. order == 0:
       rc = pcp_free_pages(&g_pmm.pcp, page, 0)
   else:
       rc = buddy_free(&g_pmm.buddy, page, order)
3. rc != 0 → -EINVAL（不要改 free_count）
4. 更新：
       g_pmm.free_count += 1UL << order
       zone.free_count = g_pmm.free_count
5. 返回 0
```

`pcp_free_pages()` 会检查 double free；
`buddy_free()` 会检查 order 是否匹配、块是否对齐。

### 3. 可选：拆分统计

`struct pmm` 可以增加：

```c
unsigned long alloc_fail;
unsigned long pcp_hit;
unsigned long pcp_drain;
```

如果加，注意 `pcp` 自己已经有 `alloc_hits`/`drain_count`，不要重复统计。

## pmm_check 应该抓到什么

- pcp 链表里的页被错误地塞进 buddy free_area；
- pcp 页 `is_free=true`；
- `buddy.free_count` 和 pcp 计数加起来不等于 `pmm.free_count`；
- order 0 free 后页仍处于 `PAGE_ALLOCATED`；
- order > 0 free 后页仍处于 `PAGE_IN_PCP`。

## 验收目标

`make test-m2c` 当前 20 个测试全部通过：

```text
[PASS] init
[PASS] alloc/free
[PASS] pfn/page roundtrip
[PASS] pfn/paddr roundtrip
[PASS] exhaust
[PASS] double free
[PASS] wrong order
[PASS] order supported
[PASS] zone
[PASS] pgdat
[PASS] random stress
[PASS] free order mismatch
[PASS] high order merge
[PASS] double free high order
[PASS] order0 pcp hit
[PASS] fragmentation
[PASS] exhaust high order
[PASS] order limit
[PASS] high order drains pcp
[PASS] random stress all orders
---------------------------------
all tests passed
```

对应的验收断言（均已覆盖）：

1. 分配 order=2，按 order=0 释放 → 错误；
2. 分配 order=2，按 order=2 释放 → 正确合并；
3. 交替分配制造碎片，高阶分配失败；
4. order-0 批量分配/释放后 pcp 命中（`order0 pcp hit`）；
5. 全部释放后 `pmm_free_count() == nr_pages`；
6. 每次操作后 `pmm_check() == 0`。

M2c 完成后还可以加：

- 水位/回收标记（M2 框架要求）；
- `free_pages[order]` 和 fragmentation 统计。

## 和 Linux 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `pmm_alloc_pages(order)` | `alloc_pages()` / `__alloc_pages()` |
| `pmm_free_pages(page, order)` | `__free_pages()` |
| order 0 → `pcp_alloc_pages()` | `rmqueue_pcplist()` |
| order > 0 → `buddy_alloc()` | `rmqueue_buddy()` / `__rmqueue()` |
| `pmm_check()` | `check_pages_state()` / `page_owner` 类调试检查 |
| `free_count` | `zone->free_area` + pcp 计数 |

M2c 只做“接线”，不引入 migratetype、watermark、GFP flag。

## 下一步

M2c 已完成并填好 [../reports/M2c.md](../reports/M2c.md)。下一步：

1. 给分配器加水位/回收标记；
2. 再做 M3：页表/TLB。
