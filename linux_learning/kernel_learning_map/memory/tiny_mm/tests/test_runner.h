#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>

#include "tmm.h"

#define CHECK(cond)                                                           \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,       \
				__LINE__, #cond);                              \
			pmm_exit();                                            \
			return 1;                                              \
		}                                                             \
	} while (0)

static inline int run_test(const char *name, int (*fn)(void))
{
	int ret = fn();

	/* 每个测试结束后清理；pmm_exit() 可重复调用 */
	pmm_exit();

	printf("[%s] %s\n", ret ? "FAIL" : "PASS", name);
	return ret;
}

#endif /* TEST_RUNNER_H */
