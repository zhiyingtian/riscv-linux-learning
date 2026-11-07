# 真实补丁实战练习 #1:VMA merge 的 use-after-free

> 来源:真实内核补丁,作者 Lorenzo Stoakes,2024-12,主线已合入(commit `42c4e4b20d9c`)
> 出处:https://lkml.org/lkml/2024/12/6/1427 | https://patchew.org/linux/20241206215229.244413-1-lorenzo.stoakes@oracle.com/
> 关联:mm12(mmap 源码实现篇,vma_merge 解析)、规划阶段 4(VMA)
> ⚠️ 版本:补丁针对 **6.13+** 的 `mm/vma.c`;你的 5.15 源码树里对应逻辑在 `mm/mmap.c` 的 `mmap_region()` / `vma_merge()`

---

## 一、补丁原文(commit message + diff)

```
mm: correctly reference merged VMA

On second merge attempt on mmap() we incorrectly discard the possibly
merged VMA, resulting in a possible use-after-free (and most certainly a
reference to the wrong VMA) in this instance in the subsequent
__mmap_complete() invocation.

Correct this mistake by reassigning vma correctly if a merge succeeds in
this case.

Suggested-by: Jann Horn <jannh@google.com>
Reported-by: syzbot+91cf8da9401355f946c3@syzkaller.appspotmail.com
Closes: https://lore.kernel.org/all/67536a25.050a0220.a30f1.0149.GAE@google.com/
Fixes: 5ac87a885aec ("mm: defer second attempt at merge on mmap()")
Signed-off-by: Lorenzo Stoakes <lorenzo.stoakes@oracle.com>
```

```diff
diff --git a/mm/vma.c b/mm/vma.c
index 8a454a7bbc80..1ec349141f5e 100644
--- a/mm/vma.c
+++ b/mm/vma.c
@@ -2460,10 +2460,13 @@ unsigned long __mmap_region(struct file *file, unsigned long addr,

 	/* If flags changed, we might be able to merge, so try again. */
 	if (map.retry_merge) {
+		struct vm_area_struct *merged;
 		VMG_MMAP_STATE(vmg, &map, vma);

 		vma_iter_config(map.vmi, map.addr, map.end);
-		vma_merge_existing_range(&vmg);
+		merged = vma_merge_existing_range(&vmg);
+		if (merged)
+			vma = merged;
 	}

 	__mmap_complete(&map, vma);
```

只有 **4 行改动**:把丢弃的返回值接住,合并成功就把 `vma` 指针指过去。

---

## 二、前置知识:这段代码在干什么(5.15 对照)

`__mmap_region()`(6.13+;5.15 叫 `mmap_region()`,在 `mm/mmap.c`)是 `mmap()` 系统调用**真正安装 VMA** 的函数,流程:

```text
mmap() → do_mmap() → mmap_region()/__mmap_region()
    ├─ 分配新的 struct vm_area_struct(vma)
    ├─ 尝试和相邻 VMA 合并(vma_merge / vma_merge_existing_range)
    │    └─ 合并成功:新 vma 被"吸收",返回合并后的 VMA;旧 vma 对象被释放
    └─ __mmap_complete():用 vma 做收尾(记录 map_count、perf mmap 事件等)
```

关键点:

- **merge 成功时,原来的 `vma` 指针可能已经指向被释放的内存**(它被相邻 VMA 吸收并销毁)——所以 merge 函数的返回值是"当前真正有效的 VMA",调用者必须接住。
- `map.retry_merge` 是较新内核加的**第二次合并尝试**路径:第一次因 flags 不允许没合并成,调整后再试一次。5.15 没有这条 retry 路径——5.15 里两次 vma_merge 是平铺写的,且第二次成功后**显式** `vm_area_free(vma)` 再跳到 `out`(处理是正确的)。新版重构后把第二次尝试收进 `retry_merge` 分支,却**忘了接返回值** → 引入 bug。

**对照任务**:在 5.15 源码树 `mm/mmap.c` 里找到 `mmap_region()`,看它两处 `vma_merge()` 调用后是怎么处理 vma 的,和上面 diff 对比。

---

## 三、实战问题(先自己答,答案在文末)

### Q1:修复前为什么是错的?

- `vma_merge_existing_range()` 的返回值是什么语义?什么时候返回 NULL?
- 如果合并成功,原来的 `vma` 指针指向的内存发生了什么?
- 随后 `__mmap_complete(&map, vma)` 用到这个悬空指针,后果是什么?(提示:commit message 里两个词:use-after-free / wrong VMA)

### Q2:这个改动改了什么?动的是"地图"上哪个环节?

- 结合你 E18 画的匿名页访问链,这段代码属于哪一步的**内部细节**?
- 4 行改动修复的是"逻辑正确性"还是"性能"?
- 如果 `merged == NULL`(合并没成功),`vma = merged` 不执行,`vma` 还是原来的——为什么这是安全的?

### Q3:如果你是 reviewer,会提什么问题?

- 这个函数里还有没有别的地方调用了同类 merge 函数却丢弃返回值?(提示:可以 grep `vma_merge_existing_range` 的所有调用点)
- `Fixes: 5ac87a885aec` 这个 tag 意味着什么?为什么一定要带?
- syzbot 是什么?它找到这个 bug 说明了什么?(提示:为什么普通测试很难触发这条路径)

---

## 四、参考答案

### Q1

1. 返回合并后的**有效 VMA 指针**;没有发生合并时返回 **NULL**。
2. 合并成功 = 新 vma 被相邻 VMA 吸收,旧 vma 对象被解除链接并**释放**(内存可能已回收)。
3. `__mmap_complete()` 拿这个悬空指针读/写 → **use-after-free**(最坏,崩溃或安全漏洞);即使内存还没被复写,记录的也是**错误的 VMA**(vm_flags、地址范围不对)→ 行为错乱。commit message 说的 "most certainly a reference to the wrong VMA" 就是这层。

### Q2

1. 属于"建立 VMA"这一步的内部:mmap 不只是插入一个新 VMA,还会**尝试和邻居合并**(这正是 mm12 大篇幅讲的 vma_merge 的用武之地);合并不只是优化,还是**正确性要求**(相邻同属性 VMA 必须合并,否则 find_vma/权限检查行为不一致)。
2. 纯**正确性修复**,不是性能——4 行代码,一个指针赋值。
3. `merged == NULL` 说明没发生合并,原 `vma` 依然存活、有效,继续用它就是对的。修复的对称性:合并成功才换指针,失败保持原样。

### Q3(开放题,给思路)

- 应 grep 所有 `vma_merge_existing_range()` / `vma_merge()` 调用点,逐个确认返回值是否被处理——这类"丢弃返回值"的 bug 往往不止一处(历史上有过连环修复)。
- `Fixes:` tag 标注了引入 bug 的 commit(`5ac87a885aec`),让维护者知道要 backport 到哪些 stable 分支,是内核补丁的必备格式。
- syzbot 是内核的**自动模糊测试(bug fuzzing)基础设施**,不停用随机输入打系统调用组合。它能找到这个 bug,说明这条 `retry_merge` 路径在正常使用中**极难走到**(需要 flags 先拒绝合并、调整后再允许),只有 fuzz 的极端输入序列能触发——这也是"为什么这种 bug 能活进主线"的原因:review 和测试都很难覆盖罕见路径。

---

## 五、给你的额外收获

1. **真实 review 文化**:Vlastimil Babka 在回信里直接吐槽 "FFS, who reviewed that patch?"——补丁进主线前有人 review 过,但没人发现返回值被丢弃。说明**代码 review 也有盲区**,fuzz + 静态分析是补充。
2. **syzkaller 是你的学习工具**:https://syzkaller.appspot.com 上公开着大量 mm 相关 bug 报告,每个 report 都带 reproducer——是比文章更鲜活的"真实源码课"素材。
3. **版本漂移的实证**:同一段逻辑,5.15 是平铺的两次 vma_merge + 显式 free,6.13 是 retry_merge + vmg 结构体——你学的 5.15 不是"过时",而是理解 6.13 补丁的基线。
