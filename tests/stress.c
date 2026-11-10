// SPDX-License-Identifier: GPL-2.0
/*
 * stress - phase 7: make the hardware misbehave (spec §9 INJECT) and load
 * the driver from several processes at once. Run after each, and look in
 * dmesg: a pass also means no kernel warnings.
 *
 *   dup_faults   every fault reported twice; an unfixable fault's
 *                duplicate must not kill the NEXT, innocent job
 *   slow_inval   invalidations land late; the driver must wait for them
 *   spurious     fault interrupts with nothing in the queue
 *   no_tlb       the device's TLB switched off: same results
 *   churn        4 processes x 2 threads: verified work in one, constant
 *                remap/madvise/mprotect/VRAM churn of the same VM in the other
 */
#include <pthread.h>
#include <sys/wait.h>

#include "common.h"

#define MiB (1ul << 20)

static uint32_t *map_anon(size_t len)
{
	uint32_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

static void inject(const char *bits)
{
	debugfs_write(DEBUGFS "svmd_inject", bits);
}

static void test_dup_faults(void)
{
	/*
	 * Job 1 touches unmapped memory: its fault can't be fixed, so the
	 * driver aborts job 1. Job 2, queued right behind it, is fine but
	 * faults too (fresh memory). Each fault is reported twice, and the
	 * driver holds each record 50 ms - long enough for the device to move
	 * on to job 2. If the driver handled job 1's DUPLICATE, it would abort
	 * whatever job is running by then: job 2.
	 */
	uint32_t *gone = map_anon(4096), *fresh = map_anon(64 * 4096);
	struct drm_svmd_cmd bad = ptr_cmd(SVMD_OP_FILL, 1, gone, NULL, 4096);
	struct drm_svmd_cmd good = ptr_cmd(SVMD_OP_FILL, 2, fresh, NULL, 64 * 4096);
	unsigned long long dups = stat_get("faults_duplicate");
	uint32_t s1 = syncobj_new(fd), s2 = syncobj_new(fd);
	int r1, r2;

	puts("duplicate fault reports must not abort an innocent job");
	munmap(gone, 4096);
	inject("0x1");				/* DUP_FAULTS */
	debugfs_write(DEBUGFS "svmd_fault_delay_ms", "50");
	submit_on(fd, &bad, 1, s1);
	submit_on(fd, &good, 1, s2);
	wait_on(fd, s1);
	wait_on(fd, s2);
	r1 = job_status(fd, s1);
	r2 = job_status(fd, s2);
	debugfs_write(DEBUGFS "svmd_fault_delay_ms", "0");
	inject("0x0");
	CHECK(r1 == -EFAULT, "job 1 (unmapped memory) failed: %d", r1);
	CHECK(r2 == 1 && fresh[0] == 2 && fresh[64 * 1024 - 1] == 2,
	      "job 2 (innocent) succeeded: %d", r2);
	CHECK(stat_get("faults_duplicate") > dups, "duplicates were seen and skipped (%llu)",
	      stat_get("faults_duplicate") - dups);
	munmap(fresh, 64 * 4096);
}

static void test_slow_inval(void)
{
	/* the remap check from svm.c, 30 times, with invalidations late */
	uint32_t *p = map_anon(4096), *out = map_anon(4096);
	struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_COPY, 0, out, p, 4096);
	int ok = 1;

	puts("slow invalidations: the driver must wait for each one");
	inject("0x2");				/* SLOW_INVAL */
	for (uint32_t i = 0; i < 30 && ok; i++) {
		for (int k = 0; k < 1024; k++)
			p[k] = i;
		ok = run(&c, 1) == 1 && out[0] == i && out[1023] == i;
		/* new page at the same address: the device must not see the old */
		if (mmap(p, 4096, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != p)
			ok = 0;
	}
	inject("0x0");
	CHECK(ok, "30 remaps, always read the current page");
	munmap(p, 4096);
	munmap(out, 4096);
}

static void test_spurious(void)
{
	uint32_t *b = map_anon(8 * MiB);
	struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_FILL, 5, b, NULL, 8 * MiB);
	int ok = 1;

	puts("spurious fault interrupts (nothing in the queue)");
	for (int i = 0; i < 20 && ok; i++) {
		inject("0x4");			/* SPURIOUS, self-clearing */
		ok = run(&c, 1) == 1;
	}
	CHECK(ok && b[0] == 5 && b[2 * 1024 * 1024 - 1] == 5,
	      "20 spurious interrupts, every job fine");
	munmap(b, 8 * MiB);
}

static void test_no_tlb(void)
{
	size_t len = 4 * MiB, words = len / 4;
	uint32_t *a = map_anon(len), *b = map_anon(len);
	struct drm_svmd_cmd c[2] = {
		ptr_cmd(SVMD_OP_COPY, 0, b, a, len),
		ptr_cmd(SVMD_OP_ADD, 3, b, NULL, len),
	};
	uint64_t bad = 0;

	puts("TLB switched off: correctness must not depend on it");
	for (size_t i = 0; i < words; i++)
		a[i] = (uint32_t)i;
	inject("0x8");				/* NO_TLB */
	CHECK(run(c, 2) == 1, "COPY + ADD without a TLB");
	inject("0x0");
	for (size_t i = 0; i < words; i++)
		bad += b[i] != (uint32_t)i + 3;
	CHECK(bad == 0, "same results (%llu wrong)", (unsigned long long)bad);
	munmap(a, len);
	munmap(b, len);
}

/*--- churn ----------------------------------------------------------------*/

#define CHURN_SECONDS	15

struct chaos { int fd; volatile int stop; unsigned long ops; };

/* knobs for narrowing a failure down (environment variables) */
static unsigned int churn_ops = 0x1f;	/* CHURN_OPS: bit n = chaos case n */
static int churn_vram = 2;		/* CHURN_VRAM: 0 off, 1 on, 2 random */
static int churn_procs = 4;		/* CHURN_PROCS */
static int churn_chaos = 1;		/* CHURN_CHAOS: 0 = no chaos thread */

/* Keep the SAME VM busy with invalidations, unmaps, faults, migrations. */
static void *chaos_thread(void *arg)
{
	struct chaos *c = arg;
	unsigned int seed = getpid();

	while (!c->stop) {
		size_t len = (1 + rand_r(&seed) % 64) * 4096;
		uint32_t *p = map_anon(len);
		struct drm_svmd_cmd cmd = ptr_cmd(SVMD_OP_FILL, 9, p, NULL, len);
		struct drm_svmd_set_param vram = {
			.param = DRM_SVMD_PARAM_PREFER_VRAM,
			.value = churn_vram == 2 ? rand_r(&seed) & 1 : churn_vram };
		int op;
		uint32_t s = syncobj_new(c->fd);

		ioctl(c->fd, DRM_IOCTL_SVMD_SET_PARAM, &vram);
		submit_on(c->fd, &cmd, 1, s);
		/*
		 * Change the memory under the running job - but never give the
		 * ADDRESS back while it runs. munmap here would let the other
		 * thread's malloc reuse the address, and the job would then
		 * correctly fill the other thread's memory: SVM means a device
		 * use-after-free behaves exactly like a CPU one.
		 */
		do			/* CHURN_OPS: which of cases 0-4 may run */
			op = rand_r(&seed) % 5;
		while (!(churn_ops & (1u << op)));
		switch (op) {
		case 0:					/* new pages, same address */
			mmap(p, len, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			break;
		case 1: madvise(p, len, MADV_DONTNEED); break;	/* pages dropped */
		case 2: mprotect(p, len, PROT_READ); break;
		case 3: p[0] = 1; break;		/* CPU touch: migrate back */
		default: break;
		}
		wait_on(c->fd, s);
		munmap(p, len);				/* only now */
		c->ops++;
	}
	return NULL;
}

/* One process: verified work while the chaos thread runs. 0 = all good. */
static int churn_process(void)
{
	int cfd = open_device();
	struct chaos c = { .fd = cfd };
	time_t end = time(NULL) + CHURN_SECONDS;
	unsigned long jobs = 0, failed = 0, wrong = 0;
	pthread_t t;
	int st;

	if (churn_chaos)
		pthread_create(&t, NULL, chaos_thread, &c);
	while (time(NULL) < end) {
		size_t words = (1 + rand() % 512) * 1024;
		uint32_t *a = malloc(words * 4), *b = malloc(words * 4);
		struct drm_svmd_cmd cmd[2] = {
			ptr_cmd(SVMD_OP_COPY, 0, b, a, words * 4),
			ptr_cmd(SVMD_OP_ADD, 11, b, NULL, words * 4),
		};

		for (size_t i = 0; i < words; i++)
			a[i] = (uint32_t)(i * 2654435761u);
		st = run_on(cfd, cmd, 2);
		if (st != 1) {
			failed++;			/* the job itself failed */
			if (failed <= 3)
				printf("        pid %d: job failed: %d\n", getpid(), st);
			goto next;
		}
		for (size_t i = 0; i < words; i++) {
			uint32_t want = (uint32_t)(i * 2654435761u) + 11;

			if (b[i] != want && !wrong++)
				printf("        pid %d: first wrong: b[%zu] of %zu = 0x%x, want 0x%x (a[i] = 0x%x)\n",
				       getpid(), i, words, b[i], want, a[i]);
		}
next:
		free(a);
		free(b);
		jobs++;
	}
	c.stop = 1;
	if (churn_chaos)
		pthread_join(t, NULL);
	printf("        pid %d: %lu jobs (%lu failed), %lu chaos ops, %lu wrong words\n",
	       getpid(), jobs, failed, c.ops, wrong);
	fflush(stdout);			/* _exit() won't flush it */
	return failed || wrong;
}

static void test_churn(void)
{
	int status, ok = 1;

	if (getenv("CHURN_OPS"))
		churn_ops = strtoul(getenv("CHURN_OPS"), NULL, 0) ?: 0x1f;
	if (getenv("CHURN_VRAM"))
		churn_vram = atoi(getenv("CHURN_VRAM"));
	if (getenv("CHURN_PROCS"))
		churn_procs = atoi(getenv("CHURN_PROCS"));
	if (getenv("CHURN_CHAOS"))
		churn_chaos = atoi(getenv("CHURN_CHAOS"));
	printf("churn: 4 processes, %d s, constant remapping of the same VMs (ops 0x%x, vram %d)\n",
	       CHURN_SECONDS, churn_ops, churn_vram);
	fflush(stdout);
	for (int i = 0; i < churn_procs; i++)
		if (fork() == 0)
			_exit(churn_process());
	for (int i = 0; i < churn_procs; i++) {
		wait(&status);
		ok &= WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	CHECK(ok, "every verified job correct in every process");
}

static void test_remap_sizes(void)
{
	/* One thread: map a random size, device fills it, check, unmap.
	 * Each new mapping reuses addresses of earlier ones, with different
	 * sizes - so new ranges overlap where old, removed ranges were. */
	unsigned int seed = 1;
	unsigned long bad_jobs = 0, iters = 0;
	time_t end = time(NULL) + 10;

	puts("remap_sizes: map / device fill / check / unmap, random sizes");
	while (time(NULL) < end) {
		size_t len = (1 + rand_r(&seed) % 64) * 4096;
		uint32_t *p = map_anon(len), v = 0x1000 + iters;
		struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_FILL, v, p, NULL, len);
		size_t badw = 0;

		if (run(&c, 1) != 1)
			bad_jobs++;
		for (size_t i = 0; i < len / 4; i++)
			badw += p[i] != v;
		if (badw && bad_jobs++ < 3)
			printf("        iter %lu: %zu of %zu words wrong at %p (len %zu)\n",
			       iters, badw, len / 4, (void *)p, len);
		munmap(p, len);
		iters++;
	}
	CHECK(bad_jobs == 0, "%lu iterations, %lu bad", iters, bad_jobs);
}

struct rs_arg { int device; volatile int stop; unsigned long iters, bad; };

/* Map a random size, (optionally) device-fill it and check it, unmap. */
static void *remap_sizes_thread(void *argp)
{
	struct rs_arg *a = argp;
	unsigned int seed = (unsigned int)(uintptr_t)argp;

	while (!a->stop) {
		size_t len = (1 + rand_r(&seed) % 64) * 4096;
		uint32_t *p = map_anon(len), v = 0x2000 + a->iters;
		struct drm_svmd_cmd c = ptr_cmd(SVMD_OP_FILL, v, p, NULL, len);

		if (a->device) {
			size_t badw = 0, first = 0;
			int st = run(&c, 1);

			for (size_t i = 0; i < len / 4; i++)
				if (p[i] != v && !badw++)
					first = i;
			if (st != 1 || badw) {
				if (a->bad < 3)
					printf("        tid %d: status %d, %zu/%zu wrong, p[%zu] = 0x%x want 0x%x, p=%p\n",
					       (int)(uintptr_t)argp & 0xffff, st, badw,
					       len / 4, first, p[first], v, (void *)p);
				a->bad++;
			}
		} else {
			p[0] = v;			/* CPU only */
		}
		munmap(p, len);
		a->iters++;
	}
	return NULL;
}

static void test_two_threads(void)
{
	/* TWO_THREADS_DEVICE=0: the second thread only maps/unmaps memory */
	int second_device = getenv("TWO_THREADS_DEVICE") ?
			    atoi(getenv("TWO_THREADS_DEVICE")) : 1;
	struct rs_arg a = { .device = 1 }, b = { .device = second_device };
	pthread_t ta, tb;

	printf("two_threads: device thread + %s thread, same process\n",
	       second_device ? "device" : "CPU-only remap");
	pthread_create(&ta, NULL, remap_sizes_thread, &a);
	pthread_create(&tb, NULL, remap_sizes_thread, &b);
	sleep(10);
	a.stop = b.stop = 1;
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	CHECK(a.bad == 0 && b.bad == 0, "%lu + %lu iterations, %lu + %lu bad",
	      a.iters, b.iters, a.bad, b.bad);
}

static void test_abort_victim(void)
{
	/*
	 * Job A is fine; job B, right behind it, touches unmapped memory and
	 * is aborted. Finished jobs are retired 200 ms late, so when B's
	 * fault is handled, A has finished on the device but is still the
	 * oldest fence on the driver's list. The abort must hit B, not A.
	 */
	uint32_t *ok = map_anon(4096), *gone = map_anon(4096);
	struct drm_svmd_cmd a = ptr_cmd(SVMD_OP_FILL, 3, ok, NULL, 4096);
	struct drm_svmd_cmd b = ptr_cmd(SVMD_OP_FILL, 4, gone, NULL, 4096);
	uint32_t sa = syncobj_new(fd), sb = syncobj_new(fd);
	int ra, rb;

	puts("an aborted job's error lands on it, not on the job before it");
	ok[0] = 0;				/* mapped before the jobs run */
	munmap(gone, 4096);
	debugfs_write(DEBUGFS "svmd_job_irq_delay_ms", "200");
	submit_on(fd, &a, 1, sa);
	submit_on(fd, &b, 1, sb);
	wait_on(fd, sa);
	wait_on(fd, sb);
	debugfs_write(DEBUGFS "svmd_job_irq_delay_ms", "0");
	ra = job_status(fd, sa);
	rb = job_status(fd, sb);
	CHECK(ra == 1 && ok[0] == 3, "job A (fine) succeeded: %d", ra);
	CHECK(rb == -EFAULT, "job B (unmapped memory) failed: %d", rb);
	munmap(ok, 4096);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
	{ "abort_victim", test_abort_victim },
	{ "two_threads", test_two_threads },
	{ "remap_sizes", test_remap_sizes },
	{ "dup_faults", test_dup_faults },
	{ "slow_inval", test_slow_inval },
	{ "spurious", test_spurious },
	{ "no_tlb", test_no_tlb },
	{ "churn", test_churn },
};

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
