# M2c 阶段报告

> 状态：脚手架已搭好，`pmm_alloc_pages()` / `pmm_free_pages()` 待实现。

## 1. 我实现了什么

- [x] `pmm_init()`：初始化 buddy + pcp
- [x] `pmm_exit()`：drain pcp 后清理
- [x] `pmm_check()`：检查 buddy + pcp + free_count
- [ ] `pmm_alloc_pages()`：order 0 走 pcp，order > 0 走 buddy
- [ ] `pmm_free_pages()`：order 0 走 pcp，order > 0 走 buddy
- [ ] 高阶 split/merge 回归
- [ ] 碎片化与随机压力
- [ ] 水位/回收标记

## 2. 当前测试结果

```text
$ make test-m2c
[PASS] init
[FAIL] alloc/free
[PASS] pfn/page roundtrip
[PASS] pfn/paddr roundtrip
[FAIL] exhaust
[FAIL] double free
[FAIL] wrong order
[FAIL] order supported
[FAIL] zone
[FAIL] pgdat
[FAIL] random stress

8 test(s) failed
```

实现 alloc/free 后应变成 11/11 全绿。

## 3. 关键设计

```text
pmm.free_count = buddy.free_count + pcp.count
```

- pcp 缓存页不在 buddy 的 free_area；
- 但它们仍可被 order-0 命中，所以算“可用空闲页”；
- order 0 free 后页是 `PAGE_IN_PCP`，不是 `PAGE_IN_BUDDY`。

## 4. 我踩的坑

| 坑 | 现象 | 怎么发现 | 怎么修 |
|---|---|---|---|
|  |  |  |  |
|  |  |  |  |

## 5. 待做验证

- [ ] 分配 order=2，按 order=0 free → 返回错误
- [ ] 分配 order=2，按 order=2 free → 合并回 order 2
- [ ] 交替分配制造碎片，高阶分配失败
- [ ] 10 万次随机 alloc/free，最终所有页回到 buddy/pcp
- [ ] 每次操作后 `pmm_check() == 0`

## 6. 和 Linux 对照

- `pmm_alloc_pages()` ↔ `alloc_pages()`
- `pmm_free_pages()` ↔ `__free_pages()`
- order 0 ↔ `rmqueue_pcplist()`
- order > 0 ↔ `__rmqueue()` / buddy
- `pmm_check()` ↔ 调试用 page 状态检查

## 7. 还没懂的问题

待填。

## 8. 进入下一阶段前的一句话总结

待填。
