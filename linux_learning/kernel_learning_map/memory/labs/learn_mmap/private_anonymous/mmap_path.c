// mmap系统调用分配虚拟地址空间,没有建立和物理内存的链接.在缺页异常中建立
SYSCALL_DEFINE6()   // sys_riscv.c
    riscv_sys_mmap()    // sys_riscv.c
        ksys_mmap_pgoff()  // mm/mmap.c
            vm_mmap_pgoff()  // mm/util.c
                do_mmap()    // mm/mmap.c
                {
                    // 如果addr=0，则调用get_unmapped_area()获取一个未映射的地址
                    addr = get_unmapped_area()  // mm/mmap.c
                    mmap_region()  // mm/mmap.c
                    {
                        // 分配VMA结构体空间,没有真正的分配物理内存.物理内存在访问申请的内存时通过缺页异常来分配
                        struct vm_area_struct *vma = vm_area_alloc()
                        {
                            vma->vm_start = addr;
                            vma->vm_end = addr + len;
                            vma->vm_flags = vm_flags;
                            vma->vm_page_prot = vm_get_page_prot(vm_flags);
                            vma->vm_pgoff = pgoff;
                        }
                        vma_link()  // mm/mmap.c
                            __vma_link()  // mm/mmap.c
                            {
                                // 将新的VMA插入到mm->mmap链表中，并更新红黑树
                                __vma_link_list(mm, vma, prev);
	                            __vma_link_rb(mm, vma, rb_link, rb_parent);
                            }
                            mm->map_count++;
                        vm_stat_account();  // mm/mmap.c
                    }  // end mmap_region()
                } // end do_mmap()

// 访问mmap分配的虚拟地址空间时,会触发缺页异常,在缺页异常中分配物理内存
do_page_fault()  // arch/riscv/mm/fault.c
    // 这里的addr就是触发缺页异常的虚拟地址
    addr = regs->badaddr;
    vma = find_vma(mm, addr);  // mm/mmap.c
    handle_mm_fault()  // mm/memory.c
        __handle_mm_fault()  // mm/memory.c
            pgd = pgd_offset(mm, address);
	        p4d = p4d_alloc(mm, pgd, address);
            vmf.pud = pud_alloc(mm, p4d, address);
            vmf.pmd = pmd_alloc(mm, vmf.pud, address);
            handle_pte_fault()  // mm/memory.c
                vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
                {
                    if (!vmf->pte) {
                        if (vma_is_anonymous(vmf->vma))
                            return do_anonymous_page(vmf);
                        else
                            return do_fault(vmf);
                    }
                }
                do_anonymous_page()  // mm/memory.c
                {
                    // 如果pte不存在,说明还没有分配物理内存,则分配一个物理页,并建立虚拟地址和物理页的映射
                    anon_vma_prepare(vma)  // rmap.h
                    {
                        // 这个函数完成avc和anon_vma的分配和链接
                        if (likely(vma->anon_vma))
                            return 0;
                        __anon_vma_prepare(vma)  // mm/rmap.c
                        {
                            struct anon_vma_chain *avc = anon_vma_chain_alloc(GFP_KERNEL)  // mm/rmap.c
                            struct anon_vma *anon_vma = find_mergeable_anon_vma(vma);
                            if (!anon_vma) {
                                anon_vma = anon_vma_alloc();
                            }
                            // 建立vma和anon_vma的链接
                            vma->anon_vma = anon_vma;
                            anon_vma_chain_link(vma, avc, anon_vma);  // mm/rmap.c
                            {
                                // 建立avc和vma的链接
                                avc->vma = vma;
                                // 建立avc和anon_vma的链接
                                avc->anon_vma = anon_vma;
                                // 将新创建的avc插入到vma->anon_vma_chain链表中.
                                // 如果是子进程的vma,则会在fork时调用anon_vma_clone()函数将父进程的vma->anon_vma_chain链表复制到子进程的vma->anon_vma_chain链表中,这样子进程的vma就可以共享父进程的anon_vma.这一操作是在fork时完成的.
                                list_add(&avc->same_vma, &vma->anon_vma_chain);
                                // 将新创建的avc插入到anon_vma->rb_root红黑树中.
                                // 这是为了在反向查找vma时快速找到对应的avc,进而找到对应的vma.
                                // 完成PA->VA的映射
                                anon_vma_interval_tree_insert(avc, &anon_vma->rb_root);
                            }
                        }
                    }
                    // 分配物理页
                    page = alloc_zeroed_user_highpage_movable(vma, vmf->address);  // mm/memory.c
                    page_add_new_anon_rmap(page, vma, vmf->address, false);  // mm/memory.c
                        // 建立反向映射关系
                        // 物理page通过mapping找到anon_vma,anon_vma通过rb_root找到avc,
                        // avc通过avc->vma找到对应的vma.完成反向映射
                        __page_set_anon_rmap(page, vma, address, 1);  // mm/memory.c
                        {
                            // PAGE_MAPPING_ANON 这个宏定义为1,表示这是一个匿名页,不是文件映射页.
                            anon_vma = (void *) anon_vma + PAGE_MAPPING_ANON;
                            // 建立物理页和anon_vma的映射
                            WRITE_ONCE(page->mapping, (struct address_space *) anon_vma);
                            // page 结构中的 index 表示该匿名页在虚拟内存区域 vma 中的偏移
                            page->index = linear_page_index(vma, address);
                        }
                } // end do_anonymous_page()
