# M2：buddy 分配器

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m1-physical-pages.md](m1-physical-pages.md)
> 完整框架：[tiny_mm_learning_framework.md](tiny_mm_learning_framework.md)
>
> **状态：🚧 脚手架已搭好；核心 `buddy_alloc()` / `buddy_free()` 还是 TODO。**

## 目标

M1 的 allocator 是“最小可用”的 bitmap 扫描器：只能 order=0。

M2 换成真正的 buddy：

```text
free_area[0]  → 1 页块
free_area[1]  → 2 页块
free_area[2]  → 4 页块
...
free_area[8]  → 256 页块
```

要掌握：

- `order` 和块大小的关系；
- 分配时如何 split；
- 释放时如何 merge；
- 为什么释放错 order/未对齐的 page 必须拒绝；
- 如何用一个 `free_count` 和每个 `free_area.nr_free` 做自检。

## 为什么先做成独立模块

M2 先不改 M1 的 `pmm.c`，而是新增：

```text
include/buddy.h
src/buddy.c
tests/test_buddy.c
```

原因：

1. M1 的 11 个测试继续全绿，不把“物理地址模型”和“分配算法”混在一起；
2. buddy 可以单独用 mock 的 `struct page` 数组验证；
3. M2c 再把 `pmm_alloc_pages()` 切到 buddy，避免一次改太多。

所以当前：

- `make test` → M1，应该全绿；
- `make test-m2` → M2，脚手架阶段有 8 个失败；
- `make test-all` → 两个都跑。

## 文件

| 文件 | 作用 |
|---|---|
| [../include/buddy.h](../include/buddy.h) | `free_area`、`struct buddy`、API |
| [../src/buddy.c](../src/buddy.c) | 初始化/自检已实现，核心函数 TODO |
| [../tests/test_buddy.c](../tests/test_buddy.c) | 10 个验收测试 |
| [../tests/test_runner_buddy.h](../tests/test_runner_buddy.h) | M2 测试 runner |

## 数据结构约定

```c
struct free_area {
    struct page *head;
    unsigned long nr_free;   /* 块数，不是页数 */
};

struct buddy {
    struct page *pages;
    unsigned long nr_pages;
    unsigned int max_order;
    struct free_area free_area[MAX_ORDER];
    unsigned long free_count; /* 空闲页数，不是块数 */
    unsigned long split_count;
    unsigned long merge_count;
};
```

- 空闲块通过 `page->buddy_next` 串成单链表；
- `free_area[o].nr_free` 记录 order=o 的空闲块个数；
- `free_count` 记录所有空闲块的**页数之和**；
- `page->order` 对空闲块头表示块 order；
- 对已分配块，块首页 `page->order` 表示分配时的 order；
- `is_free` 只对空闲块头有意义；
- `pages` 数组由调用者提供，`buddy_init()` 不 malloc，`buddy_exit()` 也不 free。

要求 `nr_pages` 是 2 的幂，测试用 256 页、`max_order=8`。

## 需要实现的 TODO

### 1. `buddy_alloc(b, order)`

建议步骤：

1. 校验 `b`、`order <= b->max_order`；
2. 从 `cur = order` 向上找第一个 `free_area[cur].head != NULL`；
3. 从该链表摘掉块头；
4. 只要 `cur > order`：
   ```c
   buddy = page + (1UL << (cur - 1));
   cur--;
   ```
   - 低半继续作为分配候选；
   - 高半挂到 `free_area[cur]`；
   - 两半的 `order` 都设为 `cur`；
   - `split_count++`；
5. 最终 `page->order = order`、`refcount = 1`、`is_free = false`、`buddy_next = NULL`；
6. `free_count -= 1UL << order`；
7. 返回块首页。

### 2. `buddy_free(b, page, order)`

建议步骤：

1. 校验 `b`、`page` 在 `pages` 范围内；
2. 校验 `page - pages` 按 `1 << order` 对齐；
3. 校验 `order <= max_order`、`page->is_free == false`、`page->refcount == 1`、`page->order == order`；
4. 记录 `freed_order = order`；
5. 循环：
   ```c
   buddy_index = index ^ (1UL << order);
   ```
   - 如果 buddy 不在范围内；
   - 或 buddy 不是同 order 的空闲块头；
   - 则不能合并，退出；
   - 否则从 `free_area[order]` 摘掉 buddy，合并到低地址的一半，`order++`，`merge_count++`；
6. 把最终块挂回 `free_area[order]`；
7. `free_count += 1UL << freed_order`（注意加的是原始释放大小，不是合并后的块大小）。

### 3. 建议写的内部 helper

```c
static void free_list_push(struct buddy *b, struct page *p, unsigned order);
static struct page *free_list_pop(struct buddy *b, unsigned order);
static int free_list_remove(struct buddy *b, struct page *p, unsigned order);
```

如果 helper 暂时没被全部使用，先别写成未使用的 `static` 函数，否则 `-Werror` 会报 `-Wunused-function`。可以边实现边加。

## 10 个验收测试

| 测试 | 抓什么 |
|---|---|
| `init` | `max_order`、`free_area[8]`、`free_count` |
| `alloc order 0` | 分配首页、字段、计数、释放恢复 |
| `alloc order 2` | 4 页对齐、order、free_count |
| `split` | 从 order 8 split 到 order 2 后各级空闲块 |
| `merge` | 释放 order 2 后合并回 order 8 |
| `double free` | 第二次 free 返回错误，不重复计数 |
| `wrong order` | 分配 order 0 却按 order 1 释放，必须拒绝 |
| `exhaust` | 256 个 order 0 全部分配，再全部释放 |
| `invalid` | NULL/非 2 的幂/order 越界 |
| `random stress` | 2 万次随机 alloc/free，最终回到满空闲 |

脚手架阶段预期：

```text
[PASS] init
[FAIL] alloc order 0
...
[PASS] invalid
[FAIL] random stress

8 test(s) failed
```

## 运行

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm

make test        # M1，应该全绿
make test-m2     # M2，当前 8 个失败
make test-all    # 两个都跑
```

如果本机编译器临时目录受限：

```bash
TMPDIR=/tmp make test-m2
```

## 通过标准

```bash
make test-m2
```

目标：

```text
[PASS] init
[PASS] alloc order 0
...
[PASS] random stress

all tests passed
```

并且：

- `make test` 仍然全绿；
- ASan/UBSan 无报错；
- 每个测试结束前 `buddy_check()` == 0；
- 每次 split/merge 后 `free_count` 和 `free_area[].nr_free` 一致；
- 释放错 order、未对齐 page、double free 都返回 `-EINVAL`。

## 必须做的 bug 注入

实现通过后，逐个故意破坏并确认测试失败：

1. **不合并**：删掉 `buddy_free()` 里的 merge 循环 → `merge` 失败；
2. **合并错半区**：把 `buddy_index = index ^ (1 << order)` 改成固定 `index + (1 << order)` → `random stress`/`merge` 失败；
3. **split 挂错高半**：高半地址算错 → `alloc order 2`/`split` 失败；
4. **忘记减/加 free_count** → `alloc order 0`/`exhaust` 失败；
5. **不检查对齐**：删掉对齐检查 → `wrong order`/`invalid` 可能失败；
6. **不检查 double free**：删掉 `page->is_free`/`refcount` 检查 → `double free` 失败。

每次恢复代码再跑全绿。

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `struct free_area` | `include/linux/mmzone.h` |
| `free_area[order].free_list` | `include/linux/mmzone.h` |
| `__rmqueue_smallest()` | `mm/page_alloc.c`，从 small order 向上找 |
| `expand()` | `mm/page_alloc.c`，split 高半 |
| `__free_one_page()` | `mm/page_alloc.c`，释放并 merge |
| `buddy_index = index ^ (1 << order)` | `__find_buddy_pfn()` / `page_is_buddy()` |

对照时只看函数语义，不要抄实现；先自己把 split/merge 画出来。

## M2b：pcp（后续）

buddy 通过后，再加 per-CPU page cache：

```text
order 0 的分配/释放先走 pcp 链表
pcp 低于 low  → 从 buddy 批量补货
pcp 高于 high → 批量还给 buddy
```

这部分单独加测试，不要和 buddy 的 split/merge 混在同一个提交里。

## M2c：集成回 pmm

buddy 和 pcp 都稳定后，再改 M1 的 `pmm.c`：

- `pmm_init()` 初始化 buddy；
- `pmm_alloc_pages(order)` 走 buddy/pcp；
- `pmm_free_pages(page, order)` 走 buddy/pcp；
- `pmm_check()` 改成检查 buddy + pcp 状态。

这时 M1 的 `order unsupported` 测试要改成“支持 order > 0”，并保留回归测试。

## 完成后

1. 填 [../reports/M2.md](../reports/M2.md)；
2. 跑 `make test` + `make test-m2`；
3. 再进入 M2b（pcp）或 M3（页表/TLB）。
