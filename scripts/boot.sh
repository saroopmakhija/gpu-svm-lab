#!/usr/bin/env bash
# Boot the guest with our QEMU + the svmd device, in the background, and wait
# until it answers SSH.
#
#   scripts/boot.sh        start (restarts our guest if one is running)
#   scripts/guest.sh CMD   run CMD in the guest as root
#   scripts/shutdown.sh    stop it
#
# Safety: the root disk is opened with -snapshot (writes go to a throwaway
# overlay; alpine.img never changes). SSH is port 2224 - the kernel lab uses
# 2222, the BPF project 2223. Only the QEMU whose pid is in run/qemu.pid is
# ever stopped.
set -euo pipefail
LAB=$(cd "$(dirname "$0")/.." && pwd)
QEMU=$LAB/qemu-src/build/qemu-system-aarch64
IMAGE=${IMAGE:-$LAB/images/Image}
ROOTFS=$LAB/images/rootfs.img
PORT=2224
RUN=$LAB/run
mkdir -p "$RUN"

[ -x "$QEMU" ]   || { echo "no QEMU: build qemu-src first" >&2; exit 1; }
[ -f "$IMAGE" ]  || { echo "no kernel: scripts/vm.sh scripts/kernel-build.sh" >&2; exit 1; }
[ -f "$ROOTFS" ] || { echo "$ROOTFS not found: scripts/vm.sh scripts/rootfs-build.sh" >&2; exit 1; }

"$LAB/scripts/shutdown.sh" >/dev/null 2>&1 || true
rm -f "$RUN/console.log"
if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  echo "port $PORT is in use by something else; not starting" >&2; exit 1
fi

# -device svmd: our device, plugged into the virt machine's PCIe root bus.
# gic-version=3 gives the interrupt controller with an ITS, which MSI-X needs.
"$QEMU" \
  -machine virt,gic-version=3 -accel hvf -cpu host -smp 4 -m 4G \
  -kernel "$IMAGE" \
  -append "root=/dev/vda rw console=ttyAMA0 nokaslr" \
  -drive if=none,file="$ROOTFS",format=raw,id=hd0 -snapshot \
  -device virtio-blk-pci,drive=hd0 \
  -netdev user,id=n0,hostfwd=tcp::$PORT-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-rng-pci \
  -device svmd \
  -virtfs local,path="$LAB",mount_tag=lab,security_model=none,readonly=on \
  -d unimp -D "$RUN/qemu.log" \
  -display none -monitor unix:"$RUN/monitor.sock",server,nowait \
  -chardev socket,id=ser0,path="$RUN/serial.sock",server=on,wait=off,logfile="$RUN/console.log" \
  -serial chardev:ser0 \
  -daemonize -pidfile "$RUN/qemu.pid"

for _ in $(seq 1 90); do
  if "$LAB/scripts/guest.sh" true 2>/dev/null; then
    "$LAB/scripts/guest.sh" 'mkdir -p /lab; mountpoint -q /lab ||
      mount -t 9p -o trans=virtio,version=9p2000.L lab /lab'
    echo "guest up: $("$LAB/scripts/guest.sh" uname -r)  (console: run/console.log)"
    exit 0
  fi
  sleep 1
done
echo "guest did not answer SSH in 90s; see $RUN/console.log" >&2
exit 1
