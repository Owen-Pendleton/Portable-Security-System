#ifndef PSS_TESTS_T_H
#define PSS_TESTS_T_H

/* Minimal test harness: CHECK records a failure and keeps going. */

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>

static int t_failures;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (!(cond)) {                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			t_failures++;                                                 \
		}                                                                     \
	} while (0)

#define T_RUN(fn)                                  \
	do {                                       \
		fprintf(stderr, "  %s\n", #fn);    \
		fn();                              \
	} while (0)

#define T_END()                                                       \
	do {                                                          \
		if (t_failures) {                                     \
			fprintf(stderr, "FAIL: %d check(s)\n", t_failures); \
			return 1;                                     \
		}                                                     \
		fprintf(stderr, "ok\n");                              \
		return 0;                                             \
	} while (0)

/* Fresh directory under $TMPDIR (or /tmp). Aborts the test run if it can't. */
__attribute__((unused)) static void t_mkdtemp(char buf[256])
{
	const char *base = getenv("TMPDIR");

	snprintf(buf, 256, "%s/pss-test-XXXXXX", base && base[0] ? base : "/tmp");
	if (mkdtemp(buf) == NULL) {
		perror("mkdtemp");
		exit(2);
	}
}

static int t_rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

__attribute__((unused)) static void t_rm_rf(const char *path)
{
	if (nftw(path, t_rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("cleanup nftw");
}

#endif
