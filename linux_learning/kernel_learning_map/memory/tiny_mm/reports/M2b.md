# M2b 阶段报告

> 填完再进入 M2c。重点写 pcp 的 refill/high/drain 状态变化。

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
A: 自动将pcp中的batch个page释放给buddy
drain 4 页后 count 是多少？
A: 如果有pcp里面有不超过4个page，则最后为0，否则为N-4
drain 前 page 的 refcount/is_free 要设置成什么？
A: page中新增了state，所以drain前is_free保持false不变，refcount=1, state=PAGE_ALLOCATED
```

## 4. 我踩的坑

| 坑 | 现象 | 怎么发现 | 怎么修 |
|---|---|---|---|
|  |  |  |  |
|  |  |  |  |

## 5. 六个 bug 注入实验

- [ ] refill 后不更新 count → `refill` 失败
- [ ] 命中时不摘链表头 → `pcp hit` 失败
- [ ] free 时不检查 double free → `free to pcp` 失败
- [ ] high 触发后不 drain → `high drain` 失败
- [ ] drain 前不恢复 page 状态 → `explicit drain` 失败
- [ ] order > 0 也走 pcp → `order > 0 bypass` 失败

## 6. 和 Linux 对照

- `struct per_cpu_pages`：
- `rmqueue_pcplist()`：
- `free_unref_page_commit()`：
- `free_pcppages_bulk()`：
- `pcp_allowed_order()`：

## 7. 还没懂的问题

NA
## 8. 进入 M2c 前的一句话总结

> pcp 的本质是：__指定order下，提前向buddy申请batch个页面的一个cache数组，在符合order条件下，可以跳过buddy系统直接分配，加快分配速度_
