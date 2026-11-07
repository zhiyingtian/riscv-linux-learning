# Linux 内存管理学习目录

> 根目录只保留本索引。新增文档按类型放进 `notes/`、`plans/`、`labs/` 等目录，避免继续堆在顶层。

## 目录结构

```text
memory/
├── README.md                 # 本索引
├── notes/                    # 自己的核心笔记
│   ├── part1_mmu_boot.md
│   ├── part2_vm_zone.md
│   ├── part3_mm_virtual.md
│   ├── part4_mm_physical.md
│   ├── 内存管理核心数据结构与地址转换.md
│   └── 内存管理知识点.md
├── plans/                    # 路线图、问题链、阶段练习
│   ├── linux_memory_framework_learning_plan.md
│   ├── memory_half_year_roadmap.md
│   ├── memory_question_chain.md
│   ├── memory_round1_exercises.md
│   └── memory_round1_weaknesses.md
├── articles/                 # 外部文章/系列原文
│   └── bin的技术小屋/
├── labs/                     # 可运行实验
│   ├── learn_mmap/
│   ├── learn_page_alloc/
│   └── cow/
├── tiny_mm/                  # tiny-mm capstone 项目
│   ├── README.md             # 总入口
│   ├── docs/                 # M0/M1 阶段文档、完整框架
│   │   ├── m0-toolchain.md
│   │   ├── m1-physical-pages.md
│   │   ├── tiny_mm_learning_framework.md
│   │   └── memory_capstone_tiny_mm.md
│   └── ...
├── exercises/                # 补丁/代码阅读练习
│   └── patch_exercise_01_vma_merge_uaf.md
├── catalog/                  # 索引类文档
│   └── memory_bug_catalog.md
└── assets/                   # 图表/绘图源文件
    ├── mm_struct_map.dot
    ├── mm_struct_map.svg
    ├── mm_struct_map.png
    ├── mmap_path.excalidraw
    └── 反向映射.excalidraw
```

## 推荐阅读顺序

1. **建立整体图**：读 [内存管理核心数据结构与地址转换.md](notes/内存管理核心数据结构与地址转换.md)，先搞清 `mm_struct → VMA → PTE → page/PFN → zone/pg_data → rmap`。
2. **查自己的路线**：看 [memory_question_chain.md](plans/memory_question_chain.md) 和 [memory_half_year_roadmap.md](plans/memory_half_year_roadmap.md)。
3. **补原理**：按需读 `notes/part1–part4`，以及 `articles/bin的技术小屋/` 的 mm1–mm13。
4. **做实验**：从 `labs/` 的四个 bug lab 开始，再进入 `tiny_mm/` 的 capstone。
5. **查 bug 类型**：遇到具体问题翻 [memory_bug_catalog.md](catalog/memory_bug_catalog.md)。

## 核心入口

- [内存管理核心数据结构与地址转换](notes/内存管理核心数据结构与地址转换.md)
- [内存管理知识点（原始问题）](notes/内存管理知识点.md)
- [tiny-mm 学习框架](tiny_mm/docs/tiny_mm_learning_framework.md)
- [tiny-mm M0/M1 实验](tiny_mm/README.md)
- [内存管理 bug 清单](catalog/memory_bug_catalog.md)

## 目录约定

- **notes/**：自己写的原理笔记、结构图说明、源码分析。
- **plans/**：路线图、问题链、阶段验收、弱项记录。
- **articles/**：外部文章原文，不混入自己的笔记。
- **labs/**：可编译/可运行的实验；每个 lab 保持自己的 README 和 Makefile。
- **tiny_mm/**：capstone 项目代码、框架、阶段报告。
- **exercises/**：补丁阅读、代码修改类练习。
- **catalog/**：索引、bug 清单、速查表。
- **assets/**：图片、SVG、Graphviz、Excalidraw 等资源。

> 如果某个实验需要移动目录，优先用 `git mv` 保留历史，并同步更新相对链接。
