# tiny-mm / M0 + M1

这是 [tiny-mm 学习框架](../tiny_mm_learning_framework.md) 的 M0/M1 实验脚手架。

- **M0**：工具链、`Makefile`、测试 runner、ASan/UBSan；
- **M1**：物理页模型、`struct page`、PFN/paddr、zone、refcount、double free/错 order 检查。

M1 的**核心函数故意留成 TODO**。现在 `make test` 会有失败，这是正常的练习入口；目标不是“跑通现成代码”，而是把这些语义自己写出来。

---

## 1. 当前目录

```text
tiny_mm/
├── Makefile
├── README.md
├── include/
│   └── tmm.h          # struct page / zone / API
├── src/
│   └── pmm.c          # M1 实现：pmm_init 已搭好，核心函数 TODO
├── tests/
│   ├── test_runner.h  # 极简测试 runner + CHECK 宏
│   └── test_pmm.c     # M1 的 11 个验收测试
└── reports/
    └── M1.md          # 阶段报告模板（填完再进 M2）
```

---

## 2. 先跑 M0

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm
make            # 编译
make test       # 跑测试（当前会失败，正常）
```

默认带 ASan/UBSan。如果你的环境没有 sanitizer：

```bash
make clean
make SAN= test
```

M0 的验收不是“测试全绿”，而是：

1. `make` 能编译；
2. `make test` 能执行并输出 PASS/FAIL；
3. ASan 能抓住一次真实的越界。

可以用下面这个小实验验证 ASan（做完删掉）：

```c
/* 临时加到某个测试里 */
int *p = malloc(sizeof(int));
p[1] = 42;   /* heap-buffer-overflow */
free(p);
```

如果 ASan 报 `heap-buffer-overflow`，M0 就通过了。

---

## 3. 当前预期结果

因为 M1 核心函数还是 TODO，`make test` 现在大概是：

```text
[PASS] init
[FAIL] alloc/free
[FAIL] pfn/page roundtrip
[FAIL] pfn/paddr roundtrip
[FAIL] exhaust
[FAIL] double free
[FAIL] wrong order
[FAIL] order unsupported
[FAIL] zone
[FAIL] pgdat
[FAIL] random stress
```

`init` 能过，是因为 `pmm_init()` 已经搭好，用来让你看到 page 数组、bitmap、zone、pglist_data 的初始状态。

---

## 4. M1 要实现的函数

打开 `src/pmm.c`，完成这些 TODO：

| 函数 | 语义 |
|---|---|
| `page_to_pfn(page)` | 由 `struct page *` 得到 PFN |
| `pfn_to_page(pfn)` | 由 PFN 得到 `struct page *`；越界返回 NULL |
| `pfn_to_phys(pfn)` | `pfn << PAGE_SHIFT` |
| `phys_to_pfn(paddr)` | `paddr >> PAGE_SHIFT` |
| `pmm_alloc_pages(order)` | M1 只支持 order=0；找空闲页、置位、refcount=1 |
| `pmm_free_pages(page, order)` | 检查是否重复释放/错 order；清位、refcount=0、free_count 恢复 |

`pmm_init()`、`pmm_exit()`、`pmm_check()`、`page_zone()` 已经实现，用来做基础设施和自检。

### 关键约定

- `g_pmm.pages[i]` 对应 `PFN = start_pfn + i`；
- bitmap 为 0 表示空闲，1 表示已分配；
- 空闲页：`is_free = true`，`refcount = 0`，`order = 0`；
- 已分配页：`is_free = false`，`refcount = 1`，`order = 0`；
- `free_count` 和 `zone.free_count` 必须同步；
- M1 只有一个 `pglist_data`（模拟 `CONFIG_NUMA=n`），`zone->zone_pgdat` 指向它；
- 释放失败时不要改任何状态，尤其不要重复 `free_count++`。

---

## 5. 11 个验收测试在测什么

| 测试 | 抓什么 |
|---|---|
| `init` | page 数组、bitmap、zone 初始状态是否一致 |
| `alloc/free` | refcount 1→0、is_free、free_count |
| `pfn/page roundtrip` | page↔PFN 是否可逆，越界 PFN 是否 NULL |
| `pfn/paddr roundtrip` | `PFN_PHYS/PHYS_PFN` 语义、非页对齐向下取整 |
| `exhaust` | 分配完返回 NULL；全部释放后计数恢复 |
| `double free` | 第二次 free 必须报错，且不能重复计数 |
| `wrong order` | 分配 order=0 却按 order=1 释放，必须拒绝 |
| `order unsupported` | M1 的 order>0 必须返回 NULL，不能假装分配 |
| `zone` | `page_zone` 范围、`zone.free_count` 同步 |
| `pgdat` | `page_pgdat`/`zone->zone_pgdat`/`node_id`/node 范围 |
| `random stress` | 10 万次随机 alloc/free 后无泄漏、无状态损坏 |

---

## 6. 通过标准

```text
make test
...
all tests passed
```

并且：

1. ASan/UBSan 没有报错；
2. `pmm_check()` 在每个测试后都通过；
3. 没有用 `malloc` 伪造页——页必须来自 `g_pmm.pages`；
4. `pmm_free_pages()` 的错误路径不修改状态；
5. 随机压力测试结束后 `pmm_free_count() == TEST_NR_PAGES`。

---

## 7. 必须做的 bug 注入练习

实现通过后，不要停在“绿了”。依次做下面 4 个实验，确认测试真的在保护你：

### 实验 A：删掉 double free 检查

把 `pmm_free_pages()` 里对 `page->is_free` 的检查删掉，重新 `make test`。

预期：`double free` 测试失败。

### 实验 B：删掉 order 检查

把对 `page->order != order` 的检查删掉，重新跑。

预期：`wrong order` 测试失败。

### 实验 C：忘记 refcount

分配时把 `refcount = 1` 删掉，重新跑。

预期：`alloc/free` 或 `pmm_check` 失败。

### 实验 D：忘记同步 free_count

释放时只置 `is_free`，不增加 `free_count`，重新跑。

预期：`exhaust` 或 `pmm_check` 失败。

做完后恢复代码，再跑一遍全绿。这个过程比“看一眼正确实现”有价值得多。

---

## 8. 对应 Linux 源码

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
| `zone` 范围 | `include/linux/mmzone.h:532` |

概念细节看：[内存管理核心数据结构与地址转换.md](../内存管理核心数据结构与地址转换.md) 的 3.4–3.6、4.5–4.8。

---

## 9. 完成后

M1 全绿后，进入 M2：

- 用 buddy 替换 bitmap；
- 支持 order 0..MAX_ORDER-1；
- 实现 split/merge、pcp、watermark、migratetype；
- 把 M1 的 `order unsupported` 测试改成 buddy 的 order 测试。

框架见：[tiny_mm_learning_framework.md](../tiny_mm_learning_framework.md)。
