#!/bin/bash
#
# Test Blackroo Linux in PS1 emulator
#

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO_FILE="$PROJECT_ROOT/output/blackroo.iso"
CUE_FILE="$PROJECT_ROOT/output/blackroo.cue"
PSX_EXE="$PROJECT_ROOT/output/psx.exe"
BIOS_FILE="$PROJECT_ROOT/Archive/SCPH1001.BIN"

echo "=== Blackroo Linux Emulator Test ==="
echo ""

if [ ! -f "$ISO_FILE" ]; then
    echo "Error: ISO not found: $ISO_FILE"
    echo "Run: ./build.sh iso"
    exit 1
fi

echo "ISO file: $ISO_FILE"
ls -lh "$ISO_FILE"
echo ""

# Check for BIOS
if [ -f "$BIOS_FILE" ]; then
    echo "BIOS file: $BIOS_FILE"
    ls -lh "$BIOS_FILE"
else
    echo "WARNING: BIOS not found at $BIOS_FILE"
    echo "Some emulators require a PlayStation BIOS to run."
fi
echo ""

# Check for available emulators
echo "Checking for PS1 emulators..."

check_emulator() {
    which "$1" &>/dev/null
}

AVAILABLE=""

if check_emulator duckstation-qt; then
    echo "  [ok] DuckStation (duckstation-qt)"
    AVAILABLE="duckstation"
fi

if check_emulator pcsx-redux; then
    echo "  [ok] PCSX-Redux (pcsx-redux)"
    AVAILABLE="${AVAILABLE:-redux}"
fi

if check_emulator pcsxr; then
    echo "  [ok] PCSXR (pcsxr)"
    AVAILABLE="${AVAILABLE:-pcsxr}"
fi

if check_emulator mednafen; then
    echo "  [ok] Mednafen (mednafen)"
    AVAILABLE="${AVAILABLE:-mednafen}"
fi

echo ""

if [ -z "$AVAILABLE" ]; then
    echo "No PS1 emulator found!"
    echo ""
    echo "Install options:"
    echo ""
    echo "Option 1: PCSXR (easiest, from Ubuntu repos):"
    echo "  sudo apt install pcsxr"
    echo ""
    echo "Option 2: DuckStation (best accuracy, AppImage):"
    echo "  wget -O ~/duckstation-linux-x64.AppImage \\"
    echo "    https://github.com/stenzek/duckstation/releases/latest/download/DuckStation-x64.AppImage"
    echo "  chmod +x ~/duckstation-linux-x64.AppImage"
    echo ""
    echo "Option 3: PCSX-Redux (best for development):"
    echo "  git clone https://github.com/grumpycoders/pcsx-redux.git"
    echo "  cd pcsx-redux && ./dockermake.sh appimage"
    echo ""
    echo "Note: Most emulators require a PS1 BIOS file (scph1001.bin)."
    echo "PCSX-Redux includes OpenBIOS which doesn't require a Sony BIOS."
    exit 1
fi

echo "Starting emulator..."
echo ""

case "$AVAILABLE" in
    duckstation)
        echo "Running: duckstation-qt \"$ISO_FILE\""
        echo ""
        echo "Note: Set BIOS path in DuckStation settings to:"
        echo "  $BIOS_FILE"
        duckstation-qt "$ISO_FILE"
        ;;
    redux)
        echo "Running: pcsx-redux -iso \"$ISO_FILE\""
        pcsx-redux -iso "$ISO_FILE"
        ;;
    pcsxr)
        # Setup BIOS for PCSXR
        PCSXR_BIOS_DIR="$HOME/.pcsxr/bios"
        if [ -f "$BIOS_FILE" ] && [ ! -f "$PCSXR_BIOS_DIR/scph1001.bin" ]; then
            echo "Setting up BIOS for PCSXR..."
            mkdir -p "$PCSXR_BIOS_DIR"
            cp "$BIOS_FILE" "$PCSXR_BIOS_DIR/scph1001.bin"
            echo "Copied BIOS to $PCSXR_BIOS_DIR/scph1001.bin"
        fi
        echo ""
        echo "Running: pcsxr -cdfile \"$CUE_FILE\""
        echo ""
        echo "If BIOS not detected, configure in:"
        echo "  Configuration -> Plugins & BIOS -> Select: scph1001.bin"
        pcsxr -cdfile "$CUE_FILE"
        ;;
    mednafen)
        # Setup BIOS for Mednafen
        MEDNAFEN_DIR="$HOME/.mednafen"
        if [ -f "$BIOS_FILE" ] && [ ! -f "$MEDNAFEN_DIR/scph1001.bin" ]; then
            echo "Setting up BIOS for Mednafen..."
            mkdir -p "$MEDNAFEN_DIR"
            cp "$BIOS_FILE" "$MEDNAFEN_DIR/scph1001.bin"
            echo "Copied BIOS to $MEDNAFEN_DIR/scph1001.bin"
        fi
        echo ""
        echo "Running: mednafen \"$CUE_FILE\""
        mednafen "$CUE_FILE"
        ;;
esac
