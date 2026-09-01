#!/bin/bash
#
# ============================================================================
#  Blackroo Linux — Single Build Entry Point
# ============================================================================
#
#  Linux 2.4 (uClinux / no-MMU) for the Sony PlayStation 1 (MIPS R3000A).
#  Boots to an interactive BusyBox shell over the PlayStation's serial port.
#
#  Everything is driven from this one script. No system cross-compiler needed:
#  the EGCS 2.91.66 MIPS toolchain is bundled in ./sdk/toolchain (see sdk/README.md).
#
#  USAGE:
#     ./build.sh deps        Check host prerequisites (32-bit libs, genext2fs)
#     ./build.sh kernel      Configure + compile the kernel  -> output/linux.elf
#     ./build.sh initrd      Build the ext2 ramdisk root fs  -> output/initrd.img
#     ./build.sh mcard       Create PS1 memory-card images   -> output/*.mcd
#     ./build.sh convert     Wrap kernel + initrd into a PS-EXE -> output/blackroo.exe
#     ./build.sh all         deps -> kernel -> initrd -> convert  (the usual path)
#     ./build.sh clean       Remove build artifacts (keeps config)
#     ./build.sh status      Show toolchain + output state
#
#  QUICK START:  ./build.sh all   then upload output/blackroo.exe (see README.md)
#
#  License: GPL v2 (kernel) — see SOURCE-ATTRIBUTION.md for component origins.
# ============================================================================

set -euo pipefail

# ── Paths ───────────────────────────────────────────────────────────────────
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$PROJECT_ROOT/blackroo"
SDK_DIR="$PROJECT_ROOT/sdk/toolchain"
TOOLS_DIR="$PROJECT_ROOT/tools"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
CONFIG_DIR="$PROJECT_ROOT/configs/kernel"
INITRD_SKEL="$PROJECT_ROOT/initrd/skeleton"
BUILD_DIR="$PROJECT_ROOT/build"
OUTPUT_DIR="$PROJECT_ROOT/output"
LOG_DIR="$PROJECT_ROOT/logs"

# ── Toolchain (bundled EGCS 2.91.66 — see sdk/README.md) ────────────────────
# The bundled toolchain is 32-bit x86 and needs /lib/ld-linux.so.2. On hosts
# without 32-bit support (and without root to `apt install libc6:i386`), run
# ./sdk/setup-local-toolchain.sh once: it builds sdk/toolchain-local, a copy
# whose ELF interpreter points at sdk/i386-runtime/. Prefer it when present
# and the system loader is missing.
if [ ! -f /lib/ld-linux.so.2 ] && [ ! -f /lib32/ld-linux.so.2 ] \
   && [ -x "$PROJECT_ROOT/sdk/toolchain-local/bin/mipsel-linux-gcc" ]; then
    SDK_DIR="$PROJECT_ROOT/sdk/toolchain-local"
fi

TC_BIN="$SDK_DIR/bin"
TC_LIB="$SDK_DIR/lib/gcc-lib/mipsel-linux/egcs-2.91.66"
export GCC_EXEC_PREFIX="$SDK_DIR/lib/gcc-lib/"

# Which kernel config to start from: 8mb | 2mb | auto  (override: DEFCONFIG=...)
# The target console is a 2 MB stock SCPH-750x (docs/21-TARGET-CONSOLE.md);
# neither of Chelson's machines has the 8 MB mod. Defaulting to the 8 MB config
# built a kernel that believed in memory the hardware does not have - it boots,
# because everything in use happens to sit low and PS1 RAM mirrors, but it is a
# corruption waiting for an allocation above 2 MB. See GR-015.
# Override for a modded console with:  DEFCONFIG=blackroo_8mb_defconfig ./build.sh kernel
DEFCONFIG="${DEFCONFIG:-blackroo_2mb_defconfig}"

# ── Pretty output ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; BLU='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${BLU}[*]${NC} $1"; }
ok()    { echo -e "${GRN}[+]${NC} $1"; }
warn()  { echo -e "${YLW}[!]${NC} $1"; }
err()   { echo -e "${RED}[x]${NC} $1" >&2; }
die()   { err "$1"; exit 1; }

mkdirs() { mkdir -p "$BUILD_DIR" "$OUTPUT_DIR" "$LOG_DIR"; }

# ── deps: verify the host can build at all ──────────────────────────────────
cmd_deps() {
    info "Checking host prerequisites..."
    local fail=0

    if [ -x "$TC_BIN/mipsel-linux-gcc" ] && "$TC_BIN/mipsel-linux-gcc" --version >/dev/null 2>&1; then
        ok "Cross toolchain: $("$TC_BIN/mipsel-linux-gcc" --version 2>&1 | head -1) (sdk/toolchain)"
    else
        err "Cross toolchain in sdk/toolchain won't run."
        echo "    The bundled EGCS toolchain is 32-bit. Install 32-bit support:"
        echo "      sudo apt install libc6:i386"
        fail=1
    fi

    if [ "$(uname -s)" = "Darwin" ] && [ -x "$TC_BIN/mipsel-linux-gcc" ]; then
        ok "macOS-native EGCS toolchain (no Linux loader needed)"
    elif [ -f /lib/ld-linux.so.2 ] || [ -f /lib32/ld-linux.so.2 ] || ldconfig -p 2>/dev/null | grep -q ld-linux.so.2; then
        ok "32-bit loader present"
    else
        err "32-bit loader missing -> sudo apt install libc6:i386"; fail=1
    fi

    command -v gcc >/dev/null      && ok "Native gcc (host tools)"      || { err "gcc missing -> sudo apt install build-essential"; fail=1; }
    command -v genext2fs >/dev/null && ok "genext2fs (initrd, no root)" || { warn "genext2fs missing -> sudo apt install genext2fs (needed for initrd)"; }

    [ "$fail" -eq 0 ] && ok "Host is ready to build." || die "Fix the items above, then re-run."
}

# ── Build host-side converter tools (native gcc) ────────────────────────────
build_host_tools() {
    info "Building host tools (native gcc)..."
    ( unset GCC_EXEC_PREFIX
    # elf2psexe needs Linux's <elf.h>; on macOS it is prebuilt in the
    # Docker/Linux host-tools step. Keep a valid existing binary there.
    if [ ! -x "$TOOLS_DIR/elf2psexe" ]; then
        gcc -O2 -Wall -o "$TOOLS_DIR/elf2psexe" "$TOOLS_DIR/elf2psexe.c"
    fi
    [ -x "$TOOLS_DIR/addpsexe_initrd" ] || gcc -O2 -Wall -o "$TOOLS_DIR/addpsexe_initrd" "$TOOLS_DIR/addpsexe_initrd.c"
    [ -x "$TOOLS_DIR/host/mkmemcard" ] || gcc -O2 -Wall -o "$TOOLS_DIR/host/mkmemcard" "$TOOLS_DIR/host/mkmemcard.c"
      # kernel-internal host helpers
    ( cd "$KERNEL_DIR" && \
      [ -x scripts/mkdep ] || gcc -o scripts/mkdep scripts/mkdep.c; \
      [ -x scripts/split-include ] || gcc -o scripts/split-include scripts/split-include.c; \
      if [ -f drivers/char/conmakehash.c ] && [ ! -x drivers/char/conmakehash ]; then gcc -O2 -o drivers/char/conmakehash drivers/char/conmakehash.c; fi )
    )
    ok "Host tools built (tools/elf2psexe, tools/addpsexe_initrd, tools/host/mkmemcard)"
}

# ── Regenerate include/linux/autoconf.h from .config ────────────────────────
gen_autoconf() {
    local cfg="$KERNEL_DIR/.config"
    local out="$KERNEL_DIR/include/linux/autoconf.h"
    set +e
    python3 - "$cfg" "$out" <<'PYGEN'
import re, sys, time
cfg, out = sys.argv[1], sys.argv[2]
lines = ["/*", " * Automatically generated by build.sh from .config -- DO NOT EDIT.", " */", "",
         "#define AUTOCONF_INCLUDED", ""]
for raw in open(cfg):
    raw = raw.strip()
    m = re.match(r'^(CONFIG_\w+)=(.*)$', raw)
    if m:
        key, val = m.group(1), m.group(2)
        if val == 'y':
            lines.append("#define %s 1" % key)
        elif val == 'm':
            lines.append("#define %s_MODULE 1" % key)
        elif val.startswith('"'):
            lines.append("#define %s %s" % (key, val))
        else:
            lines.append("#define %s (%s)" % (key, val))
        continue
    m = re.match(r'^# (CONFIG_\w+) is not set$', raw)
    if m:
        lines.append("#undef  %s" % m.group(1))
import os
text = "\n".join(lines) + "\n"
changed = (not os.path.exists(out)) or open(out).read() != text
if changed:
    open(out, 'w').write(text)
n = sum(1 for l in lines if l.startswith(('#define CONFIG', '#undef')))
print("    %d symbols -> include/linux/autoconf.h%s" % (n, "  (CHANGED)" if changed else "  (unchanged)"))
sys.exit(7 if changed else 0)
PYGEN
    local rc=$?
    set -e
    if [ "$rc" = "7" ]; then
        # A config change must invalidate every object: this tree has no
        # working `make dep`, so make would happily link stale .o files
        # compiled against the previous autoconf.h (that is how a kernel
        # built "for 2MB" kept its 8MB code). See GR-002 in GUARDRAILS.md.
        warn "Kernel config changed — discarding stale objects for a full rebuild"
        find "$KERNEL_DIR" \( -name '*.o' -o -name '*.a' \) -delete
    elif [ "$rc" != "0" ]; then
        die "autoconf.h generation failed"
    fi
}

# ── Apply the known-good kernel config ──────────────────────────────────────
setup_config() {
    info "Applying kernel config: $DEFCONFIG"
    [ -f "$CONFIG_DIR/$DEFCONFIG" ] || die "Defconfig not found: $CONFIG_DIR/$DEFCONFIG"
    cp "$CONFIG_DIR/$DEFCONFIG" "$KERNEL_DIR/.config"

    # The 2.4 build does NOT regenerate include/linux/autoconf.h by itself
    # (there is no working `make oldconfig` in this stripped tree), and the
    # checked-in autoconf.h was hand-maintained and drifted away from the
    # defconfigs — so config changes silently did nothing to the compiled
    # code. Generate it from .config every build. See GR-001 in GUARDRAILS.md.
    gen_autoconf

    # This 2.4 tree needs include/asm symlinked and (occasionally) the
    # toolchain include path patched into the Makefile.
    ( cd "$KERNEL_DIR"
      [ -L include/asm ] || ln -sf asm-mipsnommu include/asm
      if [ "$(uname -s)" = "Darwin" ]; then
          sed -i '' "s|^HoangFlag.*=.*|HoangFlag = -I${TC_LIB}/include -I${SDK_DIR}/include|" Makefile
          # GCC 12 no longer accepts the legacy -mcpu=r3000 spelling.
          # mips1 is the matching ISA/tuning target for the PS1 CPU.
          sed -i '' 's/-mcpu=r3000/-mtune=mips1/g' arch/mipsnommu/Makefile
      else
          sed -i "s|^HoangFlag.*=.*|HoangFlag = -I${TC_LIB}/include -I${SDK_DIR}/include|" Makefile
      fi
    )
    ok "Config staged (serial console + initrd + ramdisk + RAID all enabled)"
}

# ── kernel: configure + compile ─────────────────────────────────────────────
cmd_kernel() {
    mkdirs
    [ -x "$TC_BIN/mipsel-linux-gcc" ] || die "Toolchain missing — run: ./build.sh deps"
    build_host_tools
    setup_config
    local log="$LOG_DIR/kernel.log"
    info "Compiling kernel (log: $log)..."
    # NOTE: we do NOT run "make dep" — this tree ships pre-generated .depend
    # files and the unused driver subdirs (acpi/atm/...) were stripped, so the
    # 2.4 fastdep recursion would fail looking for them. Plain `make` is correct.
    ( cd "$KERNEL_DIR"
      export PATH="$TC_BIN:$PATH"
      # include/linux/version.h is generated from the Makefile's VERSION, but
      # nothing on the plain `make` path depends on it - the 2.4 rule hangs off
      # targets this tree does not use. A working copy has one lying around
      # from an earlier build, so this only bites a FRESH CHECKOUT, where the
      # first file to include <linux/version.h> fails with "No such file".
      # Ask for it explicitly.
      make include/linux/version.h
      make ) 2>&1 | tee "$log"
    if [ -f "$KERNEL_DIR/linux" ]; then
        cp "$KERNEL_DIR/linux" "$OUTPUT_DIR/linux.elf"
        ok "Kernel built -> output/linux.elf ($(du -h "$OUTPUT_DIR/linux.elf" | cut -f1))"
    else
        die "Kernel build failed — see $log"
    fi
}

# ── initrd: ext2 ramdisk root fs (no root required, uses genext2fs) ──────────
cmd_initrd() {
    mkdirs
    command -v genext2fs >/dev/null || die "genext2fs missing -> sudo apt install genext2fs"
    [ -d "$INITRD_SKEL" ]               || die "initrd skeleton missing: $INITRD_SKEL"
    [ -f "$INITRD_SKEL/bin/busybox" ]   || die "busybox missing in skeleton ($INITRD_SKEL/bin/busybox)"

    local img="$OUTPUT_DIR/initrd.img"
    info "Building 2048KB ext2 initrd from initrd/skeleton ..."
    genext2fs -b 2048 -N 256 -m 0 \
        -d "$INITRD_SKEL" \
        -D "$SCRIPTS_DIR/device_table.txt" \
        "$img"
    ok "InitRD built -> output/initrd.img ($(du -h "$img" | cut -f1))"
    info "Contents:"; debugfs -R "ls -l /" "$img" 2>/dev/null | head -20 || true
}

# ── mcard: PlayStation memory-card images ───────────────────────────────────
cmd_mcard() {
    mkdirs
    local mk="$TOOLS_DIR/host/mkmemcard"
    [ -x "$mk" ] || { info "Building mkmemcard..."; ( unset GCC_EXEC_PREFIX; gcc -O2 -Wall -o "$mk" "$TOOLS_DIR/host/mkmemcard.c"; ); }

    if [ "${1:-}" = "--raid" ]; then
        local n="${2:-4}"
        info "Creating ${n}-card RAID set..."
        "$mk" --raid "$n" "$OUTPUT_DIR/blackroo"
    else
        info "Creating two standard memory cards..."
        "$mk" "$OUTPUT_DIR/blackroo_card0.mcd" 0
        "$mk" "$OUTPUT_DIR/blackroo_card1.mcd" 1
    fi
    ok "Memory card image(s) in output/ — point DuckStation Settings > Memory Cards at them."
}

# ── convert: wrap kernel (+initrd) into a bootable PS-EXE ────────────────────
cmd_convert() {
    mkdirs
    local elf="$OUTPUT_DIR/linux.elf"
    local img="$OUTPUT_DIR/initrd.img"
    local exe="$OUTPUT_DIR/blackroo_noinitrd.exe"
    local final="$OUTPUT_DIR/blackroo.exe"

    [ -f "$elf" ] || die "Kernel not built — run: ./build.sh kernel"
    # Always rebuild: these are a few hundred milliseconds of gcc, and a
    # stale converter silently produces a wrong PS-EXE header.
    build_host_tools

    # The PS-EXE header carries the initial stack pointer and the BIOS
    # honours it, so it must be RAM that exists on the target machine.
    local stack=0x801fff00
    if grep -q '^CONFIG_PSX_16MB_RAM=y' "$KERNEL_DIR/.config" 2>/dev/null; then
        stack=0x80fff000
    elif grep -q '^CONFIG_PSX_8MB_RAM=y' "$KERNEL_DIR/.config" 2>/dev/null; then
        stack=0x807fff00
    elif grep -q '^CONFIG_PSX_4MB_RAM=y' "$KERNEL_DIR/.config" 2>/dev/null; then
        stack=0x803fff00
    fi

    info "ELF -> PS-EXE (stack $stack) ..."
    "$TOOLS_DIR/elf2psexe" "$elf" "$exe" "$stack"

    if [ -f "$img" ]; then
        info "Embedding initrd after _end (INRD magic) ..."
        # Hand over the kernel's real _end (past BSS). Without it the initrd
        # is placed by image size, lands inside BSS, and head.S erases it.
        local kend
        kend=$("$TC_BIN/mipsel-linux-nm" "$elf" 2>/dev/null | awk '$3=="_end"{print "0x"$1}' | tail -1)
        if [ -n "$kend" ]; then
            info "kernel _end = $kend"
        fi
        "$TOOLS_DIR/addpsexe_initrd" "$exe" "$img" "$final" $kend
        ok "Bootable image -> output/blackroo.exe (kernel + initrd, $(du -h "$final" | cut -f1))"
    else
        cp "$exe" "$final"
        warn "No initrd found — output/blackroo.exe is kernel-only (run ./build.sh initrd first for a shell)."
    fi
}

# ── all / clean / status ────────────────────────────────────────────────────
cmd_all()    { cmd_deps; cmd_kernel; cmd_initrd; cmd_convert; echo; ok "DONE. Upload output/blackroo.exe — see README.md (Booting)."; }

cmd_clean()  {
    info "Cleaning build artifacts..."
    ( cd "$KERNEL_DIR"; export PATH="$TC_BIN:$PATH"; make clean >/dev/null 2>&1 || true )
    rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
    ok "Clean. (Kernel .config kept; run ./build.sh kernel to rebuild.)"
}

cmd_status() {
    echo; echo "  Blackroo Linux — build status"; echo "  ============================="
    echo "  root:      $PROJECT_ROOT"
    echo "  toolchain: $TC_BIN"
    if "$TC_BIN/mipsel-linux-gcc" --version >/dev/null 2>&1; then
        echo "  compiler:  OK ($("$TC_BIN/mipsel-linux-gcc" --version 2>&1 | head -1))"
    else
        echo "  compiler:  present but won't run (sudo apt install libc6:i386)"
    fi
    echo "  defconfig: $DEFCONFIG"
    echo "  outputs:"
    for f in linux.elf initrd.img blackroo.exe blackroo_card0.mcd; do
        if [ -f "$OUTPUT_DIR/$f" ]; then echo "    [+] $f ($(du -h "$OUTPUT_DIR/$f" | cut -f1))"
        else echo "    [ ] $f"; fi
    done
    echo
}

# ── Dispatch ────────────────────────────────────────────────────────────────
case "${1:-all}" in
    deps)            cmd_deps ;;
    kernel)          cmd_kernel ;;
    initrd)          cmd_initrd ;;
    mcard|memcard)   shift; cmd_mcard "$@" ;;
    convert)         cmd_convert ;;
    all)             cmd_all ;;
    clean)           cmd_clean ;;
    status)          cmd_status ;;
    *) sed -n '2,40p' "$0" | sed 's/^#//'; exit 1 ;;
esac
