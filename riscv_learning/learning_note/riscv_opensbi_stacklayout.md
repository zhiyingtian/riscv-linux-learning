- [OpenSBI 在内存中的总体排布](#opensbi-在内存中的总体排布)
- [2 个 HART 时的具体布局](#2-个-hart-时的具体布局)
  - [说明](#说明)
- [每个 HART 的 `tp` 计算方式](#每个-hart-的-tp-计算方式)
  - [2 个 HART 时](#2-个-hart-时)
- [代码对应关系](#代码对应关系)
- [结论](#结论)

## OpenSBI 在内存中的总体排布

假设：

- `_fw_start`：固件起始地址
- `_fw_end`：固件结束地址
- `STACK_SIZE`：每个 HART 的栈区大小，等于 `SBI_PLATFORM_HART_STACK_SIZE`
- `SCRATCH_SIZE`：每个 HART 的 scratch 大小，等于 `SBI_SCRATCH_SIZE = 0x1000`
- `N`：HART 数量

那么 OpenSBI 在内存中的范围大致是：

```text
[_fw_start, _fw_end)                     : 固件本体代码/数据/BSS
[_fw_end, _fw_end + N * STACK_SIZE)     : N 个 HART 的栈槽
```

其中每个 HART 的栈槽内部还包含一个 4KB 的 `scratch` 区。

---

## 2 个 HART 时的具体布局

```
地址↑
----------------------------------------------------------------
_fw_start
   ... 固件代码 / 数据 / BSS ...
_fw_end
----------------------------------------------------------------
Hart1 栈槽
----------------------------------------------------------------
| Hart1 栈区（向下增长） | Hart1 scratch (4KB) |
----------------------------------------------------------------
Hart0 栈槽
----------------------------------------------------------------
| Hart0 栈区（向下增长） | Hart0 scratch (4KB) |
----------------------------------------------------------------
_fw_end + 2 * STACK_SIZE
```

### 说明

- `Hart0` 的栈槽在更高地址，`Hart1` 的栈槽在更低地址
- 每个栈槽的顶部预留 `SCRATCH_SIZE`（4KB）作为该 hart 的 `sbi_scratch`
- `tp` 指向该 hart 的 `scratch` 区起始地址
- `sp` 初始也设置为这个地址，因此栈向下增长到该 hart 栈槽的低端

---

## 每个 HART 的 `tp` 计算方式

代码计算公式为：

```text
tp = _fw_end + N*STACK_SIZE - hart_index*STACK_SIZE - SCRATCH_SIZE
```

其中：

- `N` = HART 数量
- `hart_index` = 当前 hart 的索引，从 0 开始
- `SCRATCH_SIZE = 0x1000`

### 2 个 HART 时

- Hart0:
  - `tp0 = _fw_end + 2*STACK_SIZE - SCRATCH_SIZE`
- Hart1:
  - `tp1 = _fw_end + 1*STACK_SIZE - SCRATCH_SIZE`

---

## 代码对应关系

这段就是计算 `tp` 的核心：

```asm
lla tp, _fw_end
mul a5, s7, s8      # a5 = HART count * STACK_SIZE
add tp, tp, a5      # tp = _fw_end + total_stack_size
mul a5, s8, s6      # a5 = STACK_SIZE * hart_index
sub tp, tp, a5      # tp = slot_top_for_this_hart
li a5, SBI_SCRATCH_SIZE
sub tp, tp, a5      # tp = scratch_base
```

- `s7` = HART count
- `s8` = STACK_SIZE
- `s6` = hart_index

Tip:
hart 信息的获取可参考:[platform/generic/platform.c](https://github.com/riscv-software-src/opensbi/blob/v1.8.1/platform/generic/platform.c) 中的 struct sbi_platform platform结构体

---

## 结论

- OpenSBI 占用内存从 `_fw_start` 开始，到 `_fw_end + N*STACK_SIZE` 结束
- 对 2 个 HART 就是 `_fw_start` 到 `_fw_end + 2*STACK_SIZE`
- 每个 HART 的 `tp` 指向其 scratch 区起始地址
- 该 scratch 区就在各自栈槽顶部，栈空间在它下面，向低地址增长