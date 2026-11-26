# M4：VMA + 缺页

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m3-pgtable.md](m3-pgtable.md)
> 状态：**🚧 M4 脚手架已搭好。**
>
> 当前 `make test-m4`：`flags & empty find_vma` 通过，其余 8 个缺页/VMA 测试待实现。
>
> 本文档只搭骨架，不直接给完整实现。先把 VMA 生命周期和缺页语义想清楚。

## 目标

M3 解决了“`vaddr` 怎么翻译成 `page`”的机制（页表 walk），但它不回答
“这一段 `vaddr` 该不该映射、以什么权限映射”。M4 在 M3 上面加一层 VMA：

```text
mm_mmap(vaddr, len, prot)
    → 建 VMA（只登记意图，不分配页、不写 PTE）
mm_access(vaddr, access)
    → find_vma → 权限检查 → 缺页
    → 首次访问分配匿名页 + 写 PTE
```

要掌握：

- VMA 是“映射意图”，PTE 是“映射结果”；二者可以不同步（惰性映射）；
- `find_vma` 语义：第一个 `vm_end > addr`，不保证包含 addr；
- 匿名缺页：`do_anonymous_page` 分配一个 0 页并写 PTE；
- `mmap` 的对齐/溢出检查，`munmap` 的 VMA 拆分；
- VMA 合并的条件（地址相邻、flags 兼容）；
- `mm_destroy` 要同时回收匿名页和页表页。

## 与 M3 的关系

M3 的 `mm_map()`/`mm_unmap()`/`mm_translate()` 直接操作 PTE，是“机制层”。
M4 不重写它们，而是在上面做“策略层”：

| 概念 | M3（机制） | M4（策略） |
|---|---|---|
| 建映射 | `mm_map(mm, vaddr, page, prot)` | `mm_mmap(mm, vaddr, len, prot)` 只建 VMA |
| 查映射 | `mm_translate(mm, vaddr, access)` | `mm_access(mm, vaddr, access)` 带缺页 |
| 拆映射 | `mm_unmap(mm, vaddr)` | `mm_munmap(mm, vaddr, len)` 还页 + 拆 VMA |
| 改权限 | `mm_protect(mm, vaddr, prot)` | `mm_mprotect(mm, vaddr, len, prot)` |

`mm_access()` 复用 M3 的 `mm_translate()`（软缺页：PTE 已在，直接返回）和
`mm_map()`（硬缺页：分配页后写 PTE）。

## 数据结构

[`include/vma.h`](../include/vma.h)：

```c
#define VM_READ   MM_PROT_READ   /* 0x1，故意和 MM_PROT_* 同值 */
#define VM_WRITE  MM_PROT_WRITE  /* 0x2 */
#define VM_EXEC   MM_PROT_EXEC   /* 0x4 */

struct vm_area_struct {
    struct mm_struct *vm_mm;
    unsigned long vm_start;    /* 包含 */
    unsigned long vm_end;      /* 不包含 */
    unsigned long vm_flags;    /* VM_READ/WRITE/EXEC */
    unsigned long vm_pgoff;    /* M5 文件页用；M4 固定 0 */
    struct vm_area_struct *vm_next;
};
```

`struct mm_struct` 新增（见 [pgtable.h](../include/pgtable.h)）：

```c
struct vm_area_struct *vma_list;   /* M4: 按 vm_start 有序的 VMA 链表头 */
```

VM flags 和 `MM_PROT_*` 同值，所以：

- `vma->vm_flags` 可以直接传给 `mm_map()` / `mm_protect()`；
- `mm_access()` 的 `access`（`MM_ACCESS_*`）可以直接 `&` 在 `vm_flags` 上判权限。

### 页内容：待定项

M4 的匿名页目前只分配不写内容（验收不读页内容）。Linux 的 `do_anonymous_page`
会给 0 页。到 M5 做 COW 时，匿名页必须有 4KB 内容缓冲区，届时把
`struct page.pgtable` 泛化成 `struct page.data` + `page_address()`。

## API

```c
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);

int mm_mmap(struct mm_struct *mm, unsigned long addr, unsigned long len,
            unsigned int prot);
int mm_munmap(struct mm_struct *mm, unsigned long addr, unsigned long len);
int mm_mprotect(struct mm_struct *mm, unsigned long addr, unsigned long len,
                unsigned int prot);

struct page *mm_access(struct mm_struct *mm, unsigned long addr,
                       unsigned int access);
```

## 缺页流程

`mm_access()` 的核心逻辑：

```text
1. vma = find_vma(mm, addr)
   没有 VMA，或 addr < vma->vm_start → 返回 NULL（空洞，类比 SIGSEGV）
2. 权限检查：!(vma->vm_flags & access) → 返回 NULL（越权）
3. page = mm_translate(mm, addr, access)
   page != NULL → 返回 page（软缺页，PTE 已在）
4. 硬缺页：page = pmm_alloc_pages(0)
   失败 → 返回 NULL（类比 OOM）
5. mm_map(mm, addr, page, vma->vm_flags)
   失败 → 归还 page，返回 NULL
6. 返回 page
```

## 已完成 / TODO

### 已完成

- `include/vma.h`：`struct vm_area_struct`、VM flags、API 声明；
- `struct mm_struct.vma_list`；
- 验收测试骨架 `tests/test_vma.c`。

### TODO

1. `find_vma(mm, addr)`
   - 在 `vma_list`（按 `vm_start` 升序）上找第一个 `vm_end > addr`；
   - 空链表 / addr 超出所有 VMA → NULL。

2. `mm_mmap(mm, addr, len, prot)`
   - 校验 mm/对齐/溢出（`addr + len` 不能回绕，不能超过 `VADDR_LIMIT`）；
   - `calloc` 一个 `struct vm_area_struct`，插入到 `vma_list` 有序位置；
   - 相邻且 `vm_flags` 相同 → 合并成一个；
   - 与现有 VMA 重叠 → `-EEXIST`（不建，也不合并）。

3. `mm_access(mm, addr, access)`
   - 上面的缺页流程；
   - 分配匿名页后写 PTE（复用 `mm_map`），匿名页归 mm 所有。

4. `mm_munmap(mm, addr, len)`
   - 找出与 `[addr, addr+len)` 相交的 VMA，拆分/收缩/删除；
   - 对区间内每个已映射的 PTE：`mm_unmap` + `pmm_free_pages(page, 0)`（put_page）；
   - 不释放没缺页的空 VMA 区间（本来就没有页）。

5. `mm_mprotect(mm, addr, len, prot)`
   - 调整相交 VMA 的 `vm_flags`（必要时拆分 VMA）；
   - 对已映射的 PTE 调 `mm_protect` 同步权限。

6. `mm_destroy` 扩展
   - 除了 PGD/PTE 页，还要释放缺页分配的匿名页；
   - 区分方法：`page->pgtable != NULL` 是页表页，`== NULL` 是数据页。

## 10 个脚手架测试

| 测试 | 目标 | 当前 |
|---|---|---|
| `flags & empty find_vma` | VM flags 与 MM_PROT 同值，空链表 find_vma 返回 NULL | ✅ PASS |
| `mmap fault` | mmap 后首次访问缺页，重复访问同一页 | ❌ TODO |
| `unmapped access` | VMA 范围外/空洞返回 NULL | ❌ TODO |
| `readonly vma` | 只读 VMA 写访问返回 NULL | ❌ TODO |
| `munmap` | munmap 后 VMA 消失、页归还 | ❌ TODO |
| `find_vma adjacent` | 相邻 VMA 的 find_vma 语义 | ❌ TODO |
| `vma merge` | 相邻兼容 VMA 合并 | ❌ TODO |
| `mprotect` | mprotect 后权限生效 | ❌ TODO |
| `destroy releases all` | mm_destroy 回收匿名页 + 页表页 | ❌ TODO |
| `random stress` | 随机 mmap/munmap/access 无泄漏 | ❌ TODO（当前因 stub 空转而过） |

当前预期：

```text
[PASS] flags & empty find_vma
[FAIL] mmap fault
[FAIL] unmapped access
[FAIL] readonly vma
[FAIL] munmap
[FAIL] find_vma adjacent
[FAIL] vma merge
[FAIL] mprotect
[FAIL] destroy releases all
[PASS] random stress

8 test(s) failed
```

实现 TODO 后应变成 10/10。

## 运行

```bash
make test-m4
```

M4 脚手架暂不加入 `test-all`，避免打断 M2/M3 回归：

```bash
make test-all   # M2c + M2d + M3 + M2a + M2b
```

debug：

```bash
make debug-m4
```

## 实现建议

先做纯链表（不碰 PTE），再做缺页：

```text
1. find_vma：只走链表
2. mm_mmap：VMA 建立 + 合并 + 重叠检查
3. mm_access：软缺页（mm_translate）先通，再补硬缺页
4. mm_munmap：清 PTE + 还页 + 拆 VMA
5. mm_mprotect：改 VMA + 同步 PTE
6. mm_destroy：回收匿名页
7. random + 泄漏检查
```

### 自检点

```text
mm_check(mm) == 0
pmm_check() == 0
mm_access 后 pmm_free_count 减少 1
mm_destroy 后 pmm_free_count 恢复
```

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `struct vm_area_struct` | `struct vm_area_struct`（`mm_types.h:319`） |
| `find_vma` | `find_vma`（`mm/mmap.c:2273`） |
| `mm_mmap` | `mmap_region` / `do_mmap` |
| `mm_access` | `handle_mm_fault`（`mm/memory.c:4773`） |
| 硬缺页 | `do_anonymous_page`（`mm/memory.c:3715`） |
| `mm_munmap` | `do_munmap` / `zap_pte_range` |
| `mm_mprotect` | `do_mprotect_pkey` |
| 释放 mm 的页 | `exit_mmap` + `free_pgtables` |

Linux 还有：

- VMA 的红黑树索引 + `vmacache`（M4 只做链表）；
- `mmap_lock` 读写锁、`mm_users/mm_count`；
- 文件映射 `vm_file/vm_ops->fault`（M5+）；
- 栈增长 `VM_GROWSDOWN`、`SIGSEGV/SIGBUS` 信号（M4 只返回 NULL）。

## 主动注入 bug

实现后逐个验证：

1. **`find_vma` 只查 `vm_end` 不查 `vm_start`** → 空洞误命中；
2. **`mm_mmap` 不检查重叠** → VMA 链表损坏；
3. **缺页忘记写 PTE** → 反复缺页 / 访问失败；
4. **`mm_munmap` 忘记 put_page** → 页泄漏；
5. **`mprotect` 只改 VMA 不改 PTE** → 权限不生效；
6. **`mm_destroy` 忘记回收匿名页** → 泄漏。

## 延伸阅读

- [M3 页表与 PFN 问答](m3-pgtable-qa.md)：页表页、`page_address()`、PFN 视角。

## 下一步

M4 完成后：

1. 把 `test-m4` 加入 `test-all`；
2. 更新 [../reports/M4.md](../reports/M4.md)；
3. 进入 M5：rmap + COW + fork。
