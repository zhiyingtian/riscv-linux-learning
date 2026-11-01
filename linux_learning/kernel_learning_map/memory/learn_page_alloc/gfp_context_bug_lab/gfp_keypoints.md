# GFP(Get Free Page) Flag 行为与常见 Bug 总结

> **内核**：`runninglinuxkernel_5.15`（Linux 5.15.0+，RISC-V）
> **配套实验**：同级目录 [`README.md`](README.md) 里的 `gfp_context_bug_lab`
> **核心观点**：GFP flag 不是一张“分配类型表”，而是调用者给分配器的一张**授权书**。大部分 GFP bug 不是 flag 拼错，而是**授权的行为超出了当前上下文或调用者能承受的范围**。

---

## 0. 先记结论

1. `GFP_KERNEL` 允许直接回收，**可能睡眠**；不能用在 spinlock、IRQ、softirq、`rcu_read_lock()` 等原子上下文。
2. `GFP_ATOMIC` / `GFP_NOWAIT` 不允许直接回收，**但会失败**；调用者必须检查 `NULL` 并准备 fallback。
3. `GFP_NOFS` / `GFP_NOIO` 只是限制回收时能不能进 FS / 做 IO，**它们仍然可能睡眠**，不是原子上下文可用的 flag。
4. `__GFP_NORETRY` **不是** `GFP_NOWAIT`；它仍可能做轻量直接回收，仍可能睡。
5. `GFP_ATOMIC` 不是“更强的 `GFP_KERNEL`”；它会动用紧急保留内存，失败率也不低，进程上下文滥用会消耗系统应急能力。
6. 其他容易忽略的行为：使用保留内存、DMA/highmem 地址限制、`MOVABLE` 迁移、`__GFP_ZERO` 清零、`__GFP_ACCOUNT` 记账、`__GFP_COMP` 复合页。

---

## 1. 核心模型：flag 是授权，上下文是能力

同一个 `GFP_KERNEL`，在不同上下文里结论不同：

|  | 进程上下文（可睡眠） | 原子上下文（不可睡眠） |
|---|---|---|
| `GFP_KERNEL` | 正确 | **bug** |
| `GFP_ATOMIC` | 合法，但可能不必要 | 正确 |

`GFP_KERNEL` 在进程上下文完全正常；`GFP_ATOMIC` 在进程上下文也不是“更安全”，它可能让本来能成功的分配失败，还可能消耗保留内存。

所以判断 GFP 是否用错，要同时看三件事：

```text
flag 允许分配器做什么
        +
当前上下文允许调用者做什么
        +
调用者能不能处理失败
```

配套实验里的三种模式，就是在隔离这三个变量：

| mode | 上下文 | GFP | 结果 |
|---|---|---|---|
| 1 | `spin_lock_irqsave()` | `GFP_KERNEL` | sleeping function 警告 |
| 2 | `spin_lock_irqsave()` | `GFP_ATOMIC` | 正常 |
| 3 | 进程上下文 | `GFP_KERNEL` | 正常（对照） |

bug 出在“配对”，不是单独某个 flag。

---

## 2. 常用 GFP 组合速查

表中 `+` 表示按位或组合（源码里写作 `|`）。

| 组合 | 关键组成 | 直接回收 | IO / FS | 可用上下文 | 典型用途 / 注意 |
|---|---|---|---|---|---|
| `GFP_KERNEL` | `__GFP_RECLAIM + __GFP_IO + __GFP_FS` | 是 | 是 / 是 | 进程上下文 | 内核普通分配；可能睡 |
| `GFP_ATOMIC` | `__GFP_HIGH + __GFP_ATOMIC + __GFP_KSWAPD_RECLAIM` | 否 | 否 / 否 | 原子上下文 | 可动用紧急保留；会失败 |
| `GFP_NOWAIT` | `__GFP_KSWAPD_RECLAIM` | 否 | 否 / 否 | 原子上下文 | 可唤醒 kswapd；失败率更高 |
| `GFP_NOIO` | `__GFP_RECLAIM` | 是 | 否 / 否 | 进程上下文 | 块层/不能物理 IO；仍可能睡 |
| `GFP_NOFS` | `__GFP_RECLAIM + __GFP_IO` | 是 | 是 / 否 | 进程上下文 | FS 路径；仍可能睡 |
| `GFP_USER` | `GFP_KERNEL + __GFP_HARDWALL` | 是 | 是 / 是 | 进程上下文 | 用户页；强制 cpuset |
| `GFP_HIGHUSER` | `GFP_USER + __GFP_HIGHMEM` | 是 | 是 / 是 | 进程上下文 | 用户页；可能需要 highmem |
| `GFP_HIGHUSER_MOVABLE` | `GFP_HIGHUSER + __GFP_MOVABLE` | 是 | 是 / 是 | 进程上下文 | LRU/用户页；可迁移 |
| `GFP_KERNEL_ACCOUNT` | `GFP_KERNEL + __GFP_ACCOUNT` | 是 | 是 / 是 | 进程上下文 | kmemcg 记账 |
| `GFP_DMA` | `__GFP_DMA` | 取决于组合 | 取决于组合 | 取决于组合 | 最低 DMA zone；通常与 `GFP_KERNEL`/`GFP_ATOMIC` 组合 |
| `GFP_DMA32` | `__GFP_DMA32` | 取决于组合 | 取决于组合 | 取决于组合 | 32 位 DMA 地址限制 |

注意：

- `GFP_ATOMIC` / `GFP_NOWAIT` 会忽略 `__GFP_IO`、`__GFP_FS`、`__GFP_DIRECT_RECLAIM` 这些“可睡眠分配”才适用的回收修饰符。
- `GFP_DMA` / `GFP_DMA32` 单独使用只是 zone 修饰位，实际调用通常是 `GFP_KERNEL | GFP_DMA` 这类组合。
- 表里的“可用上下文”只是大方向；NMI、`raw_spin_lock` 等极端上下文连 `GFP_ATOMIC`/`GFP_NOWAIT` 都不保证安全。

---

## 3. 最常出 bug 的三类行为

### 3.1 允许睡眠：`__GFP_DIRECT_RECLAIM`

涉及：`GFP_KERNEL`、`GFP_NOFS`、`GFP_NOIO`

典型错误：

```c
spin_lock_irqsave(&lock, flags);
page = alloc_pages(GFP_KERNEL, 0);   /* 允许直接回收 = 可能睡眠 */
spin_unlock_irqrestore(&lock, flags);
```

#### 为什么错

`GFP_KERNEL` 包含 `__GFP_DIRECT_RECLAIM`，表示“调用者允许直接回收”。直接回收会：

- 扫描 LRU、等页锁；
- 可能做写回、等 IO；
- 必要时 `cond_resched()`。

这些操作都可能睡眠，而 spinlock / IRQ 关闭 / softirq 上下文不允许睡眠。

#### 为什么 pcp 命中也会报警

`__alloc_pages()` 的入口：

```text
__alloc_pages()
  prepare_alloc_pages()
    might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM)   ← 先检查
    should_fail_alloc_page()                          ← 故障注入
  get_page_from_freelist()                            ← 才走 pcp/快路径
```

检查在快路径**之前**。所以：

- 这次 pcp 里刚好有页、分配成功、实际没睡；
- 警告仍然会出现；
- 下一次 pcp 空了、进入直接回收，才会变成真正的 `scheduling while atomic`、死锁或挂 CPU。

这就是“潜在 bug”和“真事故”的区别。

#### 常见误判

- “这次没睡，所以没事” → 错，上下文契约已经不满足。
- “`GFP_NOFS` / `GFP_NOIO` 是原子安全的” → 错，它们仍含 `__GFP_RECLAIM`，仍可能睡。
- “加 `__GFP_NORETRY` 就不会睡了” → 错，`__GFP_NORETRY` 仍可能做轻量直接回收，仍可能睡。

#### 修复方向

优先级从高到低：

1. 把分配移到锁外 / 原子区外；
2. 初始化时预分配，原子上下文只取用；
3. 用 mempool / 预分配池；
4. 必须在原子上下文分配时，用 `GFP_ATOMIC` 或 `GFP_NOWAIT`，并处理失败。

### 3.2 允许失败：`GFP_ATOMIC` / `GFP_NOWAIT`

典型错误：

```c
page = alloc_pages(GFP_ATOMIC, 0);
page->data = ...;           /* 没检查 NULL */
```

#### 基本事实

- `GFP_ATOMIC` 不是“保证成功”，只是“不能睡，允许动用一点紧急保留”；
- `GFP_NOWAIT` 连紧急保留都不动，失败率更高；
- 两者都可能失败，调用者必须检查返回值；
- `GFP_KERNEL` 也可能失败（例如 OOM victim），只有 `__GFP_NOFAIL` 承诺不返回失败。

#### `__GFP_NORETRY` 的陷阱

`__GFP_NORETRY` 的语义是“不要无限重试”，不是“不要回收”：

- 它仍可能做一次轻量直接回收；
- 仍可能睡眠；
- 它适合“失败可以廉价处理”的场景，不适合原子上下文。

不要把 `__GFP_NORETRY` 当成 `GFP_NOWAIT` 的替代品。

`__GFP_RETRY_MAYFAIL` 同理：它仍然允许直接回收，**仍然可能睡**，只是重试策略比 `__GFP_NORETRY` 更积极；不能用于原子上下文。

#### `__GFP_NOFAIL` 的陷阱

`__GFP_NOFAIL` 表示“分配器必须无限重试，永不返回失败”：

- 可能无限阻塞；
- 可能触发 OOM；
- 高阶分配使用它会 `WARN_ON_ONCE`（5.15 里 order > 1 就会警告）；
- 只有在“确实没有合理失败策略”时才考虑，不要拿它当“更可靠的 GFP”。

#### 正确做法

```c
page = alloc_pages(GFP_ATOMIC, 0);
if (!page) {
    /* 丢包、返回 -ENOMEM、延后重试、走预分配池……不能睡 */
}
```

### 3.3 允许回收 FS/IO：`__GFP_FS` / `__GFP_IO`

涉及：`GFP_KERNEL`

典型错误：在文件系统、块层、writeback 路径里用 `GFP_KERNEL` 分配，回收过程又回到文件系统/块层，形成递归，拿不到锁或死锁。

#### 三个组合的区别

| 组合 | 允许直接回收 | 允许物理 IO | 允许进 FS |
|---|---|---|---|
| `GFP_KERNEL` | 是 | 是 | 是 |
| `GFP_NOFS` | 是 | 是 | 否 |
| `GFP_NOIO` | 是 | 否 | 否 |

#### 正确方向

- FS 路径用 `GFP_NOFS`；
- 块层/不能做物理 IO 的路径用 `GFP_NOIO`；
- 更推荐用 `memalloc_nofs_save()` / `memalloc_noio_save()` 标记整个作用域，让内部所有分配自动继承，而不是到处传 flag。

注意：`GFP_NOFS` / `GFP_NOIO` **仍然允许直接回收，仍然可能睡眠**，不能用在 spinlock/IRQ 里。

---

## 4. 其他高频行为

| 行为 | 相关 flag | 常见 bug | 正确认识 |
|---|---|---|---|
| 使用紧急保留内存 | `__GFP_HIGH`、`__GFP_ATOMIC`、`__GFP_MEMALLOC` | 进程上下文滥用 `GFP_ATOMIC`；不节流地用 `__GFP_MEMALLOC` | 保留内存给“系统必须前进”的紧急路径；耗尽会 OOM/死锁；`__GFP_NOMEMALLOC` 可显式禁止 |
| 永不失败 | `__GFP_NOFAIL` | 高阶分配用 nofail；以为“不会挂” | 会无限重试，可能挂住/OOM；order > 1 会 warn，costly order 更不鼓励 |
| DMA/地址限制 | `GFP_DMA`、`GFP_DMA32` | 设备要 DMA 却用 `GFP_KERNEL`；以为普通内存一定能 DMA | DMA zone 很稀缺；DMA mask 要匹配；优先用 DMA API |
| highmem | `__GFP_HIGHMEM` | 对 highmem 页直接 `page_address()` | highmem 页必须 `kmap()` 才能访问；64 位通常没有 highmem |
| 迁移性 | `__GFP_MOVABLE`、`GFP_HIGHUSER_MOVABLE` | 内核数据结构误用 MOVABLE，迁移后裸指针失效 | MOVABLE 只给“可迁移”的页；内核页用 `GFP_KERNEL` |
| 归零 | `__GFP_ZERO` | 用户可见页没清零，泄露旧数据 | `alloc_pages()` 默认**不清零**；敏感/用户页要加 `__GFP_ZERO` |
| 记账 | `__GFP_ACCOUNT`、`GFP_KERNEL_ACCOUNT` | 用户触发的内核对象不记账，memcg 逃逸 | 该记账的分配要带 ACCOUNT；不是所有内核分配都需要 |
| 复合页 | `__GFP_COMP` | 高阶页当 compound page 用，tail page/refcount 出错 | 需要 compound 元数据时加 `__GFP_COMP`；高阶页生命周期要配套 |
| 抑制日志 | `__GFP_NOWARN` | 用 NOWARN 掩盖 OOM/失败 | 只抑制日志，不改变失败语义；不能当修复 |
| NUMA/放置 | `__GFP_THISNODE`、`__GFP_HARDWALL` | 以为 THISNODE 会 fallback；忽略 cpuset | THISNODE 无 fallback，容易失败；HARDWALL 强制 cpuset 限制 |

补充：

- `__GFP_RECLAIMABLE`：给 slab 的“可通过 shrinker 回收”的页用；不要和 `__GFP_MOVABLE` 混为一谈。
- `__GFP_WRITE`：表示调用者要弄脏页，影响脏页在 zone 间的分布；主要是性能/公平性，不是安全语义。
- `__GFP_ZEROTAGS`、`__GFP_SKIP_KASAN_POISON`：KASAN/HW tags 专用，普通代码不用关心。

---

## 5. 最容易混淆的六组

### 5.1 `__GFP_NORETRY` ≠ `GFP_NOWAIT`

- `__GFP_NORETRY`：仍可能有轻量直接回收，**仍可能睡**；
- `GFP_NOWAIT`：不含 `__GFP_DIRECT_RECLAIM`，不会直接回收，但会失败。

### 5.2 `GFP_NOFS` / `GFP_NOIO` ≠ 原子安全

- 它们只限制回收范围；
- 仍含 `__GFP_RECLAIM`，仍可能睡；
- 只能用于可睡眠的进程上下文。

### 5.3 `GFP_ATOMIC` ≠ “更强的 `GFP_KERNEL`”

- `GFP_ATOMIC` 去掉了直接回收（不能睡）；
- 但它加了 `__GFP_HIGH` / `__GFP_ATOMIC`，可以动用紧急保留；
- 它是给“不能睡”的上下文用的，不是给“想提高成功率”的进程上下文用的。

### 5.4 `GFP_ATOMIC` ≠ `GFP_NOWAIT`

|  | 直接回收 | 紧急保留 | 唤醒 kswapd | 典型场景 |
|---|---|---|---|---|
| `GFP_ATOMIC` | 否 | 可以 | 可以 | 中断/原子上下文，失败代价高 |
| `GFP_NOWAIT` | 否 | 不可以 | 可以 | 不想等、失败可接受 |

两者都不直接回收，但 `GFP_ATOMIC` 更容易成功，同时更容易消耗保留内存。

### 5.5 `__GFP_HIGH` / `__GFP_ATOMIC` / `__GFP_MEMALLOC` 不是一回事

- `__GFP_HIGH`：高优先级，系统前进所需；
- `__GFP_ATOMIC`：调用者不能回收/睡眠，通常中断上下文；
- `__GFP_MEMALLOC`：可以访问**所有**内存，只给“马上能释放更多内存”的极少数路径（进程退出、swap 等）。

`__GFP_MEMALLOC` 最危险：不节流会耗尽保留，导致系统无法回收，甚至死锁。`__GFP_NOMEMALLOC` 的优先级高于 `__GFP_MEMALLOC`，可显式禁止。

### 5.6 `__GFP_NOWARN` ≠ 分配不会失败

`__GFP_NOWARN` 只是不打印分配失败日志，失败本身还在。用它掩盖 OOM 警告，通常只是把问题藏起来。

---

## 6. 选 flag 的决策流程

```text
当前上下文能睡吗？
├─ 不能
│   └─ GFP_ATOMIC 或 GFP_NOWAIT
│       ├─ 检查 NULL
│       └─ 准备 fallback（丢包/返回错误/延后重试/预分配池）
└─ 能
    ├─ 回收时能进 FS / 做 IO 吗？
    │   ├─ 不能 → GFP_NOFS / GFP_NOIO
    │   │         或用 memalloc_nofs_save()/memalloc_noio_save()
    │   └─ 能   → GFP_KERNEL
    └─ 附加语义
        ├─ DMA        → GFP_DMA / GFP_DMA32（通常再 OR 上 GFP_KERNEL/GFP_ATOMIC）
        ├─ highmem    → __GFP_HIGHMEM + kmap()
        ├─ 用户/可迁移 → GFP_HIGHUSER_MOVABLE
        ├─ 记账       → __GFP_ACCOUNT / GFP_KERNEL_ACCOUNT
        ├─ 清零       → __GFP_ZERO
        ├─ 复合页     → __GFP_COMP
        └─ NUMA       → __GFP_THISNODE（无 fallback，必须处理失败）
```

最后再问一句：**失败了怎么办？**

- 默认：检查返回值，走错误路径；
- 只有确实没有合理失败策略时，才考虑 `__GFP_NOFAIL`，并避免高阶分配。

---

## 7. 调试与验证

### 7.1 看到 `BUG: sleeping function called from invalid context`

先看三行：

```text
in_atomic(): 1
irqs_disabled(): 1
preempt_count: ...
```

它们说明当前处于原子上下文。再看调用栈里 `might_sleep_if()` 的位置和 GFP mask：

- `GFP_KERNEL` / `GFP_NOFS` / `GFP_NOIO` 含 `__GFP_DIRECT_RECLAIM`，会触发检查；
- `GFP_ATOMIC` / `GFP_NOWAIT` 不含，不会因为“允许直接回收”触发。

注意：

- `might_sleep()` 有 1 秒限流，连续重跑可能看不到第二条；
- 它调用 `add_taint(TAINT_WARN)`，taint 字母 `W`，数值 `512`；
- `dmesg -C` 不会清 taint，重启才清；
- `panic_on_warn=1` 对它无效；`panic_on_taint=0x200` 才会 panic。

### 7.2 附：`should_fail_alloc_page()` 故障注入

`should_fail_alloc_page()` 不是 GFP flag 的语义，而是 page allocator 的**故障注入钩子**，用来测试调用者的失败路径。

位置：

```text
__alloc_pages()
  prepare_alloc_pages()
    might_sleep_if(...)                 ← 先检查上下文
    if (should_fail_alloc_page(...))    ← 再决定是否注入失败
        return false;                   ← __alloc_pages() 直接返回 NULL
```

效果：

- 不碰 pcp、buddy、slowpath、reclaim、OOM；
- 调用者拿到普通 `NULL`，和真实分配失败一样；
- 专测 `if (!page)` 分支、回滚、错误传播。

关键过滤器（5.15 默认值）：

| 过滤器 | 默认值 | 效果 |
|---|---|---|
| `min_order` | `1` | 默认不注入 order-0，只注入 order≥1 |
| `__GFP_NOFAIL` | 永远跳过 | 不让 nofail 分配失败 |
| `ignore_gfp_highmem` | `true` | 跳过 `__GFP_HIGHMEM` |
| `ignore_gfp_reclaim` | `true` | 跳过带 `__GFP_DIRECT_RECLAIM` 的分配，即默认不注入 `GFP_KERNEL` |
| `probability` | `0` | 默认不注入 |
| `times` | `1` | 默认注入一次 |

配置：

```text
CONFIG_FAULT_INJECTION=y
CONFIG_FAIL_PAGE_ALLOC=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y
```

让一次 **order-0 + GFP_KERNEL** 分配失败：

```bash
mount -t debugfs none /sys/kernel/debug
cd /sys/kernel/debug/fail_page_alloc

echo 0   > min-order
echo 0   > ignore-gfp-wait    # 0 = 不忽略可睡眠分配
echo 100 > probability
echo 1   > interval
echo 1   > times
```

当前这颗 `runninglinuxkernel_5.15` 的 `.config` 里 `CONFIG_FAULT_INJECTION` 没开，所以 `should_fail_alloc_page()` 是空实现，永远返回 `false`；想试验需要先打开配置并重新编译内核。

---

## 8. 源码锚点（Linux 5.15）

| 位置 | 内容 |
|---|---|
| `include/linux/gfp.h` | GFP 位定义、Watermark/Reclaim/Action modifiers 的文档注释 |
| `include/linux/gfp.h:323-324` | `GFP_ATOMIC` / `GFP_KERNEL` 组合 |
| `include/linux/gfp.h:358` | `gfpflags_allow_blocking()` 判断 `__GFP_DIRECT_RECLAIM` |
| `mm/page_alloc.c:5156` | `prepare_alloc_pages()` 里的 `might_sleep_if()` |
| `mm/page_alloc.c:5158` | `if (should_fail_alloc_page(gfp_mask, order))` |
| `mm/page_alloc.c:3765` | `__should_fail_alloc_page()` 的 page-specific 过滤 |
| `mm/page_alloc.c:3812` | `noinline should_fail_alloc_page()` + `ALLOW_ERROR_INJECTION` |
| `lib/fault-inject.c:103` | `should_fail()` 的 probability/interval/times/space 逻辑 |
| `Documentation/fault-injection/fault-injection.rst` | 故障注入的 debugfs 与启动参数说明 |

---

## 9. 检查清单

写分配代码时，逐条问自己：

- [ ] 当前上下文能睡吗？有没有在 spinlock / IRQ / softirq / RCU 读临界区里分配？
- [ ] 我用的 GFP 含 `__GFP_DIRECT_RECLAIM` 吗？这个上下文允许吗？
- [ ] 我检查分配返回值了吗？失败路径能睡吗？
- [ ] 回收过程会不会递归进 FS / 块层？需要 `GFP_NOFS` / `GFP_NOIO` 吗？
- [ ] 我是不是在用 `GFP_ATOMIC` / `__GFP_MEMALLOC` 消耗保留内存？
- [ ] DMA 地址限制、highmem、NUMA 放置对吗？
- [ ] 这块页需要清零吗？需要记账吗？需要 compound 元数据吗？
- [ ] 我是不是把内核页标成了 `MOVABLE`？
- [ ] 我是不是用 `__GFP_NOWARN` 掩盖了失败？
- [ ] 我是不是用 `__GFP_NOFAIL` 代替了失败策略？

> 参考答案见第 11.2 节；配套实验 Q1–Q7 的答案见第 11.1 节。

---

## 10. 一页纸

- GFP flag 是**授权书**：允许睡、允许回收、允许用保留、允许失败、允许迁移、允许清零/记账。
- 最常见 bug：**不能睡却允许睡**（`GFP_KERNEL` 进原子上下文）。
- 第二常见：**允许失败却不处理**（`GFP_ATOMIC`/`GFP_NOWAIT` 不检查 NULL）。
- 第三常见：**允许回收 FS/IO 导致递归**（FS/block 路径误用 `GFP_KERNEL`）。
- `GFP_NOFS` / `GFP_NOIO` 仍可能睡；`__GFP_NORETRY` 仍可能睡；`GFP_ATOMIC` 不是“更强”。
- 选 flag 的顺序：**先看上下文能否睡 → 再看回收范围 → 再看 zone/迁移/清零/记账 → 最后定失败策略**。
- 看到 `sleeping function called from invalid context`，先看 `in_atomic()` / `irqs_disabled()`，再对照 GFP 里的 `__GFP_DIRECT_RECLAIM`。
- 记住一句话：**不是 flag 名字错，而是你授权给分配器的行为，超过了当前上下文能承受的范围。**

---

## 文件

| 文件 | 作用 |
|---|---|
| `gfp_keypoints.md` | 本文：GFP flag 行为与常见 bug 总结 |
| `README.md` | 配套实验：原子上下文 + `GFP_KERNEL` 的复现步骤 |
| `gfp_context.c` | 三种模式的演示模块 |
| `Makefile` | 编译 + `make install` |
| `guest_run.sh` | guest 内依次跑 mode=1/2/3 |

---

## 11. 问题与答案

### 11.1 配套实验 Q1–Q7

#### Q1. 为什么 `prepare_alloc_pages` 在 `get_page_from_freelist` 之前？对 pcp 快路径意味着什么？

`prepare_alloc_pages()` 是 `__alloc_pages()` 的公共准备阶段，负责建立本次分配的上下文：

- `highest_zoneidx`、`zonelist`、`nodemask`、`migratetype`；
- cpuset / `GFP_HARDWALL` 相关处理；
- `might_sleep_if()` 上下文检查；
- `should_fail_alloc_page()` 故障注入；
- `alloc_flags`、`preferred_zoneref`。

`get_page_from_freelist()` 只是拿这些上下文去 pcp / buddy 里取页。把它放在前面意味着：

- 快路径和慢路径都要先经过同一套检查；
- 上下文警告和故障注入在碰 free list 之前发生；
- **pcp 命中不能绕过检查**。pcp 里刚好有页，只能说明“这次没睡”，不能说明“这个 GFP 在当前上下文合法”。

#### Q2. `mode=2` 为什么没有警告？`GFP_ATOMIC` 和 `GFP_KERNEL` 在 `__GFP_DIRECT_RECLAIM` 上有什么区别？

`GFP_ATOMIC` 不包含 `__GFP_DIRECT_RECLAIM`：

```c
#define GFP_ATOMIC (__GFP_HIGH|__GFP_ATOMIC|__GFP_KSWAPD_RECLAIM)
```

所以：

```c
might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM);
```

对 `GFP_ATOMIC` 为假，不会触发 `might_sleep()` 警告。

`GFP_KERNEL` 包含 `__GFP_RECLAIM`：

```c
#define GFP_KERNEL (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define __GFP_RECLAIM (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
```

所以 `GFP_KERNEL` 允许直接回收，可能睡眠。

注意：`GFP_ATOMIC` 仍然包含 `__GFP_KSWAPD_RECLAIM`，可以唤醒 kswapd，也可能动用一点紧急保留；它只是**不直接回收、不睡眠**，并不是“什么回收都不做”。

#### Q3. 把 `mode=1` 改成 `GFP_KERNEL | __GFP_NORETRY`，还会不会警告？为什么？

**仍然会警告。**

`__GFP_NORETRY` 只改变“重试策略”，不会清掉 `__GFP_DIRECT_RECLAIM`。`GFP_KERNEL` 仍然允许直接回收，`might_sleep_if()` 仍然为真。

而且 `__GFP_NORETRY` 自己也说明它可能做轻量直接回收：

> The VM implementation will try only very lightweight memory direct reclaim ... thus it can sleep.

所以它不能把 `GFP_KERNEL` 变成原子上下文可用的 flag；要真正去掉睡眠许可，必须清掉 `__GFP_DIRECT_RECLAIM`，例如改用 `GFP_ATOMIC` / `GFP_NOWAIT`。

#### Q4. 去掉 `gfp_demo_warm_pcp()`，在内存压力下跑 `mode=1` 会看到什么？

可能出现以下一种或多种现象：

- 先打印 `BUG: sleeping function called from invalid context`；
- 进入 `__alloc_pages_slowpath()` → 直接回收 → 在持有 spinlock、关中断的情况下调用 `cond_resched()` 或等待锁/IO；
- 打印 `BUG: scheduling while atomic`；
- 卡死、RCU stall、soft lockup，甚至整机无响应。

但结果**不确定**：如果回收很快找到页，可能只看到警告；如果 pcp / buddy 还能满足，可能连慢路径都不进；如果触发 OOM，可能挂得更久。

这正是 `gfp_demo_warm_pcp()` 存在的原因：让 demo 尽量走 pcp 快路径，只稳定复现“上下文警告”，不真的把 guest 卡死。这个实验要在能恢复的快照上做。

#### Q5. `panic_on_warn=1` 能让这个警告 panic 吗？`panic_on_taint=0x200` 呢？

`panic_on_warn=1` **不能**。`___might_sleep()` 没有走 `WARN()`，而是：

```c
printk("BUG: sleeping function called from invalid context ...");
dump_stack();
add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
```

`panic_on_warn` 只对 `WARN()` / `warn_slowpath` 生效。

`panic_on_taint=0x200` **可以**。`TAINT_WARN` 是 bit 9，数值 `512 = 0x200`；`add_taint(TAINT_WARN)` 会让 `panic_on_taint` 的掩码命中，从而 panic。

#### Q6. `in_atomic()` 和 `irqs_disabled()` 分别说明什么？`spin_lock()` 和 `spin_lock_irqsave()` 下有什么不同？

`in_atomic()` 在 5.15 里是：

```c
#define in_atomic() (preempt_count() != 0)
```

它表示“preempt count 非零”，常见于：

- 持有 spinlock；
- `preempt_disable()`；
- softirq 禁用；
- 硬中断 / softirq 上下文。

`irqs_disabled()` 表示**本地硬中断是否被关闭**，和 preempt count 是两件事。

对比：

| 场景 | `in_atomic()` | `irqs_disabled()` |
|---|---|---|
| `spin_lock()` | 1 | 0（除非之前已关中断） |
| `spin_lock_irqsave()` | 1 | 1 |
| `spin_lock_bh()` | 1 | 0（除非之前已关中断） |
| `local_irq_disable()` | 可能为 0 | 1 |
| 进程上下文 | 0 | 0 |

`in_atomic()` 不是完美的“能否睡眠”判断，内核注释也提醒它不能覆盖所有情况；`might_sleep()` 还会检查 IRQ 状态、抢占计数等。`spin_lock()` 不关硬中断，但仍然是原子上下文，仍然不能睡。

#### Q7. `GFP_ATOMIC` 分配失败，驱动应该怎么处理？为什么不能 `msleep()` 重试？

`GFP_ATOMIC` 失败后，调用者应该：

- 返回错误（如 `-ENOMEM`）；
- 丢包 / 丢弃本次请求；
- 把工作推迟到 workqueue / threaded IRQ / tasklet 之外的可睡眠上下文；
- 从预分配池 / mempool 取备用对象；
- 在安全时释放非必需内存，稍后重试。

不能 `msleep()`，因为当前就在原子上下文；`msleep()` 会主动调度，直接触发 `scheduling while atomic`。这正是 `GFP_ATOMIC` 要避免的事情：它承诺“不睡”，所以失败也必须用**不睡**的方式处理。

如果调用者其实在进程上下文，应该改用 `GFP_KERNEL`，而不是用 `GFP_ATOMIC` 然后靠 `msleep()` 补锅。

### 11.2 检查清单答案

对应第 9 节的 10 条：

1. **当前上下文能睡吗？**
   看是否在 spinlock、IRQ、softirq、RCU 读临界区、`preempt_disable()` 里。能睡才考虑 `GFP_KERNEL`；不能睡只能用 `GFP_ATOMIC` / `GFP_NOWAIT`，并处理失败。

2. **GFP 含 `__GFP_DIRECT_RECLAIM` 吗？**
   `GFP_KERNEL`、`GFP_NOFS`、`GFP_NOIO` 都含；`GFP_ATOMIC`、`GFP_NOWAIT` 不含。含它就代表“可能睡”。

3. **检查分配返回值了吗？失败路径能睡吗？**
   除 `__GFP_NOFAIL` 外都要检查 NULL；原子上下文的失败路径不能再睡眠，只能返回错误、丢请求、延后到 workqueue 或走预分配池。

4. **回收会不会递归进 FS / 块层？**
   会的话用 `GFP_NOFS` / `GFP_NOIO`，或用 `memalloc_nofs_save()` / `memalloc_noio_save()` 标记作用域。它们仍然可能睡，不能进原子上下文。

5. **是不是在用 `GFP_ATOMIC` / `__GFP_MEMALLOC` 消耗保留内存？**
   是的话问自己是否真的处于“系统必须前进”的紧急路径。普通进程上下文优先 `GFP_KERNEL`；能预分配就预分配；`__GFP_MEMALLOC` 要严格节流。

6. **DMA / highmem / NUMA 放置对吗？**
   DMA 用 `GFP_DMA` / `GFP_DMA32` 或 DMA API；highmem 页要 `kmap()`；`__GFP_THISNODE` 无 fallback，要处理失败；cpuset 限制用 `__GFP_HARDWALL`。

7. **需要清零 / 记账 / compound 吗？**
   用户可见或敏感数据要 `__GFP_ZERO`；用户触发的内核对象考虑 `__GFP_ACCOUNT` / `GFP_KERNEL_ACCOUNT`；高阶页当复合页用要 `__GFP_COMP` 并配套正确的 refcount 生命周期。

8. **是不是把内核页标成了 `MOVABLE`？**
   内核直接解引用、持有裸指针的页不能用 `__GFP_MOVABLE`；它可能被迁移，导致指针失效。内核页用 `GFP_KERNEL`，用户/页缓存才考虑 `GFP_HIGHUSER_MOVABLE`。

9. **是不是用 `__GFP_NOWARN` 掩盖失败？**
   是的话去掉。`__GFP_NOWARN` 只抑制日志，失败还在；它不能替代错误处理。

10. **是不是用 `__GFP_NOFAIL` 代替失败策略？**
    是的话重新评估。`__GFP_NOFAIL` 会无限重试，可能挂住/OOM；高阶分配更不鼓励。只有在确实没有合理失败策略时才用。

### 11.3 关于 `should_fail_alloc_page()` 的追问

**问：为什么放在真正分配之前？**
答：它要在分配器状态改变之前注入失败，才能做到不碰 pcp/buddy、不回收、不 OOM，只让调用者看到一个普通的 `NULL`。这样才能干净地测试上层错误路径。

**问：为什么默认不会让我的 order-0 `GFP_KERNEL` 分配失败？**
答：5.15 默认 `min_order = 1`，跳过 order-0；同时 `ignore_gfp_reclaim = true`，跳过带 `__GFP_DIRECT_RECLAIM` 的分配（包括 `GFP_KERNEL`）。要测 order-0 + `GFP_KERNEL`，需要 `min-order=0` 且 `ignore-gfp-wait=0`。

**问：它能模拟真实 OOM 吗？**
答：不能。它在分配入口直接返回 `NULL`，不会走直接回收、OOM killer，也不会改变 zone 水位。它模拟的是“调用者拿到 NULL”这个结果，不是“系统真的没内存”的全过程。

**问：为什么当前内核里它没效果？**
答：当前 `.config` 里 `CONFIG_FAULT_INJECTION` 没开，`CONFIG_FAIL_PAGE_ALLOC` 也就没开，`__should_fail_alloc_page()` 是 `return false` 的 inline 空实现。想用它需要先打开配置并重新编译内核。
