# M3：两级页表 + TLB 脚手架

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m2d-watermark.md](m2d-watermark.md)
> 状态：**🚧 M3 脚手架已搭好。**
>
> 当前 `make test-m3`：`create/destroy` 和 `unmapped translate` 通过；
> 其余 6 个页表/TLB 测试待实现。
>
> 本文档只搭骨架，不直接给完整实现。先把 walk/map/unmap/protect/translate 的语义想清楚。

## 目标

M2 解决“物理页怎么来”。M3 解决：

```text
虚拟地址 vaddr
    → 页表 walk
    → PTE
    → PFN
    → struct page
```

本阶段只做**两级页表**：

```text
PGD → PTE → 物理页
```

不引入 VMA、缺页、COW、页表锁；这些留给 M4/M5。

要掌握：

- 两级页表的 index 拆分；
- PTE 里 PFN 和 present/write/user 位；
- 页表页本身也是物理页，需要从 pmm 分配/释放；
- `walk(create)` 的按需建表；
- PTE 修改后为什么要 flush TLB；
- 只读页写访问为什么失败。

## 虚拟地址布局

当前使用 30 位虚拟地址：

```text
[ PGD index : 9 bits ][ PTE index : 9 bits ][ page offset : 12 bits ]
```

即：

```text
PGD_SHIFT = 12 + 9 = 21
PTE_INDEX_BITS = 9
PGD_INDEX_BITS = 9
VADDR_BITS = 30
VADDR_LIMIT = 1 << 30
```

两级页表：

```text
mm->pgd
   │
   ├── PGD[0] ──→ PTE 页 ──→ PTE[0] ──→ 物理页 A
   │                         PTE[1] ──→ 物理页 B
   │
   └── PGD[1] ──→ PTE 页 ──→ ...
```

## 数据结构

[`include/pgtable.h`](../include/pgtable.h)：

```c
typedef uint64_t pte_t;

struct pte_page {
    pte_t entries[PTE_ENTRIES];   /* 512 项，正好 4KB */
};

struct mm_struct {
    struct page *pgd_page;        /* PGD 页本身，来自 pmm */
    struct pte_page *pgd;         /* PGD 页内容 */
    unsigned int page_table_pages;/* PGD + PTE 页数 */
    unsigned long tlb_flush_count;
};
```

### PTE 编码

```text
[ PFN ][ flags ]
```

标志：

```c
PTE_PRESENT  0x1
PTE_WRITE    0x2
PTE_USER     0x4
```

辅助函数：

```c
pte_pfn(pte);             /* 取 PFN */
pfn_pte(pfn, flags);      /* 组 PTE */
pte_present(pte);         /* present 位 */
```

### 页表页的存储

`struct page` 新增：

```c
void *pgtable;  /* M3: 页表页内容；普通数据页为 NULL */
```

页表页本身也是 pmm 分配的 order-0 页，额外用 `pgtable` 指向一块 4KB 的模拟存储：

```c
page = pmm_alloc_pages(0);
page->pgtable = calloc(1, PAGE_SIZE);
```

释放顺序：

```text
free(page->pgtable);
pmm_free_pages(page, 0);
```

## API

```c
struct mm_struct *mm_create(void);
void mm_destroy(struct mm_struct *mm);

int mm_map(struct mm_struct *mm, unsigned long vaddr,
           struct page *page, unsigned int prot);

int mm_unmap(struct mm_struct *mm, unsigned long vaddr);
int mm_protect(struct mm_struct *mm, unsigned long vaddr, unsigned int prot);
struct page *mm_translate(struct mm_struct *mm, unsigned long vaddr,
                          unsigned int access);

unsigned long mm_tlb_flush_count(const struct mm_struct *mm);
unsigned int mm_page_table_pages(const struct mm_struct *mm);
int mm_check(const struct mm_struct *mm);
```

保护/访问标志：

```c
MM_PROT_READ / MM_PROT_WRITE / MM_PROT_EXEC
MM_ACCESS_READ / MM_ACCESS_WRITE / MM_ACCESS_EXEC
```

## 已完成 / TODO

### 已完成

- `mm_create()`：分配 PGD 页 + 4KB 页表存储；
- `mm_destroy()`：遍历 PGD，释放所有 PTE 页和 PGD 页；
- `mm_tlb_flush_count()` / `mm_page_table_pages()` / `mm_check()`；
- `struct page.pgtable` 和 `struct mm_struct`。

### TODO

1. `pgtable_walk(mm, vaddr, create)`
   - 校验 mm/vaddr/对齐；
   - `PGD_INDEX(vaddr)` 取 PGD entry；
   - entry 不存在且 `create=false` → NULL；
   - `create=true` → 从 pmm 分配 PTE 页，初始化 `pgtable` 存储；
   - 返回 `&pte_page->entries[PTE_INDEX(vaddr)]`。

2. `mm_map()`
   - 校验 vaddr/page；
   - `walk(create=true)`；
   - 已存在 → `-EEXIST`；
   - 写 PTE：`pfn_pte(page_to_pfn(page), flags)`；
   - flush TLB。

3. `mm_unmap()`
   - `walk(create=false)`；
   - 不存在 → `-ENOENT`；
   - 清 PTE；
   - flush TLB；
   - 不释放数据页。

4. `mm_protect()`
   - 找到 PTE；
   - 改 write 位，保留 PFN；
   - flush TLB。

5. `mm_translate()`
   - `walk(create=false)`；
   - 检查 present；
   - 写访问检查 `PTE_WRITE`；
   - `pfn_to_page(pte_pfn(pte))`。

6. TLB flush 计数
   - `mm_map()` / `mm_unmap()` / `mm_protect()` 修改 PTE 后增加 `tlb_flush_count`；
   - 当前只做计数模拟，不实现真实 TLB 缓存。

## 8 个脚手架测试

| 测试 | 目标 | 当前 |
|---|---|---|
| `create/destroy` | PGD 页从 pmm 来，销毁后归还 | ✅ PASS |
| `unmapped translate` | 未映射返回 NULL | ✅ PASS |
| `map/translate/unmap` | map 后 translate 返回同一个 page，unmap 后 NULL | ❌ TODO |
| `page table pages` | 同一 PTE 页复用，不同 PGD 新建 PTE 页 | ❌ TODO |
| `permission` | 只读页写访问失败，protect 后可写 | ❌ TODO |
| `tlb flush count` | map/protect/unmap 都增加 flush 计数 | ❌ TODO |
| `destroy releases tables` | 销毁 mm 后页表页无泄漏 | ❌ TODO |
| `random map/unmap` | 1 万次随机映射，最终无泄漏 | ❌ TODO |

当前预期：

```text
[PASS] create/destroy
[PASS] unmapped translate
[FAIL] map/translate/unmap
[FAIL] page table pages
[FAIL] permission
[FAIL] tlb flush count
[FAIL] destroy releases tables
[FAIL] random map/unmap

6 test(s) failed
```

这是脚手架状态；实现 TODO 后应变成 8/8。

## 运行

```bash
make test-m3
```

M3 脚手架暂不加入 `test-all`，避免打断 M2 回归：

```bash
make test-all   # M2c + M2d + M2a + M2b
```

debug：

```bash
make debug-m3
```

## 实现建议

先按这个顺序做，避免一次改太多：

```text
1. pgtable_walk(create=false)：只查 PGD，不分配
2. pgtable_walk(create=true)：按需分配 PTE 页
3. mm_map：写 PTE
4. mm_translate：读 PTE，先不做权限
5. mm_unmap：清 PTE
6. 权限检查
7. TLB flush 计数
8. random + 泄漏检查
```

### 自检点

实现 `walk` 时优先确认：

```text
mm_check(mm) == 0
mm_page_table_pages(mm) 与 PGD/PTE 页数一致
pmm_check() == 0
```

`pmm_check()` 能同时检查物理页总账和水位标记。

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `mm->pgd` | `mm_struct.pgd` |
| `PGD_INDEX()` | `pgd_index()` / `pgd_offset()` |
| `PTE_INDEX()` | `pte_index()` / `pte_offset_map()` |
| `pte_t` | `pte_t` |
| `pte_pfn()` | `pte_pfn()` |
| `pfn_pte()` | `pfn_pte()` / `__pte()` |
| `mm_map()` | `set_pte_at()` + `set_pte()` |
| `mm_unmap()` | `ptep_get_and_clear()` |
| `mm_protect()` | `ptep_modify_prot_start/commit()` |
| `tlb_flush_count` | `flush_tlb_*()` / TLB shootdown |
| 页表页分配 | `pte_alloc()` / `pmd_alloc()` / `pgd_alloc()` |

Linux 还有：

- 五级页表（PGD/P4D/PUD/PMD/PTE）；
- 页表锁和 RCU walk；
- huge page、migration、mprotect 的批量 flush；
- `vm_normal_page` 与 special/device 页。

## 主动注入 bug

实现后逐个验证：

1. **PTE 存 page 指针而不是 PFN** → `mm_translate` 结果错误/越界；
2. **不分配中间 PTE 页** → NULL 解引用或 map 失败；
3. **修改 PTE 不增加 flush** → `tlb flush count` 失败；
4. **销毁 mm 只 free PGD，不 free PTE 页** → `destroy releases tables` / random 泄漏；
5. **只读页忽略 PTE_WRITE** → `permission` 失败；
6. **walk 不校验对齐** → 非法地址也应返回 `-EINVAL`。

## 下一步

M3 完成后：

1. 把 `test-m3` 加入 `test-all`；
2. 更新 [../reports/M3.md](../reports/M3.md)；
3. 进入 M4：VMA + 缺页。
