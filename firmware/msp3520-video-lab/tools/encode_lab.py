#!/usr/bin/env python3
"""encode_lab.py — build the msp3520-video-lab flash assets from INTRO.mkv.

The lab firmware tests 15+ playback "techniques" (row interlacing, scanlines,
column interlace, quincunx, delta updates, pixel doubling, indexed colour,
pre-interlaced packing, dual-core pipelining). They all play the SAME cut of
the source so artifacts are comparable. This script:

  1. desqueezes the anamorphic DVD frame (SAR 8:9 -> 640x480 square pixels),
     detects and crops the 16:9 letterbox bars, center-crops to 3:2 and
     scales to the panel's 480x320 (pan-and-scan, no geometry distortion —
     motion artifacts must stay honest);
  2. auto-places the cut at the calm->burst motion transition (so the delta
     techniques show both regimes);
  3. builds ONE global 256-colour palette in RGB666 space (median cut) —
     the palette bytes ARE the ILI9488 wire bytes, so the firmware expands
     one index to 3 wire bytes with a 768-byte LUT;
  4. encodes frames as 8-bit indices with 2x2 Bayer ordered dithering
     (technique 12's "256-colour + Bayer" look, shared by all techniques);
  5. writes, into ../main/:
       base.idx    480x320 frames, row-major u8 indices (random row access)
       pre_i4.idx  same frames reordered field-major for 4:1 interlace
                   (technique 13: every stored field is exactly what is sent)
       half.idx    240x320 frames (2x horizontal doubling source, tech 11a)
       third.idx   160x320 frames (3x horizontal doubling source, tech 11b)
       delta.spans per-row changed-pixel spans vs previous frame (tech 9)
       palette.pal 256 x 3 wire RGB666 bytes (the LUT)
     every .idx/.spans file carries a small header the firmware parses.

Frame caps are chosen to fit the 8 MB flash next to ~1.1 MB of app code
(factory partition 0x7F0000). The script prints a size report and fails if
the total exceeds the budget.
"""
import subprocess
import sys
import numpy as np
from pathlib import Path
from scipy.ndimage import median_filter

SRC = "/home/z/my-project/upload/INTRO.mkv"
OUT = Path(__file__).resolve().parent.parent / "main"

# ---- geometry ----
PANEL_W, PANEL_H = 480, 320
DESQ_W, DESQ_H = 640, 480          # square-pixel 4:3 frame

# ---- cut / asset caps ----
SCAN_T0 = 7.5                        # motion scan starts here (seconds)
SCAN_FRAMES = 60                     # 2 s of scan frames
BASE_FRAMES = 20                     # 480x320 indexed frames
PREI4_FRAMES = 6                     # field-major 4:1 pack
HALF_FRAMES = 12                     # 240-wide
THIRD_FRAMES = 12                    # 160-wide
DELTA_FRAMES = 12                    # spans (greedily fit to the budget)
BUDGET_ASSETS = 6_600_000            # bytes for all assets together
CALM_LEAD = 6                        # frames of calm before the burst

BAYER2 = np.array([[0, 2], [3, 1]], dtype=np.float32) / 4.0   # 2x2, mean 0.5
DITHER_AMP = 4.0                     # /255, one RGB666 step
SPAN_COLOR_TOL = 10.0                # /255 per channel, see write_spans


def ffmpeg_raw(args):
    return subprocess.run(["ffmpeg", "-v", "error"] + args,
                          capture_output=True, check=True).stdout


def grab_frame(t, vf, w=PANEL_W, h=PANEL_H):
    raw = ffmpeg_raw(["-ss", f"{t}", "-i", SRC, "-frames:v", "1",
                      "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


def detect_crop():
    """Find the letterbox: desqueeze, then measure active rows on a few
    bright-scene frames. Returns the ffmpeg crop chain."""
    desq = f"scale={DESQ_W}:{DESQ_H}"
    probe = grab_frame(9.5, desq, DESQ_W, DESQ_H).astype(np.float32).mean(axis=(1, 2))
    active = np.where(probe > 8.0)[0]      # rows brighter than near-black
    top, bot = int(active[0]), int(DESQ_H - 1 - active[-1])
    h = DESQ_H - top - bot
    print(f"letterbox: top {top}px, bottom {bot}px -> active {h}px "
          f"({'16:9' if abs(h - 360) < 12 else 'unexpected — using measured'})")
    # center-crop the active area to 3:2 (540 x h'), then scale to panel
    w3x2 = (h * 3) // 2
    if w3x2 > DESQ_W:
        w3x2 = DESQ_W
    cc = (DESQ_W - w3x2) // 2
    return (f"{desq},crop={w3x2}:{h}:{cc}:{top},"
            f"scale={PANEL_W}:{PANEL_H}")


def motion_scan(vf):
    """Grab SCAN_FRAMES, return them + the index of the biggest jump."""
    t0 = SCAN_T0
    raw = ffmpeg_raw(["-ss", f"{t0}", "-i", SRC, "-frames:v", str(SCAN_FRAMES),
                      "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
    n = len(raw) // (PANEL_W * PANEL_H * 3)
    if n < SCAN_FRAMES:
        print(f"warning: only {n} scan frames")
    frames = (np.frombuffer(raw, np.uint8)
              .reshape(n, PANEL_H, PANEL_W, 3).astype(np.float32))
    d = np.abs(np.diff(frames, axis=0)).mean(axis=(1, 2, 3))
    burst = int(np.argmax(d))
    print(f"scan: {n} frames from t={t0}s; biggest jump at frame {burst} "
          f"(t={t0 + burst / 29.97:.2f}s, mean|d|={d[burst]:.1f})")
    start = max(0, burst - CALM_LEAD)
    return frames, start


# ---------------- palette (median cut in RGB666 space) ----------------

def median_cut(samples, n_colors=256):
    """samples: (N,3) float 0..255. Returns (n,3) uint8 palette."""
    boxes = [samples]
    while len(boxes) < n_colors:
        # split the box with the largest channel range
        best, best_rng, best_ch = None, -1.0, 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            rng = b.max(axis=0) - b.min(axis=0)
            ch = int(np.argmax(rng))
            if rng[ch] > best_rng:
                best, best_rng, best_ch = i, rng[ch], ch
        if best is None or best_rng <= 1.0:
            break
        b = boxes.pop(best)
        b = b[np.argsort(b[:, best_ch], kind="stable")]
        mid = len(b) // 2
        boxes.extend([b[:mid], b[mid:]])
    pal = np.array([b.mean(axis=0) for b in boxes if len(b)], np.float32)
    # quantize each channel to RGB666 wire values (0..252 step 4)
    pal = (np.round(pal / 4.0) * 4.0).clip(0, 252)
    # drop duplicates, then fill remaining slots with the most common colors
    pal = np.unique(pal, axis=0)
    if len(pal) > n_colors:
        pal = pal[:n_colors]
    while len(pal) < n_colors:
        pal = np.vstack([pal, pal[: n_colors - len(pal)]])
    return pal.astype(np.uint8)


def quantize_with_dither(frame_f32, pal_f32, cache):
    """frame (H,W,3) float -> (H,W) u8 palette indices, 2x2 Bayer dither.
    cache: dict exact-24-bit-dithered-color -> index (no key quantization
    slop, so visually identical inputs always map to identical indices)."""
    h, w, _ = frame_f32.shape
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    thr = (BAYER2[yy % 2, xx % 2] - 0.5) * DITHER_AMP     # (H,W)
    dith = frame_f32.copy()
    for c in range(3):
        dith[..., c] += thr
    np.clip(dith, 0, 255, out=dith)
    d8 = dith.astype(np.uint8)
    key = (d8[..., 0].astype(np.uint32) << 16
           | d8[..., 1].astype(np.uint32) << 8
           | d8[..., 2].astype(np.uint32))                # exact 24-bit
    flat_k = key.ravel()
    miss = np.unique(flat_k)
    if len(miss) > 0:
        cols = np.stack([(miss >> 16) & 255,
                         (miss >> 8) & 255,
                         miss & 255], axis=1).astype(np.float32)
        d = ((cols[:, None, :] - pal_f32[None, :, :]) ** 2).sum(axis=2)
        near = d.argmin(axis=1)
        for k, p in zip(miss.tolist(), near.tolist()):
            cache[k] = p
    lut = np.full(1 << 24, 255, np.uint8)
    ks = np.fromiter(cache.keys(), np.uint32, len(cache))
    vs = np.fromiter(cache.values(), np.uint8, len(cache))
    lut[ks] = vs
    out = lut[flat_k].reshape(h, w)
    del lut
    return out


def downsample_w(frame, factor):
    """(H,W,3) -> (H,W//factor,3) box filter."""
    h, w, _ = frame.shape
    w2 = w // factor
    return frame[:, : w2 * factor, :].reshape(h, w2, factor, 3).mean(axis=2)


# ---------------- asset writers ----------------

def write_idx(path, magic, w, h, frames_u8):
    frames = np.asarray(frames_u8, np.uint8)
    hdr = magic + w.to_bytes(2, "little") + h.to_bytes(2, "little") \
        + len(frames).to_bytes(2, "little")
    path.write_bytes(hdr + frames.tobytes())
    return len(hdr) + frames.nbytes


def write_pre_i4(path, base_frames):
    """field-major reorder: frame f, phase p -> rows {p, p+4, ...}."""
    fields = []
    for f in base_frames:
        for p in range(4):
            fields.append(f[p::4, :])          # (80, 480)
    data = np.asarray(fields, np.uint8)
    hdr = b"VLP1" + (480).to_bytes(2, "little") + (320).to_bytes(2, "little") \
        + len(base_frames).to_bytes(2, "little") + (4).to_bytes(2, "little")
    path.write_bytes(hdr + data.tobytes())
    return len(hdr) + data.nbytes


def write_spans(path, base_frames, pal, budget=None):
    """per frame: for each of 320 rows: u8 nspans, then spans
    (u16 x0, u16 len, len index bytes). A pixel is sent when its palette
    COLOUR differs from the DECODER'S CANVAS (not the previous frame!) by
    more than SPAN_COLOR_TOL per channel — comparing against the canvas
    keeps the drift bounded to the tolerance forever (per-frame comparison
    would let consecutive sub-tolerance changes accumulate). Frame 0 is
    full rows. Heavily fragmented rows (> 32 spans or > 92% coverage) are
    stored as ONE full-row span. Frames are included greedily until the
    budget is exhausted."""
    per_frame = []
    pal_i = pal.astype(np.int16)
    canvas = None
    for fr in base_frames:
        out = bytearray()
        first = canvas is None
        if first:
            canvas = fr.copy()          # decoder canvas after the full field
        for y in range(PANEL_H):
            row = fr[y]
            if first:
                spans = [(0, PANEL_W)]  # frame 0 defines the canvas
            else:
                d = np.abs(pal_i[row] - pal_i[canvas[y]]).max(axis=1)
                diff = np.flatnonzero(d > SPAN_COLOR_TOL)
                if len(diff) == 0:
                    spans = []
                else:
                    brk = np.flatnonzero(np.diff(diff) > 1)
                    starts = np.concatenate([[diff[0]], diff[brk + 1]])
                    ends = np.concatenate([diff[brk], [diff[-1]]])
                    spans = [(int(s), int(e - s + 1)) for s, e in zip(starts, ends)]
                    if len(spans) > 32 or len(diff) > 0.92 * PANEL_W:
                        spans = [(0, PANEL_W)]
            out.append(len(spans))
            for x0, ln in spans:
                out += x0.to_bytes(2, "little") + ln.to_bytes(2, "little")
                out += row[x0:x0 + ln].tobytes()
                canvas[y, x0:x0 + ln] = row[x0:x0 + ln]
        per_frame.append(bytes(out))

    # greedy frame inclusion within the budget (keep >= 6 or fail)
    hdr = bytearray(b"VLD1" + (480).to_bytes(2, "little")
                    + (320).to_bytes(2, "little"))
    keep = len(per_frame)
    if budget is not None:
        while keep > 6 and 10 + sum(len(p) for p in per_frame[:keep]) > budget:
            keep -= 1
    hdr += keep.to_bytes(2, "little")
    blob = bytes(hdr) + b"".join(per_frame[:keep])
    path.write_bytes(blob)
    sizes = [len(p) for p in per_frame[:keep]]
    print(f"delta.spans: kept {keep} frames, sizes "
          f"{[f'{s//1024}K' for s in sizes]}")
    return len(blob)


# ---------------- main ----------------

def total_so_far(sizes):
    return sum(sizes.values()) + 768


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    vf = detect_crop()
    scan, start = motion_scan(vf)
    base_rgb = scan[start:start + BASE_FRAMES]
    nbase = len(base_rgb)
    print(f"cut: {nbase} frames from scan-frame {start} "
          f"(t={SCAN_T0 + start / 29.97:.2f}s)")
    if nbase < BASE_FRAMES:
        print(f"ERROR: need {BASE_FRAMES} frames, got {nbase}")
        sys.exit(1)

    # global palette from the base cut (every 2nd pixel of every frame)
    samp = base_rgb[:, ::2, ::2, :].reshape(-1, 3)
    pal = median_cut(samp.copy(), 256)
    pal_f = pal.astype(np.float32)
    (OUT / "palette.pal").write_bytes(pal.tobytes())
    print(f"palette: 256 colours (RGB666 wire bytes), "
          f"{(OUT / 'palette.pal').stat().st_size} B")

    cache = {}
    def quant(fr):
        # 3x3 median first: kills MPEG-2 codec noise so unchanged image
        # areas quantize to IDENTICAL indices (the delta techniques depend
        # on this; same trick as the v7 converter).
        den = np.stack([median_filter(fr[..., c], size=3, mode="nearest")
                        for c in range(3)], axis=-1)
        return quantize_with_dither(den.astype(np.float32), pal_f, cache)

    base_idx = [quant(fr) for fr in base_rgb]
    # quality: decoded palette colour vs the RGB666-quantized original
    err = np.mean([np.abs(pal[base_idx[i]].astype(np.float32)
                          - (base_rgb[i].astype(np.float32) / 4).round() * 4)
                   for i in range(nbase)])
    print(f"dither+quantize: mean decode error {err:.2f}/255")

    sizes = {}
    sizes["base.idx"] = write_idx(OUT / "base.idx", b"VLB1", 480, 320, base_idx)
    sizes["pre_i4.idx"] = write_pre_i4(OUT / "pre_i4.idx", base_idx[:PREI4_FRAMES])
    half_idx = [quant(downsample_w(fr, 2)) for fr in base_rgb[:HALF_FRAMES]]
    sizes["half.idx"] = write_idx(OUT / "half.idx", b"VLB1", 240, 320, half_idx)
    third_idx = [quant(downsample_w(fr, 3)) for fr in base_rgb[:THIRD_FRAMES]]
    sizes["third.idx"] = write_idx(OUT / "third.idx", b"VLB1", 160, 320, third_idx)
    sizes["delta.spans"] = write_spans(OUT / "delta.spans", base_idx[:DELTA_FRAMES],
                                       pal, budget=BUDGET_ASSETS - total_so_far(sizes))

    total = sum(sizes.values()) + 768
    print("\nasset sizes:")
    for k, v in sizes.items():
        print(f"  {k:14s} {v:9,d} B  ({v / 1024:.0f} KiB)")
    print(f"  {'TOTAL':14s} {total:9,d} B  (budget {BUDGET_ASSETS:,d})")
    if total > BUDGET_ASSETS:
        print("OVER BUDGET — shrink the caps and re-run")
        sys.exit(1)
    print("\nall assets written to", OUT)


if __name__ == "__main__":
    main()
