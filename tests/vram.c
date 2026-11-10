// SPDX-License-Identifier: GPL-2.0
/*
 * vram - phase 6: memory migrating between system RAM and device VRAM.
 *
 * With PREFER_VRAM set, pages the device faults on are moved into its own
 * memory first. When the CPU touches them, the kernel moves them back.
 * The counters in debugfs (svmd_stats) show each move.
 */
#include <sys/wait.h>

#include "common.h"

#define MiB (1ul << 20)

static void prefer_vram(int on, int yes)
{
	struct drm_svmd_set_param p = { .param = DRM_SVMD_PARAM_PREFER_VRAM,
					.value = yes };

	if (ioctl(on, DRM_IOCTL_SVMD_SET_PARAM, &p)) {
		perror("SET_PARAM PREFER_VRAM");
		exit(1);
	}
}

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

static uint64_t bad_words(const uint32_t *p, size_t words, uint32_t base)
{
	uint64_t bad = 0;

	for (size_t i = 0; i < words; i++)
		bad += p[i] != (uint32_t)i + base;
	return bad;
}

static void test_round_trip(void)
{
	size_t len = 16 * MiB, words = len / 4;
	uint32_t *b = map_anon(len);
	struct drm_svmd_cmd add = ptr_cmd(SVMD_OP_ADD, 1, b, NULL, len);
	unsigned long long to_vram, to_ram, faults;

	puts("round trip: system RAM -> VRAM -> system RAM -> VRAM");
	for (size_t i = 0; i < words; i++)
		b[i] = (uint32_t)i;
	prefer_vram(fd, 1);

	to_vram = stat_get("pages_to_vram");
	CHECK(run(&add, 1) == 1, "device ADD 1 (faults migrate the pages)");
	CHECK(stat_get("pages_to_vram") - to_vram >= words / 1024,
	      "all %zu pages moved into VRAM (%llu)", words / 1024,
	      stat_get("pages_to_vram") - to_vram);

	faults = stat_get("device_faults");
	CHECK(run(&add, 1) == 1, "device ADD 1 again");
	CHECK(stat_get("device_faults") == faults,
	      "...with no new faults: it ran from VRAM as mapped");

	to_ram = stat_get("pages_to_ram");
	CHECK(bad_words(b, words, 2) == 0, "CPU reads i + 2 everywhere");
	CHECK(stat_get("pages_to_ram") - to_ram >= words / 1024,
	      "reading moved the pages back to RAM (%llu)",
	      stat_get("pages_to_ram") - to_ram);

	to_vram = stat_get("pages_to_vram");
	CHECK(run(&add, 1) == 1 && stat_get("pages_to_vram") > to_vram,
	      "device again: back into VRAM");
	CHECK(bad_words(b, words, 3) == 0, "CPU reads i + 3 everywhere");
	munmap(b, len);
	prefer_vram(fd, 0);
}

static void test_copy_in_vram(void)
{
	size_t len = 8 * MiB, words = len / 4;
	uint32_t *a = map_anon(len), *b = map_anon(len);
	struct drm_svmd_cmd c[2] = {
		ptr_cmd(SVMD_OP_COPY, 0, b, a, len),
		ptr_cmd(SVMD_OP_ADD, 5, b, NULL, len),
	};

	puts("both buffers in VRAM: COPY + ADD between them");
	for (size_t i = 0; i < words; i++)
		a[i] = (uint32_t)i;
	prefer_vram(fd, 1);
	CHECK(run(c, 2) == 1, "COPY a -> b, ADD 5");
	CHECK(bad_words(b, words, 5) == 0 && bad_words(a, words, 0) == 0,
	      "b == a + 5, a unchanged");
	munmap(a, len);
	munmap(b, len);
	prefer_vram(fd, 0);
}

static void test_vram_full(void)
{
	/* more than the 256 MiB of VRAM: the rest must stay in RAM */
	size_t len = 320 * MiB, words = len / 4;
	uint32_t *b = map_anon(len);
	struct drm_svmd_cmd fill = ptr_cmd(SVMD_OP_FILL, 0x77, b, NULL, len);
	unsigned long long failed = stat_get("migrations_failed");
	uint64_t bad = 0;

	puts("VRAM full: 320 MiB into 256 MiB");
	prefer_vram(fd, 1);
	CHECK(run(&fill, 1) == 1, "FILL 320 MiB succeeds");
	CHECK(stat_get("migrations_failed") > failed,
	      "some ranges couldn't move (%llu) and stayed in RAM",
	      stat_get("migrations_failed") - failed);
	for (size_t i = 0; i < words; i++)
		bad += b[i] != 0x77;
	CHECK(bad == 0, "all 320 MiB correct (%llu wrong)", (unsigned long long)bad);
	munmap(b, len);
	prefer_vram(fd, 0);
}

static void test_freed_on_exit(void)
{
	/* a child fills most of VRAM and exits without cleaning up; the
	 * parent must then be able to use that VRAM itself */
	size_t len = 200 * MiB;
	struct drm_svmd_cmd fill;
	unsigned long long failed;
	int status;

	puts("VRAM held by a process is freed when it exits");
	if (fork() == 0) {
		int cfd = open_device();
		uint32_t *b = map_anon(len);

		prefer_vram(cfd, 1);
		fill = ptr_cmd(SVMD_OP_FILL, 1, b, NULL, len);
		_exit(run_on(cfd, &fill, 1) == 1 ? 0 : 1);
	}
	wait(&status);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "child moved 200 MiB into VRAM and exited");

	failed = stat_get("migrations_failed");
	{
		uint32_t *b = map_anon(len);

		prefer_vram(fd, 1);
		fill = ptr_cmd(SVMD_OP_FILL, 2, b, NULL, len);
		CHECK(run(&fill, 1) == 1 &&
		      stat_get("migrations_failed") == failed,
		      "parent then moved 200 MiB in too: nothing left behind");
		munmap(b, len);
		prefer_vram(fd, 0);
	}
}

static void test_off_by_default(void)
{
	size_t len = 4 * MiB;
	uint32_t *b = map_anon(len);
	struct drm_svmd_cmd fill = ptr_cmd(SVMD_OP_FILL, 3, b, NULL, len);
	unsigned long long moved = stat_get("pages_to_vram");

	puts("without PREFER_VRAM nothing migrates");
	CHECK(run(&fill, 1) == 1 && stat_get("pages_to_vram") == moved,
	      "FILL 4 MiB stayed in system RAM");
	munmap(b, len);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
	{ "round_trip", test_round_trip },
	{ "copy_in_vram", test_copy_in_vram },
	{ "vram_full", test_vram_full },
	{ "freed_on_exit", test_freed_on_exit },
	{ "off_by_default", test_off_by_default },
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
