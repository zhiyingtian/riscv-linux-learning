# M0：工具链与测试框架

> 项目总入口：[../README.md](../README.md)
> 下一阶段：[m1-physical-pages.md](m1-physical-pages.md)
>
> 当前状态：M0 已通过；M1 已完成，`make test` 应全部通过。本文保留为 M0 的验收/复习指南。

## 目标

M0 不写内存管理逻辑，只把“能编译、能跑测试、能抓内存错误”的地基搭好：

- `Makefile`；
- 极简 PASS/FAIL runner；
- ASan/UBSan；
- 统一目录约定；
- 先失败、再实现的测试节奏。

## 文件

| 文件 | 作用 |
|---|---|
| [../Makefile](../Makefile) | 编译、链接、跑测试 |
| [../tests/test_runner.h](../tests/test_runner.h) | `CHECK` 宏、`run_test()` |
| [../tests/test_pmm.c](../tests/test_pmm.c) | M1 验收测试 |
| [../.gitignore](../.gitignore) | 忽略 `build/` |

## 命令

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm
make            # 编译
make test       # 跑测试
make clean      # 清理
```

默认编译选项：

```text
-std=c11 -Wall -Wextra -Werror -Wpedantic -g -O0
-fsanitize=address,undefined -fno-omit-frame-pointer
```

没有 sanitizer 的环境：

```bash
make clean
make SAN= test
```

## 验收

### 1. 能编译

```bash
make
```

要求：无 warning、无 error。

### 2. 能跑测试

```bash
make test
```

当前因为 M1 还是 TODO，预期是：

```text
[PASS] init
[FAIL] alloc/free
...
10 test(s) failed
```

这不算 M0 失败；M0 验收的是“测试框架能执行、能报告、失败时能清理”。

### 3. ASan 能抓越界

临时在任意测试里加：

```c
int *p = malloc(sizeof(int));
p[1] = 42;   /* heap-buffer-overflow */
free(p);
```

重新 `make test`，应看到：

```text
ERROR: AddressSanitizer: heap-buffer-overflow
```

看到后删掉这段代码。

### 4. 测试真的会失败

把 `tests/test_pmm.c` 里任意一个 `CHECK(...)` 改成 `CHECK(0)`，重新跑：

- 该测试必须显示 `FAIL`；
- 不能“全绿但没测到”。

## 通过标准

- [ ] `make` 成功；
- [ ] `make test` 能执行并输出 PASS/FAIL；
- [ ] ASan 能抓一次越界；
- [ ] 故意改坏一个断言后，测试会失败；
- [ ] 知道 `build/` 是生成物，不进 git。

## 完成后

进入 [M1：物理地址模型](m1-physical-pages.md)。
