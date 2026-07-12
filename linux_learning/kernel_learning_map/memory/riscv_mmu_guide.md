- [RISC-V 虚拟内存管理 (MMU) 完整解析](#risc-v-虚拟内存管理-mmu-完整解析)
  - [一、链接脚本中的地址定义](#一链接脚本中的地址定义)
    - [1.1 vmlinux.lds.S 中的核心定义](#11-vmlinuxldss-中的核心定义)
    - [1.2 核心地址宏定义](#12-核心地址宏定义)
    - [1.3 物理地址 vs 虚拟地址](#13-物理地址-vs-虚拟地址)
  - [二、head.S 中早期页表与 MMU 开启流程](#二heads-中早期页表与-mmu-开启流程)
    - [2.1 整体执行时序](#21-整体执行时序)
    - [2.2 relocate 函数逐行解析（汇编核心）](#22-relocate-函数逐行解析汇编核心)
    - [2.3 关于地址性质的对照表](#23-关于地址性质的对照表)
    - [2.4 "PC 从低 PA 变成高 VA"的完整时序](#24-pc-从低-pa-变成高-va的完整时序)
    - [2.5 la gp, \_\_global\_pointer$ 是否影响 PC 取指？](#25-la-gp-__global_pointer-是否影响-pc-取指)
    - [2.6 两次 SATP 切换的设计原因](#26-两次-satp-切换的设计原因)
      - [2.6.1 第一次 SATP（trampoline\_pg\_dir）：为什么不直接写 early\_pg\_dir？](#261-第一次-satptrampoline_pg_dir为什么不直接写-early_pg_dir)
      - [2.6.2 第二次 SATP（early\_pg\_dir）：切换到完整功能页表](#262-第二次-satpearly_pg_dir切换到完整功能页表)
  - [三、setup\_vm 函数解析](#三setup_vm-函数解析)
    - [3.1 函数签名与执行环境](#31-函数签名与执行环境)
    - [3.2 核心数据结构](#32-核心数据结构)
    - [3.3 核心页表对象（静态分配，BSS 段）](#33-核心页表对象静态分配bss-段)
    - [3.4 函数体分步解析](#34-函数体分步解析)
    - [3.5 setup\_vm 建立的页表总览](#35-setup_vm-建立的页表总览)
  - [四、setup\_vm\_final 函数解析](#四setup_vm_final-函数解析)
    - [4.1 执行环境 vs setup\_vm](#41-执行环境-vs-setup_vm)
    - [4.2 函数体分步解析](#42-函数体分步解析)
    - [4.3 最终虚拟地址空间布局](#43-最终虚拟地址空间布局)
  - [五、设计原因与架构思考](#五设计原因与架构思考)
    - [5.1 为什么需要两阶段 setup\_vm / setup\_vm\_final？](#51-为什么需要两阶段-setup_vm--setup_vm_final)
    - [5.2 为什么 early\_pg\_dir 不直接建立完整的线性映射？](#52-为什么-early_pg_dir-不直接建立完整的线性映射)
    - [5.3 为什么需要 trampoline 机制？能不能直接把 early\_pg\_dir 写进 SATP？](#53-为什么需要-trampoline-机制能不能直接把-early_pg_dir-写进-satp)
    - [5.4 为什么 fixmap 是必要的？](#54-为什么-fixmap-是必要的)
    - [5.5 为什么 va\_kernel\_pa\_offset 和 va\_pa\_offset 是两个不同的偏移？](#55-为什么-va_kernel_pa_offset-和-va_pa_offset-是两个不同的偏移)
  - [六、完整的启动流程时序图](#六完整的启动流程时序图)
  - [七、关键宏的数值关系一览](#七关键宏的数值关系一览)
  - [八、RISC-V Sv39 页表遍历过程](#八risc-v-sv39-页表遍历过程)

# RISC-V 虚拟内存管理 (MMU) 完整解析

本文档基于 Linux Kernel 5.15 源码，从链接脚本的地址定义、汇编启动阶段的页表创建，到 C 代码中 `setup_vm` 和 `setup_vm_final` 的详细实现，逐步解析 RISC-V 架构下虚拟内存管理的完整启动流程。

---

## 一、链接脚本中的地址定义

### 1.1 vmlinux.lds.S 中的核心定义

文件：`arch/riscv/kernel/vmlinux.lds.S`

```c
#define LOAD_OFFSET KERNEL_LINK_ADDR   // 内核链接地址（虚拟地址）

SECTIONS {
    . = LOAD_OFFSET;       // 链接器从 KERNEL_LINK_ADDR 开始分配符号
    _start = .;            // 内核起始虚拟地址
    HEAD_TEXT_SECTION
    . = ALIGN(PAGE_SIZE);

    .text : {              // 代码段
        _text = .;
        _stext = .;
        TEXT_TEXT
        ...
        _etext = .;
    }

    // ... data, bss ...
    _end = .;              // 内核结束虚拟地址
}
```

**关键点**：`_start`、`_text`、`_end` 等符号**都是虚拟地址**，由链接器在链接时根据 `KERNEL_LINK_ADDR` 计算得到。但在 MMU 开启之前，这些符号值**不等于实际物理地址**，需要通过偏移计算来转换。

### 1.2 核心地址宏定义

文件：`arch/riscv/include/asm/pgtable.h`、`arch/riscv/include/asm/page.h`

以 64 位内核为例（Sv39 页表机制）：

| 宏 | 值（典型 MAXPHYSMEM_128GB 配置） | 含义 |
|---|---|---|
| `PAGE_OFFSET` | `0xffffffe000000000` | 线性映射区起始虚拟地址 |
| `KERNEL_LINK_ADDR` | `0xffffffff80000000` | 内核镜像链接地址（高 VA） |
| `ADDRESS_SPACE_END` | `0xffffffffffffffff` | 虚拟地址空间末尾 |
| `KERN_VIRT_SIZE` | `-PAGE_OFFSET` = `0x2000000000` = 128GB | 内核虚拟空间大小 |
| `PGDIR_SHIFT` | 30 | PGD 级页表偏移（1GB 粒度） |
| `PGDIR_SIZE` | `1 << 30` = 1GB | 单个 PGD 条目映射大小 |
| `PMD_SHIFT` | 21 | PMD 级页表偏移（2MB 粒度） |
| `PMD_SIZE` | `1 << 21` = 2MB | 单个 PMD 条目映射大小 |
| `PAGE_SHIFT` | 12 | 页偏移（4KB 粒度） |
| `FIXADDR_START` | 接近顶部的某个地址 | 固定映射区起始 |
| `SATP_MODE_39` | `0x8000000000000000` | Sv39 模式位 |

### 1.3 物理地址 vs 虚拟地址

假设内核被 Bootloader 加载到物理地址 `0x80200000`：

```
物理空间:
    0x80200000 ──────────────────────────► _start (实际物理位置)
    0x80200000 + size ─────────────────► _end

虚拟空间 (链接时定义):
    0xffffffff80000000 ────────────────► _start (链接地址)
    0xffffffff80000000 + size ─────────► _end
```

因此内核代码需要维护这个偏移关系：

```c
// init.c 中
kernel_map.virt_addr = KERNEL_LINK_ADDR;    // 0xffffffff80000000
kernel_map.phys_addr = (uintptr_t)(&_start);  // 0x80200000 (MMU off时取的实际值)
kernel_map.va_kernel_pa_offset = kernel_map.virt_addr - kernel_map.phys_addr;
```

这样：`PA + va_kernel_pa_offset = VA`。

---

## 二、head.S 中早期页表与 MMU 开启流程

文件：`arch/riscv/kernel/head.S`

### 2.1 整体执行时序

```
_start_kernel
    ├── 清 BSS / 保存 hartid
    ├── la sp, init_thread_union + THREAD_SIZE   // 仍用物理地址
    ├── mv a0, s1                                 // dtb_pa
    └── call setup_vm  ──► (1) 构建页表 (物理地址执行)
        ├── 填充 early_pg_dir: 内核段 VA -> PA
        ├── 填充 fixmap: 用于早期 IO 映射
        ├── 填充 trampoline_pg_dir: 仅映射 KERNEL_LINK_ADDR
        └── 返回 head.S

    ├── la a0, early_pg_dir                       // 参数：early_pg_dir 的物理地址
    └── call relocate  ──► (2) 跳转到虚拟地址空间执行
        ├── (3) 返回后，SP/全局变量已经可以用 VA 访问

    ├── call setup_trap_vector
    ├── la tp, init_task                         // 现在用 VA
    ├── call soc_early_init
    └── tail start_kernel                        // 进入通用内核启动
        └── setup_arch
            └── ...
                └── paging_init()                 // (3) 调用 setup_vm_final
                      └── 建立完整线性映射 + 切换到 swapper_pg_dir
```

### 2.2 relocate 函数逐行解析（汇编核心）

下面的注释说明每一行汇编：**加载的地址从哪里来**（链接时定义、运行时计算）、**是什么类型的地址**（PA 还是 VA）、**起什么作用**。

```asm
# =============================================================================
# relocate: 将执行流从物理地址空间切换到虚拟地址空间
# 调用约定：a0 = early_pg_dir 的物理地址（由 head.S 中 la a0, early_pg_dir 传入）
# 执行前提：MMU 尚未开启，PC 仍是物理地址
# 核心思路：利用 trampoline 页表 + 一次 trap，将 PC 从低 PA 变成高 VA
# =============================================================================

relocate:

    # -------------------------------------------------------------------------
    # 指令①：la a1, kernel_map
    # -------------------------------------------------------------------------
    # 地址来源：kernel_map 是 C 代码中定义的全局变量
    #           struct kernel_mapping kernel_map __ro_after_init;
    #           链接器将 kernel_map 的符号分配在高 VA（0xffffffff_8000xxxx）
    # 地址性质：
    #   - 在当前"物理地址模式"下，la 伪指令实际是 auipc + addi
    #   - 结果 = 当前 PC (PA) + 相对于 kernel_map 符号的偏移
    #   - 因此 a1 = kernel_map 符号的**物理地址**（因为 PC 是 PA）
    # 作用：拿到 kernel_map 结构体所在的物理地址，以便读取 virt_addr 字段
    # -------------------------------------------------------------------------
    la a1, kernel_map

    # -------------------------------------------------------------------------
    # 指令②：REG_L a1, KERNEL_MAP_VIRT_ADDR(a1)
    # -------------------------------------------------------------------------
    # 宏定义：REG_L = ld（64 位加载），KERNEL_MAP_VIRT_ADDR = 0（结构体第一个字段）
    #         kernel_map 的定义见 init.c:
    #             struct kernel_mapping {
    #                 unsigned long virt_addr;   <-- 偏移 0，就是 kernel_map.virt_addr
    #                 uintptr_t phys_addr;       <-- 偏移 8
    #                 ...
    #             };
    # 加载来源：物理地址 a1 + 0 的内存内容，这是 setup_vm 中写进去的值：
    #           kernel_map.virt_addr = KERNEL_LINK_ADDR   (= 0xffffffff_80000000)
    # 地址性质：a1 现在存的是一个**虚拟地址常量**（从结构体中读出的数值，不是指针）
    # 作用：拿到内核代码应该运行的目标 VA 基址
    # -------------------------------------------------------------------------
    REG_L a1, KERNEL_MAP_VIRT_ADDR(a1)   // a1 = kernel_map.virt_addr
                                         //    = 0xffffffff_80000000（内核链接 VA）

    # -------------------------------------------------------------------------
    # 指令③：la a2, _start
    # -------------------------------------------------------------------------
    # 地址来源：_start 是链接脚本 vmlinux.lds.S 中定义的符号：_start = .
    #           链接器将它分配为虚拟地址 = KERNEL_LINK_ADDR
    # 地址性质：
    #   - 但当前 PC 是 PA，所以 auipc + addi 的结果 =
    #     当前 PC(PA) + (_start 的链接地址 - 本指令链接地址)
    #   - 实际上 = _start 所在的**物理地址**（即内核被 bootloader 加载的位置）
    #   - 通常是 0x80200000 或类似
    # 作用：拿到内核在物理内存中的起始地址，用于计算 VA-PA 偏移
    # -------------------------------------------------------------------------
    la a2, _start                         // a2 = _start 的物理地址（如 0x80200000）

    # -------------------------------------------------------------------------
    # 指令④：sub a1, a1, a2
    # -------------------------------------------------------------------------
    # 运算：a1 = kernel_map.virt_addr - _start 的物理地址
    #     = KERNEL_LINK_ADDR - phys_start
    #     = va_kernel_pa_offset（内核段 VA-PA 偏移）
    #     = (0xffffffff_80000000) - (0x80200000)
    # 地址性质：a1 是一个**差值常量**，不是地址，用于地址转换
    # 作用：这是整个 relocate 函数的核心值——后续所有 PA→VA 的转换都用它
    # -------------------------------------------------------------------------
    sub a1, a1, a2                       // a1 = va_kernel_pa_offset = VA - PA

    # -------------------------------------------------------------------------
    # 指令⑤：add ra, ra, a1
    # -------------------------------------------------------------------------
    # 运算：ra = ra + va_kernel_pa_offset
    #     = "call relocate" 下一条指令的 PA + offset
    #     = "call relocate" 下一条指令的 VA
    # 地址性质：ra 从**物理地址**变成了**虚拟地址**
    # 作用：这是为了最后 ret 指令能正确返回 VA 空间——ret = jalr zero, 0(ra)
    #       如果不修改 ra，ret 会跳回低地址 PA 导致再次异常
    # -------------------------------------------------------------------------
    add ra, ra, a1                       // ra = PA_return + offset = VA_return

    # -------------------------------------------------------------------------
    # 指令⑥：la a2, 1f
    # -------------------------------------------------------------------------
    # 1f = 引用下面"1:"标签的地址（forward reference）
    # 地址性质：当前 PC 是 PA，所以 a2 = "1:"标签所在的**物理地址**
    # 作用：获取 trap 处理入口（即"1:"标签）的 PA，以便下一步转换成 VA
    # -------------------------------------------------------------------------
    la a2, 1f                             // a2 = 1: 标签的物理地址

    # -------------------------------------------------------------------------
    # 指令⑦：add a2, a2, a1
    # -------------------------------------------------------------------------
    # 运算：a2 = "1:"标签的 PA + va_kernel_pa_offset
    #     = "1:"标签对应的**虚拟地址**
    # 地址性质：a2 现在是一个高地址 VA（0xffffffff_8000xxxx 范围）
    # 作用：准备将 stvec 设置为这个 VA
    # 关键洞察①：stvec 是 RISC-V 异常向量基址寄存器。发生异常时，
    #   硬件自动执行 PC = stvec。如果 stvec 是 VA，那么异常发生后
    #   PC 就变成了这个 VA——这就是 PC 从 PA 跳变到 VA 的唯一时机！
    # 关键洞察②：为什么不直接 `la a2, 1f` + `csrw stvec, a2`？
    #   因为那样 a2 是 PA。写入 stvec 后，异常发生时 PC = PA(低地址)，
    #   而 trampoline 页表没有映射低地址，会死循环异常。
    #   必须把 stvec 设置成高 VA，才能让异常后 PC 跳到高 VA。
    # -------------------------------------------------------------------------
    add a2, a2, a1                       // a2 = 1: 标签的虚拟地址

    # -------------------------------------------------------------------------
    # 指令⑧：csrw CSR_TVEC, a2
    # -------------------------------------------------------------------------
    # CSR_TVEC = stvec (Supervisor Trap Vector Base Address Register)
    # 写入值：1: 标签的虚拟地址（高 VA）
    # 作用：stvec = 高 VA，为下一步"写 SATP 触发异常"做准备
    #       异常发生时，硬件会自动让 PC = stvec，从而跳入 VA 空间
    # -------------------------------------------------------------------------
    csrw CSR_TVEC, a2                    // stvec = 高地址 VA

    # -------------------------------------------------------------------------
    # 指令⑨：srl a2, a0, PAGE_SHIFT
    # -------------------------------------------------------------------------
    # 运算：a0 = early_pg_dir 的物理地址（函数入参）
    #       a2 = a0 >> 12 = early_pg_dir 的 PPN（Physical Page Number）
    # 地址性质：a2 是 PPN（页号），用于拼 SATP 寄存器
    # 作用：SATP 的低 44 位是根页表的 PPN，这步提取它
    # -------------------------------------------------------------------------
    srl a2, a0, PAGE_SHIFT               // a2 = early_pg_dir 的 PPN

    # -------------------------------------------------------------------------
    # 指令⑩：li a1, SATP_MODE
    # -------------------------------------------------------------------------
    # SATP_MODE = (8UL << 60) = 0x80000000_00000000（Sv39 模式）
    # 作用：准备 SATP 的 MODE 字段（bit 60-63），MODE=8 表示启用 Sv39
    # -------------------------------------------------------------------------
    li a1, SATP_MODE                     // a1 = SATP_MODE_39

    # -------------------------------------------------------------------------
    # 指令⑪：or a2, a2, a1
    # -------------------------------------------------------------------------
    # 运算：a2 = (early_pg_dir PPN) | (SATP_MODE_39)
    #     = 完整的 SATP 值，用于写入 early_pg_dir 的根页表
    # 地址性质：a2 是控制寄存器值，不是地址
    # 作用：**暂存**起来，等会儿在高 VA 空间用它切换到真正的内核页表
    #       注意这里不是立即写入 SATP，先写 trampoline
    # -------------------------------------------------------------------------
    or a2, a2, a1                        // a2 = early_pg_dir 的完整 SATP 值（暂存）

    # -------------------------------------------------------------------------
    # 指令⑫：la a0, trampoline_pg_dir
    # -------------------------------------------------------------------------
    # 地址来源：trampoline_pg_dir 是 C 代码中定义的静态数组
    #           pgd_t trampoline_pg_dir[PTRS_PER_PGD] __page_aligned_bss;
    # 地址性质：当前 PC 是 PA，所以 a0 = trampoline_pg_dir 的**物理地址**
    # 作用：获取 trampoline 页表的基址，以便写入 SATP
    # -------------------------------------------------------------------------
    la a0, trampoline_pg_dir             // a0 = trampoline_pg_dir 的物理地址

    # -------------------------------------------------------------------------
    # 指令⑬：srl a0, a0, PAGE_SHIFT
    # -------------------------------------------------------------------------
    # a0 = trampoline_pg_dir 的 PPN
    # -------------------------------------------------------------------------
    srl a0, a0, PAGE_SHIFT

    # -------------------------------------------------------------------------
    # 指令⑭：or a0, a0, a1
    # -------------------------------------------------------------------------
    # a0 = trampoline_pg_dir 的 PPN | SATP_MODE_39
    #    = 完整的 trampoline SATP 值
    # -------------------------------------------------------------------------
    or a0, a0, a1                        // a0 = trampoline 的 SATP

    # -------------------------------------------------------------------------
    # 指令⑮：sfence.vma
    # -------------------------------------------------------------------------
    # 作用：刷新 TLB（Translation Lookaside Buffer）
    # 注意：此时 MMU 还没开，TLB 是空的，这条指令严格来说是多余的，
    #       但作为"写 SATP 前的好习惯"被保留，避免 bootloader 留下脏 TLB
    # -------------------------------------------------------------------------
    sfence.vma                           // 刷新 TLB

    # -------------------------------------------------------------------------
    # 指令⑯：csrw CSR_SATP, a0    ★★★ 这里是 MMU 开启 ★★★
    # -------------------------------------------------------------------------
    # 写入值：a0 = trampoline_pg_dir 的 SATP（MODE=Sv39, PPN=trampoline_pg_dir）
    # 作用：启用 MMU，根页表使用 trampoline_pg_dir
    # 重要后果①：下一条指令的取指——PC 仍是低地址 PA（例如 0x80200xxx），
    #   但 MMU 会把这个 PC 当作 VA 去查 trampoline_pg_dir。
    #   trampoline_pg_dir 只映射了高地址 VA（0xffffffff_80000000 范围），
    #   低地址 VA 没有映射 → **Page Fault 异常**！
    # 重要后果②：异常发生时，硬件自动：
    #   sepc = 故障地址（PC 的值，低地址）
    #   scause = 取指页故障
    #   stval = 故障地址
    #   PC = stvec（我们之前设置的**高地址 VA**）
    #   于是：PC 从低地址 PA 瞬间变成了高地址 VA！
    # 这就是整个 relocate 的核心魔术——用硬件 trap 的副作用把 PC 搬到 VA 空间。
    # -------------------------------------------------------------------------
    csrw CSR_SATP, a0                    // ★★★ MMU 开启，使用 trampoline_pg_dir ★★★
                                         // 下条指令取指 VA = 低地址 → 未映射 → Page Fault
                                         // 硬件：PC ← stvec（高 VA）

# -----------------------------------------------------------------------------
# 标签 1:  —— 执行到这里时，PC 已经是高虚拟地址了！
# -----------------------------------------------------------------------------
# 注意：我们是通过上面的 trap 异常跳进来的，不是通过正常的指令流
#       所以在"csrw CSR_SATP, a0"之后到"1:"之前的指令实际**不会被执行**
#       ——因为第一条取指就已经 Page Fault 了
# -----------------------------------------------------------------------------
.align 2
1:

    # -------------------------------------------------------------------------
    # 指令⑰：la a0, .Lsecondary_park
    # -------------------------------------------------------------------------
    # .Lsecondary_park 是 head.S 中定义的一个死循环标签
    # 地址性质：当前 PC 是 VA，所以 a0 = .Lsecondary_park 的**虚拟地址**
    # 作用：获取一个死循环地址，用于设置新的 stvec——防止后续意外的异常
    #       跳回低地址（如果 stvec 还是旧值就会出问题）
    # -------------------------------------------------------------------------
    la a0, .Lsecondary_park              // a0 = 死循环标签的 VA

    # -------------------------------------------------------------------------
    # 指令⑱：csrw CSR_TVEC, a0
    # -------------------------------------------------------------------------
    # stvec = 死循环标签的 VA
    # 作用：保护。如果之后意外触发异常，至少不会跳回低地址造成错乱，
    #       而是死在一个高 VA 地址的循环里，便于调试
    # -------------------------------------------------------------------------
    csrw CSR_TVEC, a0                    // stvec = 死循环（防误触发异常）

    # -------------------------------------------------------------------------
    # 指令⑲-⑳：la gp, __global_pointer$（在 .option norelax 保护下）
    # -------------------------------------------------------------------------
    # __global_pointer$ 是链接器定义的符号，位于 .sdata 附近（VA 空间）
    # .option norelax 的作用：禁止链接器将 la 替换成 lui+addi 加载绝对地址
    #   必须使用 auipc+addi（PC-relative 寻址）
    # 当前 PC = 高 VA，所以：
    #   auipc gp, imm20   → gp = 当前 PC(VA) + (imm20 << 12)
    #   addi  gp, gp, imm12 → gp = gp + imm12
    #   结果 = __global_pointer$ 的**虚拟地址** ✓
    # 为什么之前加载的 gp 是错的？
    #   在物理地址模式下（PC 是 PA）执行 `la gp, __global_pointer$` 时，
    #   auipc 用的是 PA PC，结果 = "某个看起来是 VA 但实际上指向 PA 区域的值"，
    #   这个 gp 值通过 gp-relative 寻址访问全局变量会出错。
    # 现在 PC 是 VA 了，auipc 得到的 gp 就是正确的 VA。
    # 是否影响 PC 取指？**不影响**。这条指令只修改 gp 寄存器，PC 继续递增。
    # 取指是由 MMU 查当前 PC(VA) 决定的，与 gp 无关。
    # -------------------------------------------------------------------------
.option push
.option norelax
    la gp, __global_pointer$             // ★ 重新加载 gp，现在得到正确的 VA
                                         //   不影响 PC，只影响 gp 寄存器
                                         //   后续通过 gp-relative 访问全局变量才能正确
.option pop

    # -------------------------------------------------------------------------
    # 指令㉑：csrw CSR_SATP, a2
    # -------------------------------------------------------------------------
    # 写入值：a2 = 之前暂存的 early_pg_dir 的完整 SATP 值
    # 作用：切换到完整的早期内核页表 early_pg_dir
    # 这次切换**不会触发异常**！因为：
    #   - 当前 PC = 高 VA（0xffffffff_8000xxxx）
    #   - early_pg_dir 已经在 setup_vm 中建立了这个高 VA 范围的映射
    #     （create_kernel_page_table 建立的 VA→PA 映射）
    #   - MMU 查 early_pg_dir 能找到对应的物理页，取指成功，继续执行
    # -------------------------------------------------------------------------
    csrw CSR_SATP, a2                    // 切换到 early_pg_dir（无异常，因为 PC 已是 VA）

    # -------------------------------------------------------------------------
    # 指令㉒：sfence.vma
    # -------------------------------------------------------------------------
    # 刷新 TLB，确保后续指令使用新页表的翻译结果
    # -------------------------------------------------------------------------
    sfence.vma                           // 刷新 TLB

    # -------------------------------------------------------------------------
    # 指令㉓：ret
    # -------------------------------------------------------------------------
    # ret = jalr zero, 0(ra) = 跳 ra 指向的地址
    # ra = 之前修改过的 VA（指令⑤ add ra, ra, a1 的结果）
    # 所以 ret 跳回 call relocate 的下一条指令的**虚拟地址**
    # 从此 head.S 后续的所有代码（setup_trap_vector、la tp, init_task 等）
    # 都在**虚拟地址空间**执行！
    # -------------------------------------------------------------------------
    ret                                  // 返回 VA，调用者继续在 VA 空间运行
```

### 2.3 关于地址性质的对照表

| 符号 / 变量 | 链接时地址 | 在物理模式下 la 得到的值 | 在虚拟模式下 la 得到的值 |
|---|---|---|---|
| `_start` | `KERNEL_LINK_ADDR`（VA） | 内核被加载的**物理地址**（因为 PC=PA） | 等于链接时的 VA ✓ |
| `kernel_map` 结构体基址 | 高 VA（.data 段某处） | `kernel_map` 的**物理地址** | 高 VA ✓ |
| `kernel_map.virt_addr` 字段内容 | — | 存储的是**数值常量** KERNEL_LINK_ADDR | 同样是 VA 常量 |
| `__global_pointer$` | 高 VA（.sdata 附近） | **错误值**（PC=PA 时 auipc 计算错） | 正确的 VA ✓ |

**理解要点**：`la reg, symbol` 得到的不是"符号的链接地址"，而是"当前 PC + 符号相对本指令的偏移"。当 PC 是 PA 时，结果是 PA；当 PC 是 VA 时，结果是 VA。

### 2.4 "PC 从低 PA 变成高 VA"的完整时序

下面用具体数值（假设内核在 0x80200000，VA 为 0xffffffff_80000000）演示 3 个关键时刻：

| 时刻 | 指令 / 事件 | PC 值 | 地址空间 |
|---|---|---|---|
| T0 | 执行 `csrw CSR_SATP, a0`（写 trampoline）之前 | `0x80200120`（PA） | 物理地址 |
| T1 | `csrw CSR_SATP, a0` 刚执行完，准备取下一条指令 | `0x80200124`（PA 值） | **但 MMU 已开**，用这个值当 VA 查表 |
| T2 | MMU 查 trampoline_pg_dir：VA=0x80200124 在低地址，无映射 | — | **Page Fault 异常** |
| T3 | 硬件异常处理：`sepc ← 0x80200124`，`scause ← 取指故障`，`PC ← stvec` | `0xffffffff_80000xxx`（VA） | ✓ **从此 PC 是虚拟地址！** |

**关键就是 T3**：RISC-V 规范规定异常发生时 `PC ← stvec`。stvec 是我们之前刻意设置的**高 VA**，所以 PC 直接变成了 VA。

### 2.5 la gp, __global_pointer$ 是否影响 PC 取指？

**不影响**。详细说明：

1. **la 伪指令的实际编码**（在 `.option norelax` 下）：
   ```
   auipc gp, 0x00001000   # gp = PC + 0x1000000
   addi  gp, gp, 0x800    # gp = gp + 0x800
   # 结果：gp = __global_pointer$ 的链接地址
   ```

2. **PC 取指与 gp 无关**：
   - 取指逻辑：`当前 PC → MMU 翻译 → 得到物理地址 → 读取指令 → PC += 4`
   - `auipc` 只修改 gp 寄存器，不修改 PC；PC 继续线性递增
   - 所以后续指令的取址不受 gp 值的影响

3. **为什么必须重新加载 gp**：
   - gp 用于 RISC-V 的 `gp-relative` 寻址（如 `lw rd, -2048(gp)`），访问小全局变量
   - 在物理地址模式下，`la gp, __global_pointer$` 得到的是错误值（因为那时 PC 是 PA）
   - 现在 PC 是 VA，重新执行 `la gp, __global_pointer$`，`auipc` 用 VA PC 计算，得到正确的 gp
   - **如果跳过这条指令**，后续通过 gp 访问全局变量（如 `kernel_map`、`early_pg_dir` 等）会访问错误地址，导致崩溃

4. **与 PC 跳转的对比**：
   - 影响 PC 的指令：`jal / jalr / ret / branch / csrw stvec（异常时由硬件修改）`
   - 不影响 PC 的指令：`la / li / add / sub / lw / csrw（普通 CSR，非 stvec）等`
   - `la gp, __global_pointer$` 属于后者，只改 gp，不改 PC

### 2.6 两次 SATP 切换的设计原因

**核心机制**：利用"一次 page fault 异常"将 PC 从低地址 PA 切换到高地址 VA。

RISC-V 规范规定：异常发生时 `PC ← stvec`。我们预先把 `stvec` 设为高 VA，写 SATP 打开 MMU 后，让 MMU 在翻译低地址 PC 时故意触发异常，硬件就自动把 PC 搬到 stvec 指向的高 VA——这就是"trap 切换"的魔术。

#### 2.6.1 第一次 SATP（trampoline_pg_dir）：为什么不直接写 early_pg_dir？

一个常见的疑问：**`early_pg_dir` 中同样建立了 KERNEL_LINK_ADDR 的高 VA 映射，直接把它写入 SATP 不行吗？**

**可以，但不保证 100% 成功。** 问题出在 `early_pg_dir` 中**除了内核段映射**，**还存在其他低地址 VA 的映射**：

```
early_pg_dir 的映射全景：
  ┌─────────────────────────────────────────────────────────────┐
  │  PGD[511]  →  内核代码段  (KERNEL_LINK_ADDR, 高 VA)  ✅       │
  │  PGD[?]    →  fixmap      (FIXADDR_START, 高 VA)     ✅     │
  │  PGD[1]    →  early_dtb_pmd → FDT (0x40000000, 低地址 VA!)  │  ← 风险
  │  PGD[其他] →  0 (空)                                        │
  └─────────────────────────────────────────────────────────────┘
```

**风险场景分析**：

```
场景：Bootloader 把内核加载到非常规地址（例如 0x40001000）
       csrw SATP, early_pg_dir  执行时 PC = 0x40001000
       MMU 查 early_pg_dir：VA = 0x40001000
       PGD[1] = PPN(early_dtb_pmd) → PMD 中有映射（FDT 的早期映射）
       → **不触发异常！**
       结果：CPU 把 FDT 数据段的内容当成指令来取 → 崩溃（且没有异常，极难调试）
```

相比之下，`trampoline_pg_dir` 是**一个人为构造的极简页表**：

```
trampoline_pg_dir:
  ┌─────────────────────────────────────────────────────────────┐
  │  PGD[0 ... 510] = 0          (低地址 100% 未映射)             │
  │  PGD[511]       = PPN(trampoline_pmd) + PAGE_TABLE          │
  │                       ↓                                     │
  │                  trampoline_pmd[k] = PPN(phys_addr) + X     │
  │                  (只映射 KERNEL_LINK_ADDR 那一个 PMD 范围)     │
  └─────────────────────────────────────────────────────────────┘
```

**trampoline 的安全保证**：

| 属性 | early_pg_dir | trampoline_pg_dir |
|---|---|---|
| 低地址 VA 取指是否一定异常？ | ❌ **不一定**（FDT/fixmap 可能意外命中） | ✅ **100% 异常**（低地址全空） |
| 异常后 stvec(高 VA) 是否一定命中？ | ✅ 是 | ✅ 是 |
| 是否依赖内核加载地址？ | **是**（某些地址会出错） | ❌ **不依赖** |
| 代码/映射复杂度 | 复杂（多段映射） | 极简（只有内核段的一个 PMD 条目） |

> **一句话总结**：`early_pg_dir` 不是"干净的"空页表，里面有 FDT、fixmap 等低地址映射。
> 直接写它，写 SATP 后的低地址 PC 取指**不一定**能触发异常。
> trampoline 页表通过"低地址全空 + 高地址只有内核段"的极简构造，
> 从数学上保证了切换路径的确定性。

#### 2.6.2 第二次 SATP（early_pg_dir）：切换到完整功能页表

第一次 SATP 切换（trampoline）只负责一件事：**把 PC 从 PA 搬到 VA**。
trampoline_pg_dir 只映射了 2MB 范围，不够用——它甚至没有映射 `.data`、`.bss` 段，没有 fixmap，不能访问 FDT。

第二次写 SATP 切换到 `early_pg_dir`：

| 步骤 | 说明 |
|---|---|
| 切换时机 | 已在高 VA 执行，`stvec` 已改成死循环保护 |
| 写入值 | 第一次切换前暂存的 `a2 = SATP(early_pg_dir)` |
| 是否会再次触发异常？ | **不会**——PC 已是高 VA，`early_pg_dir` 中高 VA 内核段的映射已建立 |
| 作用 | 获得完整的早期运行环境：内核代码段 + fixmap 访问 + FDT 访问 |

> **关键理解**：页表条目（包括 SATP 的 PPN 域）存储的**都是物理地址**，
> 不需要因为"跑到 VA 了"就重新计算。`a2` 是第一次切换前算好的，
> 在 VA 模式下直接写 SATP 仍然有效——因为 SATP 存的是物理页号，与当前运行在哪种地址空间无关。

> **工程思想类比**：trampoline 就像"紧急弹射座椅"——功能极简、无副作用、100% 可控。
> 你不会用它来开飞机，但在"PA → VA 切换"这种特殊时刻，它是最可靠的选择。
> 弹射成功后，再切换到 early_pg_dir（"完整飞机"）开始正常飞行。

---

## 三、setup_vm 函数解析

文件：`arch/riscv/mm/init.c`

### 3.1 函数签名与执行环境

```c
asmlinkage void __init setup_vm(uintptr_t dtb_pa)
```

**执行时状态**：
- MMU **关闭**，所有地址都是物理地址
- 内核 BSS 已清零
- 参数 `dtb_pa` 是设备树 blob 的**物理地址**
- 静态分配的页表内存（`early_pg_dir`、`trampoline_pg_dir`、`fixmap_pte`、`early_pmd`、`trampoline_pmd` 等）已可访问

### 3.2 核心数据结构

```c
struct kernel_mapping {
    unsigned long virt_addr;      // 内核虚拟基址 = KERNEL_LINK_ADDR
    uintptr_t phys_addr;          // 内核物理基址 = &_start (物理模式下读到的实际PA)
    uintptr_t size;               // 内核大小 = _end - _start
    unsigned long va_pa_offset;   // PAGE_OFFSET - phys_addr (线性映射用)
    unsigned long va_kernel_pa_offset;  // virt_addr - phys_addr (内核映射用)
};

struct kernel_mapping kernel_map __ro_after_init;
```

### 3.3 核心页表对象（静态分配，BSS 段）

```c
pgd_t swapper_pg_dir[PTRS_PER_PGD] __page_aligned_bss;    // 最终运行的页表
pgd_t trampoline_pg_dir[PTRS_PER_PGD] __page_aligned_bss; // 蹦床页表
pgd_t early_pg_dir[PTRS_PER_PGD] __initdata __aligned(PAGE_SIZE); // 早期页表

static pte_t fixmap_pte[PTRS_PER_PTE] __page_aligned_bss;   // fixmap 的 PTE 页
static pmd_t trampoline_pmd[PTRS_PER_PMD] __page_aligned_bss; // trampoline 的 PMD 页
static pmd_t fixmap_pmd[PTRS_PER_PMD] __page_aligned_bss;    // fixmap 的 PMD 页
static pmd_t early_pmd[PTRS_PER_PMD] __initdata __aligned(PAGE_SIZE);
```

### 3.4 函数体分步解析

**步骤 1：初始化 kernel_map（建立 PA↔VA 关系）**

```c
kernel_map.virt_addr = KERNEL_LINK_ADDR;      // 0xffffffff80000000
kernel_map.phys_addr = (uintptr_t)(&_start);    // 0x80200000 (MMU off时读到的是PA)
kernel_map.size = (uintptr_t)(&_end) - kernel_map.phys_addr;

// 两个关键偏移
kernel_map.va_pa_offset = PAGE_OFFSET - kernel_map.phys_addr;      // 线性映射偏移
kernel_map.va_kernel_pa_offset = kernel_map.virt_addr - kernel_map.phys_addr;  // 内核映射偏移

riscv_pfn_base = PFN_DOWN(kernel_map.phys_addr);

// 对齐检查
BUG_ON((PAGE_OFFSET % PGDIR_SIZE) != 0);
BUG_ON((kernel_map.phys_addr % PMD_SIZE) != 0);
```

**步骤 2：设置早期页表分配器（物理地址模式）**

```c
pt_ops.alloc_pte = alloc_pte_early;     // 在物理地址模式下，不需要分配 PTE
pt_ops.get_pte_virt = get_pte_virt_early; // 直接把 PA 当指针用 (pa == va)
pt_ops.alloc_pmd = alloc_pmd_early;     // 用静态的 early_pmd 数组
pt_ops.get_pmd_virt = get_pmd_virt_early;
```

`alloc_pmd_early` 的实现：
```c
static phys_addr_t __init alloc_pmd_early(uintptr_t va)
{
    // 确保只在 kernel_map.virt_addr 所在的 PGD 范围分配
    BUG_ON((va - kernel_map.virt_addr) >> PGDIR_SHIFT);
    return (uintptr_t)early_pmd;  // 返回静态 early_pmd 数组的物理地址
}
```

注意这里的"分配"不是动态分配，而是使用预先静态分配的 `early_pmd` 数组。这是因为在 `setup_vm` 阶段还没有任何内存分配器（slub/slub 等还没初始化）。

**步骤 3：为 early_pg_dir 设置 fixmap 的 PGD 条目**

```c
create_pgd_mapping(early_pg_dir, FIXADDR_START,
                   (uintptr_t)fixmap_pgd_next, PGDIR_SIZE, PAGE_TABLE);
```

`create_pgd_mapping` 的逻辑（简化版）：
```c
void create_pgd_mapping(pgd_t *pgdp, uintptr_t va, phys_addr_t pa,
                        phys_addr_t sz, pgprot_t prot)
{
    uintptr_t pgd_idx = pgd_index(va); // pgd_idx = (va >> 30) & 511

    if (sz == PGDIR_SIZE) {
        // 恰好一个 PGD 范围(1GB)，直接在 PGD 放叶子节点
        pgdp[pgd_idx] = pfn_pgd(PFN_DOWN(pa), prot);
        return;
    }

    // 需要下一级页表：分配一个 PMD 页
    if (pgd_val(pgdp[pgd_idx]) == 0) {
        phys_addr_t next_phys = alloc_pmd(va);
        pgdp[pgd_idx] = pfn_pgd(PFN_DOWN(next_phys), PAGE_TABLE);
        pmd_t *nextp = (pmd_t *)get_pmd_virt(next_phys); // 早期就是 PA 本身
        memset(nextp, 0, PAGE_SIZE);
    }
    // 递归到下一级
    create_pmd_mapping(pmd_page(pgdp[pgd_idx]), va, pa, sz, prot);
}
```

这里 `create_pgd_mapping(early_pg_dir, FIXADDR_START, fixmap_pgd_next, PGDIR_SIZE, PAGE_TABLE)` 的含义是：

> 在 `early_pg_dir` 的 PGD 中，为虚拟地址 `FIXADDR_START` 所在的那一页 (1GB) 建立映射，指向 `fixmap_pgd_next`（即 `fixmap_pmd`），类型为 `PAGE_TABLE`（表示这是指向下一级的指针，不是叶子）。

**步骤 4：设置 fixmap 的 PMD 条目**

```c
create_pmd_mapping(fixmap_pmd, FIXADDR_START,
                   (uintptr_t)fixmap_pte, PMD_SIZE, PAGE_TABLE);
```

含义：在 `fixmap_pmd` 中，为 `FIXADDR_START` 所在的 PMD 条目填入指向 `fixmap_pte` 的指针（类型 `PAGE_TABLE`）。

这样就建立了 `early_pg_dir` → `fixmap_pmd` → `fixmap_pte` 的层级关系。后续通过 `__set_fixmap()` 可以把任意物理地址映射到 fixmap 区域，供早期启动使用。

**步骤 5：构建 trampoline 页表（核心）**

```c
// 5a: 在 trampoline_pg_dir 中建立 KERNEL_LINK_ADDR 对应的 PGD 条目
//     指向 trampoline_pmd
create_pgd_mapping(trampoline_pg_dir, kernel_map.virt_addr,
                   (uintptr_t)trampoline_pmd, PGDIR_SIZE, PAGE_TABLE);

// 5b: 在 trampoline_pmd 中建立 VA -> PA 的直接叶子映射
//     这是一个 PMD_SIZE (2MB) 的大页映射！
create_pmd_mapping(trampoline_pmd, kernel_map.virt_addr,
                   kernel_map.phys_addr, PMD_SIZE, PAGE_KERNEL_EXEC);
```

**trampoline 页表的最终形态**：

```
trampoline_pg_dir:
    [0 ... 510] = 0 (空)
    [511]       = PPN(trampoline_pmd) | PAGE_TABLE
                      |
                      ▼
             trampoline_pmd:
                [0 ... k-1] = 0
                [k]         = PPN(phys_addr) | PAGE_KERNEL_EXEC
                              （k = (KERNEL_LINK_ADDR >> 21) & 511）
                [k+1 ... 511] = 0
```

**这是一个极度精简的页表**，只映射了内核代码所在的一个 PMD 范围（2MB）。但足够 `relocate` 函数中 trap 跳转后的代码执行了。

**步骤 6：创建内核早期页表映射（early_pg_dir）**

```c
create_kernel_page_table(early_pg_dir, true);   // true = early 阶段
```

实现（简化）：
```c
static void __init create_kernel_page_table(pgd_t *pgdir, bool early)
{
    uintptr_t va, end_va;

    end_va = kernel_map.virt_addr + kernel_map.size;
    // 以 PMD_SIZE(2MB) 为步长，逐段建立映射
    for (va = kernel_map.virt_addr; va < end_va; va += PMD_SIZE) {
        // 映射: va -> phys_addr + (va - virt_addr)
        create_pgd_mapping(pgdir, va,
            kernel_map.phys_addr + (va - kernel_map.virt_addr),
            PMD_SIZE,
            early ? PAGE_KERNEL_EXEC : pgprot_from_va(va));
    }
}
```

注意：`early=true` 时所有映射都是 `PAGE_KERNEL_EXEC`（全内核都可执行），这是因为在早期阶段我们还无法区分 text/rodata/data 段的精细权限，需要到后面 `setup_vm_final()` 和 `mark_rodata_ro()` 才收紧权限。

**步骤 7：创建设备树（DTB）的早期映射**

```c
create_fdt_early_page_table(early_pg_dir, dtb_pa);
```

实现：
```c
#define DTB_EARLY_BASE_VA  PGDIR_SIZE   // = 0x40000000，一个低地址 VA

static void __init create_fdt_early_page_table(pgd_t *pgdir, uintptr_t dtb_pa)
{
    uintptr_t pa = dtb_pa & ~(PMD_SIZE - 1);  // 对齐到 2MB

    // 在 early_pg_dir 中建立: DTB_EARLY_BASE_VA (1GB处) -> pa
    // 这是一个 PGD 级别的 1GB 大页映射
    create_pgd_mapping(early_pg_dir, DTB_EARLY_BASE_VA,
                       (uintptr_t)early_dtb_pmd, PGDIR_SIZE, PAGE_TABLE);

    // 再在 early_dtb_pmd 中建立具体的 PMD 映射
    create_pmd_mapping(early_dtb_pmd, DTB_EARLY_BASE_VA,
                       pa, PMD_SIZE, PAGE_KERNEL);
    create_pmd_mapping(early_dtb_pmd, DTB_EARLY_BASE_VA + PMD_SIZE,
                       pa + PMD_SIZE, PMD_SIZE, PAGE_KERNEL);

    // 得到早期 DTB 的访问 VA
    dtb_early_va = (void *)DTB_EARLY_BASE_VA + (dtb_pa & (PMD_SIZE - 1));
    dtb_early_pa = dtb_pa;
}
```

这一步使得内核能够在 MMU 开启后，通过 `dtb_early_va` 访问设备树。设备树包含了内存布局、CPU 数量等关键信息，是后续内存管理初始化的基础。

### 3.5 setup_vm 建立的页表总览

```
early_pg_dir:
  ├─ [pgd_idx of FIXADDR_START] -> fixmap_pmd -> fixmap_pte (供早期 IO 用)
  ├─ [pgd_idx of DTB_EARLY_BASE_VA] -> early_dtb_pmd -> 设备树物理页
  └─ [pgd_idx of KERNEL_LINK_ADDR...] -> early_pmd -> 内核代码/数据段 (多个 PMD 条目)

trampoline_pg_dir:
  └─ [pgd_idx of KERNEL_LINK_ADDR] -> trampoline_pmd -> 内核代码 (1个 PMD 条目, 2MB)
      （只够执行 relocate 函数中的 trap 路径代码）
```

---

## 四、setup_vm_final 函数解析

```c
static void __init setup_vm_final(void)
```

### 4.1 执行环境 vs setup_vm

| 阶段 | MMU | 地址空间 | 可用基础设施 |
|---|---|---|---|
| `setup_vm` | 关闭 | 物理地址 | 只有静态分配的数组 |
| `setup_vm_final` | 开启 | 虚拟地址 | memblock 内存分配器可用 |

`setup_vm_final` 被 `paging_init()` 调用，后者在 `setup_arch()` 中执行，此时内核已经运行在虚拟地址空间。

### 4.2 函数体分步解析

**步骤 1：设置 fixmap 阶段的页表分配器**

```c
pt_ops.alloc_pte = alloc_pte_fixmap;      // 用 memblock_phys_alloc 分配
pt_ops.get_pte_virt = get_pte_virt_fixmap; // 用 fixmap 临时映射物理页
pt_ops.alloc_pmd = alloc_pmd_fixmap;
pt_ops.get_pmd_virt = get_pmd_virt_fixmap;
```

和 early 阶段不同，现在我们有了 memblock 分配器，可以动态分配页表页。但因为线性映射还没完全建立，访问新分配的物理页需要通过 fixmap 临时映射。

```c
static inline pte_t *__init get_pte_virt_fixmap(phys_addr_t pa)
{
    clear_fixmap(FIX_PTE);
    return (pte_t *)set_fixmap_offset(FIX_PTE, pa);
    // 把物理地址 pa 映射到 FIX_PTE 对应的虚拟地址，填充页表后解除映射
}
```

**步骤 2：在 swapper_pg_dir 中建立 fixmap 映射**

```c
create_pgd_mapping(swapper_pg_dir, FIXADDR_START,
                   __pa_symbol(fixmap_pgd_next),  // fixmap_pmd 的物理地址
                   PGDIR_SIZE, PAGE_TABLE);
```

`swapper_pg_dir` 是最终运行时的页表根。这里先把 fixmap 映射好，因为后面建立线性映射时需要用到 fixmap 来访问新分配的页表页。

**步骤 3：建立完整的线性映射（核心工作）**

```c
for_each_mem_range(i, &start, &end) {
    // 跳过超出 memory_limit 的范围
    if (start >= end)
        break;
    if (start <= __pa(PAGE_OFFSET) && __pa(PAGE_OFFSET) < end)
        start = __pa(PAGE_OFFSET);
    if (end >= __pa(PAGE_OFFSET) + memory_limit)
        end = __pa(PAGE_OFFSET) + memory_limit;

    // 选择最佳映射粒度（尽量用 PMD 级大页）
    map_size = best_map_size(start, end - start);

    // 逐段建立映射: VA = __va(pa) = pa + va_pa_offset
    for (pa = start; pa < end; pa += map_size) {
        va = (uintptr_t)__va(pa);
        create_pgd_mapping(swapper_pg_dir, va, pa, map_size,
                           pgprot_from_va(va));
    }
}
```

这段代码遍历 memblock 中所有可用的内存范围，为每一段物理内存建立 `PAGE_OFFSET + pa` → `pa` 的线性映射。`__va(pa)` 在早期定义为：

```c
#define linear_mapping_pa_to_va(x)  ((void *)((unsigned long)(x) + kernel_map.va_pa_offset))
```

**步骤 4（64 位）：在 swapper_pg_dir 中重新建立内核映射**

```c
#ifdef CONFIG_64BIT
    create_kernel_page_table(swapper_pg_dir, false);  // false = 非 early 阶段
#endif
```

`early=false` 时权限由 `pgprot_from_va(va)` 决定：
- text 段 → `PAGE_KERNEL_READ_EXEC`（只读可执行）
- rodata 的线性映射别名 → `PAGE_KERNEL_READ`（只读）
- 其他 → `PAGE_KERNEL`（读写）

**步骤 5：清理临时 fixmap 映射**

```c
clear_fixmap(FIX_PTE);
clear_fixmap(FIX_PMD);
```

释放临时用的 fixmap 槽位。

**步骤 6：切换到 swapper_pg_dir（最终页表）**

```c
csr_write(CSR_SATP, PFN_DOWN(__pa_symbol(swapper_pg_dir)) | SATP_MODE);
local_flush_tlb_all();
```

这是第三次写入 SATP，从此内核运行在完整的、有线性映射的页表上。

**步骤 7：切换到"晚期"页表分配器**

```c
pt_ops.alloc_pte = alloc_pte_late;     // 用 __get_free_page
pt_ops.get_pte_virt = get_pte_virt_late; // 直接用 __va
pt_ops.alloc_pmd = alloc_pmd_late;
pt_ops.get_pmd_virt = get_pmd_virt_late;
```

现在线性映射已经建立，`__va(pa)` 可以直接工作，不再需要 fixmap 绕一圈了。

### 4.3 最终虚拟地址空间布局

```
高地址 (0xffffffff_ffffffff)
    ├─ [KERNEL_LINK_ADDR ~] 内核镜像映射 (VA -> PA of kernel)
    │   ├─ .text (RX)
    │   ├─ .rodata (R)
    │   └─ .data/.bss (RW)
    │
    ├─ [MODULES ~] 内核模块区域
    ├─ [BPF_JIT] BPF JIT 区域
    │
    ├─ [VMALLOC_START ~ VMALLOC_END] vmalloc 区
    ├─ [VMEMMAP_START ~ VMEMMAP_END] struct page 数组
    ├─ [PCI_IO_START ~ PCI_IO_END] PCI IO 映射区
    └─ [FIXADDR_START ~ FIXADDR_TOP] fixmap 固定映射区

[PAGE_OFFSET ~ high_memory] 线性映射区 (VA = PA + va_pa_offset)
    └─ 覆盖所有物理内存（128GB 上限）

低地址 (用户空间)
    └─ 0x0000000000000000 ~ (TASK_SIZE)
```

---

## 五、设计原因与架构思考

### 5.1 为什么需要两阶段 setup_vm / setup_vm_final？

**根本原因**：在没有 MMU 的早期阶段，我们无法使用常规的内存分配机制（slab、vmalloc 等）。但开启 MMU 又需要页表，页表又需要内存——这是一个典型的"鸡生蛋问题"。

| 阶段 | 面临的约束 | 解决方案 |
|---|---|---|
| setup_vm（MMU off） | 不能用内存分配器，只能用静态数组 | `early_pg_dir` + 静态的 `early_pmd`，只映射内核核心代码段和 fixmap/DTB |
| setup_vm_final（MMU on） | 可以运行 C 代码，有 memblock 分配器 | 动态分配页表，建立完整的线性映射，切换到 `swapper_pg_dir` |

这种"先建立最小可用环境，再跑 C 代码来扩展"的模式是几乎所有架构（x86, ARM64, RISC-V）的共同设计。

### 5.2 为什么 early_pg_dir 不直接建立完整的线性映射？

线性映射需要覆盖全部物理内存，可能有几十 GB 甚至上百 GB，需要大量的 PMD/PTE 页表页：
- 每 1GB 物理内存需要：1 个 PMD 页（4KB）→ 512 个 PMD 条目
- 128GB 物理内存 = 128 个 PMD 页 = 512KB 页表内存

这些页表在 `setup_vm` 阶段只能来自静态数组（因为没有分配器），但内核 BSS 段大小是固定的、有限的，无法为潜在的海量物理内存预留空间。所以只能先用 early 页表启动起来，等有了 memblock 分配器之后再扩展。

### 5.3 为什么需要 trampoline 机制？能不能直接把 early_pg_dir 写进 SATP？

**可以，但有前提条件**：`early_pg_dir` 必须同时映射"内核虚拟地址"和"物理地址（被当作虚拟地址用）"。也就是说：

```
方案 A（需要恒等映射）：
    early_pg_dir 中同时存在:
      - 0xffffffff80000000 -> 0x80200000  （内核映射）
      - 0x80200000 -> 0x80200000           （恒等映射，让写 SATP 后的取指能成功）

方案 B（trampoline 机制，当前代码用的）：
    利用 trap 跳转强制把 PC 变成高 VA，从而不需要恒等映射
```

当前代码选择了方案 B，因为它更通用：即使内核代码位于各种不同的物理地址（XIP、非对齐等），也能正确工作。而方案 A 需要提前知道内核代码在哪个物理地址范围，并为这些范围建立恒等映射——在某些情况下（如 XIP 内核在 Flash 上运行）会更复杂。

### 5.4 为什么 fixmap 是必要的？

fixmap 提供了一个**固定的虚拟地址窗口**，可以在任意时刻把任意物理地址映射进来访问。在 `setup_vm_final` 阶段它被用来：
1. 临时映射新分配的页表页（以便填写其中的内容）
2. 在更早的阶段用于访问 UART、GPIO 等 MMIO 寄存器

在整个内核启动阶段，fixmap 是唯一能保证"某个虚拟地址一定能映射到某个物理地址"的机制，它在线性映射建立之前、vmalloc 初始化之前都可以工作。

### 5.5 为什么 va_kernel_pa_offset 和 va_pa_offset 是两个不同的偏移？

在 64 位 RISC-V 上：
```
va_kernel_pa_offset = KERNEL_LINK_ADDR - phys_addr   // 内核映射偏移
va_pa_offset = PAGE_OFFSET - phys_addr                // 线性映射偏移
```

它们不同，因为：
- **内核镜像本身**被映射到 `KERNEL_LINK_ADDR` 这个高地址（例如 0xffffffff_80000000）
- **物理内存整体**被映射到 `PAGE_OFFSET` 开始的区域（例如 0xffffffe0_00000000）

这样内核代码有两个虚拟地址别名：
1. 通过内核映射访问：`KERNEL_LINK_ADDR + offset`
2. 通过线性映射访问：`PAGE_OFFSET + phys_addr`

这在某些场景（如 crash dump、内核分析工具）中是有用的，也允许内核同时映射比内核镜像大得多的物理内存。

---

## 六、完整的启动流程时序图

```
Bootloader
    │
    ▼ 跳到物理地址 0x80200000 (_start)
head.S (_start_kernel)
    │  仍在物理地址模式
    ├── la a0, dtb_pa          保存设备树物理地址
    └── call setup_vm
        │   (在 init.c 中运行)
        │   物理地址访问静态分配的页表数组
        │   填充 early_pg_dir / trampoline_pg_dir / fixmap
        └── 返回
    │
    ├── la a0, early_pg_dir
    └── call relocate
        │
        │   1. add ra, ra, offset   ra 变成虚拟地址
        │   2. csrw stvec, 1f(VA)   trap vector 指向高 VA
        │   3. 预先计算好 a2 = SATP(early_pg_dir)
        │   4. csrw SATP, trampoline → MMU 开启
        │      下条指令 VA = 低地址，trampoline 没映射 → PAGE FAULT
        │      → CPU 跳 stvec = VA(1:) → PC 变成高 VA！
        │
        │  1: (现在运行在高 VA)
        │   ├── csrw stvec, .Lsecondary_park  (死循环, 防误触发)
        │   ├── la gp, __global_pointer$      重载 gp 为 VA
        │   └── csrw SATP, a2 → 切换到 early_pg_dir
        │
        └── ret (ra = VA，调用者继续在虚拟地址执行)
    │
    │ ★★★ 从此所有代码运行在虚拟地址空间 ★★★
    │
    ├── call setup_trap_vector  正式的异常向量表
    ├── la tp, init_task        VA 可直接用
    └── tail start_kernel
        │
        └── setup_arch()
            └── ...
                └── paging_init()
                    │
                    ├── setup_bootmem()     memblock 初始化物理内存
                    └── setup_vm_final()
                        │
                        ├── pt_ops = fixmap_allocator
                        ├── create_pgd_mapping(swapper_pg_dir, fixmap)
                        ├── for_each_mem_range: 建立线性映射 VA->PA
                        ├── create_kernel_page_table(swapper_pg_dir, false)
                        ├── clear_fixmap(FIX_PTE, FIX_PMD)
                        ├── csrw SATP, swapper_pg_dir
                        │  ★★★ 第三次切换 SATP，最终页表 ★★★
                        └── pt_ops = late_allocator (用 __va 直接访问)
    │
    └── 内核继续初始化...
```

---

## 七、关键宏的数值关系一览

以 MAXPHYSMEM_128GB, Sv39, 64-bit 为例：

```
PAGE_OFFSET      = 0xffffffe0_00000000   // 线性映射区起点
KERN_VIRT_SIZE   = -PAGE_OFFSET
                 = 0x00000020_00000000   // = 128GB
KERNEL_LINK_ADDR = 0xffffffff_80000000   // 内核链接地址
PAGE_SHIFT       = 12                    // 4KB 页
PMD_SHIFT        = 21                    // 2MB 大页
PGDIR_SHIFT      = 30                    // 1GB
PTRS_PER_PGD     = 512
PTRS_PER_PMD     = 512
PTRS_PER_PTE     = 512
SATP_MODE_39     = 0x80000000_00000000   // MODE[63:60] = 8 = Sv39

va_kernel_pa_offset = KERNEL_LINK_ADDR - phys_addr  // 内核段 VA-PA 差
va_pa_offset        = PAGE_OFFSET - phys_addr       // 线性映射 VA-PA 差
```

---

## 八、RISC-V Sv39 页表遍历过程

给定虚拟地址 `va`，MMU 硬件自动执行：

```
va = [ vpn[2] (9 bits) | vpn[1] (9 bits) | vpn[0] (9 bits) | offset (12 bits) ]
       \______________/ \______________/ \______________/ \________________/
             38:30          29:21          20:12           11:0

Level 2 (PGD):
  pgd_addr = satp.ppn << 12                 (satp 是 SATP 寄存器)
  pte = *(pgd_addr + 8 * vpn[2])           读 PGD 条目
  if (pte.V == 0) → Page Fault
  if (pte is leaf: R|W|X != 0) → 用 pte.ppn 作为最终物理页号, 去 step 4

Level 1 (PMD):
  pmd_addr = pte.ppn << 12
  pte = *(pmd_addr + 8 * vpn[1])
  if (pte.V == 0) → Page Fault
  if (leaf) → goto step 4

Level 0 (PTE):
  pte_addr = pte.ppn << 12
  pte = *(pte_addr + 8 * vpn[0])
  if (pte.V == 0 || !(R|M|X)) → Page Fault

Step 4: 物理地址 = (pte.ppn << 12) | (va & 0xfff)

  PTE 格式（64 位）:
  | 63       54 | 53     28 | 27  19 | 18  10 | 9 8 | 7 6 5 4 3 2 1 0 |
  |   Reserved  | (PPN[2])  | PPN[1] | PPN[0] | RSW | D A G U X W R V |
                                                              | | | | | | |
                                                              | | | | | | Valid
                                                              | | | | | Read
                                                              | | | | Write
                                                              | | | Executable
                                                              | | User
                                                              | Global
                                                              Accessed
                                                              Dirty
```

每次写 SATP 后必须执行 `sfence.vma`（或等价指令）刷新 TLB，否则 MMU 可能使用旧的翻译缓存结果导致不可预知行为。