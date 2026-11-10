/* SPDX-License-Identifier: GPL-2.0 */
/*
 * common.h - helpers shared by the user-space tests: buffers, submitting,
 * waiting on syncobjs, held (sw_sync) fences, and reading a job's result.
 */
#ifndef SVMD_TESTS_COMMON_H
#define SVMD_TESTS_COMMON_H

/* each test uses only some helpers */
#pragma GCC diagnostic ignored "-Wunused-function"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/sync_file.h>

#include "svmd_regs.h"
#include "uapi/svmd_drm.h"

static int fd, failures;

#define CHECK(cond, ...) do {						\
	if (cond) { printf("  ok    "); }				\
	else { printf("  FAIL  "); failures++; }			\
	printf(__VA_ARGS__); printf("\n");				\
} while (0)

struct bo { uint32_t handle; uint32_t *map; uint64_t size; };

static struct bo bo_new_on(int on, uint64_t size)
{
	struct drm_svmd_gem_create c = { .size = size };
	struct bo b = { .size = size };

	if (ioctl(on, DRM_IOCTL_SVMD_GEM_CREATE, &c)) {
		perror("GEM_CREATE");
		exit(1);
	}
	b.handle = c.handle;
	b.map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, on,
		     c.mmap_offset);
	if (b.map == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return b;
}

static struct bo bo_new(uint64_t size)
{
	return bo_new_on(fd, size);
}

static void bo_free(struct bo *b)
{
	struct drm_gem_close c = { .handle = b->handle };

	munmap(b->map, b->size);
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* A syncobj: a slot the driver puts a job's "finished" fence into. */
static uint32_t syncobj_new(int on)
{
	struct drm_syncobj_create c = { 0 };

	if (ioctl(on, DRM_IOCTL_SYNCOBJ_CREATE, &c)) {
		perror("SYNCOBJ_CREATE");
		exit(1);
	}
	return c.handle;
}

/* submit on `on`; the job's completion goes into syncobj `sync` (0: none) */
static int submit_after(int on, struct drm_svmd_cmd *cmds, uint32_t n,
			uint32_t in_sync, uint32_t sync)
{
	struct drm_svmd_submit s = { .cmds = (uintptr_t)cmds, .count = n,
				     .in_sync = in_sync, .out_sync = sync };

	return ioctl(on, DRM_IOCTL_SVMD_SUBMIT, &s) ? -errno : 0;
}

static int submit_on(int on, struct drm_svmd_cmd *cmds, uint32_t n,
		     uint32_t sync)
{
	return submit_after(on, cmds, n, 0, sync);
}

static int submit(struct drm_svmd_cmd *cmds, uint32_t n, uint32_t *sync)
{
	if (sync)
		*sync = syncobj_new(fd);
	return submit_on(fd, cmds, n, sync ? *sync : 0);
}

/* Wait up to `ms` for the fence in a syncobj. 0, or -errno (-ETIME). */
static int wait_ms(int on, uint32_t sync, long ms)
{
	struct timespec now;
	struct drm_syncobj_wait w = { .handles = (uintptr_t)&sync,
				      .count_handles = 1 };

	clock_gettime(CLOCK_MONOTONIC, &now);	/* the deadline is absolute */
	w.timeout_nsec = now.tv_sec * 1000000000LL + now.tv_nsec +
			 ms * 1000000LL;
	return ioctl(on, DRM_IOCTL_SYNCOBJ_WAIT, &w) ? -errno : 0;
}

static int wait_on(int on, uint32_t sync)
{
	return wait_ms(on, sync, 5000);
}

/*
 * sw_sync: a test-only fence timeline the program controls (debugfs).
 * Its ioctls aren't in any uapi header, so they're spelled out here,
 * copied from drivers/dma-buf/sw_sync.c.
 */
struct sw_sync_create_fence_data { uint32_t value; char name[32]; int32_t fence; };
#define SW_SYNC_IOC_CREATE_FENCE _IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC		 _IOW('W', 1, uint32_t)

/* A syncobj holding a fence that signals only when we say so. */
static uint32_t held_fence(int on, int *timeline)
{
	struct sw_sync_create_fence_data f = { .value = 1, .name = "hold" };
	struct drm_syncobj_handle imp = { .flags =
		DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE };

	*timeline = open("/sys/kernel/debug/sync/sw_sync", O_RDWR);
	if (*timeline < 0 || ioctl(*timeline, SW_SYNC_IOC_CREATE_FENCE, &f)) {
		perror("sw_sync (is debugfs mounted, CONFIG_SW_SYNC on?)");
		exit(1);
	}
	imp.handle = syncobj_new(on);
	imp.fd = f.fence;
	if (ioctl(on, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp)) {
		perror("import sync_file");
		exit(1);
	}
	close(f.fence);
	return imp.handle;
}

static void release(int timeline)
{
	uint32_t one = 1;

	ioctl(timeline, SW_SYNC_IOC_INC, &one);
	close(timeline);
}

static int wait_seqno(uint32_t sync)
{
	return wait_on(fd, sync);
}

static struct drm_svmd_cmd cmd(uint32_t op, uint32_t arg, struct bo *dst,
			       uint64_t doff, struct bo *src, uint64_t soff,
			       uint64_t len)
{
	return (struct drm_svmd_cmd){
		.op = op, .arg32 = arg,
		.dst_handle = dst ? dst->handle : 0, .dst_offset = doff,
		.src_handle = src ? src->handle : 0, .src_offset = soff,
		.len = len,
	};
}

/*
 * How did the job behind a syncobj end? 1 = finished fine, < 0 = it failed
 * with that error (e.g. -EFAULT), 0 = still running. syncobj waits can't
 * say "failed", so export the fence as a sync_file and ask it.
 */
static int job_status(int on, uint32_t sync)
{
	struct drm_syncobj_handle h = { .handle = sync, .flags =
		DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };
	struct sync_file_info info = { 0 };

	if (ioctl(on, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h))
		return -1000 - errno;
	if (ioctl(h.fd, SYNC_IOC_FILE_INFO, &info))
		info.status = -2000 - errno;
	close(h.fd);
	return info.status;
}

/* Submit `n` commands on `on` and wait. Returns job_status(). */
static int run_on(int on, struct drm_svmd_cmd *cmds, uint32_t n)
{
	uint32_t s = syncobj_new(on);
	int ret = submit_on(on, cmds, n, s);

	if (ret)
		return ret;
	if (wait_on(on, s))
		return -ETIME;
	return job_status(on, s);
}

static int run(struct drm_svmd_cmd *cmds, uint32_t n)
{
	return run_on(fd, cmds, n);
}

/* A command on plain pointers (shared virtual memory). */
static struct drm_svmd_cmd ptr_cmd(uint32_t op, uint32_t arg, void *dst,
				   const void *src, uint64_t len)
{
	return (struct drm_svmd_cmd){ .op = op, .arg32 = arg,
				      .dst_offset = (uintptr_t)dst,
				      .src_offset = (uintptr_t)src, .len = len };
}

static int open_device(void)
{
	int f = open("/dev/dri/renderD128", O_RDWR);

	if (f < 0) {
		perror("open /dev/dri/renderD128");
		exit(1);
	}
	return f;
}

/* debugfs knobs and counters (the driver's svmd_debugfs.c) */
#define DEBUGFS "/sys/kernel/debug/dri/128/"

static void debugfs_write(const char *name, const char *val)
{
	int f = open(name, O_WRONLY);

	if (f < 0 || write(f, val, strlen(val)) < 0)
		perror(name);
	close(f);
}

static unsigned long long stat_get(const char *key)
{
	char line[128];
	unsigned long long v = 0;
	FILE *f = fopen(DEBUGFS "svmd_stats", "r");
	size_t n = strlen(key);

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ' ')
			v = strtoull(line + n, NULL, 0);
	if (f)
		fclose(f);
	return v;
}

static int finish(void)
{
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures != 0;
}

#endif
