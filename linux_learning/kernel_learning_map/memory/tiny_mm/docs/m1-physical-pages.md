# M1：物理地址模型

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m0-toolchain.md](m0-toolchain.md)
> 完整框架：[tiny_mm_learning_framework.md](tiny_mm_learning_framework.md)

## 目标

把“物理页、PFN、paddr、zone、pg_data、refcount”这几件事，用最小代码跑通：

```text
struct page ↔ PFN ↔ paddr
       │
       ├→ zone → pglist_data
       └→ refcount / free_count
```

M1 只做 order=0，buddy 留给 M2。

## 要改的文件

- 头文件：[../include/tmm.h](../include/tmm.h)
- 实现：[../src/pmm.c](../src/pmm.c)
- 测试：[../tests/test_pmm.c](../tests/test_pmm.c)
- 报告：[../reports/M1.md](../reports/M1.md)

## 已搭好的基础设施

这些函数已经实现，用来做初始化和自检：

- `pmm_init()` / `pmm_exit()`
- `pmm_check()`
- `page_zone()`
- `page_pgdat()`
- `page_to_nid()`
- `pfn_valid()`
- `pmm_free_count()` / `pmm_total_count()`

M1 的 page 数组、bitmap、zone、pglist_data 初始状态由 `pmm_init()` 建好。

## 需要实现的 TODO

| 函数 | 语义 |
|---|---|
| `page_to_pfn(page)` | `page - pages + start_pfn` |
| `pfn_to_page(pfn)` | 越界返回 `NULL` |
| `pfn_to_phys(pfn)` | `pfn << PAGE_SHIFT` |
| `phys_to_pfn(paddr)` | `paddr >> PAGE_SHIFT` |
| `pmm_alloc_pages(order)` | M1 只支持 order=0；扫描 bitmap、置位、refcount=1 |
| `pmm_free_pages(page, order)` | 检查重复释放/错 order；清位、refcount=0、free_count 恢复 |

## 关键约定

- `g_pmm.pages[i]` 对应 `PFN = start_pfn + i`；
- bitmap：`0` 空闲，`1` 已分配；
- 空闲页：`is_free = true`、`refcount = 0`、`order = 0`；
- 已分配页：`is_free = false`、`refcount = 1`、`order = 0`；
- `g_pmm.free_count` 与 `zone.free_count` 必须同步；
- `pmm_free_pages()` 失败时不能修改任何状态，尤其不能重复 `free_count++`。

## 11 个验收测试

| 测试 | 抓什么 |
|---|---|
| `init` | page 数组、bitmap、zone、node 初始状态 |
| `alloc/free` | refcount 1→0、is_free、free_count |
| `pfn/page roundtrip` | page↔PFN 可逆，越界 PFN 为 NULL |
| `pfn/paddr roundtrip` | `pfn << PAGE_SHIFT`、非页对齐向下取整 |
| `exhaust` | 分配耗尽返回 NULL；全部释放后计数恢复 |
| `double free` | 第二次 free 报错，且不重复计数 |
| `wrong order` | order=0 分配却按 order=1 释放，必须拒绝 |
| `order unsupported` | M1 的 order>0 返回 NULL |
| `zone` | `page_zone` 范围、`zone.free_count` 同步 |
| `pgdat` | `page_pgdat`/`zone->zone_pgdat`/node 范围 |
| `random stress` | 10 万次 alloc/free 后无泄漏、无状态损坏 |

## 验收

```bash
make test
```

目标：

```text
[PASS] init
[PASS] alloc/free
...
[PASS] random stress

all tests passed
```

并且：

- ASan/UBSan 无报错；
- 每个测试结束后 `pmm_check()` 都通过；
- 页必须来自 `g_pmm.pages`，不能 `malloc` 伪造；
- 随机压力结束后 `pmm_free_count() == TEST_NR_PAGES`；
- 错误路径不改变状态。

## 必须做的 4 个 bug 注入实验

实现通过后，逐个故意破坏，确认测试会失败：

1. 删掉 `pmm_free_pages()` 的 `page->is_free` 检查；
2. 删掉 `page->order != order` 检查；
3. 分配时删掉 `page->refcount = 1`；
4. 释放时只置 `is_free`，不增加 `free_count`。

每次预期对应测试失败，然后恢复代码再跑全绿。

## 对应 Linux 源码

| tiny-mm | Linux 5.15 |
|---|---|
| `struct page` | `include/linux/mm_types.h:70` |
| `page_to_pfn/pfn_to_page` | `include/asm-generic/memory_model.h:18` |
| `PFN_PHYS/PHYS_PFN` | `include/linux/pfn.h:21` |
| `pfn_valid` | `include/linux/mmzone.h:1469` |
| `page_zone` | `include/linux/mm.h:1539` |
| `page_pgdat` | `include/linux/mm.h:1544` |
| `struct pglist_data` | `include/linux/mmzone.h:800` |
| `zone->zone_pgdat` | `include/linux/mmzone.h:513` |

概念细节：

- [../../notes/内存管理核心数据结构与地址转换.md](../../notes/内存管理核心数据结构与地址转换.md) 的 3.4–3.6、4.5–4.8

## 完成后

1. 填 [../reports/M1.md](../reports/M1.md)；
2. 关闭 M0/M1 的 TODO；
3. 进入 M2：buddy + pcp（见完整框架）。
