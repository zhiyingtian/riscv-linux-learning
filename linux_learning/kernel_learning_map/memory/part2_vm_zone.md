## `zone` 结构体关键字段与物理内存的映射关系

### 一、整体架构层级

```
┌──────────────────────────────────────────────────────────────────────┐
│                  pglist_data (Node = 一个 NUMA 节点)                  │
│  node_start_pfn = 0x000000 (节点起始 PFN)                             │
│  node_zones[] ──┐                                                    │
│                 │                                                    │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │               struct zone (ZONE_NORMAL 示例)                     │ │
│  │  zone_start_pfn = 0x001000   (本 zone 起始 PFN)                  │ │
│  │  spanned_pages  = 131072       (覆盖 512MB 范围)                  │ │
│  │  present_pages  = 130048       (实际存在 508MB)                   │ │
│  │  managed_pages  = 129024       (伙伴系统管理 504MB)                │ │
│  │  free_area[MAX_ORDER] ──┐                                        │ │
│  │  per_cpu_pageset ─────┐ │                                        │ │
│  │                       │ │                                        │ │
│  │  ┌──────────────────┐ │ │  ┌──────────────────────────────┐      │ │
│  │  │ free_area[0]     │ │ │  │ per_cpu_pages (CPU 0)        │      │ │
│  │  │ free_list[*]     │ │ │  │  count = 10                  │      │ │
│  │  │ nr_free = 500    │ │ │  │  lists[MIGRATE_MOVABLE]      │      │ │ 
│  │  └──────────────────┘ │ │  └──────────────────────────────┘      │ │
│  │  ┌──────────────────┐ │ │                                        │ │
│  │  │ free_area[2]     │ │ │                                        │ │
│  │  │ free_list[*]     │ │ │                                        │ │
│  │  │ nr_free = 50     │ │ │                                        │ │
│  │  └──────────────────┘ │ │                                        │ │
│  │       ...             │ │                                        │ │
│  │  ┌──────────────────┐ │ │                                        │ │
│  │  │ free_area[10]    │ │ │                                        │ │
│  │  │ free_list[*]     │ │ │                                        │ │
│  │  │ nr_free = 3      │ │ │                                        │ │
│  │  └──────────────────┘ │ │                                        │ │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │               struct zone (ZONE_DMA32)                          │  │
│  │  zone_start_pfn = 0x000000                                      │  │
│  │  ...                                                            │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

------

### 二、关键字段与物理内存的映射表

| 字段                       | 类型                   | 含义 & 与物理内存的映射           | 公式/取值                                                    |
| :------------------------- | :--------------------- | :-------------------------------- | :----------------------------------------------------------- |
| **`zone_start_pfn`**       | `unsigned long`        | **本 zone 的起始物理页帧号**      | `zone_start_paddr >> PAGE_SHIFT` 即 zone 第一个 4KB 页的 PFN |
| **`spanned_pages`**        | `unsigned long`        | **zone 覆盖的总页面数（含空洞）** | `zone_end_pfn - zone_start_pfn`                              |
| **`present_pages`**        | `unsigned long`        | **zone 中实际存在的物理页面数**   | `spanned_pages - absent_pages(空洞)`                         |
| **`managed_pages`**        | `atomic_long_t`        | **伙伴系统实际管理的页面数**      | `present_pages - reserved_pages` （减去 bootmem 保留页）     |
| **`free_area[MAX_ORDER]`** | `struct free_area[11]` | **伙伴系统空闲链表数组**          | 每个 `free_area[n]` 管理 `2^n` 页大小的空闲块                |
| **`_watermark[NR_WMARK]`** | `unsigned long[3]`     | **水位线 (min/low/high)**         | 以 `managed_pages` 为基准计算 当 `free_area` 中空闲页低于水位线时触发回收 |
| **`per_cpu_pageset`**      | `percpu 指针`          | **每 CPU 页面缓存 (PCP)**         | 避免频繁操作 zone->lock 从 `free_area` 批量获取/归还         |
| **`vm_stat[]`**            | `atomic_long_t[]`      | **zone 统计信息**                 | `NR_FREE_PAGES`、LRU 各链表页数等                            |

------

### 三、物理内存地址 ↔ PFN ↔ zone ↔ free_area ↔ struct page 的完整映射链

```
  物理地址 (Physical Address): 0x00000000_4000_1234
                    │
                    ▼ 右移 PAGE_SHIFT (12)
  PFN (Page Frame Number): 0x0004_0001  (4KB 对齐后的页帧号)
                    │
                    ▼ 判断落在哪个 zone 的 [zone_start_pfn, zone_end_pfn) 区间
  struct zone:      zone = NODE_DATA(nid)->node_zones[zone_idx]
                    │  zone_start_pfn = 0x0004_0000
                    │  zone_end_pfn   = 0x0008_0000
                    │
                    ▼ PFN 减去 zone_start_pfn → 本 zone 内的偏移
  zone 内偏移:     offset = PFN - zone->zone_start_pfn
                    │
                    ▼ 通过 pfn_to_page(PFN) 找到
  struct page:     page = &vmemmap[PFN]    (struct page 数组，一一对应)
                    │  flags (PG_buddy, PG_locked, ...)
                    │  _refcount
                    │  lru (若在 free_list 中，则挂在 zone->free_area[n].free_list[])
                    │  private (若 PageBuddy，则存 order)
                    │
                    └─▶ 若此页空闲，且是 buddy 块的首页
                         ↳ 挂在 zone->free_area[order].free_list[migratetype] 链表中
                         ↳ 此 free_area 的 nr_free 计数 +1
```

------

### 四、`free_area` 与物理内存的详细映射

**数据结构：**

```
zone->free_area[]  (size = MAX_ORDER = 11)
│
├── free_area[0]   ← 管理 2^0 = 1 页 (4KB) 的空闲块
│   ├── free_list[MIGRATE_UNMOVABLE]   ← 链表: page1 ↔ page2 ↔ page3 ...
│   ├── free_list[MIGRATE_MOVABLE]     ← 链表: pageA ↔ pageB ↔ pageC ...
│   ├── free_list[MIGRATE_RECLAIMABLE] ← 链表
│   └── nr_free = 500                  ← 此 order 共有多少个空闲块
│
├── free_area[1]   ← 管理 2^1 = 2 页 (8KB) 的空闲块
│   ├── free_list[...]
│   └── nr_free = 200
│
├── free_area[2]   ← 管理 2^2 = 4 页 (16KB) 的空闲块
│   └── ...
│
└── free_area[10]  ← 管理 2^10 = 1024 页 (4MB) 的空闲块
    └── ...
```

**图示（单个 free_area[n] 如何指向物理页面）：**

```
zone->free_area[2].free_list[MIGRATE_MOVABLE]
    ┌─── prev ───┐   ┌─── prev ───┐   ┌─── prev ───┐
    ▼             ▼   ▼             ▼   ▼             ▼
  [head] ◀─────▶ page X ◀──────▶ page Y ◀──────▶ page Z
                PFN=0x4000        PFN=0x4010        PFN=0x4020
                ┌──────────┐     ┌──────────┐     ┌──────────┐
                │ 4 pages  │     │ 4 pages  │     │ 4 pages  │
                │ 0x4000   │     │ 0x4010   │     │ 0x4020   │
                │ ~0x400F  │     │ ~0x401F  │     │ ~0x402F  │
                └──────────┘     └──────────┘     └──────────┘
                struct page[X]   struct page[Y]   struct page[Z]
                 ├ flags = PG_buddy
                 ├ lru = 链表节点 (用于挂在 free_list)
                 └ private = 2 (这个块的 order)

nr_free = 3  ← 此 free_list 中有 3 个 order=2 的空闲块
```

**分配路径(expand)：**

```
请求 order=2:
  1. 先查 free_area[2].free_list[mt]
      → 如果找到，直接从链表取出，返回
      → free_area[2].nr_free--
      
  2. 如果没找到，向上查 free_area[3]
      → 取出一个 order=3 的块 (8 pages)
      → expand(): 后 4 pages 作为新的 order=2 块，加入 free_area[2]
      → free_area[3].nr_free--
      → free_area[2].nr_free++
      → 返回前 4 pages
```

------

### 五、三种页面计数器的区别

```
spanned_pages = zone_end_pfn - zone_start_pfn
    │
    │  包含物理地址空间中的"空洞"（hole，无实际 RAM 的地址）
    ▼
    例如: 物理地址 4GB~4.2GB 是空洞，spanned 仍然计算这段区间
┌─────────────────────────────────────────────────────┐
│  RAM  │ hole │  RAM  │ hole │         RAM           │  ← 物理地址空间
└─────────────────────────────────────────────────────┘
  ↑ spanned_pages 把这些全部算进去

present_pages = spanned_pages - absent_pages(空洞页面数)
    │
    │  只算真正有 RAM 的物理页
    ▼
    但可能包含 bootmem 保留页、固件保留页等

managed_pages = present_pages - reserved_pages
    │
    │  伙伴系统实际能分配的页面
    ▼
    free_area[].nr_free 的总和 ≤ managed_pages
    (因为有些页已被分配出去了)

      physical memory view:
      ┌──────────────────────────────────────┐
      │   managed_pages                      │
      │   ┌────────────────────────────┐     │
      │   │   free_area[] (空闲页)      │     │  ← 可被分配
      │   │   ┌──────────────┐         │     │
      │   │   │   LRU 页     │         │     │  ← 可被回收
      │   │   └──────────────┘         │     │
      │   └────────────────────────────┘     │
      │                                      │
      │   reserved_pages (bootmem, 固件等)    │
      └──────────────────────────────────────┘
```

------

### 六、per_cpu_pageset (PCP) 的位置与作用

```
struct zone
    │
    ├── per_cpu_pageset __percpu *per_cpu_pageset
    │     │
    │     ▼ 每个 CPU 有独立的缓存
    │
    │     CPU 0: struct per_cpu_pages
    │              ├ count = 15     (当前缓存了 15 个 order=0 页)
    │              ├ high = 60      (超过此值则归还 buddy)
    │              ├ batch = 30     (批量获取/归还的大小)
    │              └ lists[NR_PCP_LISTS]
    │                    ├ [MIGRATE_MOVABLE]   ← 4KB 单页链表
    │                    └ [MIGRATE_UNMOVABLE] ← 4KB 单页链表
    │
    │     CPU 1: struct per_cpu_pages  (同上，独立缓存)
    │     ...
    │
    │     作用: order=0 的小页分配直接从 PCP 拿，无需每次拿 zone->lock
    │
    └── free_area[MAX_ORDER]  ← 只有 PCP 空/满时才会与 buddy 批量交互
          当 PCP->count < batch 时，从 free_area[0] 批量获取 batch 个页面
          当 PCP->count > high   时，将 batch 个页面批量归还 free_area[0]
```

------

### 七、总结：核心映射关系图表

```
  ╔═══════════════════════════════════════════════════════════════════════╗
  ║                       zone ⇄ 物理内存 核心映射关系                        ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║   物理地址空间                                                          ║
  ║   0x00000000 ───────────────────────────────────── 0xFFFFFFFF         ║
  ║         │                            │                                ║
  ║         ▼ 按 zone 划分                ▼                                ║
  ║   ┌──────────┬──────────────┬──────────────────────┐                  ║
  ║   │ ZONE_DMA │ ZONE_DMA32   │    ZONE_NORMAL       │ ← zone 类型       ║
  ║   │ 16MB     │ 4GB          │  剩余所有             │                   ║
  ║   └──────────┴──────────────┴──────────────────────┘                   ║
  ║         │                                                              ║
  ║         ▼ 每个 zone 内部结构                                             ║
  ║   ┌──────────────────────────────────────────────────────┐             ║
  ║   │                  struct zone                         │             ║
  ║   │  zone_start_pfn ──────┐ (本 zone 起始 PFN)            │              ║
  ║   │  spanned_pages        │ (覆盖区间大小，含空洞)           │             ║
  ║   │  present_pages        │ (实际有 RAM 的页数)             │             ║
  ║   │  managed_pages        │ (buddy 可管理页数)             │              ║
  ║   │  _watermark[3]        │ (min/low/high 水位)           │              ║
  ║   │                       │                              │              ║
  ║   │  free_area[0] ◄───┐   │ (管理 1-page 空闲块)           │              ║
  ║   │  free_area[1]     │   │                              │              ║
  ║   │  free_area[2]     │   │                              │              ║
  ║   │  ...              │   │                              │              ║
  ║   │  free_area[10]    │   │                              │              ║
  ║   │                   │   │                              │              ║
  ║   │  per_cpu_pageset  │   │ (每个 CPU 的 4KB 页缓存)        │             ║
  ║   └───────────────────────┼───────────────────────────────┘             ║
  ║                           │                                             ║
  ║                           ▼                                             ║
  ║   ┌──────────────────────────────────────────────────────────┐          ║
  ║   │           free_area[order]  (以 order=2 为例)             │          ║
  ║   │   free_list[MIGRATE_MOVABLE] ◀─ 链表: pageX ↔ pageY       │          ║
  ║   │   free_list[MIGRATE_UNMOVABLE] ◀─ 链表                    │          ║
  ║   │   nr_free = N                    (空闲块数量)              │          ║
  ║   └──────────────────────────────────────────────────────────┘          ║
  ║                                  │                                      ║
  ║                                  ▼ 链表中的每个节点都是 struct page        ║
  ║   ┌──────────────────────────────────────────────────────────┐          ║
  ║   │                    struct page                           │          ║
  ║   │   flags      (PG_buddy 表示此页在 free_area 中)            │          ║
  ║   │   lru        (用于挂载到 free_area->free_list)             │          ║
  ║   │   private    (若 PageBuddy，此字段 = order)                │          ║
  ║   │   _refcount  (引用计数)                                   │           ║
  ║   │   _mapcount  (页表映射计数)                                │           ║
  ║   └──────────────────────────────────────────────────────────┘           ║
  ║                                  │                                       ║
  ║                                  ▼ page_to_pfn(page)                     ║
  ║                                 PFN                                      ║
  ║                                  │                                       ║
  ║                                  ▼ PFN << PAGE_SHIFT                     ║
  ║                               物理地址                                    ║
  ║                                                                          ║
  ╚══════════════════════════════════════════════════════════════════════════╝
```

------

### 八、从分配路径看各变量的互动

以 **分配 order=2, MIGRATE_MOVABLE** 为例：

```
alloc_pages(gfp_mask, order=2)
    │
    ▼
get_page_from_free_list(zone, 2, MIGRATE_MOVABLE)
    │
    ├─ 从 zone->free_area[2].free_list[MIGRATE_MOVABLE] 取第一页
    │   zone->free_area[2].nr_free--
    │
    ├─ 若 free_area[2] 为空
    │   │
    │   ▼ 向上查找 free_area[3], free_area[4]...
    │   在 free_area[5] 找到一个 order=5 的块
    │   zone->free_area[5].nr_free--
    │   │
    │   ▼ expand(zone, page, low=2, high=5, migratetype)
    │       │
    │       ├─ high=4, size=16: 后 16 页加入 free_area[4]
    │       │                  zone->free_area[4].nr_free++
    │       │
    │       ├─ high=3, size=8:  后 8 页加入 free_area[3]
    │       │                  zone->free_area[3].nr_free++
    │       │
    │       └─ high=2, size=4:  后 4 页加入 free_area[2]
    │                          zone->free_area[2].nr_free++
    │
    │   返回前 4 页给调用者
    │
    └─ 更新 zone->vm_stat[NR_FREE_PAGES]（减少）
```

### 九、关键结论

1. **`zone_start_pfn` 是锚点**：所有 zone 内的操作都基于这个 PFN 进行偏移计算
2. **`free_area` 是伙伴系统的核心**：通过 `struct page->lru` 链表挂接，每个 `free_area[n]` 管理 `2^n` 页大小的块
3. **`managed_pages` 是水位线基准**：`_watermark[]` 基于它计算，决定何时触发页面回收
4. **`per_cpu_pageset` 是锁优化**：order=0 的分配从 PCP 拿，无需每次竞争 `zone->lock`
5. **`struct page` 是粘合剂**：从物理地址 → PFN → zone → free_area → struct page 的链条中，`struct page` 是最底层的管理单元