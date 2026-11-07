/*
 * trigger_alloc.c —— 触发匿名页缺页分配,用于 ftrace 观测 buddy/PCP 路径
 *
 * 链路:写匿名页 → do_anonymous_page → alloc_zeroed_user_highpage_movable
 *       → alloc_page_vma(order=0) → __alloc_pages → get_page_from_freelist
 *       → rmqueue → rmqueue_pcplist → __rmqueue_pcplist → rmqueue_bulk
 *
 * 用法:
 *   ./trigger_alloc [页数] [--huge]
 *   默认 64 页(256KB),每页首字节写一次 = 一次缺页 = 一次 order-0 分配
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

int main(int argc, char **argv)
{
    int npages = 64;
    int huge = 0;

    if (argc > 1)
        npages = atoi(argv[1]);
    if (argc > 2 && strcmp(argv[2], "--huge") == 0)
        huge = 1;
    if (npages <= 0)
        npages = 64;

    size_t sz = (size_t)npages * PAGE;

    printf("pid=%d npages=%d size=%zuKB\n", getpid(), npages, sz / 1024);
    fflush(stdout);

    /*
     * 阶段 1:mmap 只建 VMA,零物理页。
     * 此时 /proc/self/smaps 的 Rss 应为 0。
     */
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("MARK mmap_done base=%p addr=%p\n", (void *)p, (void *)&p[0]);
    fflush(stdout);

    if (huge) {
        /* 可选:让 THP 参与,观察 order=9 分配(需系统启用 THP) */
        madvise(p, sz, MADV_HUGEPAGE);
        printf("MARK madvise_hugepage\n");
        fflush(stdout);
    }

    /*
     * 阶段 2:逐页写。每一步触发一次写缺页:
     *   匿名 VMA + PTE 空 + 写 → do_anonymous_page
     *   物理页来源 = alloc_zeroed_user_highpage_movable(gfp_mask, vma, vaddr)
     * 在 ftrace 上应看到 npages 条 mm_page_alloc,order=0,migratetype=1(Movable)
     *
     * 先等 300ms:给 trace_events.sh 留出设置 set_event_pid 的时间,
     * 这样 trace 里只有本进程的事件,不会被系统里其他线程(ThreadPool 等)淹没。
     */
    printf("MARK ready_for_trace\n");
    fflush(stdout);
    usleep(300 * 1000);

    for (int i = 0; i < npages; i++) {
        p[(size_t)i * PAGE] = (char)(i & 0xff);
        /* 只打印前几次和最后一次,避免刷屏干扰 trace 时间线 */
        if (i < 3 || i == npages - 1) {
            printf("MARK touch page[%d] addr=%p\n", i, (void *)&p[(size_t)i * PAGE]);
            fflush(stdout);
        }
    }

    printf("MARK done\n");
    fflush(stdout);

    /* 保持进程存活一小会儿,方便外部抓取 smaps / 观察 trace */
    sleep(1);
    return 0;
}
