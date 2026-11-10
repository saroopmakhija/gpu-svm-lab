// SPDX-License-Identifier: GPL-2.0
/*
 * svm - shared virtual memory from user space: the device works directly
 * on malloc'd pointers, faulting pages in through drm_gpusvm.
 *
 * The tests that matter most are built so that a broken driver FAILS them
 * (each was checked by breaking the driver on purpose - see README):
 *   remap         CPU swaps a page; the device must see the new one
 *   tlb_in_job    ...even in the middle of a job, with the old page in
 *                 the device's TLB (needs the notifier's TLB flush)
 *   fork          two processes, same addresses, separate memory
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/wait.h>

#include "common.h"

#define MiB (1ul << 20)

static void *map_anon(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

static uint64_t count_not(const uint32_t *p, size_t words, uint32_t (*want)(size_t))
{
	uint64_t bad = 0;

	for (size_t i = 0; i < words; i++)
		bad += p[i] != want(i);
	return bad;
}

static uint32_t want_i_plus_7(size_t i) { return (uint32_t)i + 7; }

static void test_malloc(void)
{
	size_t len = 8 * MiB, words = len / 4;
	uint32_t *src = malloc(len), *dst = malloc(len);
	unsigned long long f0 = stat_get("device_faults");
	struct drm_svmd_cmd c[2];

	puts("malloc'd memory, used by the device as-is");
	for (size_t i = 0; i < words; i++)
		src[i] = (uint32_t)i;
	memset(dst, 0, len);
	c[0] = ptr_cmd(SVMD_OP_COPY, 0, dst, src, len);
	c[1] = ptr_cmd(SVMD_OP_ADD, 7, dst, NULL, len);
	CHECK(run(c, 2) == 1, "COPY + ADD on two malloc'd 8 MiB buffers");
	CHECK(count_not(dst, words, want_i_plus_7) == 0, "dst[i] == i + 7 everywhere");
	CHECK(stat_get("device_faults") > f0,
	      "pages arrived by device faults (%llu)", stat_get("device_faults") - f0);
	free(src);
	free(dst);
}

static uint32_t global_buf[1024];

static void test_stack_and_globals(void)
{
	uint32_t stack_buf[1024];
	struct drm_svmd_cmd c[2];

	puts("stack and global variables work too");
	c[0] = ptr_cmd(SVMD_OP_FILL, 0x5eed, stack_buf, NULL, sizeof(stack_buf));
	c[1] = ptr_cmd(SVMD_OP_COPY, 0, global_buf, stack_buf, sizeof(stack_buf));
	CHECK(run(c, 2) == 1 && stack_buf[1023] == 0x5eed &&
	      global_buf[0] == 0x5eed, "FILL a stack array, COPY it to a global");
}

static void test_bad_memory(void)
{
	uint32_t *ro = map_anon(4096), *gone = map_anon(4096), *ok = map_anon(4096);
	struct drm_svmd_cmd c;

	puts("memory the process can't use is refused by the MMU, not trusted");
	ro[0] = 42;
	mprotect(ro, 4096, PROT_READ);
	munmap(gone, 4096);

	c = ptr_cmd(SVMD_OP_FILL, 1, gone, NULL, 4096);
	CHECK(run(&c, 1) == -EFAULT, "unmapped memory: job fails with -EFAULT");
	c = ptr_cmd(SVMD_OP_FILL, 1, ro, NULL, 4096);
	CHECK(run(&c, 1) == -EFAULT && ro[0] == 42,
	      "write to read-only memory: -EFAULT, memory untouched");
	c = ptr_cmd(SVMD_OP_COPY, 0, ok, ro, 4096);
	CHECK(run(&c, 1) == 1 && ok[0] == 42, "reading read-only memory is fine");
	c = ptr_cmd(SVMD_OP_FILL, 1, (void *)(1ull << 48), NULL, 4096);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "address beyond 48 bits -> EINVAL");
	c = ptr_cmd(SVMD_OP_FILL, 9, ok, NULL, 4096);
	CHECK(run(&c, 1) == 1 && ok[0] == 9, "the device still works afterwards");
	munmap(ro, 4096);
	munmap(ok, 4096);
}

/*
 * Replace the page at `p` with a brand-new one holding `v` (same address).
 * The new page is filled FIRST, elsewhere, then moved over `p` with
 * mremap: so it is always a different physical page from the old one.
 * (With MAP_FIXED + write, the allocator often hands back the page just
 * freed - and a stale device entry would then "see" the new data by luck.)
 */
static void remap_page(uint32_t *p, uint32_t v)
{
	uint32_t *n = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	for (int i = 0; i < 1024; i++)
		n[i] = v;
	if (n == MAP_FAILED ||
	    mremap(n, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, p) != p) {
		perror("mremap");
		exit(1);
	}
}

static void test_remap(void)
{
	uint32_t *p = map_anon(4096), *out = map_anon(4096);
	struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_COPY, 0, out, p, 4096);

	puts("CPU swaps a page the device has mapped: device must see the new page");
	for (int i = 0; i < 1024; i++)
		p[i] = 0x01d;
	CHECK(run(&c, 1) == 1 && out[0] == 0x01d, "device reads the old page");
	remap_page(p, 0x2e3);		/* MAP_FIXED: old page unmapped, freed */
	CHECK(run(&c, 1) == 1 && out[0] == 0x2e3 && out[1023] == 0x2e3,
	      "after the swap it reads the NEW page (got 0x%x)", out[0]);
	munmap(p, 4096);
	munmap(out, 4096);
}

struct swap_arg { uint32_t *p; };

static void *swap_later(void *arg)
{
	struct swap_arg *a = arg;

	usleep(150 * 1000);		/* the device is now stalled on Q */
	/*
	 * Drop P's page and write new data (a fresh page). madvise, not
	 * munmap/MAP_FIXED: an unmap would also queue P's range for the
	 * garbage collector, which flushes it too - and hide a missing
	 * notifier flush. Here only the notifier's flush protects P.
	 */
	madvise(a->p, 4096, MADV_DONTNEED);
	for (int i = 0; i < 1024; i++)
		a->p[i] = 0xbbb;
	return NULL;
}

static void test_tlb_in_job(void)
{
	/*
	 * One job: COPY P->d1 (P's translation is now in the device's TLB),
	 * COPY Q->d2 (Q was never touched: a fault, which the driver holds for
	 * 400 ms), COPY P->d3. While the device is stalled, another thread
	 * swaps P. SET_ROOT only flushes the TLB between jobs, so the only
	 * thing that stops the third COPY using the stale TLB entry for P is
	 * the MMU notifier's TLB invalidation.
	 */
	uint32_t *p = map_anon(4096), *q = map_anon(4096);
	uint32_t *d = map_anon(3 * 4096);
	struct drm_svmd_cmd c[3];
	struct swap_arg a = { p };
	pthread_t t;
	int st;

	puts("a page swapped mid-job, while its translation sits in the TLB");
	for (int i = 0; i < 1024; i++)
		p[i] = 0xaaa, q[i] = 0x999;
	c[0] = ptr_cmd(SVMD_OP_COPY, 0, d, p, 4096);
	c[1] = ptr_cmd(SVMD_OP_COPY, 0, d + 1024, q, 4096);
	c[2] = ptr_cmd(SVMD_OP_COPY, 0, d + 2048, p, 4096);
	/* map d and P first, so the only fault in the job is Q's */
	{
		struct drm_svmd_cmd pre[2] = {
			ptr_cmd(SVMD_OP_FILL, 0, d, NULL, 3 * 4096),
			ptr_cmd(SVMD_OP_COPY, 0, d, p, 4096),
		};
		CHECK(run(pre, 2) == 1, "warm up: P and the destination mapped");
	}
	debugfs_write(DEBUGFS "svmd_fault_delay_ms", "400");
	pthread_create(&t, NULL, swap_later, &a);
	st = run(c, 3);
	pthread_join(t, NULL);
	debugfs_write(DEBUGFS "svmd_fault_delay_ms", "0");
	CHECK(st == 1, "job finished");
	CHECK(d[0] == 0xaaa && d[1024] == 0x999, "first two copies saw P(old), Q");
	CHECK(d[2048] == 0xbbb,
	      "third copy saw the NEW P (0x%x): the stale TLB entry was flushed",
	      d[2048]);
	munmap(p, 4096);
	munmap(q, 4096);
	munmap(d, 3 * 4096);
}

static void test_big(void)
{
	size_t len = 64 * MiB, words = len / 4;
	uint32_t *b = malloc(len);
	unsigned long long f0 = stat_get("device_faults");
	struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_FILL, 77, b, NULL, len);
	uint64_t bad = 0;

	puts("64 MiB never touched by the CPU: all of it faulted in");
	CHECK(run(&c, 1) == 1, "FILL 64 MiB of fresh malloc memory");
	for (size_t i = 0; i < words; i++)
		bad += b[i] != 77;
	CHECK(bad == 0, "CPU sees the device's writes (%llu wrong)",
	      (unsigned long long)bad);
	printf("        (%llu faults for 64 MiB: drm_gpusvm maps up to 2 MiB per fault)\n",
	       stat_get("device_faults") - f0);
	free(b);
}

static void test_fork(void)
{
	/*
	 * After fork, parent and child have the SAME addresses but (after
	 * copy-on-write) different memory. Each opens the device, so each
	 * gets its own device address space. Both fill `buf` at the same
	 * address with different values, many times, interleaved.
	 */
	uint32_t *buf = map_anon(1 * MiB);
	struct drm_svmd_cmd c;
	pid_t pid;
	int ok = 1, status;

	puts("fork: two processes, same addresses, separate device address spaces");
	buf[0] = 0;
	pid = fork();
	if (pid == 0) {
		int cfd = open_device();
		uint32_t mine = 0xc41d;

		for (int i = 0; i < 50 && ok; i++) {
			c = ptr_cmd(SVMD_OP_FILL, mine + i, buf, NULL, 1 * MiB);
			ok = run_on(cfd, &c, 1) == 1 && buf[0] == mine + i &&
			     buf[262143] == mine + i;
		}
		_exit(ok ? 0 : 1);
	}
	for (int i = 0; i < 50 && ok; i++) {
		c = ptr_cmd(SVMD_OP_FILL, 0xa000 + i, buf, NULL, 1 * MiB);
		ok = run(&c, 1) == 1 && buf[0] == 0xa000u + i;
	}
	waitpid(pid, &status, 0);
	CHECK(ok, "parent: 50 fills, always saw its own values");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "child: 50 fills, always saw its own values");
	munmap(buf, 1 * MiB);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
	{ "malloc", test_malloc },
	{ "stack", test_stack_and_globals },
	{ "bad_memory", test_bad_memory },
	{ "remap", test_remap },
	{ "tlb_in_job", test_tlb_in_job },
	{ "big", test_big },
	{ "fork", test_fork },
};

/* svm [test-name...]: run all tests, or only the named ones */
int main(int argc, char **argv)
{
	fd = open_device();
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int want = argc == 1;

		for (int a = 1; a < argc; a++)
			want |= !strcmp(argv[a], tests[i].name);
		if (want)
			tests[i].fn();
	}
	return finish();
}
