#!/bin/bash
#
# Blackroo Linux Diagnostic Tool
# Analyzes build artifacts and helps debug boot issues
#

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Toolchain paths
TOOLCHAIN_ROOT="$PROJECT_ROOT/Archive/toolchain/mipsel/usr/local"
TOOLCHAIN_BIN="$TOOLCHAIN_ROOT/bin"
OBJDUMP="$TOOLCHAIN_BIN/mipsel-linux-objdump"
NM="$TOOLCHAIN_BIN/mipsel-linux-nm"
READELF="$TOOLCHAIN_BIN/mipsel-linux-readelf"

# Output paths
OUTPUT_DIR="$PROJECT_ROOT/output"
PSX_EXE="$OUTPUT_DIR/psx.exe"
LINUX_ELF="$OUTPUT_DIR/linux.elf"
LINUX_ECOFF="$OUTPUT_DIR/linux.ecoff"
ISO_FILE="$OUTPUT_DIR/blackroo.iso"
INITRD="$OUTPUT_DIR/initrd.img"

# Tools
ELF2PSEXE="$PROJECT_ROOT/tools/elf2psexe"
ELF2ECOFF="$PROJECT_ROOT/tools/elf2ecoff"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo ""
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_ok() {
    echo -e "  ${GREEN}[OK]${NC} $1"
}

print_warn() {
    echo -e "  ${YELLOW}[WARN]${NC} $1"
}

print_err() {
    echo -e "  ${RED}[ERR]${NC} $1"
}

print_info() {
    echo -e "  ${NC}[INFO]${NC} $1"
}

# Check toolchain
check_toolchain() {
    print_header "Toolchain Check"

    if [ -x "$OBJDUMP" ]; then
        print_ok "mipsel-linux-objdump: $OBJDUMP"
    else
        print_err "mipsel-linux-objdump not found at $OBJDUMP"
    fi

    if [ -x "$NM" ]; then
        print_ok "mipsel-linux-nm: $NM"
    else
        print_err "mipsel-linux-nm not found at $NM"
    fi

    if [ -x "$TOOLCHAIN_BIN/mipsel-linux-gcc" ]; then
        local ver=$("$TOOLCHAIN_BIN/mipsel-linux-gcc" --version 2>&1 | head -1)
        print_ok "mipsel-linux-gcc: $ver"
    else
        print_err "mipsel-linux-gcc not found"
    fi
}

# Check build tools
check_tools() {
    print_header "Build Tools Check"

    if [ -x "$ELF2PSEXE" ]; then
        print_ok "elf2psexe: $ELF2PSEXE"
    else
        print_warn "elf2psexe not built - run: gcc -o tools/elf2psexe tools/elf2psexe.c"
    fi

    if [ -x "$ELF2ECOFF" ]; then
        print_ok "elf2ecoff: $ELF2ECOFF"
    else
        print_warn "elf2ecoff not built"
    fi
}

# Check output files
check_outputs() {
    print_header "Output Files Check"

    for f in "$PSX_EXE" "$LINUX_ELF" "$LINUX_ECOFF" "$ISO_FILE" "$INITRD"; do
        if [ -f "$f" ]; then
            local size=$(ls -lh "$f" | awk '{print $5}')
            print_ok "$(basename $f): $size"
        else
            print_warn "$(basename $f): NOT FOUND"
        fi
    done
}

# Analyze PS-EXE header
analyze_psexe() {
    print_header "PS-EXE Header Analysis"

    if [ ! -f "$PSX_EXE" ]; then
        print_err "psx.exe not found"
        return 1
    fi

    python3 - <<'PYEOF'
import struct
import sys

exe_path = sys.argv[1] if len(sys.argv) > 1 else "output/psx.exe"

try:
    with open(exe_path, 'rb') as f:
        data = f.read(0x40)

    magic = data[0:8].decode('ascii', errors='replace')
    pc0 = struct.unpack('<I', data[0x10:0x14])[0]
    gp0 = struct.unpack('<I', data[0x14:0x18])[0]
    t_addr = struct.unpack('<I', data[0x18:0x1c])[0]
    t_size = struct.unpack('<I', data[0x1c:0x20])[0]
    d_addr = struct.unpack('<I', data[0x20:0x24])[0]
    d_size = struct.unpack('<I', data[0x24:0x28])[0]
    b_addr = struct.unpack('<I', data[0x28:0x2c])[0]
    b_size = struct.unpack('<I', data[0x2c:0x30])[0]
    sp_base = struct.unpack('<I', data[0x30:0x34])[0]

    print(f"  Magic:        '{magic}'")
    print(f"  Entry (PC0):  0x{pc0:08x}")
    print(f"  GP0:          0x{gp0:08x}")
    print(f"  Load addr:    0x{t_addr:08x}")
    print(f"  Text size:    {t_size:,} bytes ({t_size/1024/1024:.2f} MB)")
    print(f"  Data addr:    0x{d_addr:08x}")
    print(f"  Data size:    {d_size:,} bytes")
    print(f"  BSS addr:     0x{b_addr:08x}")
    print(f"  BSS size:     {b_size:,} bytes")
    print(f"  Stack:        0x{sp_base:08x}")
    print()

    # Validate
    total_mem = t_size + b_size
    print(f"  Total memory: {total_mem:,} bytes ({total_mem/1024/1024:.2f} MB)")

    if t_addr < 0x80000000:
        print("  [ERR] Load address should be in KSEG0 (0x80xxxxxx)")
    else:
        print("  [OK] Load address in KSEG0")

    if pc0 < 0x80000000:
        print("  [ERR] Entry point should be in KSEG0 (0x80xxxxxx)")
    else:
        print("  [OK] Entry point in KSEG0")

    if total_mem > 2*1024*1024:
        print(f"  [WARN] Exceeds 2MB RAM - requires 8MB mod!")

    if total_mem > 8*1024*1024:
        print(f"  [ERR] Exceeds 8MB RAM - too large!")

except Exception as e:
    print(f"  [ERR] {e}")
PYEOF
}

# Analyze ELF file
analyze_elf() {
    print_header "ELF Analysis"

    if [ ! -f "$LINUX_ELF" ]; then
        print_err "linux.elf not found"
        return 1
    fi

    if [ -x "$OBJDUMP" ]; then
        echo "  Entry point and first instructions:"
        $OBJDUMP -f "$LINUX_ELF" 2>/dev/null | grep -E "file format|start address"
        echo ""
        echo "  First 20 instructions at entry:"
        $OBJDUMP -d "$LINUX_ELF" 2>/dev/null | head -40 | tail -25
    else
        print_warn "objdump not available, using readelf"
        readelf -h "$LINUX_ELF" 2>/dev/null | grep -E "Entry|Machine|Class"
    fi
}

# Check for initrd marker in PS-EXE
check_initrd_embed() {
    print_header "InitRD Check"

    if [ ! -f "$PSX_EXE" ]; then
        print_err "psx.exe not found"
        return 1
    fi

    # Check for INRD magic at end
    local size=$(stat -c%s "$PSX_EXE")
    local check_offset=$((size - 8))

    if xxd -s $check_offset -l 4 "$PSX_EXE" 2>/dev/null | grep -q "INRD"; then
        print_ok "InitRD marker found at end of PS-EXE"
        local initrd_size=$(xxd -s $((check_offset + 4)) -l 4 -e "$PSX_EXE" 2>/dev/null | awk '{print $2}')
        print_info "InitRD size: $initrd_size"
    else
        print_info "No embedded InitRD marker (INRD) found"
        print_info "InitRD may be loaded separately or not embedded"
    fi
}

# Show memory map
show_memory_map() {
    print_header "PlayStation Memory Map"

    echo "  Standard PS1 (2MB):"
    echo "    0x00000000 - 0x001FFFFF  Main RAM (2MB)"
    echo "    0x80000000 - 0x801FFFFF  KSEG0 (cached, mirrors RAM)"
    echo ""
    echo "  8MB Modded PS1:"
    echo "    0x00000000 - 0x007FFFFF  Main RAM (8MB)"
    echo "    0x80000000 - 0x807FFFFF  KSEG0 (cached, mirrors RAM)"
    echo ""
    echo "  Your kernel requires:"

    if [ -f "$PSX_EXE" ]; then
        python3 -c "
import struct
with open('$PSX_EXE', 'rb') as f:
    f.seek(0x1c)
    t_size = struct.unpack('<I', f.read(4))[0]
    f.seek(0x2c)
    b_size = struct.unpack('<I', f.read(4))[0]
total = t_size + b_size
print(f'    {total:,} bytes ({total/1024/1024:.2f} MB)')
if total > 2*1024*1024:
    print('    -> 8MB RAM MOD REQUIRED')
"
    fi
}

# Emulator config check
check_emulator_config() {
    print_header "PCSX-Redux Configuration"

    local config="$HOME/.config/pcsx-redux/pcsx.json"

    if [ -f "$config" ]; then
        print_ok "Config found: $config"

        if grep -q '"8Megs": true' "$config"; then
            print_ok "8MB RAM: ENABLED"
        else
            print_err "8MB RAM: DISABLED - Enable in Configuration -> Emulation"
        fi

        if grep -q '"FastBoot": true' "$config"; then
            print_warn "FastBoot: ENABLED (may skip BIOS initialization)"
        else
            print_ok "FastBoot: DISABLED (full BIOS boot)"
        fi
    else
        print_warn "PCSX-Redux config not found"
    fi
}

# Run all diagnostics
run_all() {
    echo ""
    echo "========================================"
    echo "  Blackroo Linux Diagnostic Report"
    echo "========================================"
    echo "  Project: $PROJECT_ROOT"
    echo "  Date:    $(date)"
    echo ""

    check_toolchain
    check_tools
    check_outputs
    analyze_psexe
    check_initrd_embed
    show_memory_map
    check_emulator_config

    print_header "Quick Commands"
    echo "  Disassemble entry point:"
    echo "    $OBJDUMP -d output/linux.elf | head -60"
    echo ""
    echo "  Show symbols:"
    echo "    $NM output/linux.elf | grep -E 'kernel_entry|start_kernel|_stext'"
    echo ""
    echo "  Rebuild kernel:"
    echo "    ./build.sh kernel"
    echo ""
    echo "  Create ISO:"
    echo "    ./build.sh iso"
    echo ""
    echo "  Test in emulator:"
    echo "    ./scripts/test_emulator.sh"
}

# Handle arguments
case "${1:-all}" in
    toolchain)
        check_toolchain
        ;;
    tools)
        check_tools
        ;;
    outputs)
        check_outputs
        ;;
    psexe)
        analyze_psexe
        ;;
    elf)
        analyze_elf
        ;;
    initrd)
        check_initrd_embed
        ;;
    memory)
        show_memory_map
        ;;
    emulator)
        check_emulator_config
        ;;
    all|*)
        run_all
        ;;
esac
