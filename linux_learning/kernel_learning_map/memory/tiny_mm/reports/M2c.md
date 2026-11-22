# M2c 阶段报告

> 状态：已完成。`make test-m2c` 20/20 通过。

## 1. 我实现了什么

- [x] `pmm_init()`：初始化 buddy + pcp
- [x] `pmm_exit()`：drain pcp 后清理
- [x] `pmm_check()`：检查 buddy + pcp + free_count
- [x] `pmm_alloc_pages()`：order 0 走 pcp，order > 0 走 buddy
- [x] `pmm_free_pages()`：order 0 走 pcp，order > 0 走 buddy
- [x] 高阶 split/merge 回归：order=2/order=3，释放后能合并回 order-6
- [x] 碎片化与随机压力：10 万次 order 0..6，每次操作后 `pmm_check()`
- [x] 高阶分配前 drain pcp，失败时不再重试
- [x] 水位/回收标记（M2d 已完成；M2c 全量测试使用 `PMM_ALLOC_HARD`）

## 2. 当前测试结果

```text
$ make test-m2c
tiny-mm / M2c: pmm with buddy + pcp
---------------------------------
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

## 3. 关键设计

```text
pmm.free_count = buddy.free_count + pcp.count
```

- pcp 缓存页不在 buddy 的 free_area；
- 但它们仍可被 order-0 命中，所以算“可用空闲页”；
- order 0 free 后页是 `PAGE_IN_PCP`，不是 `PAGE_IN_BUDDY`；
- 高阶分配失败时先 `pcp_drain_all()` 再重试：drain 只是在 buddy 和 pcp
  之间搬页，`buddy.free_count + pcp.count` 不变，所以 `free_count` 不用改。

## 4. 我踩的坑

| 坑 | 现象 | 怎么发现 | 怎么修 |
|---|---|---|---|
| pcp 缓存页挡住高阶分配 | 全部释放后 `free_count=64`，order-6 仍返回 NULL | fragmentation 末尾加“最大块恢复”断言 | 高阶分配前先 drain pcp |
| 随机测试只覆盖 order 0 | 高阶 split/merge 没被随机压力覆盖 | 对照 M2 框架验收第 1 条 | 新增 order 0..6 的 10 万次随机测试 |
| 错 order/非块头释放没有用例 | 只测了 order-0 按 order-1 free | 对照 M2c 文档 §5 | 新增 `free order mismatch` |

## 5. 待做验证

- [x] 分配 order=2，按 order=0 free → 返回错误
- [x] 分配 order=2，按 order=2 free → 正确合并（64 页场景合并回 order-6）
- [x] 交替分配制造碎片，高阶分配失败
- [x] 10 万次随机 alloc/free，最终所有页回到 buddy/pcp
- [x] 每次操作后 `pmm_check() == 0`

## 6. 和 Linux 对照

- `pmm_alloc_pages()` ↔ `alloc_pages()`
- `pmm_free_pages()` ↔ `__free_pages()`
- order 0 ↔ `rmqueue_pcplist()`
- order > 0 ↔ `__rmqueue()` / buddy
- `pmm_check()` ↔ 调试用 page 状态检查

## 7. 还没懂的问题

- Linux 在高阶分配失败后走 reclaim/compaction，而不是直接 drain pcp；
  tiny-mm 采用“高阶分配前先 drain pcp”的简化策略，后续是否需要和 Linux 对齐？
- M2d 已完成水位/回收标记：min/low/high、`PMM_ALLOC_HARD` 与 `GFP_ATOMIC` 的简化模拟。

## 8. 进入下一阶段前的一句话总结

M2c 已完成：order 0 走 pcp、order > 0 走 buddy，`free_count` 与
`buddy.free_count + pcp.count` 保持一致，20/20 测试通过；水位/回收标记已在 M2d 完成，
下一步进入 M3 页表。
