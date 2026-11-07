# tiny-mm

这是一个用户态 C 项目，用自己实现的“小内存管理器”把 Linux 内存管理的主线串起来：

```text
mm_struct → VMA → 页表/PTE → PFN → struct page
                                  ├→ paddr
                                  ├→ zone → pg_data_t
                                  └→ rmap → 所有 VMA
```

项目按**阶段文档 + 模块代码**组织：

- **代码路径固定**：`include/`、`src/`、`tests/`；
- **每个阶段只加文档和验收**：`docs/m0-*.md`、`docs/m1-*.md`；
- 不按 M0/M1 复制源码，避免后面 M2/M3 出现多份 `pmm.c`。

---

## 当前进度

| 阶段 | 状态 | 入口 |
|---|---|---|
| M0 工具链/测试框架 | ✅ 已搭好 | [docs/m0-toolchain.md](docs/m0-toolchain.md) |
| M1 物理页模型 | ✅ 已完成（56a96a7） | [docs/m1-physical-pages.md](docs/m1-physical-pages.md) |
| M2 buddy/pcp | 未开始 | 框架里的 M2 |
| M3 页表/TLB | 未开始 | 框架里的 M3 |

> M1 已完成：`make test` 11 个测试全部通过。M2 尚未开始。

---

## 目录

```text
tiny_mm/
├── README.md                       # 本页：总入口
├── docs/
│   ├── m0-toolchain.md             # M0：编译、测试、ASan
│   ├── m1-physical-pages.md        # M1：page/PFN/paddr/zone/pg_data
│   ├── tiny_mm_learning_framework.md # M0–M9 完整框架
│   └── memory_capstone_tiny_mm.md  # 最初的项目方案
├── include/
│   └── tmm.h                       # 数据结构与 API
├── src/
│   └── pmm.c                       # 物理页实现（M1 已完成）
├── tests/
│   ├── test_runner.h               # 测试 runner
│   └── test_pmm.c                  # M1 的 11 个验收测试
├── reports/
│   └── M1.md                       # M1 阶段报告模板
├── Makefile
└── .gitignore
```

---

## 快速开始

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm
make
make test
```

默认带 ASan/UBSan。没有 sanitizer 的环境：

```bash
make clean
make SAN= test
```

M1 完成后的预期输出：

```text
[PASS] init
[PASS] alloc/free
...
[PASS] random stress

all tests passed
```

---

## M1 已完成的实现

`src/pmm.c` 中这些函数已经实现：

- `page_to_pfn()` / `pfn_to_page()`
- `pfn_to_phys()` / `phys_to_pfn()`
- `pmm_alloc_pages()`（M1 只支持 order=0）
- `pmm_free_pages()`

`pmm_init()`、`pmm_exit()`、`pmm_check()`、`page_zone()`、`page_pgdat()`、`pfn_valid()` 已经搭好。

详细说明、实现提示、11 个测试和验收标准：

- [docs/m1-physical-pages.md](docs/m1-physical-pages.md)

M1 完成后：

```text
make test
all tests passed
```

然后做 4 个 bug 注入实验，填 [reports/M1.md](reports/M1.md)，再进 M2。

---

## 文档入口

- [M0：工具链与测试框架](docs/m0-toolchain.md)
- [M1：物理地址模型](docs/m1-physical-pages.md)
- [tiny-mm 完整学习框架](docs/tiny_mm_learning_framework.md)
- [最初的项目方案](docs/memory_capstone_tiny_mm.md)

## 规则

1. 先写测试，再写实现；
2. 每实现一个模块，主动注入一个 bug，确认测试能抓住；
3. 不复制 Linux 源码，只对照函数语义；
4. 每完成一个阶段写一份 `reports/Mx.md`；
5. 最后闭卷从 `mmap` 画到 `page`。

> 代码按模块分，阶段按文档分。M2 之后也保持这个原则。
