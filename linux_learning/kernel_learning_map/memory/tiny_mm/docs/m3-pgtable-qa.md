# M3 页表与 PFN 问答整理

> 项目：[tiny-mm](../README.md)  
> 阶段：M3 两级页表 + TLB  
> 参考：Linux 5.15，x86_64 / RISC-V 的通用页表模型  
> 关联：[m3-pgtable.md](m3-pgtable.md)

本文把 M3 学习过程中关于物理页、页表页、`pgd_t`/`pte_t`、`page_address()`、`virt_to_page()` 和 PFN 的问答整理成一篇文档。

核心观点：

> **PFN 是物理页的稳定身份。**  
> `struct page`、物理地址、direct map 虚拟地址和 PTE，都是 PFN 的不同视图。

---

## 目录

1. [为什么 pmm 分配后还要 calloc 4KB](#1-为什么-pmm-分配后还要-calloc-4kb)
2. [真实 Linux 中 struct page 和 4KB 页内容的关系](#2-真实-linux-中-struct-page-和-4kb-页内容的关系)
3. [pgtable_walk 的 create 是什么](#3-pgtable_walk-的-create-是什么)
4. [pte_page 的 entries 保存什么](#4-pte_page-的-entries-保存什么)
5. [Linux 风格的 pgd_t 和 pte_t](#5-linux-风格的-pgd_t-和-pte_t)
6. [PAGE_PRESENT 的含义](#6-page_present-的含义)
7. [mm_map 要实现什么](#7-mm_map-要实现什么)
8. [pmd_alloc/pte_offset_map 与 tiny-mm 的区别](#8-pmd_allocpte_offset_map-与-tiny-mm-的区别)
9. [page_address 和 virt_to_page 如何转换](#9-page_address-和-virt_to_page-如何转换)
10. [以 PFN 为视角的完整梳理](#10-以-pfn-为视角的完整梳理)
11. [常见坑](#11-常见坑)

---

## 1. 为什么 pmm 分配后还要 calloc 4KB

tiny-mm 中：

```c
static struct page *pgtable_alloc_page(void)
{
    struct page *page = pmm_alloc_pages(0);

    if (!page)
        return NULL;

    page->pgtable = calloc(1, PAGE_SIZE);
    if (!page->pgtable) {
        pmm_free_pages(page, 0);
        return NULL;
    }

    return page;
}
```

这里有两步：

```text
pmm_alloc_pages(0)
    → 分配一个“物理页”的元数据 struct page
    → 标记这个物理页已经被占用

calloc(1, PAGE_SIZE)
    → 分配这块页表页的 4KB 内容
    → 内容全 0，表示所有 PTE 初始都非 present
```

原因是 tiny-mm 的 `struct page` 只是元数据：

```c
struct page {
    unsigned long flags;
    unsigned int order;
    int refcount;
    bool is_free;
    ...
    void *pgtable;   /* 页表页内容，普通数据页为 NULL */
};
```

它本身不是 4KB 页内容。

真实 Linux 中：

```c
struct page *page = alloc_page(GFP_KERNEL);
void *kaddr = page_address(page);
```

`alloc_page()` 返回的物理页已经有 4KB 内容，`page_address()` 通过 direct map 找到它。  
tiny-mm 没有真实 MMU 和 direct map，所以需要额外用 `calloc` 模拟页内容。

释放顺序：

```c
free(page->pgtable);
pmm_free_pages(page, 0);
```

先释放 4KB 内容，再归还物理页元数据。

---

## 2. 真实 Linux 中 struct page 和 4KB 页内容的关系

### 2.1 两者是分开的

Linux 物理内存按 4KB 页框划分。每个页框有一个 `struct page` 元数据：

```text
物理内存：

+----------------+  PFN 0
| 4KB 数据页      |
+----------------+  PFN 1
| 4KB 页表页      |
+----------------+  PFN 2
| 4KB 数据页      |
+----------------+
| ...            |
+----------------+
| struct page 数组|
| （vmemmap）     |
+----------------+
```

`struct page` 数组本身也占物理内存，但它是一个独立的大数组，不是塞在每个 4KB 页框里。

因此：

```text
4KB 页框：完整 4KB 内容
struct page：独立的元数据
```

用户映射一个数据页时，整个 4KB 都可以作为用户数据使用，不会先切掉 64B 给元数据。

### 2.2 x86_64 的典型虚拟地址布局

典型 4 级页表布局（KASLR 会改变具体值）：

```text
用户空间
0x0000000000000000 - 0x00007fffffffffff

内核空间
0xffff888000000000  direct map
0xffffea0000000000  vmemmap
0xffffc90000000000  vmalloc
...
```

- **direct map**：物理页内容的内核虚拟地址；
- **vmemmap**：`struct page` 数组的内核虚拟地址。

### 2.3 page 和 PFN 的关系

`struct page` 不是通过“数据指针”关联 4KB 内容，而是通过 PFN：

```c
page_to_pfn(page) = page - vmemmap;
pfn_to_page(pfn) = vmemmap + pfn;
```

内容地址：

```c
page_address(page)
= __va(PFN_PHYS(page_to_pfn(page)));
```

即：

```text
struct page *  →  PFN  →  物理地址  →  direct map 虚拟地址
```

### 2.4 “另一个地址空间”的准确说法

`struct page` 数组位于**内核虚拟地址区域**，不是每个进程独立的用户地址空间。

- 用户态不能直接访问 `struct page`；
- 用户页表只映射数据页的 4KB 内容；
- `struct page` 元数据不会出现在用户 VMA 中。

---

## 3. pgtable_walk 的 create 是什么

`create` 是一个 `bool`：

```c
static pte_t *pgtable_walk(struct mm_struct *mm, unsigned long vaddr,
                           bool create);
```

含义：

> **如果中间页表页不存在，是否允许现在分配一个。**

当前 tiny-mm 是两级页表：

```text
PGD → PTE 页 → PTE entry
```

`mm_create()` 已经分配了 PGD 页，所以 `walk` 主要判断 PGD entry 是否指向 PTE 页。

| PGD entry | create | 行为 |
|---|---|---|
| 已存在 | true | 返回已有 PTE |
| 已存在 | false | 返回已有 PTE |
| 为空 | true | 分配 PTE 页，写 PGD entry，返回 PTE |
| 为空 | false | 返回 NULL |

`create` 控制的是：

```text
PTE 页不存在时，要不要现建一张？
```

它不是：

- 创建数据页；
- 创建 PTE 映射；
- 创建 VMA。

数据页由调用者传给 `mm_map()`。

`mm_map()` 使用 `create=true`；`mm_unmap()`、`mm_protect()`、`mm_translate()` 使用 `create=false`。

注意：`vaddr == 0` 是合法虚拟地址，不能用 `if (!vaddr)` 判断非法；应该检查对齐和 `VADDR_LIMIT`。

---

## 4. pte_page 的 entries 保存什么

```c
typedef uint64_t pteval_t;

typedef struct {
    pteval_t pte;
} pte_t;

struct pte_page {
    pte_t entries[PTE_ENTRIES];   /* 512 × 8 = 4096 字节 */
};
```

数组元素保存的是**页表项 PTE**：

```text
[ PFN ][ flags ]
```

不是用户数据，也不是 `struct page *`。

### 4.1 PTE 的例子

```c
pte_t pte = pfn_pte(5, PTE_PRESENT | PTE_WRITE | PTE_USER);
```

展开：

```text
PFN = 5
PFN << 12 = 0x5000

PTE_PRESENT = 0x1
PTE_WRITE   = 0x2
PTE_USER    = 0x4

pte = 0x5000 | 0x1 | 0x2 | 0x4
    = 0x5007
```

解析：

```c
pte_pfn(0x5007)      // 5
pte_present(0x5007)  // true
```

### 4.2 PGD entry 和 PTE entry 含义不同

```text
PGD 页
  entries[pgd_index]  → 指向一个 PTE 页（PFN + flags）

PTE 页
  entries[pte_index]  → 指向真正的数据页（PFN + flags）
```

例如：

```text
PGD[2] = pfn_pgd(0x200, PGD_PRESENT)
PTE[7] = pfn_pte(0x100, PTE_PRESENT | PTE_USER | PTE_WRITE)
```

完整路径：

```text
vaddr
  → PGD index
  → PFN 0x200（PTE 页）
  → PTE index
  → PFN 0x100（数据页）
  → 页内 offset
```

### 4.3 0 表示未映射

`calloc` 分配的页表页初始全 0：

```text
entries[0..511] = 0
```

0 表示：

```text
这个 entry 没有 present
对应虚拟地址没有映射
```

严格判断应使用：

```c
pte_present(entry)
```

而不是只判断：

```c
entry != 0
```

因为 Linux 中可能存在“非 present 但带其他信息”的 PTE，例如 swap entry。

---

## 5. Linux 风格的 pgd_t 和 pte_t

tiny-mm 当前实现：

```c
typedef uint64_t pgdval_t;
typedef uint64_t pteval_t;

typedef struct {
    pgdval_t pgd;
} pgd_t;

typedef struct {
    pteval_t pte;
} pte_t;
```

访问器：

```c
pgd_val(pgd);
pte_val(pte);

__pgd(value);
__pte(value);
```

PGD：

```c
pgd_none(pgd);
pgd_present(pgd);
pgd_pfn(pgd);
pfn_pgd(pfn, flags);
```

PTE：

```c
pte_none(pte);
pte_present(pte);
pte_write(pte);
pte_user(pte);
pte_pfn(pte);
pfn_pte(pfn, flags);
pte_mkwrite(pte);
pte_wrprotect(pte);
pte_modify(pte, flags);
```

页表页也分开：

```c
struct pgd_page {
    pgd_t entries[PGD_ENTRIES];
};

struct pte_page {
    pte_t entries[PTE_ENTRIES];
};

struct mm_struct {
    struct page *pgd_page;
    struct pgd_page *pgd;
    unsigned int page_table_pages;
    unsigned long tlb_flush_count;
};
```

这更接近 Linux：

- 页表项是带类型的值，不是到处直接使用裸整数；
- 操作页表项优先使用 `pte_present()`、`pte_pfn()` 等访问器；
- 不同层级的页表页可以有不同的 entry 类型。

不同架构的底层定义可能不同：x86 常用 struct 包装，RISC-V 某些版本直接 `typedef unsigned long pte_t`。  
tiny-mm 采用“类型包装 + 访问器”的风格，重点是类型语义，而不是逐字复制某个架构。

---

## 6. PAGE_PRESENT 的含义

tiny-mm 中没有 `PAGE_PRESENT`，对应的是：

```c
#define PTE_PRESENT (1UL << 0)
#define PGD_PRESENT PTE_PRESENT
```

`PTE_PRESENT` 表示：

> **这个页表项当前是否描述一个有效映射。**

```text
PTE_PRESENT = 1  → 有效的地址映射，MMU 可以继续翻译
PTE_PRESENT = 0  → 未映射/无效，访问会触发缺页
```

它只表示“存在”，不表示“可写”或“用户可访问”：

```c
pte_present(pte);  // 是否存在
pte_write(pte);    // 是否可写
pte_user(pte);     // 是否用户可访问
```

PGD entry 的 present 位表示：

```text
PTE 页是否存在
```

present 和物理页分配状态是不同概念：

| 概念 | 层级 | 含义 |
|---|---|---|
| `PTE_PRESENT` | 页表项 | 虚拟地址是否有有效映射 |
| `page->is_free` | 物理页分配器 | 物理页是否在空闲集合中 |

一个页可以“已经分配，但还没有映射进某个 mm”：

```text
page->is_free = false
PTE_PRESENT   = 0
```

真实 Linux 中对应架构位通常叫 `_PAGE_PRESENT`；非 present 的 PTE 还可能保存 swap entry 等信息。

---

## 7. mm_map 要实现什么

`mm_map()` 的作用：

> **把虚拟地址 vaddr 映射到指定的物理页 page，建立一条 PTE 映射。**

它不是 `mmap()` 系统调用，也不创建 VMA。

### 7.1 目标状态

调用前：

```text
PGD entry 可能为空
PTE 页可能不存在
PTE entry 为 0
```

调用后：

```text
PGD[pgd_index] → PTE 页
PTE[pte_index] → PFN(page)
```

### 7.2 步骤

1. 校验 `mm`、`page`、`vaddr` 对齐、`vaddr < VADDR_LIMIT`；
2. 检查物理页状态：

   ```c
   page->state == PAGE_ALLOCATED
   ```

3. `pgtable_walk(mm, vaddr, true)` 找到 PTE，必要时分配 PTE 页；
4. 如果 `pte_present(*pte)`，返回 `-EEXIST`；
5. 构造 flags：

   ```c
   flags = PTE_PRESENT | PTE_USER;
   if (prot & MM_PROT_WRITE)
       flags |= PTE_WRITE;
   ```

6. 写入 PTE：

   ```c
   *pte = pfn_pte(page_to_pfn(page), flags);
   ```

7. 增加 TLB flush 计数：

   ```c
   mm->tlb_flush_count++;
   ```

### 7.3 当前实现

```c
int mm_map(struct mm_struct *mm, unsigned long vaddr, struct page *page,
           unsigned int prot)
{
    pte_t *pte;
    unsigned long flags;

    if (!mm || !page)
        return -EINVAL;

    if ((vaddr & (PAGE_SIZE - 1)) != 0)
        return -EINVAL;

    if (vaddr >= VADDR_LIMIT)
        return -EINVAL;

    if (page->state != PAGE_ALLOCATED)
        return -EINVAL;

    pte = pgtable_walk(mm, vaddr, true);
    if (!pte)
        return -ENOMEM;

    /* 同一个 vaddr 不能重复映射。 */
    if (pte_present(*pte))
        return -EEXIST;

    flags = PTE_PRESENT | PTE_USER;
    if (prot & MM_PROT_WRITE)
        flags |= PTE_WRITE;

    *pte = pfn_pte(page_to_pfn(page), flags);
    mm->tlb_flush_count++;

    return 0;
}
```

### 7.4 mm_map 不做什么

| 不做什么 | 由谁负责 |
|---|---|
| 不分配数据页 | 调用者先 `pmm_alloc_pages()` |
| 不创建 VMA | M4 |
| 不处理缺页 | M4 |
| 不复制页内容 | 调用者 |
| 不增加页面引用计数 | M3 暂不建模，M5 COW 再处理 |
| 不映射一段范围 | 当前只映射一个 4K 页 |
| 不自动 unmap 旧映射 | 已存在时返回 `-EEXIST` |
| 不释放数据页 | `mm_unmap()` 只清 PTE，数据页由调用者管理 |

---

## 8. pmd_alloc/pte_offset_map 与 tiny-mm 的区别

Linux 缺页处理中：

```c
vmf.pmd = pmd_alloc(mm, vmf.pud, address);
vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
```

### 8.1 pmd_alloc

作用：

```text
确保 address 对应的 PMD 层存在
如果 PMD 页不存在，就分配一个 PMD 页
把 PUD entry 指向这个 PMD 页
返回这个 PMD entry 的地址
```

返回类型：

```c
pmd_t *
```

调用者可以直接操作：

```c
if (pmd_none(*vmf.pmd))
    ...
```

### 8.2 pte_offset_map

作用：

```text
PMD entry
  → PMD 页基地址
  → 加上 PTE index
  → 得到 PTE entry 地址
```

返回类型：

```c
pte_t *
```

它不分配页表页，只进行地址计算。

### 8.3 tiny-mm 的对应关系

tiny-mm 没有 PMD 层：

```text
PGD → PTE
```

对应关系：

| Linux | tiny-mm |
|---|---|
| `pmd_alloc()` | `pgtable_walk(create=true)` 中分配 PTE 页 |
| `pte_offset_map()` | `pgtable_walk()` 返回 `&entries[PTE_INDEX(vaddr)]` |
| `pmd_t *` | `pgd_t *` |
| `pte_t *` | `pte_t *` |

`pgtable_alloc_page()` 返回 `struct page *`，因为它模拟的是更底层的：

```text
pmm_alloc_pages(0) + 4KB 页表内容
```

它不是 `pmd_alloc()` 的完全对应物。

完整对应：

```text
Linux：
pmd_alloc()       → 分配 PMD 页，返回 pmd_t *
pte_offset_map()  → 算出 pte_t *

tiny-mm：
pgtable_alloc_page() → 分配 PTE 页，返回 struct page *
pgtable_walk()       → 返回 pte_t *
```

`pgtable_walk()` 的最终返回值，才对应 Linux 的 `pte_t *`。

### 8.4 内核是否总是返回地址

更准确地说：

- 上层页表遍历 API 通常返回 `pmd_t *` / `pte_t *`；
- 底层页表页分配仍可能使用 `struct page *` / `pgtable_t`。

例如 x86 的 `pte_alloc_one()` 可能返回 `pgtable_t`，而 `pgtable_t` 在某些架构上就是 `struct page *`。  
之后 `pte_offset_map()` 再把它转换成 `pte_t *`。

tiny-mm 没有 direct map，不能像 Linux 那样通过 `virt_to_page(pte)` 反查元数据，所以保留 `struct page *` 是合理的模拟设计。

---

## 9. page_address 和 virt_to_page 如何转换

### 9.1 PFN 是中间桥梁

```text
struct page *       → PFN → 物理地址 → direct map 虚拟地址
direct map 虚拟地址 → 物理地址 → PFN → struct page *
```

### 9.2 page_address

对于常见 64 位系统（无 highmem）：

```c
page_address(page)
= __va(PFN_PHYS(page_to_pfn(page)))
= page_offset_base + (PFN << PAGE_SHIFT)
```

代码：

```c
static __always_inline void *lowmem_page_address(const struct page *page)
{
    return __va(PFN_PHYS(page_to_pfn(page)));
}
```

`page_address()` 返回的是 4KB 内容的**页基地址**。

### 9.3 virt_to_page

```c
#define virt_to_page(kaddr) \
    pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
```

展开：

```text
内核虚拟地址 kaddr
    ↓ __pa()
物理地址 phys
    ↓ >> PAGE_SHIFT
PFN
    ↓ pfn_to_page()
struct page *
```

### 9.4 数值例子

假设：

```text
PFN = 0x100
page_offset_base = 0xffff888000000000
```

则：

```text
phys   = 0x100 << 12 = 0x100000
kaddr  = page_offset_base + 0x100000
page   = vmemmap + 0x100
```

并且：

```c
page_address(page) == kaddr;
virt_to_page(kaddr) == page;
```

### 9.5 virt_to_page(pte) 到底找到哪个 page

假设：

```c
pte_t *pte = pte_offset_map(pmd, address);
```

`pte` 指向：

```text
PTE 页中的某个 8 字节 entry
```

`virt_to_page(pte)`：

```text
pte 的虚拟地址
    ↓ __pa(pte)
PTE entry 的物理地址
    ↓ >> PAGE_SHIFT
PTE 页的 PFN
    ↓ pfn_to_page()
PTE 页的 struct page
```

所以：

```c
virt_to_page(pte)   → 装 PTE 的页表页
```

它完全不看 `*pte` 的值。

要拿到 PTE 指向的数据页：

```c
struct page *data_page = pte_page(*pte);
```

等价于：

```c
struct page *data_page = pfn_to_page(pte_pfn(*pte));
```

即：

```text
*pte
    ↓ pte_pfn()
数据页 PFN
    ↓ pfn_to_page()
数据页的 struct page
```

### 9.6 两个 PFN 的对比

假设：

```text
PTE 页 PFN   = 0x200
数据页 PFN   = 0x100
```

则：

```c
virt_to_page(pte)   → PFN 0x200 的 struct page
pte_page(*pte)      → PFN 0x100 的 struct page
```

### 9.7 注意事项

- `virt_to_page()` 只适用于 direct map / lowmem 地址；
- 用户虚拟地址不能直接传给 `virt_to_page()`，需要先通过页表 walk 得到 PFN；
- vmalloc 地址应使用 `vmalloc_to_page()`；
- highmem 的 kmap 地址应使用 `kmap_to_page()`；
- 常见 64 位系统通常没有 highmem，`page_address()` 直接走 direct map。

---

## 10. 以 PFN 为视角的完整梳理

### 10.1 PFN 的四种视图

```text
PFN
 ├── 物理地址        phys = PFN << 12
 ├── struct page *   page = vmemmap + PFN
 ├── direct map 地址 kaddr = page_offset_base + (PFN << 12)
 └── 页表项          PTE = (PFN << 12) | flags
```

### 10.2 页表遍历就是 PFN 链

```text
PGD entry
    → PTE 页的 PFN
        → PTE 页内容
            → PTE entry
                → 数据页的 PFN
```

以两级页表为例：

```text
PGD 页     PFN = 0x300
PTE 页     PFN = 0x200
数据页     PFN = 0x100

PGD[2] = pfn_pgd(0x200, PRESENT)
PTE[7] = pfn_pte(0x100, PRESENT | USER | WRITE)
```

虚拟地址 `vaddr` 的翻译过程：

```text
vaddr
  → PGD index
  → PGD entry 中的 PFN 0x200
  → PTE 页
  → PTE index
  → PTE entry 中的 PFN 0x100
  → 数据页 + offset
```

### 10.3 Linux 的分配过程

`pmd_alloc()`：

```text
PUD entry
    → 如果为空，分配 PMD 页
    → 写入 PMD 页的 PFN
    → 返回 PMD entry 地址（pmd_t *）
```

`pte_offset_map()`：

```text
PMD entry
    → pmd_pfn(*pmd)
    → PTE 页 PFN
    → direct map 地址
    → PTE entry 地址（pte_t *）
```

### 10.4 tiny-mm 的模拟过程

```text
vaddr
  → PGD entry
  → pte_page_pfn
  → pfn_to_page(pte_page_pfn)
  → page->pgtable
  → PTE entry
  → data_pfn
  → pfn_to_page(data_pfn)
  → struct page
```

代码对应：

```c
/* PGD entry 保存 PTE 页 PFN */
mm->pgd->entries[pgd_index] =
    pfn_pgd(page_to_pfn(pte_page), PGD_PRESENT);

/* PTE entry 保存数据页 PFN */
*pte = pfn_pte(page_to_pfn(data_page), flags);
```

### 10.5 一句话总结

> 物理页的身份是 PFN。  
> `struct page` 是 `vmemmap + PFN`；  
> direct map 地址是 `page_offset_base + (PFN << 12)`；  
> PTE 的值是 `(PFN << 12) | flags`；  
> 页表项只是把“下一级页表页的 PFN”或“数据页的 PFN”编码进去。

---

## 11. 常见坑

### 11.1 混淆 `pte` 指针所在页和 `*pte` 指向页

```c
virt_to_page(pte)   // pte 指针所在页 → PTE 页
pte_page(*pte)      // PTE 值中的 PFN → 数据页
```

### 11.2 用 `if (!vaddr)` 判断非法地址

`vaddr == 0` 是合法虚拟地址。应该检查：

```c
(vaddr & (PAGE_SIZE - 1)) != 0
vaddr >= VADDR_LIMIT
```

### 11.3 把 `MM_PROT_WRITE` 和 `PTE_WRITE` 混用

两者数值碰巧相同，但语义不同：

```c
if (prot & MM_PROT_WRITE)
    flags |= PTE_WRITE;
```

`prot` 使用 `MM_PROT_*`；页表项使用 `PTE_*`。

### 11.4 walk 返回 NULL 后直接解引用

```c
pte = pgtable_walk(mm, vaddr, true);
if (!pte)
    return -ENOMEM;

if (pte_present(*pte))
    ...
```

### 11.5 混用 tab 和空格导致误导性缩进

```c
if (page->state != PAGE_ALLOCATED)
    return -EINVAL;

    pte = pgtable_walk(...);   /* 看起来像被 if 保护 */
```

在 `-Werror=misleading-indentation` 下会直接编译失败。Linux 风格使用 tab 缩进，并保持块结构清晰。

---

## 附录：Linux 与 tiny-mm 对照

| Linux | tiny-mm |
|---|---|
| `struct page` | `struct page` |
| `vmemmap + pfn` | `pfn_to_page(pfn)` |
| `page_address(page)` | `page->pgtable` |
| `__va(PFN_PHYS(pfn))` | 无 direct map |
| `virt_to_page(kaddr)` | 无直接对应 |
| `pte_page(*pte)` | `pfn_to_page(pte_pfn(*pte))` |
| `pmd_alloc()` | `pgtable_walk(create=true)` 中的分配 |
| `pte_offset_map()` | `pgtable_walk()` 返回 `pte_t *` |
| `pmd_t *` / `pte_t *` | `pgd_t *` / `pte_t *` |
| `_PAGE_PRESENT` | `PTE_PRESENT` / `PGD_PRESENT` |

---

## 参考

- [include/pgtable.h](../include/pgtable.h)
- [src/pgtable.c](../src/pgtable.c)
- [tests/test_pgtable.c](../tests/test_pgtable.c)
- [M3 页表文档](m3-pgtable.md)
- Linux 5.15：`include/linux/mm.h`、`include/asm-generic/memory_model.h`、`mm/highmem.c`、`arch/x86/include/asm/page_64.h`
