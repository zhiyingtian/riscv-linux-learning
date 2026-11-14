#ifndef TEST_RUNNER_PCP_H
#define TEST_RUNNER_PCP_H

#include <stdio.h>

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

#endif /* TEST_RUNNER_PCP_H */
