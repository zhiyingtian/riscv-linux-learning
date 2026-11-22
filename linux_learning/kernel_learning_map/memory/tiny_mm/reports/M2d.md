# M2d 阶段报告

> 状态：已完成。`make test-m2d` 7/7 通过，M2c 20/20 保持通过。

## 1. 我实现了什么

- [x] `enum zone_watermark` 和 `struct zone` 水位字段
- [x] `pmm_init()` 初始化 min/low/high
- [x] `pmm_watermark_free()`：只统计 buddy 空闲页
- [x] `pmm_watermark_ok()`：按 order 计算需要页数
- [x] `pmm_reclaim_needed()` / `pmm_reclaim_wakeups()`
- [x] `pmm_update_reclaim_state()`：low 置位、high 清零、滞回
- [x] `pmm_check()` 校验水位顺序和回收标记一致性
- [x] 普通分配受 `WMARK_MIN` 限制
- [x] `PMM_ALLOC_HARD` 允许使用 min 预留
- [x] 高阶分配前 drain pcp
- [x] 水位测试 7/7 通过

## 2. 当前测试结果

```text
$ make test-m2d
tiny-mm / M2d: watermark + reclaim mark
---------------------------------
[PASS] init
[PASS] watermark order
[PASS] pcp not counted
[PASS] hard uses reserve
[PASS] low marks reclaim
[PASS] high clears reclaim
[PASS] min blocks alloc
---------------------------------
all tests passed
```

M2c 回归：

```text
$ make test-m2c
20/20 passed
```

## 3. 关键设计

```text
水位判断：buddy.free_count
总空闲：buddy.free_count + pcp.count
```

- pcp 缓存页不在 free_area，不能算进水位；
- 水位加本次 order 才是真正的门槛；
- low 到 high 之间保持 `reclaim_needed` 不反复切换；
- 普通分配不能使用 min 预留；
- hard/atomic 分配可以使用 min 预留。

### 为什么需要 hard 分配

M2c 的耗尽测试和最大块测试需要：

```text
分配整个 64 页
```

而水位要求保留：

```text
WMARK_MIN = 8
```

两者直接冲突。用 `PMM_ALLOC_HARD` 区分：

```text
普通分配：保留 min
hard 分配：可以使用预留
```

这和 Linux 的 `ALLOC_HARDER` / 原子分配思路一致。

## 4. 我踩的坑

| 坑 | 现象 | 怎么发现 | 怎么修 |
|---|---|---|---|
| 水位 hard gate 打断 M2c | exhaust/最大块测试失败 | `make test-m2c` 5 个失败 | 增加 `PMM_ALLOC_HARD` |
| order6 永远无法分配 | `WMARK_MIN + 64 > 64` | 分析 `pmm_watermark_ok()` | 普通/hard 分离 |
| pcp 挡住水位判断 | drain 后 buddy 空闲页变化 | 高阶随机测试 `pmm_check` 失败 | 分配前 drain pcp，并在失败路径更新标记 |
| 回收标记直接赋值 | `reclaim_wakeups` 可能不增加 | 检查状态转移 | 统一走 `pmm_update_reclaim_state()` |

## 5. 验证项

- [x] free 低于 low → `reclaim_needed=true`
- [x] free 回到 high → `reclaim_needed=false`
- [x] 只唤醒一次：`reclaim_wakeups == 1`
- [x] free 低于 min → 普通分配返回 NULL
- [x] hard 分配可以使用 min 预留
- [x] order 越大，水位需要越多页
- [x] pcp 缓存不计入水位
- [x] 每次操作后 `pmm_check() == 0`
- [x] M2c 20/20、M2a 11/11、M2b 9/9 保持通过

## 6. 和 Linux 对照

- `WMARK_MIN`：硬预留，原子分配可使用
- `WMARK_LOW`：唤醒 kswapd / 标记回收
- `WMARK_HIGH`：停止回收
- `zone_watermark_ok()`：考虑 order 和 watermark
- `ALLOC_HARDER`：允许使用 min 预留
- `watermark_boost`：碎片化时临时提高水位

## 7. 还没懂的问题

- Linux 真正的 reclaim/compaction 如何和 pcp drain 交互？
- `watermark_boost` 在什么场景下最有效？

## 8. 进入下一阶段前的一句话总结

M2d 已完成：zone 用 min/low/high 做水位判断和回收标记，普通分配保留 min，
hard/atomic 分配可以使用预留；水位只统计 buddy，pcp 通过 drain 参与高阶分配。
