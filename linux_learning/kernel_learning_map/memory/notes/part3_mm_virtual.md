

# 1. 如何划分用户空间和内核空间

```c
struct mm_struct {
	unsigned long task_size;	/* size of task vm space */
}
```

mm->*task_size* = TASK_SIZE;

内核根据task_size来划分用户/内核虚拟内存空间. 

用户虚拟内存空间:[0, mm->task_size-1]

内核虚拟内存空间:[mm->task_size, 2*mm->task_size-1]

![img](https://pic3.zhimg.com/v2-2865347469b8b70955b6f219acd3e3c6_1440w.jpg)



# 2. 内核如何布局进程虚拟内存空间区域

虚拟内存空间区域(VMA, Virtual Memory Area)

```c
struct mm_struct {
    struct vm_area_struct *mmap;
	unsigned long start_code, end_code, start_data, end_data;
    unsigned long start_brk, brk, start_stack;
    unsigned long arg_start, arg_end, env_start, env_end;
}
```

![img](https://pica.zhimg.com/v2-31dbb9021b193db4b451f04ef9fe4fc4_1440w.jpg)

```c
struct vm_area_struct {

 unsigned long vm_start;  /* Our start address within vm_mm. */
 unsigned long vm_end;  /* The first byte after our end address
        within vm_mm. */
 /*
  * Access permissions of this VMA.
  */
 pgprot_t vm_page_prot;
 unsigned long vm_flags; 

 struct anon_vma *anon_vma; /* Serialized by page_table_lock */
    struct file * vm_file;  /* File we map to (can be NULL). */
 unsigned long vm_pgoff;  /* Offset (within vm_file) in PAGE_SIZE
        units */ 
 void * vm_private_data;  /* was vm_pte (shared mem) */
 /* Function pointers to deal with this struct. */
 const struct vm_operations_struct *vm_ops;
}
```

每个 vm_area_struct 结构对应于虚拟内存空间中的唯一虚拟内存区域 VMA，vm_start 指向了这块虚拟内存区域的起始地址（最低地址），vm_start 本身包含在这块虚拟内存区域内。vm_end 指向了这块虚拟内存区域的结束地址（最高地址），而 vm_end 本身包含在这块虚拟内存区域之外，所以 vm_area_struct 结构描述的是 [vm_start，vm_end) 这样一段左闭右开的虚拟内存区域。

![img](https://picx.zhimg.com/v2-647d093b59b247c701c678d84e9bec19_1440w.jpg)

# 3. 虚拟内存区域在内核中是如何被组织的

```c
struct mm_struct {
    struct vm_area_struct *mmap;  /* list of VMAs */
    struct rb_root mm_rb; /*红黑树*/
}

struct vm_area_struct {
	struct vm_area_struct *vm_next, *vm_prev; /*双向链表*/
    struct rb_node vm_rb; /*红黑树*/
}
```

vm_area_struct 结构中的 vm_next ，vm_prev 指针分别指向 VMA 节点所在双向链表中的后继节点和前驱节点，内核中的这个 VMA 双向链表是有顺序的，所有 VMA 节点按照低地址到高地址的增长方向排序。

双向链表中的最后一个 VMA 节点的 vm_next 指针指向 NULL，双向链表的头指针存储在内存描述符 struct mm_struct 结构中的 mmap 中，正是这个 mmap 串联起了整个虚拟内存空间中的虚拟内存区域。

![img](https://picx.zhimg.com/v2-1be29d6871fdf9f75e8c2a81ad910451_1440w.jpg)

我们可以通过 `cat /proc/pid/maps` 或者 `pmap pid` 查看进程的虚拟内存空间布局以及其中包含的所有内存区域。这两个命令背后的实现原理就是通过遍历内核中的这个 vm_area_struct 双向链表获取的。

内核中关于这些虚拟内存区域的操作除了遍历之外还有许多需要根据特定虚拟内存地址在虚拟内存空间中查找特定的虚拟内存区域。

尤其在进程虚拟内存空间中包含的内存区域 VMA 比较多的情况下，使用[红黑树](https://zhida.zhihu.com/search?content_id=216363268&content_type=Article&match_order=1&q=红黑树&zhida_source=entity)查找特定虚拟内存区域的时间复杂度是 O( logN ) ，可以显著减少查找所需的时间。

所以在内核中，同样的内存区域 vm_area_struct 会有两种组织形式，一种是双向链表用于高效的遍历，另一种就是红黑树用于高效的查找。

每个 VMA 区域都是红黑树中的一个节点，通过 struct vm_area_struct 结构中的 vm_rb 将自己连接到红黑树中。

而红黑树中的根节点存储在内存描述符 struct mm_struct 中的 mm_rb 中：

```c
struct mm_struct {
    struct vm_area_struct *mmap;  /* list of VMAs */
    struct rb_root mm_rb; /*红黑树*/
}

struct vm_area_struct {
	struct vm_area_struct *vm_next, *vm_prev; /*双向链表*/
    struct rb_node vm_rb; /*红黑树*/
}
```

![img](https://pic1.zhimg.com/v2-7b507ab9e98aa4285a39694b598fba96_1440w.jpg)

# 4. 程序编译后的二进制文件如何映射到虚拟内存空间中

 ELF 格式的二进制文件中的布局和我们前边讲的虚拟内存空间中的布局类似，也是一段一段的，每一段包含了不同的元数据。

> 磁盘文件中的段我们叫做 Section，内存中的段我们叫做 Segment，也就是内存区域。

磁盘文件中的这些 Section 会在进程运行之前加载到内存中并映射到内存中的 Segment。通常是多个 Section 映射到一个 Segment。

比如磁盘文件中的 .text，.rodata 等一些只读的 Section，会被映射到内存的一个只读可执行的 Segment 里（代码段）。而 .data，.bss 等一些可读写的 Section，则会被映射到内存的一个具有读写权限的 Segment 里（数据段，BSS 段）。

那么这些 ELF 格式的二进制文件中的 Section 是如何加载并映射进虚拟内存空间的呢？

内核中完成这个映射过程的函数是 load_elf_binary ，这个函数的作用很大，加载内核的是它，启动第一个用户态进程 init 的是它，fork 完了以后，调用 exec 运行一个二进制程序的也是它。当 exec 运行一个二进制程序的时候，除了解析 ELF 的格式之外，另外一个重要的事情就是建立上述提到的内存映射。

```c
static int load_elf_binary(struct linux_binprm *bprm)
{
      ...... 省略 ........
  // 设置虚拟内存空间中的内存映射区域起始地址 mmap_base
  setup_new_exec(bprm);

     ...... 省略 ........
  // 创建并初始化栈对应的 vm_area_struct 结构。
  // 设置 mm->start_stack 就是栈的起始地址也就是栈底，并将 mm->arg_start 是指向栈底的。
  retval = setup_arg_pages(bprm, randomize_stack_top(STACK_TOP),
         executable_stack);

     ...... 省略 ........
  // 将二进制文件中的代码部分映射到虚拟内存空间中
  error = elf_map(bprm->file, load_bias + vaddr, elf_ppnt,
        elf_prot, elf_flags, total_size);

     ...... 省略 ........
 // 创建并初始化堆对应的的 vm_area_struct 结构
 // 设置 current->mm->start_brk = current->mm->brk，设置堆的起始地址 start_brk，结束地址 brk。 起初两者相等表示堆是空的
  retval = set_brk(elf_bss, elf_brk, bss_prot);

     ...... 省略 ........
  // 将进程依赖的动态链接库 .so 文件映射到虚拟内存空间中的内存映射区域
  elf_entry = load_elf_interp(&loc->interp_elf_ex,
              interpreter,
              &interp_map_addr,
              load_bias, interp_elf_phdata);

     ...... 省略 ........
  // 初始化内存描述符 mm_struct
  current->mm->end_code = end_code;
  current->mm->start_code = start_code;
  current->mm->start_data = start_data;
  current->mm->end_data = end_data;
  current->mm->start_stack = bprm->p;

     ...... 省略 ........
}
```

- setup_new_exec 设置虚拟内存空间中的内存映射区域起始地址 mmap_base
- setup_arg_pages 创建并初始化栈对应的 vm_area_struct 结构。置 mm->start_stack 就是栈的起始地址也就是栈底，并将 mm->arg_start 是指向栈底的。
- elf_map 将 ELF 格式的二进制文件中.text ，.data，.bss 部分映射到虚拟内存空间中的代码段，数据段，BSS 段中。
- set_brk 创建并初始化堆对应的的 vm_area_struct 结构，设置 `current->mm->start_brk = current->mm->brk`，设置堆的起始地址 start_brk，结束地址 brk。 起初两者相等表示堆是空的。
- load_elf_interp 将进程依赖的动态链接库 .so 文件映射到虚拟内存空间中的内存映射区域
- 初始化内存描述符 mm_struct



# 5. 内核虚拟内存空间

https://zhuanlan.zhihu.com/p/577035415
