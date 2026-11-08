#ifndef TEST_RUNNER_BUDDY_H
#define TEST_RUNNER_BUDDY_H

#include <stdio.h>

/*
 * M2 的测试 runner 比 M1 简单：
 *   - buddy 不管理全局状态，测试自己 buddy_init()；
 *   - CHECK 失败只返回，不做 pmm_exit()。
 */
#define CHECK(cond)                                                           \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,       \
				__LINE__, #cond);                              \
			return 1;                                              \
		}                                                             \
	} while (0)

static inline int run_test(const char *name, int (*fn)(void))
{
	int ret = fn();

	printf("[%s] %s\n", ret ? "FAIL" : "PASS", name);
	return ret;
}

#endif /* TEST_RUNNER_BUDDY_H */
