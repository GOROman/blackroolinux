#!/bin/bash
#
# ps1view.sh — Quick PS1 composite video viewer via capture card
#
# Usage:
#   ./ps1view.sh                View PS1 output (auto-detect)
#   ./ps1view.sh pal            Force PAL mode
#   ./ps1view.sh ntsc           Force NTSC mode
#   ./ps1view.sh record out.mp4 View and record to file
#

DEVICE="${PS1_VIDEO:-/dev/video0}"
MODE="${1:-auto}"
RECORD_FILE="${2:-}"

if [ ! -e "$DEVICE" ]; then
    echo "ERROR: No capture device at $DEVICE"
    echo "  Check: ls /dev/video*"
    echo "  Install driver: cd ~/projects/smi2021 && sudo ./install.sh"
    exit 1
fi

case "$MODE" in
    pal)   SIZE="720x576"; STD="PAL"  ;;
    ntsc)  SIZE="720x480"; STD="NTSC" ;;
    *)     SIZE="720x576"; STD="PAL"  ;;  # PS1 defaults to PAL in your region
esac

echo "PS1 Video Viewer"
echo "  Device:  $DEVICE"
echo "  Mode:    $STD ($SIZE)"
if [ -n "$RECORD_FILE" ]; then
    echo "  Record:  $RECORD_FILE"
fi
echo "  Press Q to quit"
echo ""

# Try setting video standard
v4l2-ctl --device="$DEVICE" --set-standard="$STD" 2>/dev/null

if [ -n "$RECORD_FILE" ]; then
    # View + record simultaneously
    ffmpeg -f v4l2 -input_format yuyv422 -video_size "$SIZE" \
        -i "$DEVICE" \
        -f alsa -i default \
        -map 0:v -map 1:a \
        -c:v libx264 -preset ultrafast -crf 23 \
        -c:a aac -b:a 128k \
        "$RECORD_FILE" \
        -map 0:v \
        -c:v rawvideo -f sdl2 "PS1 - $STD" \
        2>/dev/null
else
    # View only — use ffplay for low latency
    ffplay -f v4l2 \
        -input_format yuyv422 \
        -video_size "$SIZE" \
        -window_title "PS1 - $STD" \
        -i "$DEVICE" \
        2>/dev/null
fi
