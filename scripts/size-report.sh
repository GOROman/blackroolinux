#!/bin/bash
# ============================================================================
#  size-report.sh — where the kernel's 700-odd KB actually is
# ============================================================================
#  docs/28 makes the whole userspace plan depend on shrinking the kernel, and
#  a size pass is only honest if every step is measured. Run this before and
#  after a change; diff the two.
#
#    ./scripts/size-report.sh                  # print the report
#    ./scripts/size-report.sh > logs/size-baseline.txt
#
#  Sizes are .text only — that is what the boot line calls "kernel code".
# ============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
K="$ROOT/blackroo"
SDK="$ROOT/sdk/toolchain"
[ -f /lib/ld-linux.so.2 ] || [ -f /lib32/ld-linux.so.2 ] || SDK="$ROOT/sdk/toolchain-local"
SIZE="$SDK/bin/mipsel-linux-size"
[ -x "$SIZE" ] || { echo "no mipsel-linux-size in $SDK/bin" >&2; exit 1; }

# .text of an object, or the sum of an archive's members. Always prints a
# number - an empty string here silently corrupts the report's sort order.
t() {
    [ -f "$1" ] || { echo 0; return; }
    "$SIZE" "$1" 2>/dev/null \
      | awk '$1 ~ /^[0-9]+$/ { n += $1 } END { print n + 0 }'
}

cd "$K" || exit 1

echo "=== Blackroo kernel size report — $(date -u '+%Y-%m-%d %H:%M UTC') ==="
echo
if [ -f linux ]; then
    echo "--- linked image (blackroo/linux) ---"
    "$SIZE" linux
    echo
fi
echo "--- top-level objects (.text) ---"
for f in arch/mipsnommu/kernel/head.o arch/mipsnommu/kernel/init_task.o \
         arch/mipsnommu/kernel/kernel.o arch/mipsnommu/mm/mm.o \
         arch/mipsnommu/ps/ps.o kernel/kernel.o mmnommu/mmnommu.o fs/fs.o \
         ipc/ipc.o net/network.o drivers/block/block.o drivers/char/char.o \
         drivers/misc/misc.o drivers/video/video.o lib/lib.a \
         arch/mipsnommu/lib/lib.a; do
    [ -f "$f" ] || continue
    printf "%8d  %s\n" "$(t "$f")" "$f"
done | sort -rn
echo
echo "--- biggest single objects ---"
for f in $(find . -name '*.o' -not -name 'built-in.o' 2>/dev/null); do
    case "$(basename "$f")" in
      fs.o|char.o|block.o|network.o|kernel.o|mmnommu.o|ps.o|core.o|ext2.o|proc.o|video.o|misc.o|mm.o|ipc.o) continue ;;
    esac
    printf "%8d  %s\n" "$(t "$f")" "${f#./}"
done | sort -rn | head -30
