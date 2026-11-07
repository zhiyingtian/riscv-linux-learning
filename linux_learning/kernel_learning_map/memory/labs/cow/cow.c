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
        getchar();
        _exit(0);
    } else {
        for (int i = 0; i < (int)sizeof(buf); i += 4096) buf[i] = 3; /* 父进程写 → COW */
        wait(NULL);
        printf("parent done\n");
        getchar();
    }
    return 0;
}