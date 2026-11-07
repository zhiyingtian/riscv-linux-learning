# Slab UAF / Double Free / 越界写实验

> **内核**：`runninglinuxkernel_5.15`（RISC-V，`CONFIG_SLUB_DEBUG=y`，但默认未开 `slub_debug`）
> **目标**：观察 slab 对象生命周期错误的三种典型形态：
> 1. `kfree()` 后继续使用旧指针 → use-after-free（UAF）；
> 2. 同一个对象 `kfree()` 两次 → double free；
> 3. `kmalloc(32)` 却写 64 字节 → 越界写，破坏相邻对象。
> **配套**：`buddy_bug_lab`（物理页 double free）、`gfp_context_bug_lab`（GFP/上下文）

---

## 0. 一句话

`kmalloc()` 返回的对象在 `kfree()` 后生命周期就结束了。旧指针不会自动变成 NULL，内存也不会立刻消失；它可能马上被下一个 `kmalloc()` 复用。再通过旧指针读写，就是在读/写别人的对象。

```c
p = kmalloc(64, GFP_KERNEL);
kfree(p);
/* p 现在是悬空指针 */
q = kmalloc(64, GFP_KERNEL);   /* 很可能拿到同一地址 */
memset(q, 0x42, 64);           /* 新对象的数据 */
printk("%02x\n", ((u8 *)p)[0]); /* UAF 读：可能读到 0x42 */
```

---

## 1. 四种模式

| mode | 行为 | 风险 | 说明 |
|---|---|---|---|
| `1` | `kfree(stale)` 后 `kmalloc(fresh)`，读 stale | 较低 | 演示 UAF 别名；默认运行 |
| `2` | 同一个对象 `kfree` 两次 | 高 | 可能破坏 freelist，建议快照 |
| `3` | `kmalloc(32)` 后写 64 字节 | 高 | 可能破坏相邻对象，建议快照 |
| `4` | `kmalloc` + 使用 + `kfree` | 无 | 正确对照 |

`guest_run.sh` 默认只跑 `mode=1` 和 `mode=4`。`mode=2/3` 需要手动执行，并先保存 guest 快照。

---

## 2. 快速开始

```bash
cd linux_learning/kernel_learning_map/memory/labs/learn_page_alloc/slab_uaf_bug_lab
make install
```

guest 里以 root 运行：

```bash
/mnt/learn_page_alloc/slab_uaf_bug_lab/guest_run.sh
```

手动跑：

```bash
dmesg -C
insmod slab_uaf.ko mode=1
rmmod slab_uaf
dmesg | tail -40
```

危险模式（先做快照）：

```bash
insmod slab_uaf.ko mode=2
insmod slab_uaf.ko mode=3
```

---

## 3. 预期 dmesg

### 3.1 mode=1：UAF 别名

```text
slab_demo: mode=1
slab_demo: stale=ffff... (allocated, filled 0x41)
slab_demo: kfree(stale) done
slab_demo: fresh=ffff...
slab_demo: alias=1 stale[0]=0x42 fresh[0]=0x42
slab_demo: stale 和 fresh 是同一地址：旧指针变成新对象的别名
slab_demo: done
slab_demo: unloaded
```

关键点：

- `stale` 在 `kfree()` 后已经失效；
- `fresh` 很可能复用同一个 slab 对象；
- 写 `fresh` 后，通过 `stale` 能读到 `0x42`，说明两个指针指向同一块内存；
- 如果 `alias=0`，说明这次没有复用同一地址，但 `stale` 仍然是悬空指针，UAF 依然存在。

`%px` 打印的是真实地址；`%p` 在 5.15 默认会哈希，不利于观察别名。

### 3.2 mode=2：double free

```text
slab_demo: mode=2
slab_demo: obj=ffff..., free once
slab_demo: free twice (this may corrupt freelist)
```

可能的结果：

- 打开 `slub_debug=F` 时，报 `Object already free` / `Freepointer corrupt`；
- 没开调试时，可能什么都不打印，但 freelist 已经被破坏；
- 后续 `kmalloc` 可能把同一个对象分给两个用户，造成静默数据破坏；
- 严重时在任意后续分配/释放路径崩溃，现场离 bug 很远。

### 3.3 mode=3：越界写

```text
slab_demo: mode=3
slab_demo: obj=ffff..., kmalloc(32), writing 64 bytes
```

可能的结果：

- 打开 `slub_debug=Z` 时，`kfree()` 检查 redzone，报 `Redzone overwritten`；
- 没开时，可能悄悄改坏相邻对象；
- 如果相邻对象是函数指针/链表节点，可能延迟到很久以后才 crash。

---

## 4. 为什么这些 bug 难查

### 4.1 UAF

- 内存还在直接映射里，读不会 fault；
- 对象可能被复用，旧指针读到的是“新对象的数据”；
- 可能表现为逻辑错误，而不是 crash；
- 需要 KASAN/KFENCE/`slub_debug` 或仔细的所有权审查。

### 4.2 double free

- 第一次 free 后，对象进入 freelist；
- 第二次 free 会把 freelist 指向自己或形成环；
- allocator 可能把同一个对象分给两个调用者；
- 两个调用者都以为自己独占，写坏对方数据；
- 没有调试配置时可能完全不报错。

### 4.3 越界写

- slab 对象之间没有页边界保护；
- 超出对象大小可能写进 redzone（如果开了）、下一个对象、或 slab 元数据；
- 破坏什么取决于布局，症状随机；
- `CONFIG_SLAB_FREELIST_HARDENED` 当前未开，freelist 被写坏也不一定立刻发现。

---

## 5. 当前内核的检测能力

```text
CONFIG_SLUB_DEBUG=y
CONFIG_SLUB_DEBUG_ON=n
CONFIG_SLAB_FREELIST_HARDENED=n
CONFIG_KASAN=n
CONFIG_KFENCE=n
```

也就是说：

- 代码里有 SLUB 调试框架，但默认没有启用；
- 没有 KASAN/KFENCE，UAF 和越界不会自动被抓住；
- 可以通过内核 cmdline 打开 `slub_debug`：

```text
slub_debug=FZPU
```

含义：

| 标志 | 作用 |
|---|---|
| `F` | free 时做基本 sanity check，抓 double free / freelist 破坏 |
| `Z` | red zone，抓对象越界写 |
| `P` | poisoning，free 后填充 poison，UAF 更容易看出 |
| `U` | user tracking，记录分配/释放调用者 |

加启动参数需要重启 guest；建议用快照，并且只在调试内核上长期开启（有性能开销）。

更彻底的方式是单独编译一个带 `CONFIG_KASAN=y` 或 `CONFIG_KFENCE=y` 的内核；不要直接改正在用的实验内核。

---

## 6. 正确做法

1. **`kfree()` 后立刻把指针置 NULL**，尤其是结构体成员和全局变量。
2. **明确所有权**：谁分配、谁释放、谁只是借用；借用方不负责 free。
3. **不要用“指针不为 NULL”判断对象是否有效**；free 不会改调用者的指针。
4. **对象释放前先摘除所有外部引用**：链表、哈希表、RCU、workqueue、定时器。
5. **并发场景用引用计数/锁/RCU**，确保最后一个使用者离开后才 free。
6. **用 `kmem_cache` 时遵守构造/析构约定**，不要 free 后再调用对象方法。
7. **开发期开 KASAN/KFENCE + SLUB_DEBUG**，不要等线上靠猜。
8. 敏感数据可用 `kfree_sensitive()` / `memzero_explicit()`，但这不是生命周期修复。

---

## 7. 思考题

**Q1.** 为什么 `kfree(p)` 之后 `p` 不会自动变成 NULL？

**Q2.** mode=1 里 `alias=0` 时，读 `stale[0]` 为什么仍然是 UAF？

**Q3.** double free 为什么可能让两个 `kmalloc()` 返回同一个地址？

**Q4.** 越界写 1 字节和越界写 32 字节，哪个更容易立刻 crash？为什么？

**Q5.** 为什么 `CONFIG_SLAB_FREELIST_HARDENED` 不能替代 KASAN？

---

## 8. 源码锚点（5.15）

| 位置 | 内容 |
|---|---|
| `mm/slub.c` | `__kmalloc()`、`kfree()`、`free_debug_processing()`、`check_object()` |
| `mm/slab_common.c` | kmalloc cache 选择、`kmem_cache` 管理 |
| `include/linux/slab.h` | `kmalloc()` / `kfree()` / `kfree_sensitive()` 接口 |
| `mm/slub.c` | `slub_debug` 启动参数解析、redzone/poison 检查 |
| `Documentation/admin-guide/kernel-parameters.txt` | `slub_debug=` 参数说明 |

---

## 9. 一页纸

- `kfree()` 结束对象生命周期，但不会改你的指针；旧指针是悬空指针。
- UAF：free 后继续读/写，可能读到新对象的数据，也可能改坏别人。
- double free：同一个对象释放两次，破坏 freelist，可能把同一地址分给两个人。
- 越界写：slab 对象之间没有页保护，破坏谁取决于布局。
- 当前内核只有 `CONFIG_SLUB_DEBUG=y`，默认没开；KASAN/KFENCE 都没开。
- 调试：`slub_debug=FZPU`，或单独编译 KASAN/KFENCE 内核。
- 修复：free 后置空、明确所有权、释放前摘除所有引用、开发期开检测工具。
