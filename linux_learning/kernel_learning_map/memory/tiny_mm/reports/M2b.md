# M2b 阶段报告

> 已完成。重点记录 pcp 的 refill/high/drain 状态变化。

## 1. 我实现了什么

- [x] `pcp_alloc_pages()`：命中、refill、miss
- [x] `pcp_free_pages()`：入缓存、double free 校验
- [x] `pcp_drain()`：恢复 page 状态后还 buddy
- [x] high 触发批量 drain
- [x] order > 0 绕过 pcp
- [x] `pcp_check()` 全部通过

## 2. 测试结果

```text
$ make test-m2b
[PASS] init
[PASS] refill
[PASS] pcp hit
[PASS] free to pcp
[PASS] high drain
[PASS] order > 0 bypass
[PASS] explicit drain
[PASS] invalid
[PASS] random stress
---------------------------------
all tests passed
```

## 3. refill/high/drain 画图

```text
初始：pcp count=0, buddy free=256

第一次 alloc order0：
    pcp 空 → buddy refill 4 页
    pcp count=3, buddy free=252
    返回 1 页

第二次 alloc order0：
    pcp 命中
    pcp count=2, buddy free=252
```

自己补全：

```text
free 到 count=8 时发生什么？
A: 自动将 pcp 中 batch=4 个 page 释放给 buddy，count 变为 4

drain 4 页后 count 是多少？
A: 如果 drain 前 count=8，则 count=4；如果 pcp 中不足 4 页，则 drain 后为 0

drain 前 page 的 refcount/is_free/state 要设置成什么？
A: is_free=false，refcount=1，state=PAGE_ALLOCATED
```

## 4. 我踩的坑

| 坑 | 现象 | 怎么发现 | 怎么修 |
|---|---|---|---|
| pcp 页用 `is_free=true` 表示缓存 | `buddy_free()` 看到 buddy `is_free=true`，但 buddy 不在 `free_area`，释放失败 | 讨论 drain 的 buddy 也在 pcp 的场景 | pcp 缓存页改成 `is_free=false`，缓存状态由 state/链表表示 |
| refill 后取出一页忘记 `count--` | `pcp_check()` 链表长度和 count 不一致 | `make test-m2b` 的 refill 失败 | 抽出统一的 `pcp_pop()`，每次摘页都减 count |
| order > 0 被错误标成 PCP | order 1 返回页 refcount=0、state=PAGE_IN_PCP | 用临时 probe 打印 state | order > 0 直接 return buddy_alloc() |
| drain 前没有把页转回已分配 | `buddy_free()` 拒绝或状态残留 | `pcp_drain()` 调试 | drain 前设 `PAGE_ALLOCATED/refcount=1`，成功后转 `PAGE_IN_BUDDY` |
| pcp 页 state 未校验 | 错误状态也能通过测试 | 补 state 断言 | `pcp_check()` 只接受 `PAGE_IN_PCP` |

## 5. 六个 bug 注入实验

- [ ] refill 后不更新 count → `refill` 失败
- [ ] 命中时不摘链表头 → `pcp hit` 失败
- [ ] free 时不检查 double free → `free to pcp` 失败
- [ ] high 触发后不 drain → `high drain` 失败
- [ ] drain 前不恢复 page 状态 → `explicit drain` 失败
- [ ] order > 0 也走 pcp → `order > 0 bypass` 失败

> 这 6 项需要逐项实际注入验证；代码通过测试不代表已经手动做过实验。

## 6. 和 Linux 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `struct pcp` | `struct per_cpu_pages`（`include/linux/mmzone.h:360`） |
| `pcp->count` | `per_cpu_pages.count` |
| `pcp->high/batch` | `per_cpu_pages.high/batch` |
| `pcp->list` | `per_cpu_pages.lists[]` 中按 order/migratetype 分链 |
| `pcp_alloc_pages()` | `rmqueue_pcplist()` / `__rmqueue_pcplist()` |
| `pcp_free_pages()` | `free_unref_page_commit()` |
| `pcp_drain()` | `free_pcppages_bulk()` |
| `order > 0` 绕过 pcp | `pcp_allowed_order()` / `rmqueue()` |

Linux 的 pcp 还多出 NUMA、migratetype、free_factor、水位/回收影响 high 等细节；M2b 只保留核心 refill/hit/free/drain。

## 7. 还没懂的问题

暂无。

## 8. 进入 M2c 前的一句话总结

> pcp 的本质是：为指定 order 预先向 buddy 申请一批页并缓存在每 CPU 链表中；分配时优先从缓存取，缓存空了批量 refill，缓存超过高水位时批量 drain，从而减少 buddy 锁竞争和链表操作。
