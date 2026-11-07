#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096UL
#define FILE_SIZE (4 * PAGE_SIZE)   /* 16KB = 4 页, 便于观察 /proc/self/smaps */

static void pause_step(const char *msg)
{
    puts(msg);
    fflush(stdout);
    getchar();
}

int main(void)
{
    const char *path = "file_data.txt";
    char seed[FILE_SIZE];
    volatile char *p;
    int fd;

    puts("[1/5] 私有文件映射: mmap(MAP_PRIVATE) 绑定一个文件。");
    puts("       读路径走 page cache(filemap_fault);写路径触发 COW,不回写文件。\n");

    /* 生成 16KB 数据文件: 每页一个可辨识字符 */
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    for (size_t i = 0; i < FILE_SIZE; i++)
        seed[i] = (char)('A' + (i / PAGE_SIZE));   /* 页0='A'*4K 页1='B'*4K ... */
    if (write(fd, seed, FILE_SIZE) != (ssize_t)FILE_SIZE) { perror("write"); return 1; }
    close(fd);
    printf("[2/5] 已生成文件 %s: 页0全'A', 页1全'B', 页2全'C', 页3全'D'\n", path);
    printf("       在另一个终端观察 /proc/%d/maps 中该文件映射的出现\n", getpid());
    pause_step("按回车: mmap(MAP_PRIVATE) 映射文件,观察映射前后 VMA 变化...");

    fd = open(path, O_RDWR);
    p = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);   /* 映射建立后 fd 可以关, 引用由 VMA->vm_file 持有 */
    printf("       mmap 成功: base=%p size=%zu bytes\n", (void *)p, FILE_SIZE);
    printf("       > 看 /proc/self/maps: 出现该文件的 rw-p 映射;smaps 里 Rss 应为 0\n");
    pause_step("按回车: 读 p[0], 触发首次文件页缺页(filemap_fault, 读入 page cache)...");

    printf("       p[0] = '%c'  (来自 page cache 的文件页)\n", p[0]);
    printf("       > 看 smaps: Rss 变 4KB, 该页为 Shared_Clean(文件页)\n");
    pause_step("按回车: 写 p[0]='X', 触发私有映射写时复制(COW)...");

    p[0] = 'X';
    printf("       已写 p[0]='X'\n");
    printf("       > 看 smaps: 该页应从 Shared_Clean 变为 Private_Dirty(COW 副本)\n");
    pause_step("按回车: 逐页写一遍全部 4 页(每页都 COW 复制一份)...");

    for (size_t i = 0; i < FILE_SIZE; i += PAGE_SIZE)
        p[i] = (char)('x' + i / PAGE_SIZE);
    printf("       已逐页写: 4 页全部变为 Private_Dirty\n");
    pause_step("按回车: 验证私有映射的写不落盘...");

    /* 重新打开文件, 确认磁盘内容没有被修改 */
    fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    if (read(fd, seed, 8) != 8) { perror("read"); return 1; }
    close(fd);
    printf("       磁盘文件前 8 字节: '%.8s'  (仍是 'AAAAAAAA')\n", seed);
    printf("       内存映射前 8 字节: '%.8s'  ('xAAA...')\n", (const char *)p);
    puts("       结论: MAP_PRIVATE 的写只改内存中的 COW 副本, 不写回文件。");
    puts("[3/5] 对照匿名映射(mmap_lazy.c): 文件映射多了 page cache / Shared 计数;");
    puts("       两者的写路径都是 COW(do_wp_page / do_cow_fault)。");
    return 0;
}
