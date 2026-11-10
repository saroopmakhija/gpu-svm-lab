#!/bin/bash
# Build the guest kernel. Runs INSIDE the Lima VM (scripts/vm.sh kernel-build.sh).
#
# Source lives on the VM's own disk: Linux has files whose names differ only
# in case, which the macOS-shared folder can't hold. The finished Image is
# copied back to images/ so QEMU on the Mac can boot it.
set -euo pipefail

TAG=v7.3-rc6
SRC=$HOME/gpu-svm/linux
OUT=$HOME/gpu-svm/build
LAB=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -d "$SRC" ]; then
  git clone --depth 1 --branch "$TAG" \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$SRC"
fi

# Our one kernel change: a switch that builds the DRM helpers our out-of-tree
# driver uses INTO the kernel. (They have no switch of their own - normally
# only in-tree drivers can ask for them.) `git -C $SRC diff` shows it.
if ! grep -q "^config DRM_SVMD_HELPERS" "$SRC/drivers/gpu/drm/Kconfig"; then
  cat >> "$SRC/drivers/gpu/drm/Kconfig" <<'KC'

config DRM_SVMD_HELPERS
	bool "Built-in helpers for the out-of-tree svmd driver (gpu-svm-lab)"
	depends on DRM=y
	select DRM_GEM_DMA_HELPER
	select DRM_SCHED
	select DEVICE_PRIVATE
	select DRM_GPUSVM
KC
fi

if [ ! -f "$OUT/.config" ]; then
  make -C "$SRC" O="$OUT" ARCH=arm64 defconfig
  cfg() { "$SRC/scripts/config" --file "$OUT/.config" "$@"; }

  # GPU subsystem
  cfg -e DRM -e DRM_SVMD_HELPERS
  # SVM: device memory represented as struct pages (phase 6)
  cfg -e MEMORY_HOTPLUG -e MEMORY_HOTREMOVE -e ZONE_DEVICE -e DEVICE_PRIVATE
  # interrupts our device uses
  cfg -e PCI_MSI -e ARM_GIC_V3_ITS
  # shared folders from the Mac (9p over virtio)
  cfg -e NET_9P -e NET_9P_VIRTIO -e 9P_FS
  # catch our own bugs early
  cfg -e DEBUG_KERNEL -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
      -e DEBUG_INFO_DWARF5 -e DYNAMIC_DEBUG
  # tag the build so `uname -r` says what it is
  cfg --set-str LOCALVERSION "-svmd"

  make -C "$SRC" O="$OUT" ARCH=arm64 olddefconfig
fi

# options added after the first build (kept in sync on every run)
#   SW_SYNC: test-only fences a program can signal itself (tests/phase2)
#   tracing + BTF: so bpftrace can attach to the driver's tracepoints
"$SRC/scripts/config" --file "$OUT/.config" -e DRM_SVMD_HELPERS \
  -e SYNC_FILE -e SW_SYNC -e DEBUG_FS \
  -e FTRACE -e FUNCTION_TRACER -e KPROBES -e KPROBE_EVENTS \
  -e UPROBE_EVENTS -e BPF_EVENTS -d DEBUG_INFO_REDUCED \
  -e DEBUG_INFO_BTF -e DEBUG_INFO_BTF_MODULES
make -C "$SRC" O="$OUT" ARCH=arm64 olddefconfig
make -C "$SRC" O="$OUT" ARCH=arm64 -j"$(nproc)" Image modules
mkdir -p "$LAB/images"
cp "$OUT/arch/arm64/boot/Image" "$LAB/images/Image"
echo "kernel: $(cat "$OUT/include/config/kernel.release") -> $LAB/images/Image"
