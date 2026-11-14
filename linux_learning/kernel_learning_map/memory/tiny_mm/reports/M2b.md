# M2b 阶段报告

> 填完再进入 M2c。重点写 pcp 的 refill/high/drain 状态变化。

## 1. 我实现了什么

- [ ] `pcp_alloc_pages()`：命中、refill、miss
- [ ] `pcp_free_pages()`：入缓存、double free 校验
- [ ] `pcp_drain()`：恢复 page 状态后还 buddy
- [ ] high 触发批量 drain
- [ ] order > 0 绕过 pcp
- [ ] `pcp_check()` 全部通过

## 2. 测试结果

```text
$ make test-m2b
# 粘贴最后 20 行
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
drain 4 页后 count 是多少？
drain 前 page 的 refcount/is_free 要设置成什么？
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

1.
2.

## 8. 进入 M2c 前的一句话总结

> pcp 的本质是：____________________________________________
