#!/bin/bash
# ============================================================================
#  serial-capture.sh — record everything the console says, from the first byte
# ============================================================================
#  GR-010: a milestone without a dated capture is a claim, not a result. This
#  just listens and writes, so a boot can be caught without anyone having to
#  attach at the right moment. Tees to the terminal as well.
#
#    ./scripts/serial-capture.sh [port] [seconds]
# ============================================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-/dev/ttyUSB1}"
SECS="${2:-900}"
OUT="$HERE/docs/captures/$(date +%Y-%m-%d-%H%M)-boot.txt"
mkdir -p "$HERE/docs/captures"
echo "capturing $PORT for ${SECS}s -> $OUT"
exec python3 - "$PORT" "$SECS" "$OUT" <<'PY'
import serial, sys, time, datetime
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.5)
f = open(out, "wb")
f.write(("Blackroo serial capture %s on %s\n%s\n"
         % (datetime.datetime.now().isoformat(timespec="seconds"), port,
            "-"*60)).encode())
f.flush()
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        f.write(d); f.flush()
        sys.stdout.write(d.decode("ascii", "replace")); sys.stdout.flush()
f.close(); s.close()
PY
