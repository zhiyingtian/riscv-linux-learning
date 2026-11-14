# M2b：per-CPU page cache（pcp）

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m2-buddy.md](m2-buddy.md)
> 完整框架：[tiny_mm_learning_framework.md](tiny_mm_learning_framework.md)
>
> **状态：🚧 脚手架已搭好；`pcp_alloc_pages()` / `pcp_free_pages()` / `pcp_drain()` 还是 TODO。**

## 目标

M2a 的 buddy 已经能按 order 分配/合并，但每次 order-0 分配都去改 `free_area` 链表太“重”。

pcp 在 buddy 上加一层 per-CPU 小缓存：

```text
order 0 alloc:
    pcp 链表有空闲页 → 直接返回
    pcp 为空          → 从 buddy 批量 refill（batch 个）
    buddy 也没有      → NULL

order 0 free:
    page 放回 pcp 链表
    pcp 页数 >= high  → 批量 drain 回 buddy

order > 0:
    绕过 pcp，直接走 buddy
```

M2b 先模拟：

- 单 CPU；
- 单 zone；
- 只缓存 order 0；
- 用 `page->buddy_next` 串 pcp 链表；
- high/batch 固定参数。

## 文件

| 文件 | 作用 |
|---|---|
| [../include/pcp.h](../include/pcp.h) | `struct pcp`、API |
| [../src/pcp.c](../src/pcp.c) | 初始化/自检已实现，核心函数 TODO |
| [../tests/test_pcp.c](../tests/test_pcp.c) | 9 个验收测试 |
| [../tests/test_runner_pcp.h](../tests/test_runner_pcp.h) | M2b 测试 runner |

pcp 不拥有页数组，也不拥有 buddy：

```c
struct pcp {
    struct buddy *buddy;  /* 指向 M2a 的 buddy */
    struct page *list;    /* order-0 pcp 空闲链表 */
    ...
};
```

## 状态约定

pcp 里的页对 buddy 来说仍然是“已分配”，不在 `free_area` 中。

| 位置 | `is_free` | `refcount` | `order` | `buddy_next` |
|---|---|---|---|---|
| buddy 空闲块头 | true | 0 | 当前 order | buddy 空闲链表 |
| pcp 缓存页 | true | 0 | 0 | pcp 链表 |
| 已分配页 | false | 1 | 分配 order | NULL |

所以：

- 从 buddy refill 到 pcp 时，要把页从“已分配”改成“pcp 缓存”状态；
- 从 pcp 取给调用者时，要恢复成“已分配”状态；
- drain 回 buddy 前，要恢复成 `is_free=false`、`refcount=1`、`order=0`；
- `pcp_check()` 只检查 pcp 链表和 `buddy_check()`，不把 pcp 页算进 buddy 空闲页。

## 需要实现的 TODO

### 1. `pcp_alloc_pages(p, order)`

```text
order > 0:
    return buddy_alloc(p->buddy, order);

order == 0:
    if (p->list) {
        page = p->list;
        p->list = page->buddy_next;
        p->count--;
        page->is_free = false;
        page->refcount = 1;
        page->buddy_next = NULL;
        p->alloc_hits++;
        return page;
    }

    p->alloc_refills++;
    for (i = 0; i < p->batch; i++) {
        page = buddy_alloc(p->buddy, 0);
        if (!page) break;
        page->is_free = true;
        page->refcount = 0;
        page->buddy_next = p->list;
        p->list = page;
        p->count++;
    }

    if (!p->list) {
        p->alloc_misses++;
        return NULL;
    }
    return pcp_alloc_pages(p, 0);  // 或直接摘一页，注意不要再 refill
```

注意：refill 后不要重复累加 `alloc_refills`。

### 2. `pcp_free_pages(p, page, order)`

```text
order > 0:
    return buddy_free(p->buddy, page, order);

order == 0:
    校验 page 非空、在 buddy->pages 范围内、
    page->order == 0、!page->is_free、refcount == 1；

    page->is_free = true;
    page->refcount = 0;
    page->order = 0;
    page->buddy_next = p->list;
    p->list = page;
    p->count++;
    p->free_count++;

    while (p->count >= p->high)
        pcp_drain(p, min(p->batch, p->count));

    return 0;
```

注意 double free：第二次调用时 `page->is_free == true`，必须返回 `-EINVAL`。

### 3. `pcp_drain(p, nr)`

```text
实际 drain 数 = min(nr, p->count)
循环：
    page = p->list;
    p->list = page->buddy_next;
    p->count--;

    /* 恢复成 buddy 眼里的“已分配页” */
    page->is_free = false;
    page->refcount = 1;
    page->order = 0;
    page->buddy_next = NULL;

    buddy_free(p->buddy, page, 0);
    p->drain_count++;
返回实际 drain 数
```

## 9 个验收测试

| 测试 | 抓什么 |
|---|---|
| `init` | high/batch/count、空链表、buddy_check |
| `refill` | pcp 空时从 buddy 批量取页 |
| `pcp hit` | 第二次分配不再动 buddy |
| `free to pcp` | 释放进缓存、double free 被拒绝 |
| `high drain` | count 达到 high 后批量还页 |
| `order > 0 bypass` | order 1 直接走 buddy，不进 pcp |
| `explicit drain` | `pcp_drain()` 指定数量 |
| `invalid` | NULL、非法 high/batch、未分配页 |
| `random stress` | 2 万次随机 alloc/free，最终全部还给 buddy |

脚手架阶段预期：

```text
[PASS] init
[FAIL] refill
...
[PASS] invalid
[FAIL] random stress

7 test(s) failed
```

## 运行

```bash
cd linux_learning/kernel_learning_map/memory/tiny_mm

make test        # M1
make test-m2     # M2a buddy
make test-m2b    # M2b pcp
make test-all    # 全部
```

如果编译器临时目录受限：

```bash
TMPDIR=/tmp make test-m2b
```

## 通过标准

```bash
make test-m2b
```

目标：

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

并且：

- `make test` / `make test-m2` 继续全绿；
- ASan/UBSan 无报错；
- 每次操作后 `pcp_check()` == 0；
- pcp 里的页数永远不会无限超过 `high`；
- 最终 `pcp_drain_all()` 后 `buddy_free_count == 256`。

## 必须做的 bug 注入

1. refill 后不更新 `count` → `refill`/`high drain` 失败；
2. pcp 命中时不摘链表头 → `pcp hit`/`random` 失败；
3. free 时不检查 double free → `free to pcp` 失败；
4. high 触发后不 drain → `high drain` 失败；
5. drain 前不把 page 恢复成 `refcount=1`/`is_free=false` → `buddy_free()` 拒绝；
6. order > 0 也走 pcp → `order > 0 bypass` 失败。

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `struct pcp` | `struct per_cpu_pages`（`include/linux/mmzone.h:360`） |
| `pcp->high` / `batch` | `per_cpu_pages.high` / `batch` |
| `pcp->list` | `per_cpu_pages.lists[NR_PCP_LISTS]` 中对应 order/migratetype 的链表 |
| `pcp_alloc_pages()` | `rmqueue_pcplist()` → `__rmqueue_pcplist()` |
| `pcp_free_pages()` | `free_unref_page_commit()` |
| `pcp_drain()` | `free_pcppages_bulk()` |
| `order > 0 bypass` | `pcp_allowed_order()` / `rmqueue()` |
| `order_to_pindex()` | `mm/page_alloc.c:659` |

Linux 的 pcp 还包含：

- NUMA 远端页过期；
- migratetype；
- free_factor 动态 batch；
- 水位/回收状态影响 high。

M2b 先只保留最核心的 refill/hit/free/drain。

## 完成后

1. 填 [../reports/M2b.md](../reports/M2b.md)；
2. 跑 `make test` + `make test-m2` + `make test-m2b`；
3. 再进入 M2c：把 `pmm.c` 的 bitmap 换成 buddy + pcp。
