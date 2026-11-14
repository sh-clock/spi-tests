#!/usr/bin/env python3
"""Visual audit of the v3 firmware's wire data: render frame 0 from the
gcc-compiled device decoder's output (the exact bytes that go on the SPI
wire), and pixel-compare against the source video frame run through the
same conversion pipeline (median filter). RGB666 quantization ((v>>3)<<3)
loses at most 7 per channel, so max channel error must be <= 7."""

import sys

sys.path.insert(0, "/home/z/my-project/scripts")
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
from convert_video import decode_frames, W, H  # noqa: E402

CDEC = "/tmp/c_decoded_lines.bin"
SRC = "/home/z/my-project/upload/Hill[729].mp4"

data = open(CDEC, "rb").read()
BPL = W * 3
expected = 238 * 160 * BPL
assert len(data) == expected, f"decoder output {len(data)} != {expected}"

f0 = np.frombuffer(data[0:160 * BPL], dtype=np.uint8).reshape(160, W, 3)
f1 = np.frombuffer(data[160 * BPL:320 * BPL], dtype=np.uint8).reshape(160, W, 3)
frame = np.empty((H, W, 3), dtype=np.uint8)
frame[0::2] = f0            # field 0 = even display rows
frame[1::2] = f1            # field 1 = odd display rows
Image.fromarray(frame).save("/tmp/v3_frame0.png")

frames = decode_frames(SRC, "320:208:0:16", 15)
ref = frames[0]
err = np.abs(frame.astype(np.int16) - ref.astype(np.int16))
print(f"decoder frame 0 vs source (same pipeline): "
      f"max channel error {err.max()}, mean {err.mean():.3f}")
assert err.max() <= 7, "PIXEL AUDIT FAILED: error above quantization bound"

# every 20th frame too, to prove the whole stream (not just frame 0)
for fi in range(20, 119, 20):
    fr = np.empty((H, W, 3), dtype=np.uint8)
    base = (2 * fi) * 160 * BPL
    ev = np.frombuffer(data[base:base + 160 * BPL], dtype=np.uint8).reshape(160, W, 3)
    od = np.frombuffer(data[base + 160 * BPL:base + 320 * BPL], dtype=np.uint8).reshape(160, W, 3)
    fr[0::2] = ev
    fr[1::2] = od
    e = np.abs(fr.astype(np.int16) - frames[fi].astype(np.int16)).max()
    print(f"  frame {fi}: max channel error {e}")
    assert e <= 7, f"PIXEL AUDIT FAILED at frame {fi}"

ss = np.concatenate([ref, frame], axis=1)
Image.fromarray(ss).save("/tmp/v3_sidebyside.png")
print("PIXEL AUDIT: PASS — all checked frames within the RGB666 "
      "quantization bound (max err <= 7/255 per channel)")
