#!/bin/sh
# Plug our device into the QEMU source tree (safe to run repeatedly).
#
# Our files stay in qemu-device/ and include/ - the source of truth.
# QEMU's tree only gets symlinks to them plus two one-line build hooks,
# so `git diff` in qemu-src shows exactly what we changed.
set -eu
LAB=$(cd "$(dirname "$0")/.." && pwd)
Q=$LAB/qemu-src

ln -sf "$LAB/qemu-device/svmd.c" "$Q/hw/misc/svmd.c"
ln -sf "$LAB/include/svmd_regs.h" "$Q/hw/misc/svmd_regs.h"

# meson: compile svmd.c when CONFIG_SVMD is on
grep -q "CONFIG_SVMD" "$Q/hw/misc/meson.build" ||
  printf "\nsystem_ss.add(when: 'CONFIG_SVMD', if_true: files('svmd.c'))\n" \
    >> "$Q/hw/misc/meson.build"

# Kconfig: CONFIG_SVMD exists, and is on wherever PCI + MSI work
grep -q "^config SVMD" "$Q/hw/misc/Kconfig" ||
  printf "\nconfig SVMD\n    bool\n    default y\n    depends on PCI && MSI_NONBROKEN\n" \
    >> "$Q/hw/misc/Kconfig"

echo "svmd wired into $Q"
