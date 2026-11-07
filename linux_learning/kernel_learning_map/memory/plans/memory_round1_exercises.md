# Linux Memory 第一轮小练习(自测 + 加深理解)

> 配套:`../articles/bin的技术小屋` mm1–mm13、自己的笔记 part1–part4、`linux_memory_framework_learning_plan.md`(规划)
> 覆盖规划阶段:0(MMU 启动)、1(核心数据结构)、2(memblock)、3(buddy)、4(VMA)、5(缺页/页表)、9(slab)+ 阶段6(COW 预习)

## 使用说明

- 共 18 个小练习,每个 10~30 分钟,建议一次做 2~3 个。
- 三类题型:
  - 🧠 **概念题**:先不查资料,凭记忆回答;答不上来标记为弱点,回到对应文章。
  - 🖥️ **实验题**:在 Linux 上跑(宿主机或 qemu RISC-V 虚拟机都行),需要 `gcc`、`/proc` 文件系统,可选 `perf`。
  - ✏️ **画图题**:合上文章,自己画,画完再对照文章修正。
- 每题标注了对应的文章/源码与规划阶段,方便做完后"回看"。
- **答案在文末"参考答案"一节,先做完再对。**
- 做完之后:把答案或实验输出截图发给我,我来批改和补充讲解。

---

## 阶段0/1:MMU 启动与核心数据结构(3 题)

### E1 🧠 [基础] 两次 SATP 切换(对应 part1、规划阶段0)

RISC-V 启动早期为什么要做**两次** SATP 切换,而不是直接写入 `early_pg_dir`?

1. 第一次切换用 `trampoline_pg_dir` 的目的是什么?
2. 第二次才切换到 `early_pg_dir`,为什么不能合并成一次?
3. 用自己的话说:切换瞬间 PC 还指向低地址,如何保证切换后指令还能取到?

### E2 🧠 [基础] 物理地址 → PFN → struct page → zone 的换算(对应 mm2、part2、规划阶段1)

假设某 64 位机器上物理地址 `0x000000004000_1234`(4KB 页):

1. 它的 PFN 是多少?写一下计算过程。
2. 用 `pfn_to_page()` 找到 `struct page`,内部实际是 `&vmemmap[PFN]`,为什么能这样直接索引?
3. 要判断这个页属于哪个 zone,内核是怎么做的?涉及 `zone_start_pfn` / `zone_end_pfn` 的什么逻辑?
4. `spanned_pages`、`present_pages`、`managed_pages` 三个数各回答什么问题?大小关系如何?

### E3 🖥️ [基础] 观察 zone 与内存总量(对应 mm2、mm4,规划阶段1/3)

在你的机器上执行:

```bash
cat /proc/zoneinfo | grep -E "Node|zone|managed|present|min|low|high" | head -60
cat /proc/meminfo | grep -E "MemTotal|MemFree|Cached|Slab"
cat /proc/buddyinfo
```

1. 找 Node 0 的 ZONE_NORMAL,记下 `managed`、`min`、`low`、`high`。
2. 把各 zone 的 `managed` 加起来,和 `MemTotal` 对比,为什么两者不相等?
3. 解释 `min / low / high` 三个水位线各自触发的动作(kswapd / direct reclaim / 回收目标)。

---

## 阶段2/3:memblock 与 Buddy(4 题)

### E4 🧠 [基础] memblock 的两张表(对应 mm2 前文回顾、规划阶段2)

1. `memblock.memory` 和 `memblock.reserved` 分别记录什么?
2. 内核镜像、initrd、DTB 为什么要 `memblock_reserve()`?
3. `memblock_free_all()` 做了什么?"free all" 是把所有内存释放吗?
4. 为什么启动早期不能直接使用 buddy,而要先经过 memblock?(提示:buddy 自己也需要内存来初始化)

### E5 ✏️ [基础] 推演 buddy 的拆分与合并(对应 mm4、规划阶段3)

假设物理内存共 16 个页(编号 0~15),初始时是一整块 order=4 的空闲块。

1. 请求分配 order=1(2 页),画出拆分过程:保留哪一半,哪一半挂回哪个 order 的链表?
2. 释放这 2 页,和相邻伙伴合并,一步步画到恢复 order=4。
3. 为什么 buddy 的伙伴计算公式是 `buddy = page ^ (1 << order)`?「异或」在这里表达了什么几何含义?

### E6 🧠 [基础] GFP_KERNEL vs GFP_ATOMIC(对应 mm3、规划阶段3)

1. 两者最本质的差别是什么?(提示:能不能睡眠、能不能走 direct reclaim)
2. 从 GFP 位来说,`GFP_ATOMIC` 少了哪个 `__GFP_*` 位?
3. 为什么中断上下文/持有自旋锁时只能用 `GFP_ATOMIC`?如果此时调用会睡眠的分配会怎样?
4. `GFP_ATOMIC` 分配失败的概率为什么更高?它靠什么"额外手段"提高成功率?

### E7 🖥️ [进阶] 观察水位线与 buddyinfo 分布(对应 mm4、规划阶段3)

1. 用 E3 的输出:ZONE_NORMAL 里 order=0 和 order=9 的空闲块数量差多少?为什么高阶块这么少?
2. 尝试 `echo 1 > /proc/sys/vm/drop_caches`(需要 root),再看 `free -h` 和 buddyinfo 的 order=0 变化,解释原因。
3. 思考:如果 buddyinfo 显示大量 order>=3 的块,而 order 0/1 很少,说明什么?内存分配会有什么表现?

---

## 阶段4:VMA 与 mmap(3 题)

### E8 🖥️ [基础] mmap 懒分配实验(对应 mm11/mm12、规划阶段4)

把下面程序存为 `mmap_lazy.c` 编译运行(`gcc -o mmap_lazy mmap_lazy.c`):

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    size_t sz = 128UL * 1024 * 1024;              /* 128MB */
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    printf("pid=%d mapped=%p size=%zuMB\n", getpid(), p, sz / 1024 / 1024);
    printf(">>> 现在去另一个终端看 /proc/%d/smaps 和 /proc/%d/statm,再回车\n", getpid(), getpid());
    getchar();
    for (size_t i = 0; i < sz; i += 4096) p[i] = 1;   /* 逐页写 */
    printf(">>> 已逐页写入,再看一次 smaps/statm,再回车\n");
    getchar();
    return 0;
}
```

1. 第一次暂停时,`/proc/<pid>/smaps` 里这个映射的 `Rss` 大约是多少?`VmSize` 呢?
2. 逐页写完后,Rss 变成多少?
3. 用一句话解释:mmap() 到底"分配"了什么?物理页是什么时候才分配的?

### E9 🧠 [基础] VMA 与 PTE 的关系(对应 mm1/mm11、规划阶段4)

1. 一个 VMA 和一个 PTE 的粒度差多少?一个 VMA 能覆盖多少个 PTE?
2. VMA 里记录了什么信息(范围、权限、文件、偏移),PTE 里又记录了什么?
3. 为什么说"VMA 不等于页表映射"?请举一个"有 VMA 但没有任何 PTE"的场景。
4. page fault 处理时,第一步为什么要 `find_vma()`?它回答什么问题?

### E10 🖥️ [进阶] 分析真实进程的 VMA 布局(对应 mm1、规划阶段4)

1. 打开一个编辑器或浏览器,`cat /proc/<pid>/maps`(或 `pmap <pid>`)。
2. 指出其中:可执行文件映射、共享库映射、堆(heap)、栈(stack)、匿名映射各长什么样(注意权限位 rwxp 和文件名)。
3. 找出 `[heap]` 和 `[stack]` 之间隔了多远?为什么中间有这么大一段"空洞"?
4. 找一个 `r-xp` 的库映射,思考:两个进程加载同一个 so,它们的物理页可以共享吗?靠什么机制?

---

## 阶段5:缺页异常与页表(3 题)

### E11 🖥️ [基础] 数一数 minor fault(对应 mm13、规划阶段5)

用 E8 的程序,但去掉 `getchar()`,改成直接逐页写,然后:

```bash
perf stat -e minor-faults,major-faults,page-faults ./mmap_lazy_nowrite
```

1. 写 32768 个页(128MB / 4KB)后,`minor-faults` 大约是多少?为什么大约等于页数?
2. 为什么几乎没有 `major-faults`?
3. 如果把 `MAP_ANONYMOUS` 换成 `MAP_PRIVATE` 文件映射再读文件,第一次读会有什么 fault?(提示:major fault)
4. 思考:为什么 page fault 是"正常路径"而不是"错误路径"?

### E12 🧠 [进阶] 四种缺页分类(对应 mm13、规划阶段5)

把下面四种场景分别对应到处理函数,并说明各自分配/建立的是什么:

| 场景 | 入口函数 | 做了什么 |
|---|---|---|
| 首次写匿名页 | `do_anonymous_page()` | ? |
| 首次读文件映射页 | `do_fault()` → ? | ? |
| fork 后写共享的只读页 | `do_wp_page()` → ? | ? |
| swap 出来的匿名页被访问 | `do_swap_page()` | ? |

1. 填完表格。
2. `do_anonymous_page` 在读 vs 写时行为有何不同?(提示:zero page)
3. 用户态地址范围之外的访问(如空指针),会走到哪一步被拒?(提示:SIGSEGV 是哪里发的)

### E13 ✏️ [进阶] Sv39 虚拟地址拆分(对应 part1、规划阶段5,RISC-V 特有)

在 RISC-V Sv39 下,虚拟地址 `0x0000_0000_4000_1234` 拆分为:

```
VPN[2] (bit 38:30) = ?
VPN[1] (bit 29:21) = ?
VPN[0] (bit 20:12) = ?
offset (bit 11:0)  = ?
```

1. 写出每个字段的值和计算过程(提示:右移 + 与 0x1ff)。
2. 页表遍历顺序是 PGD(PGDir)→ PUD → PMD → PTE 四级;Sv39 实际是几级?对应哪几个字段?
3. 如果要把 `0xffffffc000000000`(内核线性映射区)拆开,`VPN[2]` 会很大,为什么?这说明了内核地址空间和用户地址空间在页表上的什么关系?

---

## 阶段6 预习:fork / COW(2 题)

> 说明:阶段6 你还没系统学,但 mm13 已经讲过 `do_wp_page`,这两题用来**验证 + 预习**。

### E14 🖥️ [进阶] fork COW 实验(对应 mm13、规划阶段6)

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

char buf[16 * 1024 * 1024];   /* 16MB 全局数组 */

int main(void) {
    for (int i = 0; i < (int)sizeof(buf); i += 4096) buf[i] = 1;  /* 先全部分配 */
    printf("pid=%d 写之前:看 smaps 的 Private_Dirty,回车\n", getpid());
    getchar();
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < (int)sizeof(buf); i += 4096) buf[i] = 2; /* 子进程写 → COW */
        printf("child done\n");
        _exit(0);
    } else {
        for (int i = 0; i < (int)sizeof(buf); i += 4096) buf[i] = 3; /* 父进程写 → COW */
        wait(NULL);
        printf("parent done\n");
    }
    return 0;
}
```

1. fork 后、双方写入前:父子的 RSS 加起来 ≈ 多少?为什么没有翻倍?
2. 双方各自写入后:各自 RSS ≈ 多少?物理内存多占了多少?
3. 这多出来的内存是靠什么机制"按需"产生的?
4. 假设 fork 后子进程直接 `exec()` 新程序,COW 机制浪费了吗?为什么?(提示:exec 会替换地址空间)

### E15 🧠 [基础] refcount / mapcount / rmap(对应 mm2、规划阶段6)

1. `_refcount`(refcount)回答什么问题?`_mapcount` 回答什么问题?
2. 一个页被 3 个进程共享映射,refcount 和 mapcount 分别是多少?(假设无其他引用)
3. 反向映射 rmap(anon_vma)要回答什么问题?为什么 reclaim / migration 都需要它?
4. 为什么 mapcount 用负数表示"未被映射"?

---

## 阶段9:slab / kmalloc(2 题)

### E16 🖥️ [基础] 观察 slabinfo(对应 mm5–mm9、规划阶段9)

```bash
head -5 /proc/slabinfo
grep -E "kmalloc-(8|16|32|64|128|256|512|1024|2048|4096) " /proc/slabinfo
```

1. 列出 kmalloc 的 size class(8、16、32……4096),观察每个的 `objsize`。
2. 为什么需要这么多尺寸档位?直接都用 4KB 页不行吗?
3. 一个 `struct task_struct`(几 KB)用哪个 size class 分配?`struct page` 呢?
4. `kmalloc(3000)` 会落到哪个档位?浪费了多少?

### E17 🧠 [基础] kmalloc vs vmalloc vs alloc_pages(对应 mm9、规划阶段9)

1. `kmalloc(8KB)` 和 `vmalloc(8KB)` 的语义差异是什么?(物理连续?虚拟连续?)
2. 为什么 `vmalloc` 得到的地址不能直接用于 DMA?
3. `kmalloc(2MB)` 与 `alloc_pages(order=9)` 是什么关系?(提示:超过 kmalloc 上限时)
4. 内核栈为什么用 `alloc_pages`/`THREAD_SIZE_ORDER` 而不是 vmalloc?(提示:栈需要物理连续吗?页表开销?)

---

## 总验收(1 题)

### E18 ✏️ [总验收] 画两条主干链路(对应规划第二十节验收标准 1、2)

合上所有文章,在纸上画:

1. **内存初始化链**:`DTB memory 信息 → memblock → sparse_init/struct page → free_area_init/zone → memblock_free_all → buddy`。每个箭头上标注"这步解决了什么"。
2. **一次匿名页访问链**:`mmap → 建立 VMA → 首次写入 → page fault → find_vma → handle_mm_fault → do_anonymous_page → alloc page → set PTE`。标注每一步发生在哪个文件/函数。

画完和规划第二十节的验收图对比,看差在哪一步——**差的那一步就是你的弱点**。

---

## 参考答案(先做完再看)

### E1 两次 SATP

1. 第一次写 `trampoline_pg_dir`:此时 PC 还在物理低地址,`trampoline_pg_dir` 里**同时**映射了低地址(物理地址即虚拟地址)和内核高地址,保证切换瞬间新旧地址都能取指,过渡安全。
2. 不能合并:`early_pg_dir` 只建立了内核虚拟地址空间(高地址)的映射,没有低地址恒等映射。若第一次就切到它,切换瞬间 PC 还在低地址,指令立刻取不到。
3. 本质:页表切换是"瞬间的、异步生效的",必须让切换前后的地址都有效。`relocate` 负责把 PC 从低地址跳到高地址,此时 `early_pg_dir` 才可用;`setup_vm_final()` 再把内核页表完善为最终形态(线性映射 + fixmap + vmemmap 等)。两次切换对应「低→高」的地址身份转变,一次是"安全垫",一次是"正式页表"。

### E2 物理地址 → PFN → struct page → zone

1. `PFN = 0x0000000040001234 >> 12 = 0x40001`(物理地址右移 12 位;高位补 0)。
2. SPARSEMEM_VMEMMAP 下,`struct page` 数组被映射在一个固定的虚拟区间 `vmemmap[]`,`pfn_to_page(pfn)` 就是 `vmemmap + pfn`(数组下标即 PFN,连续)。所以能直接索引,前提是内存模型保证"每个 PFN 都有一个对应的 struct page 槽位"。
3. 从 `pgdat->node_zones[]` 逐个比较:`pfn >= zone->zone_start_pfn && pfn < zone_end_pfn`(zone_end = zone_start + spanned)。落在哪个区间就属于哪个 zone。也可以在 memmap 里按 pfn 反查 section 再定位 zone。
4. `spanned` = 区间总长(含空洞);`present` = 实际存在的页(去掉空洞);`managed` = 交给 buddy 的页(present 再去掉保留页)。三者关系:`spanned >= present >= managed`。`MemTotal` 由各 zone managed 求和而来,但会略小于物理总量,因为内核镜像、页表、struct page 自身等被保留。

### E3 观察 zone

1~2. `/proc/zoneinfo` 的 `managed` 之和 ≈ `MemTotal`(还要加上各保留部分),因为 buddy 只能管理"交给它的页";`spanned` 还可能包含空洞。`MemTotal` 小于物理内存总量是正常的。
3. 水位线:`min` = 保证原子分配的最低空闲量,低于它分配走 **direct reclaim**(可能睡眠);`low` = 低于它唤醒 **kswapd** 后台回收;`high` = kswapd 的回收目标,达到即停止。所以 kswapd 在 [low, high] 区间反复工作,平时 free 落在 high 附近是正常状态。

### E4 memblock

1. `memblock.memory` 记录"哪些物理范围可用";`memblock.reserved` 记录"哪些已被占用、不能碰"。可用内存 = memory − reserved。
2. 内核镜像、initrd、DTB 都是已占用内存:不能当空闲页分出去,否则代码和数据会被覆盖,必须 reserve。
3. `memblock_free_all()` 把 memory 中未被 reserved 的页**逐块释放给 buddy allocator**(调 `free_page` 系列进入伙伴系统)。它之后 memblock 基本"退役",长期分配交给 buddy。
4. 因为 buddy 自身需要内存来初始化:zone 结构、free_area 链表、struct page 数组都需要内存;在它们建立之前无法用 buddy。memblock 用简单的 region 数组记录范围,不依赖这些设施,适合引导早期。这是"先有鸡还是先有蛋"问题的经典解法:先有最小可用分配器,再用它搭建正式分配器。

### E5 buddy 拆分与合并

1. 初始 order=4(16 页 [0..15])。请求 order=1:从 order=4 拆起:
   - order4 [0..15] → 拆成两个 order3:[0..7]、[8..15],后者挂回 order3 链表;
   - order3 [0..7] → 拆成两个 order2:[0..3]、[4..7],后者挂回 order2 链表;
   - order2 [0..3] → 拆成两个 order1:[0..1]、[2..3],后者挂回 order1 链表;
   - **保留低半部分 [0..1] 交付分配**(`expand()` 只把高半部分放回链表)。
2. 释放 [0..1]:其伙伴是 `[0..1] ^ (1<<1) = [2..3]`。若 [2..3] 空闲(order1)→ 合并成 order2 [0..3];再查伙伴 `[0..3] ^ (1<<2) = [4..7]`,若空闲 → order3 [0..7];再查 `[0..7] ^ (1<<3) = [8..15]`,若空闲 → order4 [0..15],恢复完整块。
3. 异或 `1<<order` 就是"翻转伙伴位":同一 order 的两个块互为伙伴,地址在 order 位相差 1(即相邻且按 2^order 对齐)。异或一次得到对方地址,是硬件级的廉价运算——这正是 buddy 高效的原因。

### E6 GFP_KERNEL vs GFP_ATOMIC

1. 本质差别:**能否睡眠、能否走慢路径(direct reclaim / compaction / OOM)**。GFP_KERNEL 允许睡眠,分配失败可唤醒回收线程甚至直接回收;GFP_ATOMIC 不允许睡眠。
2. `GFP_ATOMIC = GFP_NOWAIT | __GFP_HIGH`,它没有 `__GFP_DIRECT_RECLAIM`(直接回收需要睡眠),只有 `__GFP_KSWAPD_RECLAIM`(唤醒后台线程不睡眠)。
3. 中断/自旋锁上下文禁止睡眠:睡眠意味着调度器切换任务,但当前上下文不能切换(中断栈、锁未释放),一旦睡眠会导致死锁/系统崩溃。`GFP_ATOMIC` 承诺不睡眠,所以只从空闲页 + 紧急预留(reserve)里取。
4. 不能回收就不能"创造"内存,可用来源少,所以失败率更高。它的"额外手段"是 `ALLOC_HARDER` + 使用 watermark min 以下的 reserve 池(紧急预留),以及唤醒 kswapd 等后台帮忙,但**等不到**回收结果。

### E7 水位线与 buddyinfo

1. order 越高块越大、越稀有:高阶块是低阶块合并的产物,长期运行中碎片化使高阶块急剧减少;内核大量分配 order=0,且高频分配释放导致 order0 被拆分后难以合并回高阶。数量差可以到几个数量级。
2. `drop_caches` 释放 clean 的 page cache/slab 可回收部分,`free` 的 available 增加,`buddyinfo` 的 order=0 增多(低阶块主要来自刚回收的 4KB 页)。注意它只影响可回收缓存,不影响 anon 页。
3. 大量高阶块 + 少量低阶块说明内存"整"但碎片化分配多(或刚做过 compaction)。order0 频繁短缺会导致 PCP 列表也紧张、分配需要走慢路径,表现为分配延迟上升。

### E8 mmap 懒分配

1. 第一次暂停:`VmSize` ≈ 128MB(地址空间已承诺),`Rss` ≈ 0(一个物理页都没占)。
2. 写完后:Rss ≈ 128MB(32768 个页全部真实分配)。
3. mmap() 只建立了 **VMA(虚拟区间描述)**,即"承诺了这个区间存在且可用",没有分配任何物理页;物理页是**首次访问(page fault)时按需分配**的——这就是 demand paging / lazy allocation。

### E9 VMA 与 PTE

1. 一个 VMA 覆盖一段连续的虚拟区间(可能几十 MB),一个 PTE 只映射一个 4KB 页;一个 VMA 可覆盖成千上万个 PTE。
2. VMA:范围(start/end)、权限(VM_READ/WRITE/EXEC)、文件对象 + 偏移(文件映射)、flags(私有/共享、栈/堆…);PTE:物理页帧号、权限位、Present/Dirty/Accessed 等硬件位。
3. "有 VMA 无 PTE":mmap 后未访问的区间——VMA 已建,页表项为空;访问时才建立 PTE。这是懒分配的直接证据。
4. `find_vma()` 判断"这个地址是否落在某个合法的 VMA 区间内",回答"这次访问合法吗"。合法 → 按 VMA 类型补齐页表;非法(空指针、越界)→ 直接 SIGSEGV。它是 fault 处理的第一道关卡。

### E10 进程 VMA 布局

1. 可执行文件映射:通常 `r-xp` 且带可执行文件路径(如 /usr/bin/xxx);共享库:类似但路径在 /lib;堆 `[heap]`:`rw-p`,从 brk 开始;栈 `[stack]`:`rw-p`,在高地址(向下生长);匿名映射:无文件路径的 `rw-p`(malloc 大块、线程栈等)。
2. 堆和栈之间隔着巨大的空洞,因为地址空间是"承诺"的,堆可以向上长、栈向下长,中间留给 mmap 和其他库,物理页只在用到时分配——空洞只是虚拟的,不占内存。
3. 可以共享:两个进程映射同一个 so,只要页是只读的(或 COW),内核会通过 page cache 让它们指向**同一个物理页**;写库时靠 COW 或私有映射复制。

### E11 minor fault

1. `minor-faults` ≈ 32768(写 32768 个页,每页首次写触发一次 anonymous page fault;该页尚未映射,PTE 缺失,但页表/页已在内存中,属 minor)。会有少量额外 minor fault(程序加载、栈、运行时本身)。
2. 没有 major fault:全程不读磁盘,所有页都在内存中即时分配;major fault 只出现在需要从磁盘读数据(文件映射首次读、swap in)时。
3. 文件映射首次读 → 页不在 page cache → 需要读磁盘 → **major fault**(页表建立 + IO 等待)。
4. page fault 是"按需分配/按需加载"的执行点,是虚拟内存的**正常机制**:没有它,mmap 必须预先分配全部页(浪费)、文件映射无法按需读(慢)。所以它叫"缺页"但不等于"错误"。

### E12 四种缺页

| 场景 | 入口 | 做了什么 |
|---|---|---|
| 首次写匿名页 | `do_anonymous_page()` | 分配一个物理页,填 0 语义,建立 PTE 并置写权限 |
| 首次读文件映射页 | `do_fault()` → `filemap_fault()` | 查 page cache,miss 则发起磁盘读;命中则建 PTE |
| fork 后写共享只读页 | `do_wp_page()` → `wp_page_copy()` | 复制物理页,更新当前进程 PTE 指向新页并置写 |
| swap 出的匿名页被访问 | `do_swap_page()` | 从 swap 区读回页,必要时重建匿名映射,清除 swap entry |

1. 见上表。
2. **读**匿名页:直接映射全局 **zero page**(共享的零页,不分配),PTE 无写权限——省内存;**写**匿名页:分配新物理页(因为要写),置写权限。所以"只读不写"的大段匿名内存几乎不占物理内存。
3. `find_vma()` 找不到对应 VMA(或权限不符)→ 内核判定非法访问,向进程发 **SIGSEGV**(用户态)或 oops(内核态)。fault 处理里"合法就补页表,非法就杀进程"是核心分水岭。

### E13 Sv39 拆分

```
addr = 0x0000_0000_4000_1234
offset    = addr & 0xfff            = 0x234
VPN[0]    = (addr >> 12) & 0x1ff    = (0x40001) & 0x1ff = 1
VPN[1]    = (addr >> 21) & 0x1ff    = (0x200) & 0x1ff   = 0
VPN[2]    = (addr >> 30) & 0x1ff    = (1) & 0x1ff       = 1
```

1. 结果:VPN[2]=1, VPN[1]=0, VPN[0]=1, offset=0x234。
2. Sv39 实际是**三级**页表:一级目录页表(PGD,由 VPN[2] 索引)→ 二级目录(PMD,由 VPN[1] 索引)→ 三级目录/页表(PTE,由 VPN[0] 索引),最后 offset 定位页内。x86 的四级是 PGD→PUD→PMD→PTE,多一级 PUD。
3. `0xffffffc000000000` 的高位全是 1(VPN[2] 接近 0x1ff 甚至更高位,因为 Sv39 虚拟地址只有 39 位,VPN[2] 最大 0x1ff,内核用高位全 1 表示地址空间属性,如线性映射区标志)。这说明内核和用户共用一套 39 位地址空间,靠高地址半区(或标志位)区分;三级页表从同一个根(swapper_pg_dir 或用户根)向下,通过高位字段分开。

### E14 fork COW

1. fork 后、写入前:父子**共享**全部 16MB 物理页(PTE 被置为只读),两者 RSS 加起来 ≈ 16MB,不翻倍——这就是"fork 不复制物理页"。
2. 双方各自写入后:每个进程写自己的页都触发 COW(复制),各自 RSS ≈ 16MB,物理内存多占 ≈ 16MB(原共享页 + 父新页 + 子新页,共约 3×?——准确说:初始 16MB 共享,父写 16MB 页各复制一次、子写 16MB 各复制一次,最终物理占用约 16+16=32MB,即两份;初始共享页在两进程写入后各自变成私有新页)。观察点:Private_Dirty 在写前后翻倍。
3. 多出来的内存由**写时复制(COW)按需触发**:谁写谁触发 write-protect fault(`do_wp_page`),只复制正在写的那一页。写多少复制多少。
4. 不浪费:fork 后子进程直接 exec 时,exec 会**替换整个地址空间**(重建 mm),所有未写的共享页原样保留、子进程的页被丢弃,COW 避免了"先复制再丢弃"。这也是"fork + exec"组合廉价的原因。若 exec 前子进程写了少量页,也只复制了那几页。

### E15 refcount / mapcount / rmap

1. `_refcount` 回答"这个物理页现在被多少个使用者持有着"(内核引用计数,含映射、slab、page cache、内核自身等);`_mapcount` 回答"有多少个用户态 PTE 映射了它"。
2. 3 个进程共享映射:mapcount = 3;refcount ≥ 3(可能还有 page cache 等额外引用,通常正好等于 3 + 额外引用)。
3. rmap(anon_vma / mapping)回答"**谁**映射了这页",即从物理页反查所有 PTE 位置。reclaim 要 unmap 页面、migration 要改映射目标,都必须先知道所有映射点,否则改了一处漏了别处。
4. mapcount 用负数(如 -1)表示"未被任何 PTE 映射",因为 0 被约定为"已被某个引用占据/无映射"的过渡态,用负数可以直接复用"非零即被映射"的判断,且与 refcount 的语义区分开。

### E16 slabinfo

1. `objsize` 就是每个对象实际大小,size class 从 8B 到若干 KB(通常 8/16/32/…/4096/8192),每档一个 kmalloc cache。
2. 内核对象大小不一(几字节到几 KB),直接按页(4KB)分配会大量浪费;size class 让每个对象占用刚好合适的槽位,同时 slab 批量管理页内对象,分配释放快、碎片少。档位是"时间换空间、空间换时间"的折中。
3. `struct task_struct` 通常几 KB → 落到 kmalloc-4k/8k 或专门的 task_struct cache;`struct page` 由 vmemmap 数组管理(不是 kmalloc)。
4. `kmalloc(3000)` → 落到 kmalloc-4096 档,浪费 4096−3000 ≈ 1KB。size class 幂次化必然有内部浪费,但换来分配 O(1)。

### E17 kmalloc / vmalloc / alloc_pages

1. `kmalloc(8KB)`:物理连续、虚拟连续(直接映射区或 slab 页),可直接做 DMA;`vmalloc(8KB)`:只保证虚拟连续,物理页可能分散,页表需单独建立。
2. DMA 需要**物理连续**的内存(设备按物理地址访问,只能跨越有限范围),vmalloc 的物理页分散在任意位置,无法满足。
3. 超过 slab 上限(约 8KB~32KB,看配置)的 kmalloc 会退化为从 buddy 直接分配(`alloc_pages`)连续页,所以 `kmalloc(2MB)` 本质 ≈ `alloc_pages(order=9)`,只是接口语义不同。
4. 内核栈要**物理连续 + 快速**且访问极频繁,用 vmalloc 会增加 TLB 和页表开销;`alloc_pages(THREAD_SIZE_ORDER)` 一次拿到连续页,直接映射区访问,代价最低。

### E18 两条主干链路(验收)

1. 内存初始化链:
   - `DTB`(memory 节点/保留内存)→ `memblock_add()` 填充 `memblock.memory`(记录可用范围)
   - `memblock_reserve()` 排除内核/initrd/dtb/页表等(记录不可碰)
   - `sparse_init()`:建立 `struct page` 数组(vmemmap)(解决"每个物理页一个描述符")
   - `free_area_init()` / `zone_sizes_init()`:初始化 node/zone、watermark、free_area 链表(解决"分层管理")
   - `memblock_free_all()`:把剩余页逐块释放进 buddy(解决"交接给正式分配器")
   - 之后 `alloc_pages()` 走 buddy。
2. 匿名页访问链:
   - `mmap()` → `do_mmap()` → `mmap_region()` 建立 VMA(只有描述,无物理页)
   - 首次写入 → CPU 缺页异常 → `arch/riscv/mm/fault.c` `do_page_fault()` → `find_vma()` 校验合法性
   - `handle_mm_fault()` → `__handle_mm_fault()` → `handle_pte_fault()` → `do_anonymous_page()`
   - `do_anonymous_page()`:分配物理页(`alloc_page_vma` / zero page 逻辑)→ `pte_alloc` 建页表 → `set_pte_at` 写入 PTE
   - 返回用户态,指令重放执行成功。
   - 对比规划第二十节验收图,缺哪一步就回去看哪篇文章(常见缺口:sparse_init 的角色、zero page 的读路径、handle_pte_fault 的分流)。

---

## 做完之后

1. 把做错的 / 卡住的题号记下来,这就是你下一轮要重点读的文章清单。
2. 把答案或实验输出发给我,我帮你批改、补充和纠正。
3. 全部过关后,就可以按规划进入**阶段 6(fork/COW/rmap)源码主线**了——E14/E15 已经给你埋好了伏笔。
