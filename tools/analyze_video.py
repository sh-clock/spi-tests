#!/usr/bin/env python3
"""Analyze Hill[729].mp4 for the interlaced-video converter:
- decode to 480x320 rgb24 (stretched to fill, as decided)
- measure per-line RLE compressibility in RGB666 (wire format) and RGB565
- detect letterbox bars / uniform borders
- report projected blob sizes for the ESP32-S3 (budget ~5.5 MB)"""

import subprocess, sys
import numpy as np

SRC = "/home/z/my-project/upload/Hill[729].mp4"
RAW = "/tmp/hill_480x320_rgb24"
W, H = 480, 320
N_FRAMES = 119

# decode: scale to 480x320 (stretch), keep 15 fps, rgb24 raw
subprocess.run(
    ["ffmpeg", "-y", "-v", "error", "-i", SRC,
     "-vf", "scale=480:320", "-r", "15",
     "-f", "rawvideo", "-pix_fmt", "rgb24", RAW],
    check=True)

import os
size = os.path.getsize(RAW)
n = size // (W * H * 3)
print(f"decoded {size} bytes = {n} frames of {W}x{H} rgb24")

data = np.fromfile(RAW, dtype=np.uint8).reshape(n, H, W, 3)

# ---- border uniformity / letterbox detection (first, middle, last frame)
for idx in (0, n // 2, n - 1):
    f = data[idx]
    row_means = f.reshape(H, W * 3).mean(axis=1)
    col_means = f.reshape(H, W * 3).mean(axis=0).reshape(W, 3).mean(axis=1)
    top = 0
    while top < H and row_means[top] < 8:
        top += 1
    bot = 0
    while bot < H and row_means[H - 1 - bot] < 8:
        bot += 1
    left = 0
    while left < W and col_means[left] < 8:
        left += 1
    right = 0
    while right < W and col_means[W - 1 - right] < 8:
        right += 1
    print(f"frame {idx}: black bars  top={top} bottom={bot} left={left} "
          f"right={right}  -> content {W-left-right}x{H-top-bot} "
          f"at (+{left},+{top})")

# ---- unique colors per frame
uniq = [len(np.unique(data[i].reshape(-1, 3), axis=0)) for i in range(0, n, 10)]
print(f"unique colors per frame (sampled): min={min(uniq)} max={max(uniq)} "
      f"avg={sum(uniq)/len(uniq):.0f}")

def rle_size_rgb666(lines):
    """per-line RLE over 3-byte RGB666 units: [count-1][R][G][B]"""
    total = 0
    worst = 0
    for line in lines:
        # runs of identical pixels
        same = np.all(line[1:] == line[:-1], axis=1)
        # run boundaries
        breaks = np.where(~same)[0] + 1
        starts = np.concatenate(([0], breaks))
        ends = np.concatenate((breaks, [len(line)]))
        runs = len(starts)
        sz = 0
        for s, e in zip(starts, ends):
            L = e - s
            sz += 4 * ((L + 255) // 256)   # count byte per 256 px
        total += sz
        worst = max(worst, sz)
    return total, worst

def rle_size_rgb565(lines):
    """per-line RLE over 2-byte RGB565 units: [count-1][hi][lo]"""
    total = 0
    for line in lines:
        same = np.all(line[1:] == line[:-1], axis=1)
        breaks = np.where(~same)[0] + 1
        starts = np.concatenate(([0], breaks))
        ends = np.concatenate((breaks, [len(line)]))
        for s, e in zip(starts, ends):
            total += 3 * ((e - s + 255) // 256)
    return total

# sample frames across the video for speed: every 4th frame
tot666 = 0; tot565 = 0; tot_raw = 0; worst_line = 0
sample_idx = range(0, n, 4)
for i in sample_idx:
    f = data[i]
    # RGB666 wire bytes: R=(r>>3)<<3 i.e. top 5 bits replicated; G top6; B top5
    r6 = (f[:, :, 0] >> 3) << 3
    g6 = (f[:, :, 1] >> 2) << 2
    b6 = (f[:, :, 2] >> 3) << 3
    lines666 = np.stack((r6, g6, b6), axis=2)  # H x W x 3
    t666, w666 = rle_size_rgb666(lines666)
    tot666 += t666
    worst_line = max(worst_line, w666)
    r5 = (f[:, :, 0] >> 3).astype(np.uint16) << 11
    g5 = (f[:, :, 1] >> 2).astype(np.uint16) << 5
    b5 = (f[:, :, 2] >> 3).astype(np.uint16)
    px565 = r5 | g5 | b5
    hi = ((px565 >> 8) & 0xFF).astype(np.uint8)
    lo = (px565 & 0xFF).astype(np.uint8)
    tot565 += rle_size_rgb565(np.stack((hi, lo), axis=2))
    tot_raw += H * W * 3

scale = n / len(sample_idx)
est666 = tot666 * scale
est565 = tot565 * scale
print(f"\nraw RGB666 total:            {tot_raw*scale/1e6:8.2f} MB")
print(f"per-line RLE RGB666 est:     {est666/1e6:8.2f} MB  "
      f"({tot_raw*scale/est666:.1f}x)  worst line {worst_line} B (raw=1440)")
print(f"per-line RLE RGB565 est:     {est565/1e6:8.2f} MB  "
      f"({tot_raw*scale/est565:.1f}x, needs on-device 565->666)")
print(f"\nbudget: ~5.5 MB in an 8 MB flash app partition")
