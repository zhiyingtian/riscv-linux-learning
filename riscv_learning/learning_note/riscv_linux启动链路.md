- [OPENSBI](#opensbi)
  - [从\_start到sbi\_init()之间做了什么](#从_start到sbi_init之间做了什么)
    - [1. 选择哪个 HART 来引导（boot hart）](#1-选择哪个-hart-来引导boot-hart)
    - [2. 引导 HART 竞争锁（引导抽签）](#2-引导-hart-竞争锁引导抽签)
    - [3. 运行时重定位 `.rela.dyn`（修正绝对地址）](#3-运行时重定位-reladyn修正绝对地址)
    - [4. 重置寄存器状态](#4-重置寄存器状态)
    - [5. 清零 BSS 段](#5-清零-bss-段)
    - [6. 设置临时陷阱处理入口](#6-设置临时陷阱处理入口)
    - [7. 清除 MDT（Machine Debug Trap）标志](#7-清除-mdtmachine-debug-trap标志)
    - [8. 设置临时栈](#8-设置临时栈)
    - [9. 保存启动信息](#9-保存启动信息)
    - [10. 平台初始化](#10-平台初始化)
    - [11. 读取平台配置：HART 数量、栈大小、堆大小、堆偏移](#11-读取平台配置hart-数量栈大小堆大小堆偏移)
    - [12. 初始化所有 HART 的 scratch 空间与堆基地址](#12-初始化所有-hart-的-scratch-空间与堆基地址)
    - [13. 迁移设备树（FDT）](#13-迁移设备树fdt)
    - [14. 标记 boot hart 已完成初始化](#14-标记-boot-hart-已完成初始化)
    - [15. 其他 HART 等待 boot hart 完成](#15-其他-hart-等待-boot-hart-完成)
    - [16. 非 boot hart 的 warm start 路径](#16-非-boot-hart-的-warm-start-路径)
    - [17. 读取平台 HART 信息，找到自己的 HART index](#17-读取平台-hart-信息找到自己的-hart-index)
    - [18. 计算当前 HART 的 scratch 区地址](#18-计算当前-hart-的-scratch-区地址)
    - [19. 更新 MSCRATCH](#19-更新-mscratch)
    - [20. 设置当前 HART 的栈](#20-设置当前-hart-的栈)
    - [21. 安装实际 trap 入口](#21-安装实际-trap-入口)
    - [22. 再次清 MDT](#22-再次清-mdt)
    - [23. 最终调用 `sbi_init`](#23-最终调用-sbi_init)
  - [总结：为什么要这样做](#总结为什么要这样做)


# OPENSBI
## 从_start到sbi_init()之间做了什么
参考文件:[fw_base.S](https://github.com/riscv-software-src/opensbi/blob/v1.8.1/firmware/fw_base.S) 函数:_start

下面按流程把 fw_base.S 中在执行 `sbi_init` 之前的工作逐条拆开，并说明每一步的目的。

---

### 1. 选择哪个 HART 来引导（boot hart）

```asm
MOV_3R s0, a0, s1, a1, s2, a2
call fw_boot_hart
add a6, a0, zero
MOV_3R a0, s0, a1, s1, a2, s2
li a7, -1
beq a6, a7, _try_lottery
bne a0, a6, _wait_for_boot_hart
```

- 作用：判断当前 HART 是否“首选引导 HART”。
- 目的：只有一个 HART 负责做初始化工作，其他 HART 等待。
- `fw_boot_hart` 返回了 preferred boot hart id。

---

### 2. 引导 HART 竞争锁（引导抽签）

```asm
_try_lottery:
lla a6, _boot_lottery
li a7, BOOT_LOTTERY_ACQUIRED
amoswap.w a6, a7, (a6)
bnez a6, _wait_for_boot_hart
```

- 作用：用原子操作尝试把 `_boot_lottery` 置为“已被获得”。
- 目的：防止多个 HART 同时进入初始化阶段，保证只有一个 HART 实际做初始化。

---

### 3. 运行时重定位 `.rela.dyn`（修正绝对地址）

```asm
li t0, FW_TEXT_START
lla t1, _fw_start
sub t2, t1, t0
lla t0, __rela_dyn_start
lla t1, __rela_dyn_end
beq t0, t1, _relocate_done
...
```

- 作用：计算加载地址偏移 `t2 = _fw_start - FW_TEXT_START`，遍历所有 `.rela.dyn` 重定位项。
- 目的：把链接时假定的地址修正成实际运行时地址，保证全局指针/GOT/绝对地址有效。

对于每条 `R_RISCV_RELATIVE` 重定位项：

- 读取 `r_offset`
- 读取 `r_addend`
- `r_offset` 加偏移后得到实际待初始化的指针地址
- `r_addend` 加偏移后得到指针指向的目标地址实际值
- 写回内存

这一步是启动前最关键的“地址修正”。

---

### 4. 重置寄存器状态

```asm
li ra, 0
call _reset_regs
```

- 作用：调用 `_reset_regs` 清零大部分通用寄存器。
- 目的：清除旧状态，避免寄存器中残留的垃圾值影响后续固件执行。
- 保留的寄存器：`ra`, `a0`, `a1`, `a2`, `a3`, `a4`，这些保存了传入参数和返回值。

---

### 5. 清零 BSS 段

```asm
lla s4, _bss_start
lla s5, _bss_end
_bss_zero:
REG_S zero, (s4)
add s4, s4, __SIZEOF_POINTER__
blt s4, s5, _bss_zero
```

- 作用：把 `.bss` 区域全部清为 0。
- 目的：保证未初始化数据段符合 C 规范，所有全局/静态变量初始为 0。

---

### 6. 设置临时陷阱处理入口

```asm
lla s4, _start_hang
csrw CSR_MTVEC, s4
```

- 作用：把机器模式异常向量设置为 `_start_hang`。
- 目的：初始化阶段如果发生异常，进入一个可控的死循环，便于调试而不是随机崩溃。

---

### 7. 清除 MDT（Machine Debug Trap）标志

```asm
CLEAR_MDT t0
```

- 作用：清空机器态中的 MDT 标志。
- 目的：避免因之前异常或调试状态残留而触发双重异常，做好安全启动准备。

---

### 8. 设置临时栈

```asm
lla s4, _fw_end
li s5, (SBI_SCRATCH_SIZE * 2)
add sp, s4, s5
```

- 作用：把 `sp` 指向固件末尾上方的临时栈空间。
- 目的：启动早期阶段需要一个合法栈，旧的栈位置可能不可用或未初始化。

---

### 9. 保存启动信息

```asm
MOV_5R s0, a0, s1, a1, s2, a2, s3, a3, s4, a4
call fw_save_info
MOV_5R a0, s0, a1, s1, a2, s2, a3, s3, a4, s4
```

- 作用：调用 `fw_save_info` 保存启动时传入的参数。
- 目的：后续固件运行时需要这些引导参数（例如设备树地址、启动模式等）。

---

### 10. 平台初始化

```asm
MOV_5R s0, a0, s1, a1, s2, a2, s3, a3, s4, a4
call fw_platform_init
add t0, a0, zero
MOV_5R a0, s0, a1, s1, a2, s2, a3, s3, a4, s4
add a1, t0, zero
```

- 作用：调用 `fw_platform_init` 初始化平台相关数据结构。
- 目的：让平台驱动或平台描述准备好，返回一个参数给后续处理。

---

### 11. 读取平台配置：HART 数量、栈大小、堆大小、堆偏移

```asm
lla a4, platform
lwu s7, SBI_PLATFORM_HART_COUNT_OFFSET(a4)
lwu s8, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(a4)
lwu s9, SBI_PLATFORM_HEAP_SIZE_OFFSET(a4)
```

- 作用：从 platform 结构读取 HART 相关参数。
- 目的：后续为每个 HART 分配 scratch 和栈空间。

---

### 12. 初始化所有 HART 的 scratch 空间与堆基地址

这一段比较长，目的就是给每个 HART 生成独立 scratch 区，并计算堆基地址。

```asm
lla tp, _fw_end
mul a5, s7, s8
add tp, tp, a5
lla s10, _fw_start
sub s10, tp, s10
add tp, tp, s9
add t3, tp, zero
li t2, 1
li t1, 0
_scratch_init:
...
REG_S a4, SBI_SCRATCH_FW_START_OFFSET(tp)
...
REG_S a4, SBI_SCRATCH_WARMBOOT_ADDR_OFFSET(tp)
...
REG_S t1, SBI_SCRATCH_HARTINDEX_OFFSET(tp)
add t1, t1, t2
blt t1, s7, _scratch_init
```

具体工作：

- 计算每个 HART scratch 区的地址
- 记录固件起始地址、固件大小、RW 区偏移、堆偏移、堆大小
- 保存 `next_arg1/next_addr/next_mode` 等启动参数
- 保存 `_start_warm` 和 platform 的入口
- 清空 trap_context、tmp0
- 保存 `hart index`

目的：

- 为每个 HART 提供独立、可访问的运行时 scratch 区
- 后续每个 HART 可以通过 scratch 区找到自己的状态、参数、堆信息

---

### 13. 迁移设备树（FDT）

```asm
beqz a1, _fdt_reloc_done
call fw_next_arg1
...
fdt copy loop
```

- 作用：如果传入了 FDT（设备树），将其从旧地址拷贝到新地址。
- 目的：确保内存中的设备树位于目标位置，避免原地址被覆盖或不可访问。
- 这一步影响后续启动参数和 payload 的访问地址。

---

### 14. 标记 boot hart 已完成初始化

```asm
li t0, BOOT_STATUS_BOOT_HART_DONE
lla t1, _boot_status
fence rw, rw
REG_S t0, 0(t1)
j _start_warm
```

- 作用：把 `_boot_status` 写成“boot hart 完成”。
- 目的：通知其他 HART 现在可以继续启动。

---

### 15. 其他 HART 等待 boot hart 完成

```asm
_wait_for_boot_hart:
li t0, BOOT_STATUS_BOOT_HART_DONE
lla t1, _boot_status
REG_L t1, 0(t1)
div t2, t2, zero
...
bne t0, t1, _wait_for_boot_hart
```

- 作用：非引导 HART 循环等待 `_boot_status` 被设置。
- 目的：其他 HART 在主初始化完成前不进入后续执行，避免竞争或资源未准备好。
- `div t2, t2, zero` 只是“空循环”用来减少总线压力。

---

### 16. 非 boot hart 的 warm start 路径

```asm
_start_warm:
li ra, 0
call _reset_regs
csrw CSR_MIE, zero
```

- 作用：再次清寄存器、禁用所有中断。
- 目的：让后续 HART 进入统一的干净状态。

---

### 17. 读取平台 HART 信息，找到自己的 HART index

```asm
lla a4, platform
lwu s7, ...
lwu s8, ...
REG_L s9, SBI_PLATFORM_HART_INDEX2ID_OFFSET(a4)
csrr s6, CSR_MHARTID
...
```

- 作用：读取平台里的 HART 数量、栈大小，以及 hartid->index 映射。
- 目的：让当前 HART 确认自己在平台中的“序号”，用于分配私有资源。

---

### 18. 计算当前 HART 的 scratch 区地址

```asm
lla tp, _fw_end
mul a5, s7, s8
add tp, tp, a5
mul a5, s8, s6
sub tp, tp, a5
li a5, SBI_SCRATCH_SIZE
sub tp, tp, a5
```

- 作用：计算本 HART 的 scratch 区地址。
- 目的：后面将 `mscratch` 设置为这个地址，并作为任务上下文存储区。

---

### 19. 更新 MSCRATCH

```asm
csrw CSR_MSCRATCH, tp
```

- 作用：把当前 HART 的 scratch 地址写入 `mscratch` CSR。
- 目的：异常/trap 处理时可以快速访问当前 HART 对应的 scratch 区。

---

### 20. 设置当前 HART 的栈

```asm
add sp, tp, zero
```

- 作用：把堆栈指向 scratch 区顶部（或底部，具体取决于布局）。
- 目的：后续运行需要合法执行栈。

---

### 21. 安装实际 trap 入口

```asm
lla a4, _trap_handler
csrr a5, CSR_MISA
...
csrw CSR_MTVEC, a4
```

- 作用：根据是否支持 Hypervisor，选择普通 trap 或 HYP trap 处理入口。
- 目的：保证在后续执行中断/异常时进入 SBI trap 处理路径。

---

### 22. 再次清 MDT

```asm
CLEAR_MDT t0
```

- 作用：再次清除 MDT 标志。
- 目的：确认进入 `sbi_init` 前 trap 环境没有遗留 MDT 状态。

---

### 23. 最终调用 `sbi_init`

```asm
csrr a0, CSR_MSCRATCH
call sbi_init
```

- 作用：把 `mscratch` 的值传给 `sbi_init`，开始 SBI 运行时初始化。
- 目的：进入固件主初始化逻辑，构建 SBI 服务、设备检查、中断/定时器初始化等。

---

## 总结：为什么要这样做

这段代码的总体目标是：

- 确保只有一个 HART 做全局初始化
- 修正地址，使固件可在非固定加载地址执行
- 清除寄存器与内存状态，避免旧值干扰
- 为所有 HART 建立私有 scratch 区和栈
- 为异常处理安装临时入口
- 准备平台信息和启动参数
- 等待引导 HART 完成，统一进入 `sbi_init`

换句话说：这部分代码完成的是“启动阶段的环境准备”，它不是 SBI 的核心服务，而是让 `sbi_init` 能在一个干净、正确的运行时上下文里开始工作。
