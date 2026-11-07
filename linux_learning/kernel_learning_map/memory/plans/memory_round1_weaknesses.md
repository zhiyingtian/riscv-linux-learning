# Linux Memory 第一轮弱点清单(查漏补缺)

> 配套:`memory_round1_exercises.md`、`linux_memory_framework_learning_plan.md`、`../articles/bin的技术小屋` mm1–mm13、笔记 part1–part4
> 用途:每做一组练习,把**答错/卡住/表述不准**的点记进来;复习时只看这份清单,能不看答案讲清楚每条,才标记 ✅。
> 记录:2025-08 第一组 E1–E3 批改后;同月追加第二组 E4–E7。

---

## 第一组 E1–E3(阶段 0/1)

### E1 两次 SATP 切换(对应 part1 笔记 2.6 节、规划阶段0)

- [ ] **W1 机制混淆:两套"切换保障"讲法要分清**
  - 本笔记(part1 2.6 节)的机制是:**trampoline 页表低地址全空 → 写 SATP 后低地址翻译必然失败 → 触发 page fault → `PC ← stvec`(切换前已设为高 VA)→ 一步跳到高地址**。
  - 另一套老讲法("低地址恒等映射 PA=VA + relocate 跳转")在本笔记里**不是**实际机制,别混用。
- [ ] **W2 第一次 SATP 写 trampoline 时 MMU 就已开启**,不是第二次才开;第二次只是**换页表**(换到 early_pg_dir),不是"开 MMU"。
- [ ] **W3 为什么不直接写 early_pg_dir**:early_pg_dir 里有**低地址映射** `PGD[1] → early_dtb_pmd → FDT(0x40000000)`。若内核恰被 bootloader 加载到 FDT 区附近(如 0x40001000),低地址取指**不触发异常**,而是把 FDT 数据当指令执行 → **静默崩溃、无异常、极难调试**。
  - trampoline 的极简构造(`PGD[0..510]=0`,高地址只留 KERNEL_LINK_ADDR 一个 PMD 范围)保证**必然异常**,行为确定、不依赖内核加载地址。
- [ ] **W4 切换后如何继续取指**(Q3):低地址全空 → 必然 trap → `PC ← stvec`(高 VA)→ 高地址内核段在 trampoline 中有映射 → 取指成功。**"全空的是低地址,不是高地址"**。
- [ ] **记忆点**:两次切换 = 一次"弹射座椅"(trampoline,极简、确定性)+ 一次"正式页表"(early_pg_dir,完整运行环境)。SATP 存的是**物理页号**,与当前跑在哪种地址空间无关,`a2` 切换前算好即可直接写。

### E2 物理地址 → PFN → struct page → zone(对应 mm2、part2、规划阶段1)

- [ ] **W5 PFN 公式(必须脱口而出)**:`PFN = phys_addr >> 12`(4KB 页,右移 12 = 2^12)。
  - 例:`0x000000004000_1234 >> 12 = 0x40001`(0x40000 + 页内偏移 0x234)。
  - 后续 buddy 的 `page ^ (1 << order)`、`pfn_to_page` 全依赖它,忘了会连环卡。
- [ ] **W6 为什么 `&vmemmap[PFN]` 能直接索引**:三要素缺一不可——
  1. vmemmap 是**连续虚拟区间**,struct page 按 PFN 顺序连续排列;
  2. **下标即 PFN**(数组第 PFN 个元素 = 第 PFN 个物理页的描述符);
  3. **元素定长**(`sizeof(struct page)` 固定)→ O(1) 定位。
  - 前提:SPARSEMEM_VMEMMAP 保证每个 PFN 都有槽位。
- [ ] **W7 zone 归属判断(补精确)**:条件 `zone_start_pfn <= pfn < zone_end_pfn`,其中 `zone_end_pfn = zone_start_pfn + spanned_pages`;先按 node 定位(`pgdat->node_zones[]`),再逐个 zone 比区间。
- [ ] **W8 三数语义与大小关系**:`spanned >= present >= managed`。
  - `spanned` = 区间总长(含空洞);`present` = 实际存在的页(去空洞);`managed` = **present − 保留页**(struct page 数组、内核镜像、页表、reserved),即真正交给 buddy 的页。

### E3 观察 zone / 水位线(对应 mm2、mm4、规划阶段1/3)

- [ ] **W9 MemTotal 与 managed 的关系(本组最重要的纠正)**:
  - **`sum(managed) == MemTotal` 是恒等式**(MemTotal 的定义就是遍历所有 zone 的 managed_pages 求和),不是巧合,更不是"把 reserved 算进 managed"。
  - 三个量分清楚:物理总容量(本机 16 GiB)> MemTotal(= sum(managed) ≈ 15.3 GiB)。**差的 0.7 GiB 就是 reserved**:内核镜像、vmemmap(struct page 数组自身!)、页表、bootloader/固件保留区。
  - reserved 页既不进 managed 也不进 MemTotal;managed 已经**排除**了 reserved。
- [ ] **W10 水位线语义**:
  - `low`:free 低于它 → 唤醒 **kswapd** 后台回收;
  - `high`:**kswapd 的回收停止目标**(回收直到 free ≥ high 才停),不是"内存充足"标志;kswapd 工作区间 [low, high],free 常年落在该区间是常态;
  - `min`:free 低于它 → 分配走 **direct reclaim**(当前线程自己回收,可睡眠);
  - min 以下还有 **reserve 池**,只放行紧急分配(`ALLOC_HARDER`/原子分配)。

#### 本机参考数据(2025-08 实测,方便复查对比)

```
Node 0:
  DMA   : managed=3841    min=16   low=20   high=24
  DMA32 : managed=328411  min=1308 low=1635 high=1962
  Normal: managed=3681732 min=15570 low=19462 high=23354
sum(managed) = 4013984 页 × 4KB = 16055936 kB == MemTotal
MemTotal=16055936 kB  MemFree=573484 kB  Cached=8862012 kB  Slab=835168 kB

/proc/buddyinfo (order 0→10):
  Normal: 47686 13461 5417 2008 124 37 46 13 0 4 1   ← order0 极多,order≥9 极少
```

---

## 第二组 E4–E7(阶段 2/3)

### E4 memblock 的两张表(对应 mm2 前文回顾、规划阶段2)

- [ ] **W11 memory / reserved 关系(首答方向反了)**:`memblock.memory` 是**全集**(DTB memory 节点上报的全部物理范围,**包含**将被保留的部分);`memblock.reserved` 是**子集**(已被占用、不能碰);**可用内存 = memory − reserved**。
- [ ] **W12 memblock_reserve 的目的不是安全**:不是"防篡改/窃取",而是**正确性**——内核镜像/initrd/DTB 已存放代码数据,不 reserve 就会被 buddy 当空闲页分出去覆盖,导致内核代码被改写崩溃。**"防止被分配器覆盖"**。
- [ ] **W13 `memblock_free_all()` 的语义**:不是"释放所有内存",而是把 **memory 中未被 reserved 的页逐块释放进 buddy allocator**(内部调 `free_page` 系列),之后 memblock 基本**退役**,长期分配交给 buddy。
- [ ] **W14 为什么先 memblock 后 buddy(先有鸡还是先有蛋)**:buddy 要运行必须先有 zone 结构、free_area 链表、`struct page` 数组——**这些设施本身就要内存**;在它们建立前无法用 buddy 分配它们自己。所以先用**不依赖任何设施**的 memblock(region 数组记录范围)过渡,等基础设施建好再移交。
  - 别只答"buddy 的数据来自 memblock"——那是"数据从哪来",不是"顺序为什么必要"。

### E5 buddy 拆分与合并(对应 mm4、规划阶段3)

- [ ] **W15 伙伴异或的几何含义(补精确)**:`buddy = page ^ (1 << order)` 本质是**翻转第 order 位**——互为伙伴的两块起始地址只在 order 位不同(相差 1),其余位全同。几何含义:**伙伴必然相邻,且按 2^order 对齐**;异或一次 O(1) 得到对方,是 buddy 高效的原因。
  - 术语:分配侧拆完把多余半块回填链表 = **`expand()`**;释放侧逐级查伙伴合并 = **`__free_one_page()`**。
  - 拆分实例(16 页,请求 order=1):order4 `[0..15]` → 挂 order3 `[8..15]`、拆 `[0..7]` → 挂 order2 `[4..7]`、拆 `[0..3]` → 挂 order1 `[2..3]`、交付 `[0..1]`。

### E6 GFP_KERNEL vs GFP_ATOMIC(对应 mm3、规划阶段3)

- [ ] **W16 GFP_ATOMIC 缺的位(核心考点,首答忘记)**:少了 **`__GFP_DIRECT_RECLAIM`**(直接回收位)。`GFP_ATOMIC = GFP_NOWAIT | __GFP_HIGH`,只保留 `__GFP_KSWAPD_RECLAIM`(唤醒 kswapd 后台回收,**不睡眠**)。
  - 记忆链:**direct reclaim 必须睡眠**(要等回收结果),所以"不能睡眠"和"不能走 direct reclaim"是同一根源。
- [ ] **W17 GFP_ATOMIC 失败率高 + 额外手段**:失败率高是因为**不能回收就不能"创造"内存**,可用来源少(只剩空闲页 + 预留)。额外手段是 **`ALLOC_HARDER` 允许使用 min 水位线以下的 reserve 池**——这是给特定分配的**豁免权**,**不是"降低水位线"**(水位线是全局静态参数,不为单个分配改变)。

### E7 观察 buddyinfo(对应 mm4、规划阶段3)

- [ ] **W18 buddy 分配从小阶开始找(首答说反)**:buddy 先找 order 0/1,**没有才逐级往高阶拆**;高阶块多 ≠ 优先用高阶块。大量 order>=3 + 少量 order 0/1 的表现:**低阶频繁短缺 → 每次分配下探拆块 → 分配延迟上升**,长期触发 kswapd/compaction 频繁工作。
  - 判断"内存碎片少"只是可能原因之一(也可能刚 compaction、或低阶页被长期占用)。
- [ ] **W19 drop_caches 机制(本机无 root 未实跑,口头推演)**:`echo 1 > /proc/sys/vm/drop_caches` 只释放**可回收的 clean page cache**,**不碰 anonymous 页和脏页**。效果:`free` 上升但 `available` 基本不变(那些内存本来就可回收);buddyinfo **order=0 增多**(回收逐页进行,合并成高阶需相邻,可遇不可求,高阶块变化不大)。

---

## 深层问题(结构性,不针对单题)

- [ ] **D1 ../notes/part4_mm_physical.md 仅 28 行**,E2/E3 考的 zone/managed/watermark/reserved 正是它的地盘,笔记严重不足。**建议补写 part4**(覆盖:zone 字段、watermark 计算、managed/present/spanned 的来源、reserved 流向)。
- [ ] **D2 "读过 ≠ 掌握"**:E1 的 trap 机制是 part1 笔记 2.6 节自己写过的,首答却完全没答出。教训:**读完文章立刻做练习/复述,别攒着**。
- [ ] **D3 常见答题模式问题(第二组多题中招)**:
  - 只给否定判断、不给内容(E4-Q3"free all 肯定不是释放所有内存"——没说是什么);
  - 答"数据从哪来"代替"为什么这样设计"(E4-Q4);
  - 凭感觉编安全理由(E4-Q2"防篡改窃取")。
  - **对策:凡"是什么/为什么"题,先写完整句(主语+谓语+宾语),再检查是否回答了题干动词。**

---

## 复查方法

1. 隔 2~3 天,合上文档和文章,**重做 E1–E3、E4–E7**;
2. 每重做对一条,对应 W 项标 ✅;
3. 全部 ✅ 后再进第三组 E8–E10(阶段 4:VMA/mmap),并把新弱点追加到本清单;
4. E7-Q2(drop_caches 实验)本机无 root 未实跑,找机会在有 root 的机器/qemu 上补做,对照 W19。
