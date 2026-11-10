#!/bin/sh
# Run a shell command inside the running guest, as root.
#   scripts/guest.sh 'insmod /lab/driver/svmd.ko && dmesg | tail'
LAB=$(cd "$(dirname "$0")/.." && pwd)
exec ssh -p 2224 -i "$LAB/run/guest_key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o LogLevel=ERROR -o ConnectTimeout=3 -o BatchMode=yes root@localhost "$@"
