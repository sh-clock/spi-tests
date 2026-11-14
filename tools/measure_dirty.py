#!/usr/bin/env python3
"""Measure the wire-byte savings of skipping unchanged line spans between
consecutive same-parity fields (candidate "MIV3" dirty-line format), using
the exact same processing pipeline as convert_video.py (median filter +
RGB666 quantization) on the real Hill video.

The player sends fields in the order: frame0-even, frame0-odd, frame1-even,
... so each field only needs to update the lines that differ from the
PREVIOUS field of the same parity (= previous frame, same rows). A line
that is byte-identical can be skipped entirely (0 wire bytes); a partially
changed line needs its dirty span [x0..x1] plus a per-line CASET.
"""
import sys

sys.path.insert(0, "/home/z/my-project/scripts")
from convert_video import decode_frames, to_rgb666, W, H  # noqa: E402
import numpy as np  # noqa: E402

SRC = "/home/z/my-project/upload/Hill[729].mp4"

frames = decode_frames(SRC, "320:208:0:16", 15)
w = to_rgb666(frames)
n = w.shape[0]

FULL_LINE = W * 3
OVH_FULL = 6    # PASET(5) + RAMWR(1); CASET 0..479 shared per field
OVH_SPAN = 11   # CASET(5) + PASET(5) + RAMWR(1) for a partial span

skipped = changed = partial = 0
tot_full = tot_dirty = 0
per_frame_dirty = []
for i in range(1, n):
    prev, cur = w[i - 1], w[i]
    diff = np.any(cur != prev, axis=2)          # (320, 480) per-pixel
    fr_dirty = 0
    for parity in (0, 1):
        for r in range(parity, H, 2):
            d = diff[r]
            nz = np.flatnonzero(d)
            if nz.size == 0:
                skipped += 1
                continue
            span = int(nz[-1]) - int(nz[0]) + 1
            if span == W:
                changed += 1
                fr_dirty += OVH_FULL + FULL_LINE
            else:
                partial += 1
                fr_dirty += OVH_SPAN + span * 3
    tot_full += 2 * (H // 2) * (OVH_FULL + FULL_LINE)
    tot_dirty += fr_dirty
    per_frame_dirty.append(fr_dirty)

lines_total = (n - 1) * H
print(f"frames compared: {n - 1}, field lines: {lines_total}")
print(f"  identical lines (skippable): {skipped} "
      f"({100 * skipped / lines_total:.1f}%)")
print(f"  full-width changed lines:    {changed} "
      f"({100 * changed / lines_total:.1f}%)")
print(f"  partial-span lines:          {partial} "
      f"({100 * partial / lines_total:.1f}%)")
bf_full = tot_full / (n - 1)
bf_dirty = tot_dirty / (n - 1)
wf = max(per_frame_dirty)
print(f"bytes per frame: full={bf_full:,.0f}  dirty={bf_dirty:,.0f} "
      f"({100 * bf_dirty / bf_full:.1f}% of full)")
print(f"  worst frame {wf:,.0f} B, best frame {min(per_frame_dirty):,.0f} B")
for mbps, name in ((4.98e6, "40 MHz"), (9.6e6, "80 MHz")):
    print(f"implied fps @ {name} bus: full={mbps / bf_full:.1f}  "
          f"dirty-avg={mbps / bf_dirty:.1f}  dirty-worst={mbps / wf:.1f}")
