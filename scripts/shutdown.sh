#!/bin/sh
# Stop OUR guest (only the QEMU whose pid we recorded).
LAB=$(cd "$(dirname "$0")/.." && pwd)
PIDFILE=$LAB/run/qemu.pid
[ -f "$PIDFILE" ] || { echo "no guest running"; exit 0; }
PID=$(cat "$PIDFILE")
if kill -0 "$PID" 2>/dev/null; then
  kill "$PID"
  for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
  echo "stopped guest (pid $PID)"
fi
rm -f "$PIDFILE"
