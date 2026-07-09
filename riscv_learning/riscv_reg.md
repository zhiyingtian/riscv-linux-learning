# RISC-V 通用寄存器列表与 ABI 约定

## 寄存器概览

RISC-V 架构定义了 32 个通用整数寄存器（XLEN 位宽，RV32 为 32 位，RV64 为 64 位），以及可选的 32 个浮点寄存器。每个寄存器都有其 ABI（Application Binary Interface）名称和约定用途。

| 寄存器 | ABI 名称 | 描述 | 保存者 |
|--------|----------|------|--------|
| x0 | zero | 硬连线零，读取始终返回 0，写入被忽略 | — |
| x1 | ra | 返回地址（Return Address） | Caller |
| x2 | sp | 栈指针（Stack Pointer） | Callee |
| x3 | gp | 全局指针（Global Pointer） | — |
| x4 | tp | 线程指针（Thread Pointer） | — |
| x5 | t0 | 临时寄存器 0 / 备用链接寄存器 | Caller |
| x6 | t1 | 临时寄存器 1 | Caller |
| x7 | t2 | 临时寄存器 2 | Caller |
| x8 | s0 / fp | 保存寄存器 0 / 帧指针（Frame Pointer） | Callee |
| x9 | s1 | 保存寄存器 1 | Callee |
| x10 | a0 | 函数参数 0 / 返回值 0 | Caller |
| x11 | a1 | 函数参数 1 / 返回值 1 | Caller |
| x12 | a2 | 函数参数 2 | Caller |
| x13 | a3 | 函数参数 3 | Caller |
| x14 | a4 | 函数参数 4 | Caller |
| x15 | a5 | 函数参数 5 | Caller |
| x16 | a6 | 函数参数 6 | Caller |
| x17 | a7 | 函数参数 7 | Caller |
| x18 | s2 | 保存寄存器 2 | Callee |
| x19 | s3 | 保存寄存器 3 | Callee |
| x20 | s4 | 保存寄存器 4 | Callee |
| x21 | s5 | 保存寄存器 5 | Callee |
| x22 | s6 | 保存寄存器 6 | Callee |
| x23 | s7 | 保存寄存器 7 | Callee |
| x24 | s8 | 保存寄存器 8 | Callee |
| x25 | s9 | 保存寄存器 9 | Callee |
| x26 | s10 | 保存寄存器 10 | Callee |
| x27 | s11 | 保存寄存器 11 | Callee |
| x28 | t3 | 临时寄存器 3 | Caller |
| x29 | t4 | 临时寄存器 4 | Caller |
| x30 | t5 | 临时寄存器 5 | Caller |
| x31 | t6 | 临时寄存器 6 | Caller |

## 寄存器分类

### 临时寄存器（Temporary Registers — Caller-saved）

| 寄存器 | 说明 |
|--------|------|
| t0 ~ t2 (x5 ~ x7) | 临时寄存器，调用者负责保存 |
| t3 ~ t6 (x28 ~ x31) | 临时寄存器，调用者负责保存 |

调用者（Caller）在函数调用前，如需保留这些寄存器的值，必须自行保存到栈上。被调用者（Callee）可以随意使用这些寄存器，无需恢复。

### 保存寄存器（Saved Registers — Callee-saved）

| 寄存器 | 说明 |
|--------|------|
| s0/fp (x8) | 保存寄存器 / 帧指针 |
| s1 (x9) | 保存寄存器 |
| s2 ~ s11 (x18 ~ x27) | 保存寄存器 |

被调用者（Callee）如果使用这些寄存器，必须在函数入口保存原值，在函数返回前恢复。调用者（Caller）可以假定这些值在函数调用后保持不变。

### 函数参数与返回值寄存器

| 寄存器 | 说明 |
|--------|------|
| a0 ~ a1 (x10 ~ x11) | 函数参数 0~1，同时也用于返回值 |
| a2 ~ a7 (x12 ~ x17) | 函数参数 2~7 |

当函数参数超过 8 个时，多余的参数通过栈传递。

### 特殊用途寄存器

| 寄存器 | 说明 |
|--------|------|
| zero (x0) | 硬连线零寄存器，读取永远为 0，写入无效。常用于与 0 比较、初始化寄存器等 |
| ra (x1) | 返回地址寄存器，`jal`/`jalr` 指令将返回地址写入此寄存器 |
| sp (x2) | 栈指针，指向当前栈顶。栈向下增长，需保持 16 字节对齐 |
| gp (x3) | 全局指针，用于访问全局数据（`.sdata` 和 `.sbss` 段），由链接器定义 |
| tp (x4) | 线程指针，用于访问线程局部存储（TLS） |

## 函数调用约定

### 调用者保存 vs 被调用者保存

```
调用前（Caller）:
  如果 a0~a7, t0~t6, ra 中的值在调用后还需要使用，
  调用者必须将它们保存到栈上。

调用中（Callee）:
  如果被调用者需要使用 s0~s11, sp 寄存器，
  必须在函数入口保存原值，返回前恢复。

调用后:
  a0~a1 可能包含返回值，其他 Caller-saved 寄存器值未定义。
```

### 栈帧布局

```
高地址
+-----------------+
|   调用者栈帧     |
+-----------------+
|   参数 > 8 个    |  ← 调用者传入的溢出参数
+-----------------+
|   ra 返回地址    |  ← 被调用者保存（如有需要）
+-----------------+
|   fp 旧帧指针    |  ← 被调用者保存（如有需要）
+-----------------+
|   s0 ~ s11      |  ← 被调用者保存的寄存器（如有需要）
+-----------------+
|   局部变量       |
+-----------------+
|   溢出参数区     |  ← 为调用子函数准备的参数
+-----------------+  ← sp 栈指针
低地址
```

## 浮点寄存器（RV32F/RV64F/RV32D/RV64D 扩展）

| 寄存器 | ABI 名称 | 描述 | 保存者 |
|--------|----------|------|--------|
| f0 | ft0 | 浮点临时寄存器 0 | Caller |
| f1 | ft1 | 浮点临时寄存器 1 | Caller |
| f2 | ft2 | 浮点临时寄存器 2 | Caller |
| f3 | ft3 | 浮点临时寄存器 3 | Caller |
| f4 | ft4 | 浮点临时寄存器 4 | Caller |
| f5 | ft5 | 浮点临时寄存器 5 | Caller |
| f6 | ft6 | 浮点临时寄存器 6 | Caller |
| f7 | ft7 | 浮点临时寄存器 7 | Caller |
| f8 | fs0 | 浮点保存寄存器 0 | Callee |
| f9 | fs1 | 浮点保存寄存器 1 | Callee |
| f10 | fa0 | 浮点参数 0 / 返回值 0 | Caller |
| f11 | fa1 | 浮点参数 1 / 返回值 1 | Caller |
| f12 | fa2 | 浮点参数 2 | Caller |
| f13 | fa3 | 浮点参数 3 | Caller |
| f14 | fa4 | 浮点参数 4 | Caller |
| f15 | fa5 | 浮点参数 5 | Caller |
| f16 | fa6 | 浮点参数 6 | Caller |
| f17 | fa7 | 浮点参数 7 | Caller |
| f18 | fs2 | 浮点保存寄存器 2 | Callee |
| f19 | fs3 | 浮点保存寄存器 3 | Callee |
| f20 | fs4 | 浮点保存寄存器 4 | Callee |
| f21 | fs5 | 浮点保存寄存器 5 | Callee |
| f22 | fs6 | 浮点保存寄存器 6 | Callee |
| f23 | fs7 | 浮点保存寄存器 7 | Callee |
| f24 | fs8 | 浮点保存寄存器 8 | Callee |
| f25 | fs9 | 浮点保存寄存器 9 | Callee |
| f26 | fs10 | 浮点保存寄存器 10 | Callee |
| f27 | fs11 | 浮点保存寄存器 11 | Callee |
| f28 | ft8 | 浮点临时寄存器 8 | Caller |
| f29 | ft9 | 浮点临时寄存器 9 | Caller |
| f30 | ft10 | 浮点临时寄存器 10 | Caller |
| f31 | ft11 | 浮点临时寄存器 11 | Caller |

## 参考来源

- [RISC-V ELF psABI Specification](https://github.com/riscv-non-isa/riscv-elf-psabi-doc)
- [RISC-V ISA Specification Volume 1: Unprivileged ISA](https://github.com/riscv/riscv-isa-manual)