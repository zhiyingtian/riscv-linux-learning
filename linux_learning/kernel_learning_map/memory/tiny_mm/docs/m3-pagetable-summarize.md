# M3 页表知识点核查与修正

> 核查对象：M3 页表学习总结
> 核查内核：`~/workspace/riscv/runninglinuxkernel_5.15`
> 架构与配置：RISC-V 64 位、Sv39、`CONFIG_PGTABLE_LEVELS=3`、`CONFIG_FLATMEM=y`
> 关联文档：[m3-pgtable.md](m3-pgtable.md)、[m3-pgtable-qa.md](m3-pgtable-qa.md)

---

## 0. 核查环境

当前内核关键配置：

```text
# .config:240-242
CONFIG_VA_BITS=39
CONFIG_PAGE_OFFSET=0xffffffe000000000

# .config:255
CONFIG_PGTABLE_LEVELS=3

# .config:467-470
CONFIG_FLATMEM_MANUAL=y
CONFIG_FLATMEM=y
CONFIG_SPARSEMEM_VMEMMAP_ENABLE=y
```

需要注意：

```text
CONFIG_SPARSEMEM_VMEMMAP_ENABLE=y
```

只表示“支持启用”，不等于：

```text
CONFIG_SPARSEMEM_VMEMMAP=y
```

这份内核实际使用 FLATMEM，所以：

```c
pfn_to_page(pfn) = mem_map + (pfn - ARCH_PFN_OFFSET);
```

不是：

```c
vmemmap + pfn;
```

另外，通过当前 `vmlinux` 查询：

```text
(gdb) p sizeof(struct page)
$1 = 64
```

本内核的 `struct page` 大小是 **64B**。

### 0.1 类型约定：`struct page` 还是 `struct page *`

后面解释函数时，入参和返回值都明确标注类型：

| 表达式 | 入参类型 | 返回类型 |
|---|---|---|
| `page_to_pfn(page)` | `struct page *` | `unsigned long` |
| `pfn_to_page(pfn)` | `unsigned long` | `struct page *` |
| `page_address(page)` | `const struct page *` | `void *` |
| `page_to_virt(page)` | `struct page *` | `void *` |
| `page_to_phys(page)` | `struct page *` | `phys_addr_t` |
| `virt_to_page(vaddr)` | 内核虚拟地址 | `struct page *` |
| `pte_page(pte)` | `pte_t`（值，不是指针） | `struct page *` |
| `pfn_pte(pfn, prot)` | PFN + `pgprot_t` | `pte_t` |

几个容易混淆的点：

```text
struct page     是结构体类型
struct page *   是指向元数据的指针
page            通常是一个 struct page * 变量
*page           才是 struct page 对象本身

page_address(page)   正确：入参是 struct page *
page_address(*page)  错误：入参变成了 struct page

pte_page(*pte)       正确：入参是 pte_t 值
pte_page(pte)        错误：把 pte_t * 当成了 pte_t
```

---

## 1. 修正后的核心结论

```text
1. Sv39 是 PGD/PMD/PTE 三级页表。
   每级页表页 4KB、512 个 entry、每个 entry 8B。
   但 PMD/PTE 页是按需分配的，不是每个进程一开始就有完整三级页表。

2. 页表属于 mm_struct，不属于 VMA。
   VMA 只是虚拟地址区间 + 权限。
   一个 mm 只有一个 PGD 根，多个 VMA 可以共享 PMD/PTE 页。
   mmap() 不分配页表页，第一次访问缺页时才按需分配。

3. 非叶子 PGD/PMD entry 保存下一级页表页的 PFN + flags；
   普通 present PTE 保存数据页 PFN + flags。
   PMD 可能是 2MB 叶子大页；PGD 1GB 叶子是 Sv39 硬件能力，
   但 Linux RISC-V 5.15 默认不按 PGD 大页处理。
   非 present PTE 还可能保存 swap/migration 信息。

4. 当前内核是 FLATMEM：
   pfn_to_page(pfn) = mem_map + pfn - ARCH_PFN_OFFSET
   struct page 数组由 mem_map 指向。
   元数据大小约为 max_mapnr * sizeof(struct page)。
   PHYS_MEM >> PAGE_SHIFT * sizeof(struct page) 有优先级错误。

5. 缺页路径：
   pgd_offset()     不分配
   p4d_alloc()      当前折叠，不分配
   pud_alloc()      当前折叠，不分配
   pmd_alloc()      需要时分配 PMD 页
   pte_alloc()      需要时分配 PTE 页
   pte_offset_map() 只计算 PTE entry 地址，不分配
```

---

## 2. 逐条核查

### 2.1 第 1 条：PGD/PMD/PTE 的大小

#### 原总结

> PGD/PMD/PTE 这三级页表，在 Sv39 模式下，每个都占一个 page(4K) 的大小，每个页表页中有 2^9=512 个页表项，每个页表项占 8B。

#### 判断

**基本正确，但要补充“按需分配”。**

Sv39 的地址拆分：

```text
39 位虚拟地址：
[ PGD index 9 位 ][ PMD index 9 位 ][ PTE index 9 位 ][ offset 12 位 ]
```

对应：

```text
PGDIR_SHIFT = 30
PMD_SHIFT   = 21
PTE_SHIFT   = 12
```

源码：

```c
/* arch/riscv/include/asm/pgtable-64.h:11-29 */
#define PGDIR_SHIFT     30
#define PMD_SHIFT       21

typedef struct {
    unsigned long pmd;
} pmd_t;

#define PTRS_PER_PMD    (PAGE_SIZE / sizeof(pmd_t))
```

```c
/* arch/riscv/include/asm/pgtable.h:108-115 */
#ifdef CONFIG_MMU
/* Number of entries in the page global directory */
#define PTRS_PER_PGD    (PAGE_SIZE / sizeof(pgd_t))
/* Number of entries in the page table */
#define PTRS_PER_PTE    (PAGE_SIZE / sizeof(pte_t))

/* Number of PGD entries that a user-mode program can use */
#define USER_PTRS_PER_PGD   (TASK_SIZE / PGDIR_SIZE)
```

```c
/* arch/riscv/include/asm/page.h:51-59 */
/* Page Global Directory entry */
typedef struct {
    unsigned long pgd;
} pgd_t;

/* Page Table entry */
typedef struct {
    unsigned long pte;
} pte_t;
```

64 位下 `unsigned long` 是 8B，所以：

```text
512 × 8B = 4096B = 4KB
```

#### 需要补充：页表页是懒分配

```text
PGD 页：mm 创建时分配
PMD 页：第一次访问某个 1GB 区域时按需分配
PTE 页：第一次访问某个 2MB 区域时按需分配
```

一个进程可能只有 PGD 页，PMD/PTE 页尚未分配。

`mmap()` 只创建 VMA，不会立即分配 PMD/PTE 页。

#### 需要补充：PUD/P4D 在当前配置下折叠

当前是三级页表：

```text
PGD → PMD → PTE
```

Linux 通用代码中的 P4D/PUD 在这份内核里没有独立页表页：

```c
/* include/asm-generic/pgtable-nop4d.h:7-12 */
#define __PAGETABLE_P4D_FOLDED 1
typedef struct { pgd_t pgd; } p4d_t;
#define PTRS_PER_P4D            1
```

```c
/* include/asm-generic/pgtable-nopud.h:9-19 */
#define __PAGETABLE_PUD_FOLDED 1
typedef struct { p4d_t p4d; } pud_t;
#define PTRS_PER_PUD    1
```

因此：

```c
/* include/asm-generic/pgtable-nop4d.h:50 */
#define p4d_alloc_one(mm, address)      NULL

/* include/asm-generic/pgtable-nopud.h:58 */
#define pud_alloc_one(mm, address)      NULL
```

所以在这份内核里，`p4d_alloc()`、`pud_alloc()` 不会分配新的页表页。

---

### 2.2 第 2 条：不是每个 VMA 都有三级页表

#### 原总结

> 每个 VMA 都有这三级页表，那么保存 PGD/PMD/PTE 页表的内存地址在哪里呢？

#### 判断

**错误。**

正确关系：

```text
struct mm_struct
  ├── pgd_t *pgd        ← 一个 mm 只有一套页表根
  ├── VMA 链表
  │     ├── VMA1
  │     └── VMA2
  └── ...
```

VMA 只是虚拟地址区间：

```c
/* include/linux/mm_types.h:319-341 */
struct vm_area_struct {
    unsigned long vm_start;
    unsigned long vm_end;
    ...
    struct mm_struct *vm_mm;    /* The address space we belong to. */
    ...
};
```

页表根在 `mm_struct`：

```c
/* include/linux/mm_types.h:402-421 */
struct mm_struct {
    struct {
        struct vm_area_struct *mmap;    /* list of VMAs */
        struct rb_root mm_rb;
        ...
        pgd_t *pgd;
        ...
    } __randomize_layout;
    ...
};
```

`pgd_alloc()` 在 mm 创建时分配 PGD 根：

```c
/* arch/riscv/include/asm/pgalloc.h:41-54 */
static inline pgd_t *pgd_alloc(struct mm_struct *mm)
{
    pgd_t *pgd;

    pgd = (pgd_t *)__get_free_page(GFP_KERNEL);
    if (likely(pgd != NULL)) {
        memset(pgd, 0, USER_PTRS_PER_PGD * sizeof(pgd_t));
        /* Copy kernel mappings */
        memcpy(pgd + USER_PTRS_PER_PGD,
            init_mm.pgd + USER_PTRS_PER_PGD,
            (PTRS_PER_PGD - USER_PTRS_PER_PGD) * sizeof(pgd_t));
    }
    return pgd;
}
```

因此：

- 一个进程/线程组只有一个 `mm->pgd`；
- 所有 VMA 共享这个 `mm` 的页表；
- 多个 VMA 可以共享同一个 PTE 页；
- `mmap()` 不分配页表页；
- 第一次访问缺页时才建立 PMD/PTE。

#### 页表页地址在哪里

```text
PGD 根：
    mm->pgd 是内核虚拟地址
    物理地址可由 __pa(mm->pgd) 得到

PMD 页：
    PGD entry 保存 PMD 页 PFN
    pmd_offset()/pmd_page_vaddr() 可得到 PMD 页内容地址

PTE 页：
    PMD entry 保存 PTE 页 PFN
    pte_offset_map() 可得到 PTE entry 地址
```

---

### 2.3 第 3 条：页表项保存什么

#### 原总结

> PGD 和 PMD 里面的页表项保存下一级页表的 PFN 和 flag，PTE 保存物理内存地址的 PFN 和 flag。

#### 判断

**在“普通 4K 映射、非叶子项”的范围内正确。**

但要注意三种情况。

#### 1. PGD/PMD 也可能是叶子项

RISC-V 用 `_PAGE_LEAF` 区分叶子项：

```c
/* arch/riscv/include/asm/pgtable-bits.h:36-46 */
#define _PAGE_PFN_SHIFT 10

/*
 * when all of R/W/X are zero, the PTE is a pointer to the next level
 * of the page table; otherwise, it is a leaf PTE.
 */
#define _PAGE_LEAF (_PAGE_READ | _PAGE_WRITE | _PAGE_EXEC)
```

所以：

```text
PGD entry 叶子 → 1GB 大页（Sv39 硬件允许）
PMD entry 叶子 → 2MB 大页
PTE entry 叶子 → 4KB 页
```

判断 PMD 是否叶子：

```c
/* arch/riscv/include/asm/pgtable.h:205-208 */
#define pmd_leaf    pmd_leaf
static inline int pmd_leaf(pmd_t pmd)
{
    return pmd_present(pmd) && (pmd_val(pmd) & _PAGE_LEAF);
}
```

因此不能绝对地说：

```text
PGD/PMD 永远保存下一级页表的 PFN
```

不过还要注意 Linux 软件层是否真的使用 PGD 大页。RISC-V 5.15 定义了 `pmd_leaf()`，但没有定义 `pgd_leaf()`；通用代码的默认值是：

```c
/* include/linux/pgtable.h:1584-1586 */
#ifndef pgd_leaf
#define pgd_leaf(x) 0
#endif
```

所以在这份内核里：

- PMD 叶子大页是实际支持的；
- PGD 1GB 叶子是 Sv39 硬件能力，但 Linux RISC-V 5.15 的通用代码默认不把它当叶子项处理。

#### 2. PTE 的 PFN 编码不是简单 `PFN << 12`

RISC-V 的 flags 占低 10 位：

```c
/* arch/riscv/include/asm/pgtable-bits.h:36 */
#define _PAGE_PFN_SHIFT 10
```

PFN 编解码：

```c
/* arch/riscv/include/asm/pgtable.h:251-263 */
/* Yields the page frame number (PFN) of a page table entry */
static inline unsigned long pte_pfn(pte_t pte)
{
    return (pte_val(pte) >> _PAGE_PFN_SHIFT);
}

/* Constructs a page table entry */
static inline pte_t pfn_pte(unsigned long pfn, pgprot_t prot)
{
    return __pte((pfn << _PAGE_PFN_SHIFT) | pgprot_val(prot));
}
```

普通 present PTE 才表示数据页 PFN：

```text
PTE present + 非特殊项
    → 数据页 PFN + flags
```

非 present 且非空的 PTE 可能是：

```text
swap entry
migration entry
```

注意：`_PAGE_PROT_NONE` 不是“非 present”这一类。RISC-V 的 `pte_present()` 会把 `_PAGE_PROT_NONE` 也算 present；它表示“软件认为存在但不可访问”的页，硬件上 V=0。

```c
/* arch/riscv/include/asm/pgtable-bits.h:31-34 */
#define _PAGE_PROT_NONE _PAGE_READ

/* arch/riscv/include/asm/pgtable.h:267-269 */
static inline int pte_present(pte_t pte)
{
    return (pte_val(pte) & (_PAGE_PRESENT | _PAGE_PROT_NONE));
}
```

所以“PTE 保存物理内存地址的 PFN”要加限定：

> **对普通 present 的 4K PTE，才直接表示数据页 PFN。**

#### 3. 页表页初始为 0，entry 按需填充

原句“会在访问的时候来填充指定索引下的页表项”基本正确。

页表页分配时使用 `GFP_PGTABLE_USER` / `GFP_PGTABLE_KERNEL`，它们包含 `__GFP_ZERO`：

```c
/* include/asm-generic/pgalloc.h:7-8 */
#define GFP_PGTABLE_KERNEL  (GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER    (GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)
```

所以新页表页的 512 个 entry 初始都是 0：

```text
entry == 0
    → 非 present / 未映射
```

只有缺页或建立映射时，才填充对应索引的 entry。已经存在的 entry 不会因为访问而重复创建。

---

### 2.4 第 4 条：struct page、PFN 和 mem_map

#### 原总结

> Linux 内核中通过 struct page 结构体指针来全局统一管理系统中真实的物理地址。通过 PFN = phy_addr >> PAGE_SHIFT 来唯一确定 struct page 和 phy_addr 的映射关系。而描述物理内存地址的 struct page 元数据由内核保存在一个特定的物理内存地方，占用的空间为：phy_mem >> PAGE_SHIFT * sizeof(struct page)。而因为在开启 MMU 后，内核只能使用虚拟地址，因此管理物理内存的 struct page 的 vir_addr 为 vmemmap + PFN。

#### 判断

**方向对，但这份内核的具体实现不是 `vmemmap + PFN`。**

#### 1. 当前内核是 FLATMEM

```c
/* include/asm-generic/memory_model.h:12-26 */
#if defined(CONFIG_FLATMEM)

#ifndef ARCH_PFN_OFFSET
#define ARCH_PFN_OFFSET     (0UL)
#endif

#define __pfn_to_page(pfn)  (mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page) ((unsigned long)((page) - mem_map) + \
                             ARCH_PFN_OFFSET)

#elif defined(CONFIG_SPARSEMEM_VMEMMAP)

/* memmap is virtually contiguous.  */
#define __pfn_to_page(pfn)  (vmemmap + (pfn))
#define __page_to_pfn(page) (unsigned long)((page) - vmemmap)
```

当前配置：

```text
CONFIG_FLATMEM=y
CONFIG_SPARSEMEM_VMEMMAP_ENABLE=y
```

但没有：

```text
CONFIG_SPARSEMEM_VMEMMAP=y
```

所以 `pfn_to_page()` 实际走：

```c
mem_map + (pfn - ARCH_PFN_OFFSET)
```

而不是：

```c
vmemmap + pfn
```

`mem_map` 定义：

```c
/* mm/memory.c:93-98 */
#ifndef CONFIG_NUMA
unsigned long max_mapnr;
EXPORT_SYMBOL(max_mapnr);

struct page *mem_map;
EXPORT_SYMBOL(mem_map);
#endif
```

`ARCH_PFN_OFFSET`：

```c
/* arch/riscv/include/asm/page.h:81-86 */
#ifdef CONFIG_MMU
extern unsigned long riscv_pfn_base;
#define ARCH_PFN_OFFSET     (riscv_pfn_base)
#else
#define ARCH_PFN_OFFSET     (PAGE_OFFSET >> PAGE_SHIFT)
#endif
```

因此：

```text
struct page *page
= mem_map + (pfn - ARCH_PFN_OFFSET)
```

PFN 只对有效物理页有意义；访问 `struct page` 前要保证 `pfn_valid(pfn)`。FLATMEM 下 `pfn_valid()` 会检查 PFN 是否落在 `[ARCH_PFN_OFFSET, ARCH_PFN_OFFSET + max_mapnr)` 范围内。

#### 2. 元数据空间公式的优先级错误

原式：

```c
phy_mem >> PAGE_SHIFT * sizeof(struct page)
```

由于 `*` 优先级高于 `>>`，实际是：

```c
phy_mem >> (PAGE_SHIFT * sizeof(struct page))
```

正确写法：

```c
nr_pages = phy_mem >> PAGE_SHIFT;
meta_bytes = nr_pages * sizeof(struct page);
```

或者：

```c
meta_bytes = (phy_mem >> PAGE_SHIFT) * sizeof(struct page);
```

更贴近当前内核的写法：

```c
meta_bytes = max_mapnr * sizeof(struct page);
```

`max_mapnr` 表示当前内核管理的页帧数量上限（`max_low_pfn - ARCH_PFN_OFFSET`）。
FLATMEM 下 `mem_map` 数组覆盖这个连续范围，中间的空洞不会让数组本身缩短。

本内核中：

```text
sizeof(struct page) = 64B
64 / 4096 = 1.5625%
```

#### 3. 内容地址、元数据地址不是一回事

```text
struct page 元数据：
    mem_map + pfn - ARCH_PFN_OFFSET
    （SPARSEMEM_VMEMMAP 下才是 vmemmap + pfn）

页内容 direct map 地址：
    pfn_to_virt(pfn)
    = __va(pfn_to_phys(pfn))
```

源码：

```c
/* arch/riscv/include/asm/page.h:147-158 */
#define phys_to_pfn(phys)   (PFN_DOWN(phys))
#define pfn_to_phys(pfn)    (PFN_PHYS(pfn))

#define virt_to_pfn(vaddr)  (phys_to_pfn(__pa(vaddr)))
#define pfn_to_virt(pfn)    (__va(pfn_to_phys(pfn)))

#define virt_to_page(vaddr) (pfn_to_page(virt_to_pfn(vaddr)))
#define page_to_virt(page)  (pfn_to_virt(page_to_pfn(page)))

#define page_to_phys(page)  (pfn_to_phys(page_to_pfn(page)))
#define page_to_bus(page)   (page_to_phys(page))
#define phys_to_page(paddr) (pfn_to_page(phys_to_pfn(paddr)))
```

所以：

```text
mem_map + pfn - ARCH_PFN_OFFSET   → struct page 元数据
pfn_to_virt(pfn) / __va(...)      → 4KB 内容 direct map 地址
```

---

### 2.4.1 Linux 中 `struct page *` 的来源、唯一性和生命周期

这里只讨论 Linux 内核，不讨论 tiny-mm。

#### 1. `struct page *` 指向全局元数据数组，不是“每次分配创建的对象”

当前 FLATMEM 模型：

```c
/* include/asm-generic/memory_model.h:18-20 */
#define __pfn_to_page(pfn)  (mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page) ((unsigned long)((page) - mem_map) + ARCH_PFN_OFFSET)
```

所以：

```text
struct page 数组：全局存在
struct page *：数组中的地址
alloc_pages()：返回其中一个地址，并不会分配 struct page 本体
```

#### 2. buddy 保证“分配独占性”

buddy 从空闲链表中摘除页块：

```c
/* mm/page_alloc.c:2443-2468 */
static __always_inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
                                int migratetype)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    /* Find a page of the appropriate size in the preferred list */
    for (current_order = order; current_order < MAX_ORDER; ++current_order) {
        area = &(zone->free_area[current_order]);
        page = get_page_from_free_area(area, migratetype);
        if (!page)
            continue;
        del_page_from_free_list(page, zone, current_order);
        expand(zone, page, order, current_order, migratetype);
        set_pcppage_migratetype(page, migratetype);
        return page;
    }

    return NULL;
}
```

并且该路径在 zone lock 下执行：

```c
/* mm/page_alloc.c:3701-3724 */
spin_lock_irqsave(&zone->lock, flags);

do {
    page = NULL;
    ...
    if (!page)
        page = __rmqueue(zone, order, migratetype, alloc_flags);
} while (page && check_new_pages(page, order));
if (!page)
    goto failed;

__mod_zone_freepage_state(zone, -(1 << order),
                          get_pcppage_migratetype(page));
spin_unlock_irqrestore(&zone->lock, flags);
```

因此在页块仍然被分配、没有释放的前提下：

```text
同一个空闲页块不会同时被两个线程从 buddy 中拿走
```

order-0 还可能走 pcp（per-CPU page list），pcp 也有自己的锁。
但核心不变量相同：同一个已分配页块不会被重复分配。

#### 3. 但 page 指针不是“所有权令牌”

页释放时：

```c
/* mm/page_alloc.c:5451-5454 */
void __free_pages(struct page *page, unsigned int order)
{
    if (put_page_testzero(page))
        free_the_page(page, order);
    ...
}
```

`struct page *` 仍指向同一个元数据元素，但物理页已经回到分配器，之后可能被重新分配。

所以：

```text
page 指针相同
≠
这次分配的所有权仍然属于你
```

释放后继续使用旧 `page` 指针，就是 use-after-free。

#### 4. 引用计数和映射计数

```c
/* include/linux/mm_types.h:196-216 */
union {
    /*
     * If the page can be mapped to userspace, encodes the number
     * of times this page is referenced by a page table.
     */
    atomic_t _mapcount;
    ...
};

/* Usage count. *DO NOT USE DIRECTLY*. See page_ref.h */
atomic_t _refcount;
```

增加引用：

```c
/* include/linux/page_ref.h:114-119 */
static inline void page_ref_inc(struct page *page)
{
    atomic_inc(&page->_refcount);
    ...
}
```

因此：

- `_refcount`：页面被多少使用者引用；
- `_mapcount`：页面被多少个页表映射；
- 页表映射一个页面，不等于“独占这个物理页”；
- 同一个物理页可以被多个 PTE、多个 mm 共享。

#### 5. 结论

```text
buddy 保证的是：
    同一个空闲页块不会同时分配给两个申请者

buddy 不保证的是：
    同一个物理页只能被一个线程使用
    同一个 struct page * 永远属于同一个所有者
```

所以：

> **在页表翻译层面，可以把 `struct page *` 当作物理页的身份句柄；**
>
> **但在 Linux 内核里，分配独占性、引用计数和生命周期仍然必须关注。**

---

### 2.5 第 5 条：缺页时的页表分配

#### 原总结

> 在首次访问缺页异常里面，会向内核逐级申请页表空间(pud_alloc/pmd_alloc/pte_offset_map)来填充上一级页表项。

#### 判断

**这个调用列表不准确。**

问题：

1. `pte_offset_map()` 不分配；
2. 当前三级页表配置下，`pud_alloc()` 也不分配；
3. 真正分配 PTE 页的是 `pte_alloc()`。

#### 1. `__handle_mm_fault()` 的主路径

```c
/* mm/memory.c:4633-4638 */
pgd = pgd_offset(mm, address);
p4d = p4d_alloc(mm, pgd, address);
if (!p4d)
    return VM_FAULT_OOM;

vmf.pud = pud_alloc(mm, p4d, address);
if (!vmf.pud)
    return VM_FAULT_OOM;
```

```c
/* mm/memory.c:4665-4667 */
vmf.pmd = pmd_alloc(mm, vmf.pud, address);
if (!vmf.pmd)
    return VM_FAULT_OOM;
```

当前三级页表下：

```text
pgd_offset()   不分配，mm->pgd 已存在
p4d_alloc()    折叠，不分配
pud_alloc()    折叠，不分配
pmd_alloc()    可能分配 PMD 页
```

`pmd_alloc()` 本身：

```c
/* include/linux/mm.h:2171-2175 */
static inline pmd_t *pmd_alloc(struct mm_struct *mm, pud_t *pud,
                               unsigned long address)
{
    return (unlikely(pud_none(*pud)) && __pmd_alloc(mm, pud, address)) ?
        NULL : pmd_offset(pud, address);
}
```

`pmd_alloc_one()` 分配 PMD 页并返回页内容地址：

```c
/* include/asm-generic/pgalloc.h:119-134 */
static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long addr)
{
    struct page *page;
    gfp_t gfp = GFP_PGTABLE_USER;

    if (mm == &init_mm)
        gfp = GFP_PGTABLE_KERNEL;
    page = alloc_pages(gfp, 0);
    if (!page)
        return NULL;
    if (!pgtable_pmd_page_ctor(page)) {
        __free_pages(page, 0);
        return NULL;
    }
    return (pmd_t *)page_address(page);
}
```

#### 2. `handle_pte_fault()` 决定是否需要 PTE 页

```c
/* mm/memory.c:4517-4524 */
if (unlikely(pmd_none(*vmf->pmd))) {
    /*
     * Leave __pte_alloc() until later: because vm_ops->fault may
     * want to allocate huge page, and if we expose page table
     * for an instant, it will be difficult to retract from
     * concurrent faults and from rmap lookups.
     */
    vmf->pte = NULL;
}
```

```c
/* mm/memory.c:4546-4547 */
vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
vmf->orig_pte = *vmf->pte;
```

注意：

- PMD 为空时，`handle_pte_fault()` 暂时不调用 `pte_offset_map()`；
- 它把 `vmf->pte` 设为 NULL，后面交给 `do_anonymous_page()` 或 `do_fault()`；
- `pte_offset_map()` 只计算 PTE entry 地址，不分配 PTE 页。

#### 3. 真正分配 PTE 页的是 `pte_alloc()`

```c
/* include/linux/mm.h:2278 */
#define pte_alloc(mm, pmd) \
    (unlikely(pmd_none(*(pmd))) && __pte_alloc(mm, pmd))
```

```c
/* mm/memory.c:436-467 */
int __pte_alloc(struct mm_struct *mm, pmd_t *pmd)
{
    spinlock_t *ptl;
    pgtable_t new = pte_alloc_one(mm);
    if (!new)
        return -ENOMEM;

    smp_wmb();

    ptl = pmd_lock(mm, pmd);
    if (likely(pmd_none(*pmd))) {
        mm_inc_nr_ptes(mm);
        pmd_populate(mm, pmd, new);
        new = NULL;
    }
    spin_unlock(ptl);
    if (new)
        pte_free(mm, new);
    return 0;
}
```

匿名缺页中：

```c
/* mm/memory.c:3736-3738 */
if (pte_alloc(vma->vm_mm, vmf->pmd))
    return VM_FAULT_OOM;
```

```c
/* mm/memory.c:3748-3749 */
vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd,
                               vmf->address, &vmf->ptl);
```

#### 4. `pte_offset_map()` 只计算地址

```c
/* include/linux/pgtable.h:87-102 */
static inline pte_t *pte_offset_kernel(pmd_t *pmd, unsigned long address)
{
    return (pte_t *)pmd_page_vaddr(*pmd) + pte_index(address);
}

#if defined(CONFIG_HIGHPTE)
#define pte_offset_map(dir, address) \
    ((pte_t *)kmap_atomic(pmd_page(*(dir))) + pte_index((address)))
#else
#define pte_offset_map(dir, address) pte_offset_kernel((dir), (address))
#endif
```

`pmd_page_vaddr()` 通过 PMD entry 里的 PFN 找 PTE 页内容：

```c
/* arch/riscv/include/asm/pgtable.h:236-239 */
static inline unsigned long pmd_page_vaddr(pmd_t pmd)
{
    return (unsigned long)pfn_to_virt(pmd_val(pmd) >> _PAGE_PFN_SHIFT);
}
```

#### 5. 这些函数返回什么

| 函数 | 返回 | 是否分配页表页 |
|---|---|---|
| `pgd_offset()` | `pgd_t *`，PGD entry 地址 | 否 |
| `p4d_alloc()` | `p4d_t *`，P4D entry 地址 | 当前折叠，否 |
| `pud_alloc()` | `pud_t *`，PUD entry 地址 | 当前折叠，否 |
| `pmd_alloc()` | `pmd_t *`，PMD entry 地址 | 可能需要 PMD 页 |
| `pte_alloc()` | `int`，成功 0，失败非 0；底层 `__pte_alloc()` 返回 `-ENOMEM` | 可能需要 PTE 页 |
| `pte_alloc_one()` | `pgtable_t` / `struct page *` | 是，分配 PTE 页 |
| `pmd_alloc_one()` | `pmd_t *`，PMD 页基址 | 是，分配 PMD 页 |
| `pte_offset_map()` | `pte_t *`，PTE entry 地址 | 否 |

所以正确顺序是：

```text
pmd_alloc()        需要时分配 PMD 页
pte_alloc()        需要时分配 PTE 页
pte_offset_map()   计算 PTE entry 地址
```

而不是：

```text
pud_alloc/pmd_alloc/pte_offset_map 都分配页表
```

---

## 3. 两个括号问题

### 3.1 申请到的页表页 `virt_addr` 走什么映射

答：**direct map / linear map，内核线性映射。**

页表页本身也是普通物理页，由 buddy 分配：

```c
/* include/asm-generic/pgalloc.h:59-71 */
static inline pgtable_t __pte_alloc_one(struct mm_struct *mm, gfp_t gfp)
{
    struct page *pte;

    pte = alloc_page(gfp);
    if (!pte)
        return NULL;
    if (!pgtable_pte_page_ctor(pte)) {
        __free_page(pte);
        return NULL;
    }

    return pte;
}
```

PTE 页返回的是 `struct page *`：

```c
/* arch/riscv/include/asm/page.h:65 */
typedef struct page *pgtable_t;
```

PMD 页返回的是页内容地址：

```c
/* include/asm-generic/pgalloc.h:133 */
return (pmd_t *)page_address(page);
```

`page_address(const struct page *page)` 的入参是 `const struct page *`，返回 `void *`：把页元数据指针转换成页内容的内核虚拟地址。在常见 64 位系统中走 direct map。

RISC-V 的 `pfn_to_virt()`：

```c
/* arch/riscv/include/asm/page.h:150-151 */
#define virt_to_pfn(vaddr)  (phys_to_pfn(__pa(vaddr)))
#define pfn_to_virt(pfn)    (__va(pfn_to_phys(pfn)))
```

`__va()`：

```c
/* arch/riscv/include/asm/page.h:145 */
#define __va(x) ((void *)__pa_to_va_nodebug((phys_addr_t)(x)))
```

所以页表页内容地址是：

```text
PFN → 物理地址 → __va() → direct map 虚拟地址
```

不是 `vmemmap`。

### 3.2 从 `struct page *` 推导虚拟地址

这里的 `page` 是 **`struct page *`**，不是 `struct page`。

```c
struct page *page;          /* 指向 struct page 元数据 */
unsigned long pfn = page_to_pfn(page);   /* 入参：struct page * */
```

答：**仍然是 direct map / linear map。**

自上而下推导：

```text
struct page *page
    │
    │ ① page_to_pfn(page)
    │    FLATMEM:
    │    pfn = (page - mem_map) + ARCH_PFN_OFFSET
    ▼
unsigned long pfn
    │
    │ ② pfn_to_phys(pfn)
    │    phys = pfn << PAGE_SHIFT
    ▼
phys_addr_t phys
    │
    │ ③ __va(phys)
    │    linear map:
    │    kaddr = phys + kernel_map.va_pa_offset
    ▼
void *kaddr = page_address(page)
```

因此：

```c
void *kaddr = page_address(page);
```

也可以写成：

```c
void *kaddr = __va(pfn_to_phys(page_to_pfn(page)));
```

这条链的每一步都是**从元数据指针到页内容虚拟地址**：

```text
struct page *       （元数据指针，不是 4KB 内容）
    ↓ page_to_pfn
PFN                 （物理页帧号）
    ↓ pfn_to_phys
物理地址
    ↓ __va
direct map 虚拟地址  （4KB 内容地址）
```

注意：`page_address(page)` 的入参是 `const struct page *`，返回 `void *`。
`page_to_virt(page)` 在低内存/direct map 场景下得到同样的页内容地址。

它返回的是这个物理页的 **4KB 内容地址**，不是 `struct page` 元数据地址。

对比：

```text
mem_map + pfn - ARCH_PFN_OFFSET
    → struct page 元数据地址（FLATMEM）

vmemmap + pfn
    → struct page 元数据地址（SPARSEMEM_VMEMMAP）

__va(pfn_to_phys(pfn))
    → 页内容 direct map 地址
```

### 3.3 补充：页表页也有 `struct page`

```c
/* arch/riscv/include/asm/pgalloc.h:24-30 */
static inline void pmd_populate(struct mm_struct *mm,
    pmd_t *pmd, pgtable_t pte)
{
    unsigned long pfn = virt_to_pfn(page_address(pte));

    set_pmd(pmd, __pmd((pfn << _PAGE_PFN_SHIFT) | _PAGE_TABLE));
}
```

这里：

```text
pte 的类型是 pgtable_t，也就是 struct page *
page_address(pte) 的入参是 struct page *，返回 PTE 页内容地址
virt_to_pfn(...) 的入参是内核虚拟地址，返回 PTE 页 PFN
```

而：

```text
virt_to_page(pte)   → 入参是 pte_t * 地址，返回装 PTE 的页表页的 struct page *
pte_page(*pte)      → 入参是 pte_t 值，返回 PTE 指向的数据页的 struct page *
```

这两者不是同一个页。

---

## 4. 一张总图

```text
struct mm_struct
  └── mm->pgd
        │
        ▼
     PGD 页（PGD 页内容在 direct map 中）
        │  PGD entry：PMD 页 PFN + flags
        ▼
     PMD 页
        │  PMD entry：PTE 页 PFN + flags
        ▼
     PTE 页
        │  PTE entry：数据页 PFN + flags
        ▼
     数据页

页表页内容地址：pfn_to_virt(pfn) / page_address(page)   /* page: struct page * */
页表页元数据：  mem_map + pfn - ARCH_PFN_OFFSET
数据页元数据：  pte_page(*pte) / pfn_to_page(pte_pfn(*pte))
                 /* pte: pte_t *；*pte: pte_t；返回: struct page * */
```

---

## 5. 修正版总结

```text
1. Sv39 三级页表：PGD/PMD/PTE。
   每级页表页 4KB、512 项、每项 8B。
   PGD 根在 mm 创建时分配；PMD/PTE 页按地址范围在缺页时按需分配。
   PUD/P4D 在当前 3-level 配置中折叠，不额外分配页表页。

2. 页表属于 mm_struct，不属于 VMA。
   VMA 只是虚拟地址区间和权限；多个 VMA 可以共享同一套 PGD/PMD/PTE 页。
   mmap() 不分配页表页，第一次访问缺页时才按需分配。

3. 非叶子 PGD/PMD entry 保存下一级页表页的 PFN + flags；
   普通 present PTE 保存数据页 PFN + flags。
   PMD 可能是 2MB 叶子大页；PGD 1GB 叶子是 Sv39 硬件能力，
   但 Linux RISC-V 5.15 未定义 pgd_leaf，默认不按 PGD 大页处理。
   非 present PTE 可能保存 swap/migration 信息。

4. 当前 runninglinuxkernel 使用 FLATMEM：
   pfn_to_page(pfn) = mem_map + pfn - ARCH_PFN_OFFSET
   不是 vmemmap + pfn。
   struct page 元数据数组由 mem_map 指向，大小约为
   max_mapnr * sizeof(struct page)。
   本内核 sizeof(struct page) = 64B。
   phy_mem >> PAGE_SHIFT * sizeof(struct page) 有优先级错误，
   应为 (phy_mem >> PAGE_SHIFT) * sizeof(struct page)。

5. 缺页路径：
   pgd_offset()     不分配
   p4d_alloc()      当前折叠，不分配
   pud_alloc()      当前折叠，不分配
   pmd_alloc()      需要时分配 PMD 页
   pte_alloc()      需要时分配 PTE 页
   pte_offset_map() 只计算 PTE entry 地址，不分配

   页表页内容走 direct map：
   pfn_to_virt(pfn) / __va(pfn_to_phys(pfn))
   页表页元数据走 mem_map（或 vmemmap，取决于内存模型）。
```

---

## 6. 关键源码索引

| 内容 | 路径与行号 |
|---|---|
| RISC-V `pgd_t` / `pte_t` | `arch/riscv/include/asm/page.h:51-59` |
| RISC-V `pmd_t` | `arch/riscv/include/asm/pgtable-64.h:21-29` |
| 页表项数量 | `arch/riscv/include/asm/pgtable.h:108-115` |
| Sv39 地址位移 | `arch/riscv/include/asm/pgtable-64.h:11-29` |
| PMD 叶子页判断 | `arch/riscv/include/asm/pgtable.h:205-208` |
| PGD 叶子默认值 | `include/linux/pgtable.h:1584-1586` |
| PTE PFN 编解码 | `arch/riscv/include/asm/pgtable.h:251-263` |
| `_PAGE_LEAF` / `_PAGE_PFN_SHIFT` | `arch/riscv/include/asm/pgtable-bits.h:36-46` |
| `mm_struct.pgd` | `include/linux/mm_types.h:402-421` |
| `vm_area_struct.vm_mm` | `include/linux/mm_types.h:319-341` |
| `pgd_alloc()` | `arch/riscv/include/asm/pgalloc.h:41-54` |
| `pmd_alloc()` | `include/linux/mm.h:2171-2175` |
| `pmd_alloc_one()` | `include/asm-generic/pgalloc.h:119-134` |
| `pte_alloc()` | `include/linux/mm.h:2278` |
| `__pte_alloc()` | `mm/memory.c:436-467` |
| `pte_alloc_one()` | `include/asm-generic/pgalloc.h:59-86` |
| `pte_offset_kernel()` | `include/linux/pgtable.h:87-102` |
| `pmd_page_vaddr()` | `arch/riscv/include/asm/pgtable.h:236-239` |
| `__handle_mm_fault()` | `mm/memory.c:4633-4667` |
| `handle_pte_fault()` | `mm/memory.c:4517-4547` |
| `do_anonymous_page()` | `mm/memory.c:3736-3749` |
| FLATMEM `pfn_to_page` | `include/asm-generic/memory_model.h:12-26` |
| `mem_map` / `max_mapnr` | `mm/memory.c:93-98` |
| buddy 摘除空闲页 | `mm/page_alloc.c:2443-2468` |
| zone lock 保护分配 | `mm/page_alloc.c:3701-3724` |
| `__free_pages()` | `mm/page_alloc.c:5451-5454` |
| `_mapcount` / `_refcount` | `include/linux/mm_types.h:196-216` |
| `page_ref_inc()` | `include/linux/page_ref.h:114-119` |
| `ARCH_PFN_OFFSET` | `arch/riscv/include/asm/page.h:81-86` |
| `pfn_to_virt()` / `virt_to_page()` | `arch/riscv/include/asm/page.h:147-158` |
| 当前配置 | `.config:240-242`、`.config:255`、`.config:467-470` |

---

## 7. 最需要记住的三句话

```text
1. 页表属于 mm，不属于 VMA。
2. pmd_alloc()/pte_offset_map() 返回的是 entry 地址；
   pte_alloc() 才负责按需分配 PTE 页。
3. 页表页内容走 direct map；struct page 元数据走 mem_map 或 vmemmap，
   两者不是同一个虚拟地址区域。
```
