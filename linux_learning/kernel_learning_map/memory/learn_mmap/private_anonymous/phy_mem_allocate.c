// 当前的内存分配函数是针对内核态的内存分配，主要用于内核模块和驱动程序的内存需求。对于用户态的内存分配，尤其是高页（highpage）和可移动（movable）的内存分配，需要使用专门的函数来处理。
// 这个函数的主要作用是为用户态的高页和可移动内存分配一个零初始化的物理页，并返回该页的指针。它会根据传入的虚拟内存区域（vma）和虚拟地址（vaddr），选择合适的内存分配策略和标志（gfp_mask），然后调用内核的内存分配函数来实际分配物理页。

struct page *page = alloc_zeroed_user_highpage_movable(struct vm_area_struct *vma,
                                                       unsigned long vaddr)
    alloc_page_vma(flags, vma, vaddr) // provided by "linux/gfp.h" 最终展开为alloc_pages函数
    // 精确说明(!CONFIG_NUMA 时):include/linux/gfp.h:599 把 alloc_pages_vma 宏定义为
    //     #define alloc_pages_vma(gfp_mask, order, vma, addr, node, false)  alloc_pages(gfp_mask, order)
    //   vma / addr / node 三个参数全部被丢弃;order 由 alloc_page_vma 宏(gfp.h:605)固定传 0
    //   → 伙伴系统只分配一个 4KB 的 page。
    //   CONFIG_NUMA 打开时,vma/addr 才参与 mempolicy 决策(用于选 node)。
    alloc_pages(gfp_t gfp_mask, unsigned int order); // linux/gfp.h
        alloc_pages_node(numa_node_id(), gfp_mask, order);
            __alloc_pages_node(nid, gfp_mask, order);
                // This is the 'heart' of the zoned buddy allocator.
                __alloc_pages(gfp_mask, order, nid, NULL);
                    // 这个函数是伙伴系统的核心分配函数。里面核心的三个函数分别为：
                    // 下面函数主要作用是设置gfp flag和初始化struct alloc_context ac结构体，ac结构体用于在不同内存分配辅助函数中传递内存分配参数。
                    // prepare_alloc_pages()
                    // 下面函数主要作用是从伙伴系统的空闲页链表中查找合适的空闲页，并进行分配。如果没有找到合适的页，则会进入慢路径，尝试回收内存或者触发OOM。
                    // get_page_from_freelist()
                    // 下面函数主要作用是处理分配失败的情况，包括尝试回收内存、触发OOM等。一旦进入慢速路径，则情况变得复杂，可能会涉及到内存回收、等待、唤醒等操作。
                    // __alloc_pages_slowpath()
                    prepare_alloc_pages()
                    // 首先尝试快速路径，也就是从freelist中查找
                    page = get_page_from_freelist(alloc_gfp, order, alloc_flags, &ac);
                    {
                        // 虽然 get_page_from_freelist 函数的代码比较冗长，但是其核心逻辑比较简单，主干框架就是通过 for_next_zone_zonelist_nodemask 来遍历当前 NUMA 节点以及备用节点的所有内存区域（zonelist），然后逐个通过 zone_watermark_fast 检查这些内存区域 zone 中的剩余空闲内存容量是否在指定的水位线 mark 之上。如果满足水位线的要求则直接调用 rmqueue 进入伙伴系统分配内存，分配成功之后通过 prep_new_page 初始化分配好的内存页 page。

                        // 如果当前正在遍历的 zone 中剩余空闲内存容量在指定的水位线 mark 之下，就需要通过 node_reclaim 触发内存回收，随后通过 zone_watermark_ok 检查经过内存回收之后，内核是否回收到了足够的内存以满足本次内存分配的需要。如果内存回收到了足够的内存则 `zone_watermark_ok = true` 随后跳转到 try_this_zone 分支在本内存区域 zone 中分配内存。否则继续遍历下一个 zone。
                        // 补充两个前置条件(5.15):①带 ALLOC_NO_WATERMARKS 的分配直接 goto try_this_zone,不检查水位线;②只有 node_reclaim_enabled() 为真才会真正调 node_reclaim,否则直接 try_this_zone。这套水位线/回收的细节属于 slowpath,留到 M3(Q7)再展开。
                        retry:
                        for_next_zone_zonelist_nodemask()
                        {

                        try_this_zone:
                            // 
                            page = rmqueue(ac->preferred_zoneref->zone, zone, order,
                                           gfp_mask, alloc_flags, ac->migratetype);
                            {
                                // 1.首先从pcplist中尝试分配
                                if (likely(pcp_allowed_order(order))) {
                                    // 在5.15内核中 order <= PAGE_ALLOC_COSTLY_ORDER(展开为3)时，也就是申请1~8个page时，可以直接从pcplist中分配内存页，这是一种快速分配路径。pcplist是每个CPU维护的一个小型空闲页链表，主要用于快速分配小块内存，避免频繁访问全局的freelist。pcp: per cpu pageset
                                    page = rmqueue_pcplist(preferred_zone, zone, order, \
                                        gfp_flags, migratetype, alloc_flags);
                                    //  pcplist 空时并不是"返回 NULL 后再去 freelist 重试":
                                    //   __rmqueue_pcplist() 内部(mm/page_alloc.c:3602)已经做了补充动作:
                                    //     if (list_empty(list))
                                    //         alloced = rmqueue_bulk(zone, order, batch, list, migratetype, alloc_flags);
                                    //     // ↑ 直接从 buddy 批量搬 batch 个页进 pcp
                                    //     补充后 list 仍空 → 才 return NULL
                                    //   此处 goto out:成功或失败都跳出,order<=3 永远不会落到下面的 __rmqueue()。
                                    //   失败时由上层 get_page_from_freelist 换下一个 zone,或最终进 slowpath。
                                    goto out;
                                }
                                // 【可达性】能走到这里的只有 order > 3:
                                //   order <= 3 已在上面的 pcp_allowed_order 分支 goto out,
                                //   所以 MIGRATE_HIGHATOMIC(高原子紧急预留池)只服务高阶分配。
                                if (order > 0 && alloc_flags & ALLOC_HARDER) {
                                    // 2.高阶 + 紧急分配:先从 MIGRATE_HIGHATOMIC 的链表找
                                    page = __rmqueue_smallest(zone, order, MIGRATE_HIGHATOMIC);
                                    if (!page)
                                    // 3.最后到这里，需要走伙伴系统的常规分配流程
                                        page = __rmqueue(zone, order, migratetype, alloc_flags);
                                }
                            }
                            if (page) {
                                // 如果rmqueue()函数成功分配到了page，那么就完成page的初始化工作
                                prep_new_page(page, order, gfp_mask, alloc_flags);
                            }
                        }
                    }
                    if (likely(page))
                        goto out;
                    // 如果快速分配路径失败，则会执行下面函数进入慢速路径
                    // 慢速路径的内存分配情况非常复杂，可能会涉及到内存回收、等待、唤醒等操作。慢速路径的主要目标是尝试释放一些内存，以便满足当前的分配请求。如果在慢速路径中仍然无法分配到内存页，则可能会触发OOM（Out Of Memory）处理机制。
                    page = __alloc_pages_slowpath(alloc_gfp, order, &ac);
