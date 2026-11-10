#!/bin/bash
# Build a second guest kernel with KASAN (the kernel's memory-error checker:
# use-after-free, out-of-bounds) and the driver against it. Runs INSIDE the
# Lima VM (scripts/vm.sh scripts/kasan-build.sh). Slower kernel, same tests:
#   IMAGE=images/Image-kasan scripts/boot.sh
#   scripts/guest.sh 'insmod /lab/images/svmd-kasan.ko && /lab/tests/svm'
set -euo pipefail

SRC=$HOME/gpu-svm/linux
OUT=$HOME/gpu-svm/build-kasan
LAB=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -f "$OUT/.config" ]; then
  mkdir -p "$OUT"
  cp "$HOME/gpu-svm/build/.config" "$OUT/.config"
  "$SRC/scripts/config" --file "$OUT/.config" \
    -e KASAN -e KASAN_GENERIC -e KASAN_INLINE -d KASAN_OUTLINE \
    -e DEBUG_KMEMLEAK -e PAGE_POISONING \
    --set-str LOCALVERSION "-svmd-kasan"
  make -C "$SRC" O="$OUT" ARCH=arm64 olddefconfig
fi
make -C "$SRC" O="$OUT" ARCH=arm64 -j"$(nproc)" Image modules
cp "$OUT/arch/arm64/boot/Image" "$LAB/images/Image-kasan"

# The module must be built against this exact kernel: build a copy of the
# driver source here, so it doesn't clash with the normal build.
rm -rf "$HOME/gpu-svm/driver-kasan"
cp -r "$LAB/driver" "$HOME/gpu-svm/driver-kasan"
make -C "$HOME/gpu-svm/driver-kasan" KDIR="$OUT" clean >/dev/null
make -C "$HOME/gpu-svm/driver-kasan" KDIR="$OUT" \
  SVMD_INCLUDE="$LAB/include" >/dev/null
cp "$HOME/gpu-svm/driver-kasan/svmd.ko" "$LAB/images/svmd-kasan.ko"
echo "kasan kernel + driver: images/Image-kasan, images/svmd-kasan.ko"
