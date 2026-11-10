# gpu-svm-lab

A GPU-like PCI device ("SVMD") that I designed and emulated in QEMU, and the
Linux 7.3 DRM driver for it — built up to **shared virtual memory**: a
program passes plain `malloc()` pointers to the device, which page-faults
them in through `drm_gpusvm`, the same kernel framework Intel's `xe`
driver uses. Pages can also **migrate into the device's own memory (VRAM)**
and back.

```c
int *a = malloc(64 << 20);                 // never touched by the CPU
submit(fd, FILL(a, 7, 64 << 20));          // the device faults it in
wait(fd);  assert(a[123456] == 7);         // the CPU sees the result
```

Both sides follow one hardware manual: [docs/hardware-spec.md](docs/hardware-spec.md).

## What's in it

| layer | what | where |
|---|---|---|
| device | command ring + doorbell, fences, MSI-X, 4-level MMU, non-coherent TLB, replayable faults, VRAM, a copy engine, fault injection | `qemu-device/svmd.c` |
| scheduling | `drm_sched`, hardware `dma_fence`s, syncobjs, explicit + implicit sync | `driver/svmd_{ring,sched,drv}.c` |
| memory | per-process device page tables, TLB invalidation | `driver/svmd_mmu.c` |
| SVM | `drm_gpusvm`: fault handling, MMU notifier, garbage collection | `driver/svmd_{svm,fault}.c` |
| VRAM | `MEMORY_DEVICE_PRIVATE` pages, `drm_pagemap` migration | `driver/svmd_{vram,copy}.c` |
| observability | tracepoints, debugfs counters, an invariant checker and flight recorder | `driver/svmd_{trace.h,debugfs.c}` |

## Results

- **5 self-tests at every probe**, including one that *proves* the TLB is
  stale until invalidated (`driver/svmd_selftest.c`)
- **4 user-space suites, 73/73 checks passing** — `phase2` (26: buffers,
  sync), `svm` (19: shared virtual memory), `vram` (16: migration),
  `stress` (12: fault injection, multi-process churn)
- **Clean under KASAN and kmemleak** (use-after-free / out-of-bounds /
  leak detectors), and lockdep throughout
- **Every key test proven able to fail**: `scripts/mutation-check.py`
  re-introduces 6 real bugs one at a time; all 6 are caught
- **9 real bugs found and fixed**, most by tests and checkers rather than
  by reading

## Measuring it with BPF

The driver has tracepoints (`driver/svmd_trace.h`); `scripts/svmd-trace.bt`
turns them into histograms. A sample run (an emulated device, so the shape matters more than the absolute numbers):

- **fault resolution** is bimodal: ~2–8 µs for small ranges, ~0.25–1 ms
  for 2 MiB ranges (fetch 512 pages, write 512 entries)
- **invalidation** (clear entries + flush TLB + wait) mostly 4–8 µs
- **migration** 8,192 pages each way, RAM ↔ VRAM via the copy engine

## Phases

| phase | goal |
|---|---|
| 0 | hardware manual |
| 1 | device appears, registers answer, 3 MSI-X interrupts arrive |
| 2 | command ring, doorbell, FILL/COPY/ADD/FENCE, DRM driver + ioctls |
| 2b | `drm_sched` + `dma_fence`, syncobjs, explicit + implicit sync |
| 3 | device page tables, TLB, invalidation |
| 4 | replayable faults (retry / abort) |
| 5 | SVM with `drm_gpusvm` |
| 6 | VRAM + migration with `drm_pagemap`, copy engine |
| 7 | fault injection, stress, KASAN, mutation checks, tracepoints + BPF |

## Layout

```
docs/          hardware-spec.md (the contract)
include/       svmd_regs.h - the spec as numbers, shared by QEMU and the driver
include/uapi/  svmd_drm.h - the ioctls user programs call
qemu-device/   svmd.c - the device (symlinked into qemu-src/hw/misc)
driver/        the Linux driver (one job per file)
tests/         phase2.c svm.c vram.c stress.c, common.h
scripts/       build, boot, guest access, mutation-check, kasan, bpftrace
qemu-src/      QEMU 11.0.2 + our 2-line build hook         (not ours)
images/        guest kernels + rootfs                       (built)
run/           guest logs, sockets, VM-only SSH key         (runtime)
```

## Running it

On the Mac, from this folder:

```sh
# once
./scripts/qemu-setup.sh && ninja -C qemu-src/build qemu-system-aarch64
./scripts/vm.sh scripts/kernel-build.sh      # Linux 7.3-rc6, in the Lima VM
./scripts/vm.sh scripts/rootfs-build.sh      # Alpine 3.24 disk

# build and run
ninja -C qemu-src/build qemu-system-aarch64  # after editing qemu-device/
./scripts/vm.sh make -C driver               # after editing driver/
./scripts/vm.sh make -C tests                # after editing tests/
./scripts/boot.sh                            # (re)start the guest
./scripts/guest.sh 'insmod /lab/driver/svmd.ko; dmesg | grep svmd'
./scripts/guest.sh '/lab/tests/svm'          # or phase2, vram, stress
./scripts/guest.sh '/lab/tests/svm remap'    # one test by name
./scripts/shutdown.sh

# checks
./scripts/mutation-check.py                  # do the tests catch real bugs?
./scripts/vm.sh scripts/kasan-build.sh       # then:
IMAGE=images/Image-kasan ./scripts/boot.sh
./scripts/guest.sh 'insmod /lab/images/svmd-kasan.ko && /lab/tests/stress'
./scripts/guest.sh 'bpftrace /lab/scripts/svmd-trace.bt'   # while tests run
```

Unloading: `echo 0000:00:04.0 > /sys/bus/pci/drivers/svmd/unbind; rmmod svmd`
(VRAM pins the module until the device is unbound).

The guest sees this folder read-only at `/lab`; its disk is opened with
`-snapshot`, so every boot starts clean. `scripts/console.py` reaches the
guest's serial console when SSH can't.

### Debugging knobs (`/sys/kernel/debug/dri/128/`)

| file | what |
|---|---|
| `svmd_stats` | device + driver counters: faults, TLB, invalidations, migrations, audit |
| `svmd_inject` | spec §9 fault injection: duplicate faults, slow invalidation, spurious interrupts, no TLB |
| `svmd_fault_delay_ms` | hold every fault (tests race the CPU against a stalled device) |
| `svmd_job_irq_delay_ms` | retire finished jobs late (tests the abort path) |
| `svmd_audit` | after every fault, check each device entry maps the page the CPU maps |
| `svmd_inval_log` | flight recorder of binds/zaps/GC for the first bad address |

## The one kernel change

`scripts/kernel-build.sh` adds `DRM_SVMD_HELPERS` to `drivers/gpu/drm/Kconfig`:
a switch that builds the DRM helpers we use (GEM DMA, scheduler, GPU SVM)
into the kernel — they have no switch of their own; normally only in-tree
drivers can ask for them. That's why `uname -r` ends in `-dirty`.
