# M2d：水位与回收标记

> 项目总入口：[../README.md](../README.md)
> 前置：[m2c-pmm-integration.md](m2c-pmm-integration.md)
> 状态：**✅ 已完成；`make test-m2d` 7/7 通过，`make test-m2c` 20/20 保持通过。**
>
> 验收测试见 [../tests/test_watermark.c](../tests/test_watermark.c)，
> 阶段报告见 [../reports/M2d.md](../reports/M2d.md)。

## 目标

M2c 已经完成 pmm 的 buddy + pcp 接线。M2d 在 zone 上模拟 Linux 的三级水位和回收标记：

```text
free >= high        → 健康，清除 reclaim_needed
low <= free < high  → 滞回区，保持当前标记
free < low          → reclaim_needed = true
free < min          → 普通分配失败；PMM_ALLOC_HARD 可以使用预留
```

水位判断使用 **buddy 空闲页数**，不是 `pmm_free_count()`：

```text
pmm_free_count()      = buddy.free_count + pcp.count
pmm_watermark_free()  = buddy.free_count
```

原因：

- pcp 缓存页不在 buddy 的 `free_area` 里；
- 它们不能参与高阶连续块分配；
- Linux 的 `NR_FREE_PAGES` 也主要反映 buddy 空闲页。

## 数据结构

[`include/tmm.h`](../include/tmm.h) 新增：

```c
enum zone_watermark {
    WMARK_MIN,
    WMARK_LOW,
    WMARK_HIGH,
    NR_WMARK,
};

struct zone {
    ...
    unsigned long watermark[NR_WMARK];
    bool reclaim_needed;
    unsigned long reclaim_wakeups;
};
```

含义：

| 字段 | 含义 |
|---|---|
| `watermark[WMARK_MIN]` | 硬预留，普通分配不能低于它 |
| `watermark[WMARK_LOW]` | 低于它时标记需要回收 |
| `watermark[WMARK_HIGH]` | 回到它以上时清除回收标记 |
| `reclaim_needed` | 当前是否已标记回收 |
| `reclaim_wakeups` | 从健康进入 low 的次数 |

当前固定参数：

```c
#define PMM_WMARK_MIN  8
#define PMM_WMARK_LOW  10
#define PMM_WMARK_HIGH 12
```

适用于 64 页测试内存。

## API

```c
bool pmm_watermark_ok(unsigned int order, enum zone_watermark mark);
unsigned long pmm_watermark_free(void);
bool pmm_reclaim_needed(void);
unsigned long pmm_reclaim_wakeups(void);

struct page *pmm_alloc_pages_flags(unsigned int order, unsigned int flags);
```

### 水位判断

```c
need = 1UL << order;
return pmm_watermark_free() >= zone->watermark[mark] + need;
```

注意：

- 必须把本次 order 的页数算进去；
- `order >= sizeof(unsigned long) * 8` 时直接返回 false，避免移位 UB。

### 普通分配与 hard 分配

```c
pmm_alloc_pages(order);       /* 普通分配，受 WMARK_MIN 限制 */
pmm_alloc_pages_flags(order, PMM_ALLOC_HARD);
```

`PMM_ALLOC_HARD` 类似 Linux 的 `ALLOC_HARDER` / 原子分配路径：

- 普通分配不能使用 min 预留；
- hard 分配可以使用 min 预留；
- M2c 的耗尽测试和最大块测试使用 hard 分配；
- 常规业务分配仍然使用普通路径。

这样解决了之前的冲突：

```text
水位要求保留 min
M2c 又要求能分配整个 64 页
```

两者通过不同的分配标志共存。

## 实现

### 1. 初始化

[`pmm_init()`](../src/pmm.c) 初始化：

```c
watermark[WMARK_MIN]  = 8;
watermark[WMARK_LOW]  = 10;
watermark[WMARK_HIGH] = 12;
reclaim_needed = false;
reclaim_wakeups = 0;
```

### 2. 回收标记

`pmm_update_reclaim_state()`：

```c
if (buddy.free_count < watermark[WMARK_LOW]) {
    if (!reclaim_needed) {
        reclaim_needed = true;
        reclaim_wakeups++;
    }
} else if (buddy.free_count >= watermark[WMARK_HIGH]) {
    reclaim_needed = false;
}
```

特点：

- 只在 `false -> true` 时增加 `reclaim_wakeups`；
- low ~ high 之间保持原状态，形成滞回；
- alloc/free 后都会调用，保证标记与 buddy 空闲页同步。

### 3. 分配路径

`pmm_alloc_pages_flags()` 的顺序：

```text
1. order > 0 且 pcp 有缓存 → 先 drain pcp
2. 普通分配检查 WMARK_MIN
3. order 0 走 pcp，order > 0 走 buddy
4. 高阶分配失败只返回 NULL（pcp 已在第 1 步 drain）
5. 成功后更新 free_count 和回收标记
```

关键点：

- 水位检查前先 drain pcp，避免“pcp 缓存挡住高阶分配”；
- hard 分配跳过 min 检查，可以使用预留；
- 失败的路径也要更新回收标记，因为 drain pcp 可能改变 buddy 空闲页。

### 4. 释放路径

order 0 释放进 pcp，order > 0 释放进 buddy；随后：

```c
free_count += 1 << order;
pmm_update_reclaim_state();
```

### 5. `pmm_check()`

现在检查：

- buddy/pcp 自检；
- `free_count == buddy.free_count + pcp.count`；
- `min <= low <= high <= nr_pages`；
- `buddy.free_count >= high` 时不能仍标记回收；
- `buddy.free_count < low` 时必须已经标记回收。

## 7 个验收测试

| 测试 | 抓什么 |
|---|---|
| `init` | 水位顺序、初始标记、buddy 空闲 |
| `watermark order` | order 越大需要越多页 |
| `pcp not counted` | 水位只算 buddy，不算 pcp |
| `hard uses reserve` | 普通分配不能动 min，hard 可以 |
| `low marks reclaim` | free < low 时置位，且只唤醒一次 |
| `high clears reclaim` | free >= high 时清除 |
| `min blocks alloc` | free <= min 时普通分配返回 NULL |

当前结果：

```text
[PASS] init
[PASS] watermark order
[PASS] pcp not counted
[PASS] hard uses reserve
[PASS] low marks reclaim
[PASS] high clears reclaim
[PASS] min blocks alloc

all tests passed
```

## 运行

```bash
make test-m2d
```

debug 构建：

```bash
make debug-m2d
```

回归：

```bash
make test-m2c
make test-m2
make test-m2b
```

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `WMARK_MIN/LOW/HIGH` | `enum zone_watermarks` |
| `zone.watermark[]` | `zone._watermark[NR_WMARK]` |
| `pmm_watermark_free()` | `zone_page_state(NR_FREE_PAGES)` |
| `pmm_watermark_ok()` | `zone_watermark_ok()` |
| `reclaim_needed` | kswapd 唤醒/回收状态 |
| `PMM_ALLOC_HARD` | `ALLOC_HARDER` / `GFP_ATOMIC` 的简化 |
| `pcp_drain_all()` | `free_pcppages_bulk()` |

Linux 还包含：

- `low = min * 5/4`、`high = min * 3/2`；
- `ALLOC_WMARK_MIN/LOW/HIGH`；
- direct reclaim、compaction、OOM；
- `watermark_boost` 处理碎片化。

M2d 只模拟阈值、标记、滞回和 hard/普通分配差异，不模拟真正的页面回收。

## 下一步

M2d 已完成。下一步可以：

1. 进入 M3：页表/TLB；
2. 或者继续在 M2 框架里补水位动态计算、watermark_boost 和回收 stub。
