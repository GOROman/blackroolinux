#!/bin/bash
# ============================================================================
#  serial-autoupload.sh — wait for kloader, then upload, without babysitting
# ============================================================================
#  blackroo-serial.py's `upload` waits for the BK>> beacon and then gives up.
#  That means whoever is at the console and whoever is at the keyboard have to
#  agree on timing. This just keeps retrying, so the console can be brought to
#  `Serial Shell (115200)` whenever, and the upload fires by itself.
#
#    ./scripts/serial-autoupload.sh [exe] [port]
#
#  Ctrl-C to stop. Logs every attempt so a failure mid-upload is visible.
# ============================================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="${1:-$HERE/output/blackroo.exe}"
PORT="${2:-/dev/ttyUSB1}"
# Optional third argument: the kernel command line. Without it the kernel uses
# whatever kloader's settings build. With it, a root device can be tested
# WITHOUT burning a disc - the kernel comes over the wire, the disc in the
# drive supplies the filesystem.
CMDLINE="${3:-}"
LOG="$HERE/logs/serial-autoupload.log"

[ -f "$EXE" ] || { echo "no such file: $EXE" >&2; exit 1; }
mkdir -p "$HERE/logs"

echo "watching $PORT for kloader; will send $(basename "$EXE") ($(stat -c%s "$EXE") bytes)"
[ -n "$CMDLINE" ] && echo "cmdline: $CMDLINE"
echo "on the PS1: kloader CD -> 'Serial Shell (115200)'  (needs a real pad, not BlueRetro in DEV_KB)"

n=0
while true; do
    n=$((n+1))
    # Belt and braces: check the exit code AND the output. blackroo-serial.py
    # used to exit 0 on a failed upload (fixed), and this loop reported
    # "UPLOADED" for an upload that never happened. Never trust one signal for
    # something that cannot be seen from here.
    if [ -n "$CMDLINE" ]; then
        out="$(python3 "$HERE/tools/host/blackroo-serial.py" "$PORT" --timeout 25 \
               --cmdline "$CMDLINE" upload "$EXE" --no-console 2>&1)"
    else
        out="$(python3 "$HERE/tools/host/blackroo-serial.py" "$PORT" --timeout 25 \
               upload "$EXE" --no-console 2>&1)"
    fi
    rc=$?
    printf '%s\n' "$out" >>"$LOG"
    if [ $rc -eq 0 ] && ! printf '%s' "$out" | grep -qi "error\|timeout\|no beacon"; then
        echo "attempt $n: UPLOADED — kernel is running, attach with:"
        echo "  python3 tools/host/blackroo-serial.py $PORT console"
        exit 0
    fi
    printf "\rattempt %d: no beacon yet (%s)  " "$n" "$(date +%H:%M:%S)"
    # A failure that returns instantly is not a timeout - it is usually the
    # port being held by another process (a stale console session, or this
    # script's own previous run). Without this the loop spins flat out.
    if printf '%s' "$out" | grep -qi "could not open\|busy\|permission"; then
        printf "\r  port busy: %s\n" \
               "$(printf '%s' "$out" | grep -i 'could not open\|busy\|permission' | head -1)"
        sleep 5
    fi
done
