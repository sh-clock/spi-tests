#!/usr/bin/env python3
"""Visual audit of the firmware's wire data: render frames from the
gcc-compiled device decoder's output (the exact bytes that go on the SPI
wire), and pixel-compare against the source video frames run through the
same conversion pipeline (median filter). RGB666 quantization ((v>>3)<<3)
loses at most 7 per channel, so max channel error must be <= 7.
Handles both blob scans: progressive (frames in order) and interlaced
(even/odd field pairs)."""

import sys

sys.path.insert(0, "/home/z/my-project/scripts")
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
from convert_video import decode_frames, W, H  # noqa: E402

CDEC = "/tmp/c_decoded_lines.bin"
SRC = "/home/z/my-project/upload/Hill[729].mp4"
BLOB = "/home/z/my-project/download/msp3520-video-oc-test/main/video.bin"

hdr = open(BLOB, "rb").read(24)
n_frames = hdr[8] | (hdr[9] << 8)
flags = hdr[10] | (hdr[11] << 8)
n_units = int.from_bytes(hdr[16:20], "little")
progressive = bool(flags & 2)
print(f"blob: {n_frames} frames, {n_units} units, "
      f"{'progressive' if progressive else 'interlaced'}")

data = open(CDEC, "rb").read()
BPL = W * 3
unit_lines = H if progressive else H // 2
expected = n_units * unit_lines * BPL
assert len(data) == expected, f"decoder output {len(data)} != {expected}"


def frame_bytes(fi):
    """reconstruct display frame fi from the decoded line stream"""
    fr = np.empty((H, W, 3), dtype=np.uint8)
    if progressive:
        base = fi * H * BPL
        fr[:] = np.frombuffer(data[base:base + H * BPL],
                              dtype=np.uint8).reshape(H, W, 3)
    else:
        base = (2 * fi) * (H // 2) * BPL
        ev = np.frombuffer(data[base:base + (H // 2) * BPL],
                           dtype=np.uint8).reshape(H // 2, W, 3)
        od = np.frombuffer(data[base + (H // 2) * BPL:base + H * BPL],
                           dtype=np.uint8).reshape(H // 2, W, 3)
        fr[0::2] = ev
        fr[1::2] = od
    return fr


frame = frame_bytes(0)
Image.fromarray(frame).save("/tmp/v4_frame0.png")

frames = decode_frames(SRC, "320:208:0:16", 15)
ref = frames[0]
err = np.abs(frame.astype(np.int16) - ref.astype(np.int16))
print(f"decoder frame 0 vs source (same pipeline): "
      f"max channel error {err.max()}, mean {err.mean():.3f}")
assert err.max() <= 7, "PIXEL AUDIT FAILED: error above quantization bound"

for fi in range(20, n_frames, 20):
    fr = frame_bytes(fi)
    e = np.abs(fr.astype(np.int16) - frames[fi].astype(np.int16)).max()
    print(f"  frame {fi}: max channel error {e}")
    assert e <= 7, f"PIXEL AUDIT FAILED at frame {fi}"

ss = np.concatenate([ref, frame], axis=1)
Image.fromarray(ss).save("/tmp/v4_sidebyside.png")
print("PIXEL AUDIT: PASS — all checked frames within the RGB666 "
      "quantization bound (max err <= 7/255 per channel)")
