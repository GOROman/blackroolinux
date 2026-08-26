#!/bin/bash
#
# setup-emulators.sh — Download and configure PS1/PS2 emulators for testing
#
# Emulators are NOT included in git (too large). This script downloads
# and sets them up in vendor/emulators/bin/.
#
# Supported:
#   - DuckStation (PS1, most accurate, recommended)
#   - PCSX-Redux (PS1, development-focused, good debugger)
#
# Attribution: These are third-party open-source projects.
#   DuckStation: https://github.com/stenzek/duckstation (GPL-3.0)
#   PCSX-Redux: https://github.com/grumpycoders/pcsx-redux (GPL-2.0)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
EMU_DIR="$PROJECT_ROOT/vendor/emulators/bin"

mkdir -p "$EMU_DIR"

echo "============================================"
echo " Blackroo Linux — Emulator Setup"
echo "============================================"
echo ""
echo "This downloads PS1 emulators for kernel testing."
echo "Emulators are stored in vendor/emulators/bin/ (gitignored)."
echo ""

install_duckstation() {
    echo "[DuckStation] Checking..."
    if [ -f "$EMU_DIR/DuckStation.AppImage" ]; then
        echo "  Already installed."
        return 0
    fi

    # Check if there's one in the project root (from original setup)
    if [ -f "$PROJECT_ROOT/DuckStation.AppImage" ]; then
        echo "  Found in project root, moving to vendor/emulators/bin/..."
        mv "$PROJECT_ROOT/DuckStation.AppImage" "$EMU_DIR/"
        echo "  Done."
        return 0
    fi

    echo "  Downloading latest DuckStation AppImage..."
    echo "  (This is ~90MB)"

    # Try GitHub releases
    local URL="https://github.com/stenzek/duckstation/releases/download/latest/DuckStation-x64.AppImage"
    if wget -q --show-progress -O "$EMU_DIR/DuckStation.AppImage" "$URL" 2>/dev/null; then
        chmod +x "$EMU_DIR/DuckStation.AppImage"
        echo "  Installed."
    else
        echo "  [WARN] Download failed. Download manually from:"
        echo "         https://github.com/stenzek/duckstation/releases"
        echo "         Place the AppImage in: $EMU_DIR/"
    fi
}

install_pcsx_redux() {
    echo "[PCSX-Redux] Checking..."
    if [ -d "$EMU_DIR/pcsx-redux" ] || [ -f "$EMU_DIR/pcsx-redux/PCSX-Redux" ]; then
        echo "  Already installed."
        return 0
    fi

    # Check if there's one in the project root
    local EXISTING=$(find "$PROJECT_ROOT" -maxdepth 1 -name "PCSX-Redux*" -type d 2>/dev/null | head -1)
    if [ -n "$EXISTING" ]; then
        echo "  Found in project root, moving to vendor/emulators/bin/..."
        mv "$EXISTING" "$EMU_DIR/pcsx-redux"
        echo "  Done."
        return 0
    fi

    echo "  PCSX-Redux must be built from source or downloaded from:"
    echo "  https://github.com/grumpycoders/pcsx-redux/releases"
    echo "  Place the extracted directory in: $EMU_DIR/pcsx-redux/"
}

move_existing_emulators() {
    # Move any emulators already in the project root to the proper location
    if [ -f "$PROJECT_ROOT/DuckStation.AppImage" ]; then
        echo "Moving existing DuckStation to vendor/emulators/bin/..."
        mv "$PROJECT_ROOT/DuckStation.AppImage" "$EMU_DIR/"
    fi

    local PCSX_DIR=$(find "$PROJECT_ROOT" -maxdepth 1 -name "PCSX-Redux*" -type d 2>/dev/null | head -1)
    if [ -n "$PCSX_DIR" ]; then
        echo "Moving existing PCSX-Redux to vendor/emulators/bin/..."
        mv "$PCSX_DIR" "$EMU_DIR/pcsx-redux"
    fi
}

echo "--- Organizing existing emulators ---"
move_existing_emulators

echo ""
echo "--- Setting up emulators ---"
install_duckstation
echo ""
install_pcsx_redux

echo ""
echo "============================================"
echo " Emulator setup complete."
echo ""
echo " To test the kernel:"
echo "   make test"
echo ""
echo " Or manually:"
echo "   $EMU_DIR/DuckStation.AppImage -exe output/kernel.exe"
echo "============================================"
