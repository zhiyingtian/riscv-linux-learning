# 半年学习路线图(修订版)——以验收为节点

> 现状:已学完 RISC-V MMU 启动(part1)、bin的技术小屋 mm1–mm13、完成实验 E8/E10/E11/E13/E14、概念题 E9/E12/E15。
> 环境:宿主机 x86-64(实验)、`~/workspace/riscv/runninglinuxkernel_5.15`(5.15 源码树,与 part1 笔记同版本)。
> 方法:机制 → 源码 → 实验 三角法,一个机制 1~2 天,验收过了才进下一个。
> **导航**:先读 `memory_question_chain.md`(8 个问题 + 三档范围 + 停止条件)——那份是总纲,本文件是执行清单。

---

## 0. 小时账本(诚实版)

| 项目 | 数值 |
|---|---|
| 工作日 | 5 × 2h = 10h/周 |
| 周末(按 4~6h/天) | 8~12h/周 |
| 每周可用 | 18~22h |
| 6 个月毛时长 | 480~570h |
| 保守可用(扣除状态差/意外) | ~400h |
| 剩余总需求(含返工系数) | 200~310h |

**结论:够,且有 0.5~2 个月缓冲。前提:①"学完"= 第一轮主干贯通,不是精通;②周末有整块时间做实验;③方法不漂回"只看文章"。**

---

## 1. 每周节奏模板

### 工作日 2h(碎片时间)
```text
0:00-0:25  复习上一节点的验收图/笔记(对抗遗忘)
0:25-1:35  读当前机制源码主线(5.15 树,单条调用链,不追 corner case)
1:35-2:00  写 5 行笔记 + 标记未懂点(留给周末实验验证)
```

### 周末 4h 块(整块时间 = 记忆固化器)
```text
1h  把本周未懂点做成实验(写 C 程序 / qemu gdb 单步)
2h  画当前阶段验收链 + 对照规划检验问题
1h  下周机制预习:先看数据结构
```

---

## 2. 分阶段计划(每节点 = 目标 → 源码 → 实验 → 验收)

### M1:阶段 6 fork/COW/rmap(约 2 周,20~30h)
- **目标**:理解进程复制为何不复制物理页;COW 与反向映射
- **源码**(runninglinuxkernel_5.15 树):
  - `kernel/fork.c`:`copy_mm → dup_mm → dup_mmap → copy_page_range`
  - `mm/memory.c`:`do_wp_page → wp_page_copy`
  - `mm/rmap.c`:`anon_vma`、`page_add_anon_rmap`、`try_to_unmap`
- **实验**:E14 已做;进阶:qemu riscv + gdb 在 `do_wp_page` 断点单步
- **验收**:画出 fork COW 流程链;讲清 refcount/mapcount/rmap 三者区别(规划第 4 周产出)

### M2:阶段 7 page cache(约 1.5 周,15~20h)
- **目标**:文件 IO 为何绕不开内存;page cache 与 readahead/writeback
- **源码**:`mm/filemap.c`、`mm/readahead.c`、`mm/page-writeback.c`、`mm/truncate.c`
- **实验**:读大文件两次 + `drop_caches` 第三次,对比耗时与 Cached(规划实验 3)
- **验收**:画出 read file 的 page cache 路径;解释 dirty page 为何不能直接回收

### M3:阶段 8 reclaim/swap/OOM(约 2 周,25~35h)
- **目标**:内存压力下系统如何存活;LRU/kswapd/direct reclaim/OOM
- **源码**:`mm/vmscan.c`(kswapd → shrink_node)、`mm/swap.c`、`mm/oom_kill.c`
- **实验**:内存压力程序观察 kswapd 与 `/proc/vmstat`(规划实验 4)
- **验收**:画出 direct reclaim vs kswapd 路径;解释 file/anon LRU 差异

### M4:第一轮验收 + 补漏(约 1 周,10~15h)
- **验收标准**(规划第二十节):合上文章画出 5 条链
  - 内存初始化链 / 匿名页访问链 / fork COW 链 / 文件读取链 / 内存压力回收链
- 对照各阶段"检验问题"逐项打勾,弱点回到对应文章补读
- 产出:一张完整的第一轮主干因果图(规划第二十三节那张图)

### S1:Scheduler 主线(约 4~5 周,80~130h)
- **目标**:task_struct、调度器结构、CFS、runqueue、上下文切换、wakeup
- **源码**:`kernel/sched/core.c`、`kernel/sched/fair.c`、`include/linux/sched.h`、`arch/riscv/kernel/process.c`
- **衔接内存**:task 内核栈/slab 分配、mm 与 active_mm、切换时的地址空间切换
- **验收**:画出 schedule() → pick_next_task → context_switch 链路;解释内核线程为何无用户地址空间

### I1:Interrupt/异常(约 2~3 周,50~80h)
- **目标**:entry 流程、irqchip、softirq、workqueue、上下文差异
- **源码**:`arch/riscv/kernel/entry.S`、`kernel/irq/`、`kernel/softirq.c`
- **衔接内存**:GFP_ATOMIC 为何存在、中断上下文为何不能睡眠
- **验收**:画出一次外部中断从 trap 到 softirq 的完整路径;讲清进程上下文/中断上下文差异

### 缓冲:4~6 周(复习、返工、意外)

**总计:M1+M2+M3+M4+S1+I1 ≈ 12~15 周,加缓冲 ≈ 4~5.5 个月,半年内完成。**

---

## 3. 风险与对策

| 风险 | 对策 |
|---|---|
| 2h 碎片会话冷启动损耗 | 固定会话结构(复习→新机制→笔记),会话结束必须留笔记 |
| 遗忘曲线 | 每次会话开头 25 分钟复习上一节点验收图 |
| 方法漂回"只看文章" | 每个机制强制一个 30 分钟实验;实验是记忆锚点 |
| 版本漂移困惑 | 只读 5.15 树源码;lore 邮件只看与已学机制强相关的 fix |
| 沮丧/进度感弱 | 以节点打卡代替"学了多少文章";每过一节点画一次验收图 |
| 周末时间不足 | 若周末 <4h/天,砍范围:内存只学主线、scheduler 只学 CFS 核心 |

---

## 4. 下一步(本周)

1. **完成 E18**:画两条验收链(内存初始化链 + 匿名页访问链),5 分钟,给自己"我确实懂了"的证据。
2. **启动 M1**:读 `kernel/fork.c` 的 `copy_mm` 主线,对照 E14 实验现象。
3. 每完成一个节点,在本文档对应位置打勾。
