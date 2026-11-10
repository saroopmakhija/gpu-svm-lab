// SPDX-License-Identifier: GPL-2.0
/*
 * phase2 - drive the svmd device from user space, like real software would.
 *
 * Waiting uses DRM syncobjs (the standard mechanism); the last test checks
 * implicit sync between two separately opened files sharing one buffer.
 *
 * Built statically in the Lima VM (scripts/vm.sh make -C tests) so it runs
 * on the Alpine guest:  /lab/tests/phase2
 */
#include "common.h"

static void test_basic(void)
{
	struct bo a = bo_new(1 << 20), b = bo_new(1 << 20);
	uint64_t words = a.size / 4, bad = 0;
	struct drm_svmd_cmd c[2];
	uint32_t seqno;

	puts("CPU writes, device copies + adds, CPU reads back");
	for (uint64_t i = 0; i < words; i++)
		a.map[i] = (uint32_t)i;
	c[0] = cmd(SVMD_OP_COPY, 0, &b, 0, &a, 0, a.size);
	c[1] = cmd(SVMD_OP_ADD, 1000, &b, 0, NULL, 0, b.size);
	CHECK(submit(c, 2, &seqno) == 0, "submit COPY + ADD");
	CHECK(wait_seqno(seqno) == 0, "wait on its syncobj");
	for (uint64_t i = 0; i < words; i++)
		bad += b.map[i] != (uint32_t)i + 1000;
	CHECK(bad == 0, "b[i] == i + 1000 for all %llu words (%llu wrong)",
	      (unsigned long long)words, (unsigned long long)bad);
	bo_free(&a);
	bo_free(&b);
}

static void test_big_add(void)
{
	/* 4 MiB > the engine's 1 MiB budget per run: ADD must resume, not
	 * restart, or some words get +2 (or +0) instead of +1 */
	struct bo a = bo_new(4 << 20);
	uint64_t words = a.size / 4, bad = 0;
	struct drm_svmd_cmd c[2];
	uint32_t seqno;

	puts("a 4 MiB ADD is split across engine runs and resumed");
	c[0] = cmd(SVMD_OP_FILL, 41, &a, 0, NULL, 0, a.size);
	c[1] = cmd(SVMD_OP_ADD, 1, &a, 0, NULL, 0, a.size);
	CHECK(submit(c, 2, &seqno) == 0 && wait_seqno(seqno) == 0,
	      "FILL 41 then ADD 1 over 4 MiB");
	for (uint64_t i = 0; i < words; i++)
		bad += a.map[i] != 42;
	CHECK(bad == 0, "every word is 42 (%llu wrong)", (unsigned long long)bad);
	bo_free(&a);
}

static void test_rejects(void)
{
	struct bo a = bo_new(8192);
	struct drm_svmd_cmd c;

	puts("the driver refuses bad commands (the device would not!)");
	c = cmd(SVMD_OP_FILL, 0, &a, 0, NULL, 0, 4096);
	c.dst_handle = 999;
	CHECK(submit(&c, 1, NULL) == -ENOENT, "unknown buffer handle -> ENOENT");
	c = cmd(SVMD_OP_FILL, 0, &a, 4096, NULL, 0, 8192);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "range past the end -> EINVAL");
	c = cmd(SVMD_OP_FILL, 0, &a, 8, NULL, 0, UINT64_MAX - 3);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "offset+len overflow -> EINVAL");
	c = cmd(SVMD_OP_COPY, 0, &a, 0, &a, 100, 4096);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "overlapping COPY -> EINVAL");
	c = cmd(SVMD_OP_FENCE, 7, NULL, 0, NULL, 0, 0);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "user FENCE -> EINVAL");
	c = cmd(SVMD_OP_ADD, 1, &a, 0, NULL, 0, 6);
	CHECK(submit(&c, 1, NULL) == -EINVAL, "ADD of 6 bytes -> EINVAL");
	c = cmd(SVMD_OP_FILL, 0, &a, 0, NULL, 0, 4096);
	CHECK(submit_on(fd, &c, 1, 12345) == -ENOENT, "unknown out syncobj -> ENOENT");
	bo_free(&a);
}

static void test_ring_full(void)
{
	/* 200 batches x 3 slots (2 cmds + fence) > 256 ring slots */
	struct bo a = bo_new(4096);
	struct drm_svmd_cmd c[2];
	uint32_t seqno = 0;
	int ok = 1;

	puts("more work than the ring holds: submit must wait for space");
	c[0] = cmd(SVMD_OP_ADD, 1, &a, 0, NULL, 0, 4096);
	c[1] = cmd(SVMD_OP_ADD, 2, &a, 0, NULL, 0, 4096);
	a.map[0] = 0;
	for (int i = 0; i < 200 && ok; i++)
		ok = submit(c, 2, &seqno) == 0;
	CHECK(ok, "200 submits accepted");
	CHECK(wait_seqno(seqno) == 0, "last batch finished");
	CHECK(a.map[0] == 600, "a[0] == 200 * (1 + 2) = 600 (got %u)", a.map[0]);
	bo_free(&a);
}

static void test_freed_while_busy(void)
{
	/* close + unmap a buffer while the device still uses it: the job's
	 * reference must keep the memory alive until the fence */
	struct bo a = bo_new(4 << 20);
	struct drm_svmd_cmd c = cmd(SVMD_OP_FILL, 7, &a, 0, NULL, 0, a.size);
	uint32_t seqno;

	puts("buffer freed by the program while the device still writes it");
	CHECK(submit(&c, 1, &seqno) == 0, "submit a 4 MiB FILL");
	bo_free(&a);
	CHECK(wait_seqno(seqno) == 0, "job still completes after close+munmap");
}

static void test_explicit_sync(void)
{
	/* in_sync: a job must not start before the fence it was given */
	struct bo a = bo_new(4096);
	struct drm_svmd_cmd c = cmd(SVMD_OP_FILL, 5, &a, 0, NULL, 0, 4096);
	int timeline;
	uint32_t hold = held_fence(fd, &timeline), done = syncobj_new(fd);

	puts("explicit sync: a job waits for the fence it is given (in_sync)");
	a.map[0] = 0;
	CHECK(submit_after(fd, &c, 1, hold, done) == 0, "submit FILL behind a held fence");
	CHECK(wait_ms(fd, done, 200) == -ETIME && a.map[0] == 0,
	      "200 ms later it has NOT run (word still %u)", a.map[0]);
	release(timeline);
	CHECK(wait_on(fd, done) == 0 && a.map[0] == 5, "released: it runs (word %u)",
	      a.map[0]);
	bo_free(&a);
}

static void test_implicit_sync(void)
{
	/*
	 * Two separately opened files = two queues. They share one buffer
	 * (via a dma-buf fd). File 1's FILL is held back by a fence; file 2's
	 * ADD has no dependency of its own. Only implicit sync - the FILL's
	 * fence sitting on the shared buffer - stops the ADD from running
	 * first. (Checked: with implicit sync disabled, this test fails.)
	 */
	int fd2 = open("/dev/dri/renderD128", O_RDWR);
	struct bo a = bo_new(1 << 20);
	struct drm_prime_handle exp = { .handle = a.handle, .flags = DRM_CLOEXEC };
	struct drm_prime_handle imp = { 0 };
	struct drm_svmd_cmd fill = cmd(SVMD_OP_FILL, 100, &a, 0, NULL, 0, a.size);
	struct drm_svmd_cmd add;
	int timeline;
	uint32_t hold = held_fence(fd, &timeline), s1 = syncobj_new(fd);
	uint32_t s2 = syncobj_new(fd2);
	uint64_t bad = 0;

	puts("implicit sync: another file's job on a shared buffer waits its turn");
	CHECK(fd2 >= 0 && !ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &exp) &&
	      ((imp.fd = exp.fd), !ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imp)),
	      "share the buffer with a second open file (dma-buf)");
	add = (struct drm_svmd_cmd){ .op = SVMD_OP_ADD, .arg32 = 1,
				     .dst_handle = imp.handle, .len = a.size };
	CHECK(submit_after(fd, &fill, 1, hold, s1) == 0,
	      "file 1: FILL 100, held back by a fence");
	CHECK(submit_on(fd2, &add, 1, s2) == 0, "file 2: ADD 1, no fence of its own");
	CHECK(wait_ms(fd2, s2, 200) == -ETIME,
	      "200 ms later file 2's ADD has NOT run - it waits for the FILL");
	release(timeline);
	CHECK(wait_on(fd2, s2) == 0, "released: file 2's job finishes");
	for (uint64_t i = 0; i < a.size / 4; i++)
		bad += a.map[i] != 101;
	CHECK(bad == 0, "every word is 101, so ADD ran after FILL (%llu wrong)",
	      (unsigned long long)bad);
	close(exp.fd);
	close(fd2);
	bo_free(&a);
}

int main(void)
{
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) {
		perror("open /dev/dri/renderD128");
		return 1;
	}
	test_basic();
	test_big_add();
	test_rejects();
	test_ring_full();
	test_freed_while_busy();
	test_explicit_sync();
	test_implicit_sync();
	return finish();
}
