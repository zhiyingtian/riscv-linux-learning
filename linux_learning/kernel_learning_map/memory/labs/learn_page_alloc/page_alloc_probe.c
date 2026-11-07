/*
 * page_alloc_probe.c —— 用户态观测物理页分配(buddy)
 *
 * 背景:用户态不能直接调 alloc_pages,但每次匿名页缺页都会让内核走
 *   do_anonymous_page → alloc_zeroed_user_highpage_movable → alloc_pages(order=0)
 * 所以逐页写 = 逐页触发真实的 buddy 分配。本程序就是把这个过程
 * 和 /proc 指标对齐给你看。
 *
 * 用法:
 *   ./page_alloc_probe touch [MB]    分配 + 逐页写 → munmap,看 MemFree/buddy 来回
 *   ./page_alloc_probe leak [MB]     分配后不释放(保持 30s),看内存不回来
 *   ./page_alloc_probe mlock [MB]    分配 + mlock,制造不可回收页
 *   ./page_alloc_probe cow [MB]      fork 后子进程只写一半,看 COW 只复制写过的页
 *
 * 注意:本内核 CONFIG_TRANSPARENT_HUGEPAGE 未开,所以用户态缺页永远是
 * order=0 分配,不会消费 buddy 的高阶块——这也是用户态观测的边界。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define PAGE_SZ   4096UL
#define MAX_ORDER 11

struct snap {
	unsigned long memfree_kb;
	unsigned long memavail_kb;
	unsigned long buddy[MAX_ORDER];   /* 所有 zone 汇总 */
	long minflt;
};

static void read_meminfo(struct snap *s)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];

	s->memfree_kb = s->memavail_kb = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long v;
		if (sscanf(line, "MemFree: %lu kB", &v) == 1)
			s->memfree_kb = v;
		else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1)
			s->memavail_kb = v;
	}
	fclose(f);
}

static void read_buddyinfo(struct snap *s)
{
	FILE *f = fopen("/proc/buddyinfo", "r");
	char line[512];

	memset(s->buddy, 0, sizeof(s->buddy));
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long v[MAX_ORDER];
		char *z = strstr(line, "zone");
		int off = 0, n = 0;

		if (!z)
			continue;
		z += 4;
		while (*z == ' ')
			z++;
		while (z[off] && z[off] != ' ')
			off++;
		if (off == 0)
			continue;
		z += off;
		while (*z && n < MAX_ORDER) {
			while (*z == ' ')
				z++;
			if (*z < '0' || *z > '9')
				break;
			v[n++] = strtoul(z, &z, 10);
		}
		if (n < MAX_ORDER)
			continue;
		for (int i = 0; i < MAX_ORDER; i++)
			s->buddy[i] += v[i];
	}
	fclose(f);
}

static void take_snap(struct snap *s)
{
	struct rusage ru;

	read_meminfo(s);
	read_buddyinfo(s);
	memset(&ru, 0, sizeof(ru));
	getrusage(RUSAGE_SELF, &ru);
	s->minflt = ru.ru_minflt;
}

static void print_snap(const char *tag, const struct snap *s)
{
	printf("[%s]\n", tag);
	printf("  MemFree=%lu KB  MemAvailable=%lu KB  minor_faults=%ld\n",
	       s->memfree_kb, s->memavail_kb, s->minflt);
	printf("  buddy:");
	for (int i = 0; i < MAX_ORDER; i++)
		printf(" %lu", s->buddy[i]);
	printf("   (order 0..10)\n");
}

static void print_delta(const char *what, const struct snap *a,
			const struct snap *b)
{
	printf("\n%s:\n", what);
	printf("  MemFree %+ld KB   minor_faults %+ld\n",
	       (long)(b->memfree_kb - a->memfree_kb),
	       b->minflt - a->minflt);
	printf("  buddy order0 %+ld, order1 %+ld, order2 %+ld, order3 %+ld\n",
	       (long)(b->buddy[0] - a->buddy[0]),
	       (long)(b->buddy[1] - a->buddy[1]),
	       (long)(b->buddy[2] - a->buddy[2]),
	       (long)(b->buddy[3] - a->buddy[3]));
}

static char *alloc_pages_user(size_t mb)
{
	size_t len = mb * 1024 * 1024;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

/* 逐页写:每一页触发一次真实缺页 → 一次 order-0 buddy 分配 */
static void touch_all(char *p, size_t mb)
{
	size_t len = mb * 1024 * 1024;

	for (size_t i = 0; i < len; i += PAGE_SZ)
		p[i] = (char)(i >> 12);
}

static size_t parse_mb(int argc, char **argv, size_t def)
{
	if (argc > 2) {
		long v = strtol(argv[2], NULL, 10);
		if (v > 0 && v <= 1024)
			return (size_t)v;
	}
	return def;
}

static void do_touch(size_t mb)
{
	struct snap before, after, freed;
	char *p;

	take_snap(&before);
	print_snap("before", &before);

	p = alloc_pages_user(mb);
	touch_all(p, mb);
	take_snap(&after);
	print_snap("after touch", &after);
	print_delta("touch 带来的变化", &before, &after);

	munmap(p, mb * 1024 * 1024);
	take_snap(&freed);
	print_snap("after munmap", &freed);
	print_delta("munmap 带来的变化", &after, &freed);

	printf("\n[提示] 分配量 %zu MB ≈ %zu 页;minor_faults 增量应接近页数\n",
	       mb, mb * 1024 / 4);
}

static void do_leak(size_t mb)
{
	struct snap before, after;
	char *p;

	take_snap(&before);
	print_snap("before", &before);

	p = alloc_pages_user(mb);
	touch_all(p, mb);
	take_snap(&after);
	print_snap("after touch", &after);
	print_delta("泄漏带来的变化", &before, &after);

	printf("\n[提示] 进程不退出、不 munmap,内存不会回来。保持 30s...\n");
	printf("       另一个终端观察:/proc/meminfo、/proc/buddyinfo\n");
	sleep(30);
	printf("[提示] 进程退出后内核才会回收这批页\n");
	(void)p;
}

static void do_mlock(size_t mb)
{
	struct snap before, after;
	char *p;

	take_snap(&before);
	print_snap("before", &before);

	p = alloc_pages_user(mb);
	touch_all(p, mb);

	if (mlock(p, mb * 1024 * 1024) != 0) {
		fprintf(stderr, "mlock 失败: %s(可能需要 ulimit -l)\n",
			strerror(errno));
		munmap(p, mb * 1024 * 1024);
		return;
	}

	take_snap(&after);
	print_snap("after mlock", &after);
	print_delta("mlock 带来的变化", &before, &after);

	printf("\n[提示] mlock 的页不可回收,内存压力下会把其他进程逼进回收/OOM\n");
	printf("       保持 30s,可在另一终端制造压力对比...\n");
	sleep(30);
	munlock(p, mb * 1024 * 1024);
	munmap(p, mb * 1024 * 1024);
}

static void do_cow(size_t mb)
{
	struct snap before, after;
	size_t len = mb * 1024 * 1024;
	char *p;
	pid_t pid;

	take_snap(&before);
	print_snap("before", &before);

	p = alloc_pages_user(mb);
	touch_all(p, mb);          /* 先让父进程把页都建好 */

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return;
	}
	if (pid == 0) {
		/* 子进程只写一半页 → 只对这一半触发 COW 复制 */
		struct snap c0, c1;

		take_snap(&c0);
		for (size_t i = 0; i < len; i += PAGE_SZ * 2)
			p[i] = 'x';
		take_snap(&c1);
		print_snap("child after half-write", &c1);
		print_delta("子进程 COW 开销(只写一半页)", &c0, &c1);
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	take_snap(&after);
	print_snap("parent after child exit", &after);
	print_delta("父进程视角的变化", &before, &after);

	munmap(p, len);
	printf("\n[提示] COW 只复制被写过的页;mapping 期间父子共享只读页\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "touch";
	size_t mb = parse_mb(argc, argv, 64);

	printf("mode=%s size=%zuMB\n\n", mode, mb);

	if (!strcmp(mode, "touch"))
		do_touch(mb);
	else if (!strcmp(mode, "leak"))
		do_leak(mb);
	else if (!strcmp(mode, "mlock"))
		do_mlock(mb);
	else if (!strcmp(mode, "cow"))
		do_cow(mb);
	else {
		fprintf(stderr, "未知模式: %s\n", mode);
		fprintf(stderr, "用法: %s [touch|leak|mlock|cow] [MB]\n", argv[0]);
		return 1;
	}
	return 0;
}
