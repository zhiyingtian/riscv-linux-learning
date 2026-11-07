#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static void touch_pages(volatile char *p, size_t size)
{
    const size_t page_size = 4096;

    for (size_t i = 0; i < size; i += page_size) {
        p[i] = 0xA5;
    }
}

int main(void)
{
    const size_t sz = 4 * 4 * 1024;  /* 16KB, 便于观察 /proc/self/smaps */
    volatile char *p;

    puts("[1/4] 这个程序演示匿名 mmap。它不绑定任何文件，映射后由内核在页错误时分配物理页。\n");
    puts("[2/4] 先在另一个终端启动 gdb / qemu 内核调试，按回车继续...");
    fflush(stdout);
    getchar();

    p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("[3/4] mmap() 成功：base=%p, size=%zu bytes (%zu KB)\n",
           (void *)p, sz, sz / (1024UL));
    printf("> 现在看 /proc/%d/maps 和 /proc/%d/smaps；再按回车继续\n",
           getpid(), getpid());
           
    /* 先做少量访问：触发首次页错误，观察 do_anonymous_page / handle_mm_fault */
    printf("prepare write addr 0: %p 按回车继续\n", &p[0]);
    getchar();
    p[0] = 1;
    printf("prepare write addr 1: %p 按回车继续\n", &p[1]);
    getchar();
    p[1] = 1;
    printf("prepare write addr 4096: %p 按回车继续\n", &p[4096]);
    getchar();
    p[4096] = 2;
    printf("prepare write addr 8192: %p 按回车继续\n", &p[8192]);
    getchar();
    p[8192] = 3;
    puts("[4/4] 已触发 3 个页的首次写入，接着看 /proc/self/statm / smaps");

    /* 再逐页写一遍，观察匿名映射的缺页分配和 VMA 建立 */
    touch_pages(p, sz);

    printf("> 逐页写入完成，程序结束。可再看 /proc/%d/smaps\n", getpid());
    return 0;
}