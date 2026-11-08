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
| M2 buddy/pcp | 🚧 脚手架已搭好（buddy TODO） | [docs/m2-buddy.md](docs/m2-buddy.md) |
| M3 页表/TLB | 未开始 | 框架里的 M3 |

> M1 已完成：`make test` 11 个测试全部通过。
>
> M2 脚手架已搭好：`make test-m2` 目前是 8 个失败（等 `buddy_alloc`/`buddy_free`）。

---

## 目录

```text
tiny_mm/
├── README.md                       # 本页：总入口
├── docs/
│   ├── m0-toolchain.md             # M0：编译、测试、ASan
│   ├── m1-physical-pages.md        # M1：page/PFN/paddr/zone/pg_data
│   ├── m2-buddy.md                 # M2：buddy split/merge、10 个测试
│   ├── tiny_mm_learning_framework.md # M0–M9 完整框架
│   └── memory_capstone_tiny_mm.md  # 最初的项目方案
├── include/
│   ├── tmm.h                       # 数据结构与 API
│   └── buddy.h                     # M2 buddy 数据结构/API
├── src/
│   ├── pmm.c                       # 物理页实现（M1 已完成）
│   └── buddy.c                     # M2 buddy（核心 TODO）
├── tests/
│   ├── test_runner.h               # M1 测试 runner
│   ├── test_pmm.c                  # M1 的 11 个验收测试
│   ├── test_runner_buddy.h         # M2 测试 runner
│   └── test_buddy.c                # M2 的 10 个验收测试
├── reports/
│   ├── M1.md                       # M1 阶段报告模板
│   └── M2.md                       # M2 阶段报告模板
├── Makefile
└── .gitignore
```

---

## 快速开始

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm
make
make test        # M1，11/11 通过
make test-m2     # M2，脚手架阶段 8 个失败
make test-all    # 两个都跑
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

然后做 4 个 bug 注入实验，填 [reports/M1.md](reports/M1.md)。

---

## M2 脚手架：buddy

M2 先做成独立模块，不改 M1 的 `pmm.c`：

- `include/buddy.h`：`free_area`、`struct buddy`、API；
- `src/buddy.c`：`buddy_init()`/`buddy_check()` 已实现，`buddy_alloc()`/`buddy_free()` 是 TODO；
- `tests/test_buddy.c`：10 个测试，脚手架阶段 `init` 和 `invalid` 通过，其余 8 个失败。

运行：

```bash
make test-m2
```

预期：

```text
[PASS] init
[FAIL] alloc order 0
...
[FAIL] random stress

8 test(s) failed
```

详细 split/merge 步骤、测试表、bug 注入和 Linux 对照：

- [docs/m2-buddy.md](docs/m2-buddy.md)

M2 通过后填 [reports/M2.md](reports/M2.md)，再加 pcp（M2b）。

---

## 文档入口

- [M0：工具链与测试框架](docs/m0-toolchain.md)
- [M1：物理地址模型](docs/m1-physical-pages.md)
- [M2：buddy 分配器](docs/m2-buddy.md)
- [tiny-mm 完整学习框架](docs/tiny_mm_learning_framework.md)
- [最初的项目方案](docs/memory_capstone_tiny_mm.md)

## 规则

1. 先写测试，再写实现；
2. 每实现一个模块，主动注入一个 bug，确认测试能抓住；
3. 不复制 Linux 源码，只对照函数语义；
4. 每完成一个阶段写一份 `reports/Mx.md`；
5. 最后闭卷从 `mmap` 画到 `page`。

> 代码按模块分，阶段按文档分。M2 之后也保持这个原则。
