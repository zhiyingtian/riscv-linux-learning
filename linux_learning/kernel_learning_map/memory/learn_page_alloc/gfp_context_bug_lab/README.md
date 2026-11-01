# 物理页分配 Debug 实验：原子上下文里的 GFP_KERNEL

> **内核**：`runninglinuxkernel_5.15`（Linux 5.15.0+，RISC-V，`-O0`）
> **关键配置**：`CONFIG_DEBUG_ATOMIC_SLEEP=y`，`CONFIG_PREEMPT_NONE=y`，`CONFIG_DEBUG_VM=n`
> **平台**：QEMU `virt`，1 GiB，SMP=2
> **目标**：复现 `BUG: sleeping function called from invalid context`，理解 GFP 标志和“当前上下文能不能睡眠”的关系。
> **配套总结**：[gfp_keypoints.md](gfp_keypoints.md)（GFP flag 行为与常见 bug 总结）

---

## 0. 一句话

`spin_lock_irqsave()` 保护的临界区里调用：

```c
alloc_pages(GFP_KERNEL, 0);
```

会得到：

```text
BUG: sleeping function called from invalid context at mm/page_alloc.c:5156
in_atomic(): 1, irqs_disabled(): 1 ...
```

即使这次分配从 pcp 拿到了页、**实际并没有睡眠**，这个警告也会出现。因为 `GFP_KERNEL` 表示“调用者允许直接回收”，而直接回收可能睡眠；在原子上下文里用它，本身就是错的。

---

## 1. 三种模式

模块 `gfp_context.ko` 只有一个参数 `mode`：

| mode | 上下文 | GFP | 预期 |
|---|---|---|---|
| `1` | `spin_lock_irqsave()` | `GFP_KERNEL` | **警告**：sleeping function called from invalid context |
| `2` | `spin_lock_irqsave()` | `GFP_ATOMIC` | 无警告 |
| `3` | 进程上下文 | `GFP_KERNEL` | 无警告（对照） |

`mode=1` 是 bug 演示；`mode=2` 是同一临界区里的正确写法；`mode=3` 证明 `GFP_KERNEL` 本身没问题，问题在**上下文**。

---

## 2. 5.15 里的调用链

```text
gfp_demo_init()
  spin_lock_irqsave(&demo_lock, flags)
    alloc_pages(GFP_KERNEL, 0)
      __alloc_pages()                              mm/page_alloc.c:5343
        prepare_alloc_pages()
          might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM)
            might_sleep()
              ___might_sleep()                     kernel/sched/core.c:9520
                printk("BUG: sleeping function called from invalid context ...")
                dump_stack()
                add_taint(TAINT_WARN)              kernel/sched/core.c:9540
        get_page_from_freelist()                   ← 可能从 pcp 直接拿到页
  spin_unlock_irqrestore()
```

关键在 `prepare_alloc_pages()`：

```c
might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM);
```

它位于 `__alloc_pages()` 的**快路径之前**，所以“这次从 pcp 拿到了页”并不能绕过这条检查。检查的是**你声明的 GFP 语义**，不是你这次实际有没有睡。

### 2.1 `GFP_KERNEL` 为什么“可能睡”

[`include/linux/gfp.h:324`](../../../../../../runninglinuxkernel_5.15/include/linux/gfp.h#L324)：

```c
#define GFP_KERNEL (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
```

而：

```c
#define __GFP_RECLAIM (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
```

`__GFP_DIRECT_RECLAIM` 的意思是：**允许调用者自己进入直接回收**。直接回收会扫描 LRU、等页锁、做写回，必要时 `cond_resched()`，所以可能睡眠。

[`include/linux/gfp.h:323`](../../../../../../runninglinuxkernel_5.15/include/linux/gfp.h#L323)：

```c
#define GFP_ATOMIC (__GFP_HIGH | __GFP_ATOMIC | __GFP_KSWAPD_RECLAIM)
```

`GFP_ATOMIC` **不含** `__GFP_DIRECT_RECLAIM`，所以不会进入直接回收，也不会在原子上下文里因为“允许回收”而被 `might_sleep()` 抓。

---

## 3. 快速开始

### 3.1 host 编译并同步到 9p

```bash
cd linux_learning/kernel_learning_map/memory/learn_page_alloc/gfp_context_bug_lab
make install
```

`make install` 会编译 `gfp_context.ko`，并复制到：

```text
runninglinuxkernel_5.15/kmodules/learn_page_alloc/gfp_context_bug_lab/
```

### 3.2 guest 里运行

```bash
# 登录 benshushu / 123，然后 su root
/mnt/learn_page_alloc/gfp_context_bug_lab/guest_run.sh
```

脚本会依次跑 `mode=1/2/3`，每轮清一次 dmesg，最后打印 taint。

也可以手动：

```bash
cd /mnt/learn_page_alloc/gfp_context_bug_lab

dmesg -C
insmod gfp_context.ko mode=1
rmmod gfp_context
dmesg | tail -60

dmesg -C
insmod gfp_context.ko mode=2
rmmod gfp_context
dmesg | tail -20
```

---

## 4. 预期 dmesg

### 4.1 mode=1：原子上下文 + GFP_KERNEL

```text
gfp_demo: mode=1, in_atomic()=0, irqs_disabled()=0
gfp_demo: atomic/GFP_KERNEL: about to alloc_pages(0x..., 0)
BUG: sleeping function called from invalid context at mm/page_alloc.c:5156
in_atomic(): 1, irqs_disabled(): 1, non_block: 0, pid: 123, name: insmod
preempt_count: 2, expected: 0
CPU: 0 PID: 123 Comm: insmod Tainted: G           O      5.15.0+ #3
Hardware name: riscv-virtio,qemu (DT)
Call Trace:
[<ffffffff...>] dump_backtrace+0x...
[<ffffffff...>] show_stack+0x...
[<ffffffff...>] dump_stack_lvl+0x...
[<ffffffff...>] ___might_sleep+0x...
[<ffffffff...>] __might_resched+0x...
[<ffffffff...>] prepare_alloc_pages+0x...
[<ffffffff...>] __alloc_pages+0x...
[<ffffffff...>] gfp_demo_init+0x...
[<ffffffff...>] do_one_initcall+0x...
...
gfp_demo: atomic/GFP_KERNEL: gfp=0x... pfn=... refcount=1
gfp_demo: done
gfp_demo: unloaded
```

几个要点：

- `in_atomic(): 1`：当前 `preempt_count` 非零（`spin_lock_irqsave` 造成的）。
- `irqs_disabled(): 1`：`spin_lock_irqsave` 关了本地中断。
- `preempt_count: 2`：锁 + 关中断的计数，具体值随实现略有差异。
- `mm/page_alloc.c:5156`：`prepare_alloc_pages()` 里 `might_sleep_if()` 的位置，行号以你的树为准。
- 最后仍然打印了 `pfn=... refcount=1`：说明**这次分配成功了**，警告不是“分配失败”，而是“上下文用错”。

### 4.2 mode=2：原子上下文 + GFP_ATOMIC

```text
gfp_demo: mode=2, in_atomic()=0, irqs_disabled()=0
gfp_demo: atomic/GFP_ATOMIC: about to alloc_pages(0x..., 0)
gfp_demo: atomic/GFP_ATOMIC: gfp=0x... pfn=... refcount=1
gfp_demo: done
gfp_demo: unloaded
```

没有 `sleeping function` 警告；`GFP_ATOMIC` 不允许直接回收，符合原子上下文约束。

### 4.3 mode=3：进程上下文 + GFP_KERNEL

```text
gfp_demo: mode=3, in_atomic()=0, irqs_disabled()=0
gfp_demo: process/GFP_KERNEL: about to alloc_pages(0x..., 0)
gfp_demo: process/GFP_KERNEL: gfp=0x... pfn=... refcount=1
gfp_demo: done
gfp_demo: unloaded
```

也没有警告；同样的 `GFP_KERNEL`，在可以睡眠的进程上下文里是合法的。

---

## 5. 为什么从 pcp 拿到页也会报？

因为 `GFP_KERNEL` 是一条**契约**：

> “我允许内核在分配过程中睡眠（直接回收、等锁、写回）。”

`might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM)` 检查的是这条契约和当前上下文是否匹配，不是“这次实际有没有睡”。

这次 pcp 里刚好有页，`get_page_from_freelist()` 很快返回，确实没有睡；但代码仍然是错的：

- 下一次 pcp 空了、水位低了，`__alloc_pages_slowpath()` 会进入直接回收；
- 直接回收里可能 `cond_resched()`、等页面锁、等 IO；
- 此时你还持有 spinlock / 关着中断，就会变成 `scheduling while atomic`，甚至死锁、挂住 CPU。

所以这个警告的价值是：**在“还没炸”的时候，把定时炸弹指出来。**

---

## 6. 从警告变成真事故的路径

```text
alloc_pages(GFP_KERNEL, 0)
  get_page_from_freelist() 失败（pcp 空 / 水位不够）
    __alloc_pages_slowpath()
      __alloc_pages_direct_reclaim()
        __perform_reclaim()
          cond_resched() / 等锁 / 等 IO     ← 真的睡了
```

在 `spin_lock_irqsave()` 里，这对应：

```text
BUG: scheduling while atomic
```

或者更糟：CPU 卡死、锁永远不释放。

本 demo 在 `mode=1` 里先调用 `gfp_demo_warm_pcp()`，往当前 CPU 的 pcp 放一页，目的就是**尽量让分配走快路径，只触发警告，不真的进入回收**。这是为了让实验可控，不是修复 bug。真实代码不能靠“pcp 里刚好有页”来掩盖错误。

---

## 7. GFP 标志速查

| 标志 | 含 `__GFP_DIRECT_RECLAIM`？ | 原子上下文可用？ | 典型用途 |
|---|---|---|---|
| `GFP_KERNEL` | 是 | **否** | 进程上下文，允许睡眠 |
| `GFP_ATOMIC` | 否 | 是 | 中断、spinlock、softirq |
| `GFP_NOWAIT` | 否 | 是 | 不想等、失败就返回 |
| `GFP_NOFS` | 是 | 否 | 进程上下文，避免递归进 FS |
| `GFP_NOIO` | 是 | 否 | 进程上下文，避免递归 IO |
| `GFP_HIGHUSER_MOVABLE` | 是 | 否 | 用户页，可迁移 |

注意：

- `__GFP_NORETRY` **不会**清掉 `__GFP_DIRECT_RECLAIM`；它只是“别无限重试”，不代表“原子上下文可用”。
- `__GFP_NOWARN` 只抑制分配失败日志，不改变“可能睡眠”的语义。
- `GFP_ATOMIC` 不是“保证成功”，它可能会失败；必须检查返回值并准备 fallback。

---

## 8. 正确写法

### 8.1 把分配移到锁外

```c
page = alloc_pages(GFP_KERNEL, 0);   /* 进程上下文，可以睡 */
if (!page)
    return -ENOMEM;

spin_lock_irqsave(&lock, flags);
/* 只做不能睡眠的、很短的工作 */
spin_unlock_irqrestore(&lock, flags);
```

如果页只在锁内短时间使用，也可以在锁外分配、锁内使用、锁外释放。

### 8.2 原子上下文里用 `GFP_ATOMIC` / `GFP_NOWAIT`

```c
page = alloc_pages(GFP_ATOMIC, 0);
if (!page) {
    /* 失败路径：丢包、返回 -ENOMEM、稍后重试，不能睡 */
}
```

### 8.3 预分配池 / mempool

在初始化时用 `GFP_KERNEL` 预分配，原子上下文只从池里取；池空时走失败路径，不在原子上下文里现场分配。

### 8.4 不要在这些地方用 `GFP_KERNEL`

- `spin_lock()` / `spin_lock_irqsave()` 临界区；
- 硬中断、softirq、tasklet；
- `rcu_read_lock()` 的读临界区（是否可睡取决于配置，默认按不可睡处理）；
- 任何 `preempt_disable()` / `local_irq_save()` 之后。

---

## 9. 动手问题

**Q1.** `mode=1` 的调用栈里，为什么 `prepare_alloc_pages` 在 `get_page_from_freelist` 之前？这对“pcp 快路径”意味着什么？

**Q2.** `mode=2` 为什么没有警告？`GFP_ATOMIC` 和 `GFP_KERNEL` 在 `__GFP_DIRECT_RECLAIM` 上的区别是什么？

**Q3.** 把 `mode=1` 改成 `GFP_KERNEL | __GFP_NORETRY`，还会不会警告？为什么？

**Q4.** 把 `gfp_demo_warm_pcp()` 去掉，然后在 guest 里先制造内存压力，再跑 `mode=1`，观察是否出现 `scheduling while atomic`。**做这个实验前先保存 guest 状态，可能会卡死。**

**Q5.** `panic_on_warn=1` 能让这个警告 panic 吗？`panic_on_taint=0x200` 呢？为什么？

**Q6.** `in_atomic()` 和 `irqs_disabled()` 分别说明什么？`spin_lock()` 和 `spin_lock_irqsave()` 下这两个值有什么不同？

**Q7.** 如果 `GFP_ATOMIC` 分配失败，驱动应该怎么处理？为什么不能在这里 `msleep()` 重试？

> 参考答案见 [gfp_keypoints.md](gfp_keypoints.md) 的「11. 问题与答案」。如果要边看实验边对照，建议先自己回答，再翻答案。

---

## 10. 配置与 taint

- `CONFIG_DEBUG_ATOMIC_SLEEP=y`：没有它，`might_sleep()` 不会做这套检查。
- `CONFIG_DEBUG_PREEMPT=y`（可选）：警告里会额外打印 “Preemption disabled at:” 和禁用点的符号。
- `might_sleep()` 的警告有 **1 秒限流**；连续快速重跑可能看不到第二条。
- 它调用 `add_taint(TAINT_WARN)`，taint 字母是 `W`，数值是 `512`（bit 9）。
- `dmesg -C` 只清环形缓冲区，**不会清 taint**；重启才清。
- `panic_on_warn=1` 对它无效：它没有走 `WARN()`，而是直接 `printk()` + `dump_stack()` + `add_taint()`。
- 想让它 panic，用 `panic_on_taint=0x200`（`TAINT_WARN` 的 bit）。

查看 taint：

```bash
cat /proc/sys/kernel/tainted
# 512 表示只有 TAINT_WARN；如果还有 O，说明加载了树外模块
```

---

## 11. 源码锚点（5.15）

| 位置 | 作用 |
|---|---|
| `include/linux/gfp.h:323` | `GFP_ATOMIC`：不含 `__GFP_DIRECT_RECLAIM` |
| `include/linux/gfp.h:324` | `GFP_KERNEL`：含 `__GFP_RECLAIM`（直接 + kswapd） |
| `include/linux/gfp.h:358` | `gfpflags_allow_blocking()`：判断是否允许阻塞 |
| `include/linux/kernel.h:183` | `might_sleep_if(cond)` |
| `mm/page_alloc.c:5156` | `prepare_alloc_pages()` 里的 `might_sleep_if(gfp_mask & __GFP_DIRECT_RECLAIM)` |
| `mm/page_alloc.c:5343` | `__alloc_pages()`：快路径 / 慢路径入口 |
| `mm/page_alloc.c:4872` | `__alloc_pages_slowpath()` |
| `mm/page_alloc.c:4608` | `__alloc_pages_direct_reclaim()`：直接回收，可能睡眠 |
| `kernel/sched/core.c:9520` | `___might_sleep()` 打印 “BUG: sleeping function...” |
| `kernel/sched/core.c:9540` | `add_taint(TAINT_WARN)` |
| `include/linux/panic.h:68` | `TAINT_WARN = 9`（512 / 0x200） |

---

## 12. 一页纸

- `GFP_KERNEL` = 允许直接回收 = 可能睡眠；只能用在可睡眠的进程上下文。
- `GFP_ATOMIC` / `GFP_NOWAIT` 不含直接回收，可用于原子上下文，但可能失败。
- `alloc_pages(GFP_KERNEL)` 在 spinlock/IRQ/softirq 里，`prepare_alloc_pages()` 会先 `might_sleep_if()` 报警，**即使这次从 pcp 拿到了页**。
- 警告不是“已经睡了”，而是“你声明了可以睡，但当前上下文不允许”。
- 真正的危险在 pcp 空、快路径失败后的直接回收：`scheduling while atomic`、死锁、挂 CPU。
- 修复优先级：**移到锁外** > **预分配池** > **原子上下文用 `GFP_ATOMIC`/`GFP_NOWAIT` 并处理失败**。
- 看到 `BUG: sleeping function called from invalid context`，先看 `in_atomic()` / `irqs_disabled()`，再对照 GFP 标志，不要只盯着分配返回值。

---

## 文件

| 文件 | 作用 |
|---|---|
| `gfp_keypoints.md` | GFP flag 行为与常见 bug 总结 |
| `gfp_context.c` | 三种模式的演示模块 |
| `Makefile` | 交叉编译 + `make install` 到 9p 共享目录 |
| `guest_run.sh` | guest 内依次跑 mode=1/2/3 并打印 dmesg |
| `README.md` | 本文 |
