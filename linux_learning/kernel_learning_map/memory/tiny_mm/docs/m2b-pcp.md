# M2b：per-CPU page cache（pcp）

> 项目总入口：[../README.md](../README.md)
> 上一阶段：[m2-buddy.md](m2-buddy.md)
> 下一阶段：M2c，把 pmm 的 bitmap 换成 buddy + pcp
> 完整框架：[tiny_mm_learning_framework.md](tiny_mm_learning_framework.md)
>
> **状态：✅ M2b 已完成（`make test-m2b` 9/9 pass）。**

## 目标

M2a 的 buddy 已经能按 order 分配/合并，但每次 order-0 分配都要改 `free_area` 链表。

pcp 在 buddy 上加一层 order-0 小缓存：

```text
order 0 alloc:
    pcp 链表有页 → 直接返回
    pcp 为空     → 从 buddy 批量 refill（batch 个）
    buddy 也没有 → NULL

order 0 free:
    page 放回 pcp 链表
    pcp 页数 >= high → 批量 drain 回 buddy

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
| [../include/pcp.h](../include/pcp.h) | `struct pcp`、API、字段语义 |
| [../src/pcp.c](../src/pcp.c) | pcp 实现 |
| [../tests/test_pcp.c](../tests/test_pcp.c) | 9 个验收测试 |
| [../tests/test_runner_pcp.h](../tests/test_runner_pcp.h) | M2b 测试 runner |

pcp 不拥有页数组，也不拥有 buddy：

```c
struct pcp {
    struct buddy *buddy;  /* 指向 M2a 的 buddy */
    struct page *list;    /* order-0 pcp 空闲链表 */
    unsigned int count;   /* 当前 pcp 里的页数 */
    ...
};
```

## 数据结构

```c
struct pcp {
    struct buddy *buddy;
    struct page *list;
    unsigned int count;   /* 当前 pcp 缓存页数，不是块数 */
    unsigned int high;    /* count >= high 触发 drain */
    unsigned int batch;   /* 每次 refill/drain 的批量 */

    unsigned long alloc_hits;    /* 从 pcp 命中的次数 */
    unsigned long alloc_refills; /* pcp 为空、refill 的次数 */
    unsigned long alloc_misses;  /* pcp 和 buddy 都拿不到页的次数 */
    unsigned long nr_put;        /* 累计放进 pcp 的页数（refill + free） */
    unsigned long drain_count;   /* 从 pcp 还给 buddy 的页数 */
};
```

### `count` 和 `nr_put`

- `count`：当前 pcp 缓存占用，必须等于链表长度；
- `nr_put`：累计统计，只增不减，记录“放进 pcp 的页数”。

不要用 `nr_put` 做 high/drain 判断。

`buddy.free_count` 又是另一个含义：buddy 当前空闲页数。
三者不要混：

```text
pcp.count     当前 pcp 缓存页数
pcp.nr_put    累计放进 pcp 的页数
buddy.free_count  当前 buddy 空闲页数
```

## 状态约定

pcp 页不在 buddy 的 `free_area` 里，所以对 buddy 来说仍是“已分配”。

| 位置 | `is_free` | `refcount` | `order` | `state` |
|---|---|---|---|---|
| buddy 空闲块头 | true | 0 | 当前 order | `PAGE_IN_BUDDY` |
| pcp 缓存页 | false | 0 | 0 | `PAGE_IN_PCP` |
| 已分配页 | false | 1 | 分配 order | `PAGE_ALLOCATED` |

关键规则：

- pcp 缓存页的 `is_free` 必须是 **false**；
- 不能用 `is_free=true` 表示 pcp 缓存，否则 `buddy_free()` 会误以为 buddy 在 `free_area` 里；
- `pcp_check()` 要求 pcp 链表里的每个页：
  - `is_free == false`
  - `refcount == 0`
  - `order == 0`
  - `state == PAGE_IN_PCP`

## 实现

### 1. `pcp_pop()`

从 pcp 链表摘一页，并恢复成已分配页：

```c
static struct page *pcp_pop(struct pcp *p)
{
    struct page *page = p->list;

    if (!page)
        return NULL;

    p->list = page->buddy_next;
    page->buddy_next = NULL;
    page->is_free = false;
    page->refcount = 1;
    page->state = PAGE_ALLOCATED;
    p->count--;
    p->alloc_hits++;

    return page;
}
```

### 2. `pcp_alloc_pages(p, order)`

```text
order > 0:
    return buddy_alloc(p->buddy, order);   // 完全绕过 pcp

order == 0:
    1. page = pcp_pop(p);
       如果非空，直接返回；
    2. pcp 为空，且 buddy->free_count > 0：
       从 buddy 批量 refill 最多 batch 页；
       每页转成 PAGE_IN_PCP/refcount=0/is_free=false；
       再 pcp_pop() 一页返回；
    3. 一页也拿不到：
       alloc_misses++，返回 NULL。
```

注意：

- refill 的页来自 buddy，不是用户 free，但会累加 `nr_put`；`nr_put` 只做统计，不参与 high/drain 判断；
- order > 0 返回的页保持 `PAGE_ALLOCATED/refcount=1`，不要标成 PCP。

### 3. `pcp_free_pages(p, page, order)`

```text
order > 0:
    return buddy_free(p->buddy, page, order);

order == 0:
    校验 page：
        !is_free
        refcount == 1
        order == 0
        state == PAGE_ALLOCATED

    转成 pcp 缓存页：
        is_free = false
        refcount = 0
        state = PAGE_IN_PCP
        挂到 pcp 链表头
        count++，nr_put++

    if (count >= high)
        pcp_drain(p, batch);
```

double free 校验：

- 第二次 free 时 `refcount != 1`，必须返回 `-EINVAL`。

### 4. `pcp_drain(p, nr)`

```text
实际 drain 数 = min(nr, count)

循环：
    page = p->list;
    p->list = page->buddy_next;
    page->buddy_next = NULL;

    /* pcp 缓存页 → buddy 眼中的已分配页 */
    page->is_free = false;
    page->refcount = 1;
    page->state = PAGE_ALLOCATED;

    if (buddy_free(p->buddy, page, 0) == 0) {
        count--;
        drain_count++;
        drained++;
    } else {
        /* 理论上不会失败；恢复成 PCP 后放回链表 */
        page->is_free = false;
        page->refcount = 0;
        page->state = PAGE_IN_PCP;
        page->buddy_next = p->list;
        p->list = page;
        break;
    }

返回 drained;
```

关键：**先把 pcp 页转成 `PAGE_ALLOCATED`，再调用 `buddy_free()`**。
这样 `buddy_free()` 不需要认识 pcp 状态。

### 5. `pcp_check(p)`

检查：

1. `count` 等于 pcp 链表长度；
2. pcp 页范围合法；
3. pcp 页是 `is_free=false/refcount=0/order=0/state=PAGE_IN_PCP`；
4. `buddy_check()` 通过。

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
| `invalid` | NULL、非法 high/batch、未分配页、错误 state |
| `random stress` | 2 万次随机 alloc/free，最终全部还给 buddy |

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
5. drain 前不把 page 恢复成 `PAGE_ALLOCATED/refcount=1` → `explicit drain` 失败；
6. order > 0 也走 pcp → `order > 0 bypass` 失败。

## 和 Linux 5.15 对照

| tiny-mm | Linux 5.15 |
|---|---|
| `struct pcp` | `struct per_cpu_pages`（`include/linux/mmzone.h:360`） |
| `pcp->high` / `batch` | `per_cpu_pages.high` / `batch` |
| `pcp->list` | `per_cpu_pages.lists[NR_PCP_LISTS]` 中对应 order/migratetype 的链表 |
| `pcp->count` | `per_cpu_pages.count` |
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

1. 已填 [../reports/M2b.md](../reports/M2b.md)；
2. 跑 `make test` + `make test-m2` + `make test-m2b`；
3. M2c 已完成：`pmm` 已换成 buddy + pcp（`make test-m2c` 20/20）。
