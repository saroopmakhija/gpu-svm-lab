#!/bin/bash
# Build images/rootfs.img: a small Alpine Linux disk for the guest.
# Runs INSIDE the Lima VM (scripts/vm.sh scripts/rootfs-build.sh); uses sudo.
#
# The VM is arm64 like the guest, so we can chroot into the Alpine files and
# install packages directly - no emulation needed.
set -euo pipefail

LAB=$(cd "$(dirname "$0")/.." && pwd)
VER=3.24.2
TAR=alpine-minirootfs-$VER-aarch64.tar.gz
SHA=9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773
URL=https://dl-cdn.alpinelinux.org/alpine/v${VER%.*}/releases/aarch64/$TAR
WORK=$HOME/gpu-svm/rootfs
DIR=$WORK/root
IMG=$LAB/images/rootfs.img
KEY=$LAB/run/guest_key.pub   # a key made only for this VM (scripts/guest.sh uses it)

mkdir -p "$WORK" "$LAB/images"
[ -f "$WORK/$TAR" ] || curl -fsSL -o "$WORK/$TAR" "$URL"
echo "$SHA  $WORK/$TAR" | sha256sum -c -

sudo rm -rf "$DIR"; mkdir -p "$DIR"
sudo tar -xzf "$WORK/$TAR" -C "$DIR"
sudo cp /etc/resolv.conf "$DIR/etc/resolv.conf"

# Inside the guest's files: packages, services, logins.
sudo chroot "$DIR" /bin/sh -eu <<'EOF'
apk add --no-cache openrc openssh kmod pciutils util-linux bash \
    strace gdb python3 busybox-openrc bpftrace
# services: devices, filesystems, network, ssh
for s in sysfs devfs dmesg; do rc-update add $s sysinit; done
for s in modules sysctl hostname bootmisc syslog networking; do rc-update add $s boot; done
rc-update add sshd default
# serial console login on ttyAMA0 (QEMU's -serial)
echo 'ttyAMA0::respawn:/sbin/getty -L 115200 ttyAMA0 vt100' >> /etc/inittab
echo svmd-guest > /etc/hostname
printf 'auto lo\niface lo inet loopback\nauto eth0\niface eth0 inet dhcp\n' \
    > /etc/network/interfaces
printf '/dev/vda / ext4 rw,relatime 0 1\ndebugfs /sys/kernel/debug debugfs defaults 0 0\n' > /etc/fstab
# root: key login only
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
# our boot script retries every second; don't ban it for that
echo 'PerSourcePenalties no' >> /etc/ssh/sshd_config
mkdir -p /root/.ssh /lab
passwd -d root
EOF
sudo install -m 600 "$KEY" "$DIR/root/.ssh/authorized_keys"

# Pack the directory into an ext4 image (no loop mount needed).
# (built on the VM's disk, then copied: root can't write the Mac share)
rm -f "$WORK/rootfs.img"
truncate -s 2G "$WORK/rootfs.img"
sudo mkfs.ext4 -q -L svmd-root -d "$DIR" "$WORK/rootfs.img"
sudo chown "$(id -u):$(id -g)" "$WORK/rootfs.img"
cp "$WORK/rootfs.img" "$IMG"
echo "rootfs: $IMG"
