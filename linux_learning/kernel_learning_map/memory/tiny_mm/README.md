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
| M2 buddy/pcp | ✅ M2a 完成（11/11）；✅ M2b 完成（9/9）；🚧 M2c 集成脚手架 | [docs/m2-buddy.md](docs/m2-buddy.md) · [docs/m2b-pcp.md](docs/m2b-pcp.md) · [docs/m2c-pmm-integration.md](docs/m2c-pmm-integration.md) |
| M3 页表/TLB | 未开始 | 框架里的 M3 |

> M1 已完成（历史）：原 `make test` 的 11 个 bitmap 测试已由 M2c 验收替代。
>
> M2a buddy 已完成：`make test-m2` 11 个测试全部通过。
>
> M2b pcp 已完成：`make test-m2b` 9 个测试全部通过。
>
> M2c pmm 集成脚手架已搭好：`pmm_init()`/`pmm_check()` 已完成，alloc/free 是 TODO；当前 `make test-m2c` 3 个通过、8 个失败。

---

## 目录

```text
tiny_mm/
├── README.md                       # 本页：总入口
├── docs/
│   ├── m0-toolchain.md             # M0：编译、测试、ASan
│   ├── m1-physical-pages.md        # M1：page/PFN/paddr/zone/pg_data
│   ├── m2-buddy.md                 # M2a：buddy split/merge、11 个测试
│   ├── m2b-pcp.md                  # M2b：pcp refill/high/drain、9 个测试
│   ├── m2c-pmm-integration.md      # M2c：pmm 接入 buddy + pcp
│   ├── tiny_mm_learning_framework.md # M0–M9 完整框架
│   └── memory_capstone_tiny_mm.md  # 最初的项目方案
├── include/
│   ├── tmm.h                       # 数据结构与 API
│   ├── buddy.h                     # M2a buddy 数据结构/API
│   └── pcp.h                       # M2b pcp 数据结构/API
├── src/
│   ├── pmm.c                       # M2c：buddy + pcp 集成（alloc/free TODO）
│   ├── buddy.c                     # M2a buddy（已完成）
│   └── pcp.c                       # M2b pcp（已完成）
├── tests/
│   ├── test_runner.h               # M1 测试 runner
│   ├── test_pmm.c                  # M2c 的 11 个验收测试
│   ├── test_runner_buddy.h         # M2a 测试 runner
│   ├── test_buddy.c                # M2a 的 11 个验收测试
│   ├── test_runner_pcp.h           # M2b 测试 runner
│   └── test_pcp.c                  # M2b 的 9 个验收测试
├── reports/
│   ├── M1.md                       # M1 阶段报告模板
│   ├── M2.md                       # M2a 阶段报告
│   ├── M2b.md                      # M2b 阶段报告
│   └── M2c.md                      # M2c 阶段报告脚手架
├── Makefile
└── .gitignore
```

---

## 快速开始

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm
make
make test        # M2c，脚手架 3/11 通过
make test-m2c    # 同上，M2c 集成
make test-m2     # M2a，11/11 通过
make test-m2b    # M2b，9/9 通过
make test-all    # M2c + M2a + M2b
```

默认带 ASan/UBSan。没有 sanitizer 的环境：

```bash
make clean
make SAN= test
```

M2c 脚手架当前的预期输出：

```text
[PASS] init
[FAIL] alloc/free
[PASS] pfn/page roundtrip
[PASS] pfn/paddr roundtrip
...
8 test(s) failed
```

实现 `pmm_alloc_pages()`/`pmm_free_pages()` 后应变成 11/11 全绿。

---

## VS Code 调试

仓库根目录的 `.vscode/launch.json` 已经加了 tiny-mm 的 gdb 配置：

- `tiny-mm: debug M2b pcp`
- `tiny-mm: debug M2 buddy`
- `tiny-mm: debug M2c pmm`（原 M1 调试配置）

使用方式：

1. 安装 VS Code 的 **C/C++** 扩展；
2. 打开 `src/buddy.c`，在 `buddy_alloc()` / `buddy_free()` 里打断点；
3. 按 `F5`，选择 `tiny-mm: debug M2b pcp`、`tiny-mm: debug M2 buddy` 或 `tiny-mm: debug M2c pmm`；
4. 启动前会自动执行对应的 debug 构建任务。

调试构建使用独立的 `build-debug/`，并且**关闭 ASan/UBSan**，避免 sanitizer 干扰 gdb：

```text
CFLAGS  = -O0 -g
SAN     =
program = build-debug/test_buddy
```

手动构建：

```bash
make debug-m2b
# 或
make debug-m2
# 或
make debug-m2c
```

普通的 `make test` / `make test-m2` 仍然使用 `build/` 和 sanitizer，两者互不覆盖。

如果 `gdb` 不在 `/usr/bin/gdb`，修改 `.vscode/launch.json` 里的 `miDebuggerPath`。

---

## M1：物理页模型（历史阶段）

M1 已完成并保留说明；当前 `src/pmm.c` 已经进入 M2c，`make test` 现在跑的是 M2c 验收，不再是 M1 bitmap 版本。

M1 负责：

- `page_to_pfn()` / `pfn_to_page()`
- `pfn_to_phys()` / `phys_to_pfn()`
- zone/pgdat/page_to_nid
- 11 个物理页模型验收测试

M1 的历史说明和 bitmap 版本语义：

- [docs/m1-physical-pages.md](docs/m1-physical-pages.md)
- 历史报告：[reports/M1.md](reports/M1.md)

M2c 会把测试里的 order-0 free 断言从 `is_free=true` 改成 `PAGE_IN_PCP`。

---

## M2a：buddy 已完成

M2a 当时先做成独立模块，不改 M1 的 `pmm.c`（M2c 才正式接入 pmm）：

- `include/buddy.h`：`free_area`、`struct buddy`、API；
- `src/buddy.c`：`buddy_init()`/`buddy_check()`/`buddy_alloc()`/`buddy_free()` 已实现；
- `tests/test_buddy.c`：11 个测试，当前全部通过。

运行：

```bash
make test-m2
```

预期：

```text
[PASS] init
[PASS] alloc order 0
...
[PASS] random stress

all tests passed
```

详细 split/merge 步骤、测试表、bug 注入和 Linux 对照：

- [docs/m2-buddy.md](docs/m2-buddy.md)

填 [reports/M2.md](reports/M2.md)，再做 M2b 的 pcp。

---

## M2b：pcp 已完成

M2b 在 buddy 上加一层 order-0 的 per-CPU page cache：

- `include/pcp.h`：`struct pcp`、API；
- `src/pcp.c`：refill/hit/free/drain 已实现；
- `tests/test_pcp.c`：9 个测试，当前全部通过。

运行：

```bash
make test-m2b
```

预期：

```text
[PASS] init
[PASS] refill
[PASS] pcp hit
[PASS] free to pcp
[PASS] high drain
[PASS] order > 0 bypass
[PASS] explicit drain
[PASS] invalid
[PASS] random stress

all tests passed
```

详细 refill/high/drain 步骤、状态模型、测试表、bug 注入和 Linux 对照：

- [docs/m2b-pcp.md](docs/m2b-pcp.md)

M2b 已完成，报告见 [reports/M2b.md](reports/M2b.md)。

---

## M2c：pmm 集成（脚手架）

M2c 把 M1 的 bitmap 分配器切到 buddy + pcp：

- `pmm_init()`：初始化 buddy + pcp（已实现）；
- `pmm_check()`：检查 buddy + pcp + free_count（已实现）；
- `pmm_alloc_pages()` / `pmm_free_pages()`：TODO；
- order 0 走 pcp，order > 0 走 buddy；
- `pmm.free_count = buddy.free_count + pcp.count`。

运行：

```bash
make test-m2c
```

当前预期：

```text
[PASS] init
[FAIL] alloc/free
[PASS] pfn/page roundtrip
[PASS] pfn/paddr roundtrip
[FAIL] exhaust
[FAIL] double free
[FAIL] wrong order
[FAIL] order supported
[FAIL] zone
[FAIL] pgdat
[FAIL] random stress

8 test(s) failed
```

实现说明、状态语义、验收目标：

- [docs/m2c-pmm-integration.md](docs/m2c-pmm-integration.md)

完成后填 [reports/M2c.md](reports/M2c.md)，再考虑水位/回收标记和 M3。

---

## 文档入口

- [M0：工具链与测试框架](docs/m0-toolchain.md)
- [M1：物理地址模型](docs/m1-physical-pages.md)
- [M2a：buddy 分配器](docs/m2-buddy.md)
- [M2b：pcp](docs/m2b-pcp.md)
- [M2c：pmm 集成](docs/m2c-pmm-integration.md)
- [tiny-mm 完整学习框架](docs/tiny_mm_learning_framework.md)
- [最初的项目方案](docs/memory_capstone_tiny_mm.md)

## 规则

1. 先写测试，再写实现；
2. 每实现一个模块，主动注入一个 bug，确认测试能抓住；
3. 不复制 Linux 源码，只对照函数语义；
4. 每完成一个阶段写一份 `reports/Mx.md`；
5. 最后闭卷从 `mmap` 画到 `page`。

> 代码按模块分，阶段按文档分。M2 之后也保持这个原则。
