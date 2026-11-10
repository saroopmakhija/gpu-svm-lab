#!/bin/sh
# Run a command inside the Lima VM `kdev`, in this folder.
# (The Mac home directory is mounted at the same path inside the VM.)
#   scripts/vm.sh scripts/kernel-build.sh
set -eu
LAB=$(cd "$(dirname "$0")/.." && pwd)
exec limactl shell --workdir "$LAB" kdev -- "$@"
