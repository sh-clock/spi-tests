#!/bin/bash
# package_lab.sh — assemble the download/ deliverables for the video lab.
# Run AFTER a successful idf.py build. Produces:
#   download/msp3520-video-lab/            source tree (cleaned)
#   download/msp3520-video-lab-firmware/   binaries + FLASH-INSTRUCTIONS
#   both zipped
set -euo pipefail

ROOT=/home/z/my-project
LAB=$ROOT/firmware/msp3520-video-lab
BUILD=$LAB/build
DL=$ROOT/download
FW=$DL/msp3520-video-lab-firmware
SRC=$DL/msp3520-video-lab
IDF=$ROOT/.idf/v6.1/esp-idf
export IDF_TOOLS_PATH=$ROOT/.idf/tools

APP=$BUILD/msp3520_video_lab.bin
BOOT=$BUILD/bootloader/bootloader.bin
PT=$BUILD/partition_table/partition-table.bin
for f in "$BOOT" "$PT" "$APP"; do
    [ -s "$f" ] || { echo "missing build output: $f"; exit 1; }
done

. $IDF/export.sh >/dev/null

rm -rf "$SRC" "$FW"
mkdir -p "$SRC" "$FW"

# --- source package (cleaned) ---
cp "$LAB/CMakeLists.txt" "$LAB/partitions.csv" "$LAB/sdkconfig.defaults" \
   "$LAB/README.md" "$SRC/"
cp -r "$LAB/main" "$LAB/tools" "$SRC/"
rm -rf "$SRC/main"/*.o
# strip editor/OS noise if any
find "$SRC" -name '.DS_Store' -delete 2>/dev/null || true

# --- firmware package ---
cp "$BOOT" "$FW/bootloader.bin"
cp "$PT" "$FW/partition-table.bin"
cp "$APP" "$FW/"
esptool --chip esp32s3 merge-bin -o "$FW/msp3520_video_lab_full.bin" \
    --flash_mode keep --flash_size keep --flash_freq keep \
    0x0 "$BOOT" \
    0x8000 "$PT" \
    0x10000 "$APP"
esptool image-info "$FW/msp3520_video_lab_full.bin" > "$FW/image_info.txt" 2>&1 \
    || esptool image_info "$FW/msp3520_video_lab_full.bin" > "$FW/image_info.txt"

APP_SZ=$(stat -c%s "$APP")
FULL_SZ=$(stat -c%s "$FW/msp3520_video_lab_full.bin")
APP_HX=$(printf '0x%X' "$APP_SZ")
PART_SZ=$((0x7F0000))
PCT=$((APP_SZ * 100 / PART_SZ))

cat > "$FW/FLASH-INSTRUCTIONS.txt" <<EOF
MSP3520 VIDEO LAB - FULL CLIP EDITION (12.2 s, all techniques)
================================================================
ESP32-S3 (esp32s3) - ESP-IDF v6.1.0 - built 2026-11-16
Layout: all assets embedded in the app, 8 MB flash (DevKitC-1 N8 /
N8R2 / N16 all work).

WHAT'S NEW (vs the 20-frame lab you validated)
  - THE WHOLE VIDEO IS IN. Every technique now plays all 122 frames
    (12.2 s at 10 fps - essentially real-time on the 40 MHz wire)
    instead of the 0.67 s cut. Static scanlines (2/3) are unchanged.
  - The base asset is COMPRESSED (RLE+LZSS per frame, 2.78x) and
    decoded on demand into one DRAM staging frame (labvid.c). The old
    half/third/pre_i4/delta assets are derived at runtime from the
    same staged frames; technique 9 diffs against the previous frame
    live (tolerance 0 - it reconverges every frame, no drift).
  - Verified: Python mirror + the unmodified device decoder compiled
    on the host decode the blob byte-identically (122/122 frames).

FILES
  bootloader.bin              flash at 0x0
  partition-table.bin         flash at 0x8000
  msp3520_video_lab.bin       flash at 0x10000  ($APP_HX - the video is in it)
  msp3520_video_lab_full.bin  all three merged - flash at 0x0 (easiest)

FLASH IT (pick your COM port; no --flash-mode/--flash-size/--flash-freq
flags needed - the image already carries the right settings):
  C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe -m esptool --chip esp32s3 -p COM10 -b 460800 --before default-reset --after hard-reset write-flash 0x0 msp3520_video_lab_full.bin
  Flashing the ~7 MB image takes a while - that is the video going in.

WHICH USB PORT? EITHER ONE WORKS
  - "COM/UART" port (CP210x bridge): "Silicon Labs CP210x ... (COMx)"
    in Device Manager (install the Silicon Labs driver if missing).
  - The native USB port works too; the console is mirrored on both.

WHAT YOU SHOULD SEE
  115200 baud menu listing techniques 1..15 (+ 7a/7b/11a/11b), 'c'
  (clock), 'i' (asset info). Type a number, watch the technique play
  the FULL clip in a loop; any key returns to the menu. 2 = the
  static scanlines you liked, on the whole video now.

SIZE
  app $APP_SZ B of $PART_SZ B factory partition (${PCT}% used)

SD CARD? Not needed for this clip - the full 12.2 s fits with ~19%
to spare. Full 29.97 fps (17.9-22.1 MB) or multiple clips would need
one; the decoder streams from a file unchanged when that day comes
(WORKLOG 20).
EOF

# --- zips ---
rm -f "$DL/msp3520-video-lab.zip" "$DL/msp3520-video-lab-firmware.zip"
cd "$DL" && zip -qr msp3520-video-lab.zip msp3520-video-lab
cd "$DL" && zip -qr msp3520-video-lab-firmware.zip msp3520-video-lab-firmware

echo "=== package summary ==="
ls -la "$FW"
echo "app: $APP_SZ B / $PART_SZ B (${PCT}%)"
du -sh "$SRC" "$FW" "$DL"/*.zip
