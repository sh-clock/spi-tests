#!/usr/bin/env python3
"""measure_full.py — how big is the FULL 12s INTRO.mkv clip under the lab codec?

Measures, on the real content (calm window t~2-6s, burst window t~8.5-12s):
  - dither: 'bayer2' (current lab look) vs 'line' (v7's 8-row line dither)
    vs 'none' — size AND decode error
  - codec: per-frame RLE -> LZSS (the v6/v7 stack on 1-byte indices),
    per-frame independent vs one sequential stream (cross-frame window)
  - LZSS window 64K vs 32K (firmware DRAM cost of the ring)
  - 15 fps estimate: per-frame independence means dropping frames is exact
    arithmetic on the measured per-frame sizes

Nothing is written into the firmware tree; results print to stdout.
"""
import subprocess
import sys
import time
import numpy as np
from scipy.ndimage import median_filter

SRC = "/home/z/my-project/upload/INTRO.mkv"
W, H = 480, 320
CALM = slice(60, 180)      # t = 2.0-6.0 s (calm regime)
BURST = slice(255, 361)    # t = 8.5-12.0 s (burst regime)
BUDGET = 7.55e6            # bytes available for the base asset

BAYER2 = np.array([[0, 2], [3, 1]], dtype=np.float32) / 4.0
LINE8 = np.array([0, 32, 8, 40, 2, 34, 10, 42], dtype=np.float32) / 64.0
AMP = 4.0                  # /255, one RGB666 step


def grab(t, vf, w, h):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", f"{t}", "-i", SRC, "-frames:v", "1",
         "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


def build_vf():
    desq_w, desq_h = 640, 480
    probe = grab(9.5, f"scale={desq_w}:{desq_h}", desq_w, desq_h).astype(np.float32).mean(axis=(1, 2))
    active = np.where(probe > 8.0)[0]
    top, bot = int(active[0]), int(desq_h - 1 - active[-1])
    h = desq_h - top - bot
    w3x2 = min((h * 3) // 2, desq_w)
    cc = (desq_w - w3x2) // 2
    return (f"scale={desq_w}:{desq_h},crop={w3x2}:{h}:{cc}:{top},"
            f"scale={W}:{H}")


def decode_all(vf):
    t0 = time.time()
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", SRC, "-vf", vf,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    n = len(raw) // (W * H * 3)
    frames = np.frombuffer(raw, np.uint8).reshape(n, H, W, 3)
    print(f"decoded {n} frames @ native 29.97 fps ({time.time()-t0:.0f}s)")
    return frames


def median_cut(samples, n_colors=256):
    boxes = [samples]
    while len(boxes) < n_colors:
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
    pal = (np.round(pal / 4.0) * 4.0).clip(0, 252)
    pal = np.unique(pal, axis=0)
    if len(pal) > n_colors:
        pal = pal[:n_colors]
    while len(pal) < n_colors:
        pal = np.vstack([pal, pal[: n_colors - len(pal)]])
    return pal.astype(np.uint8)


def quantize(frame_f32, pal_f32, cache, dither):
    h, w, _ = frame_f32.shape
    if dither == "bayer2":
        yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
        thr = (BAYER2[yy % 2, xx % 2] - 0.5) * AMP
    elif dither == "line":
        yy = np.arange(h)[:, None]
        thr = np.broadcast_to((LINE8[yy % 8] - 0.5) * AMP, (h, w))
    else:
        thr = np.zeros((h, w), np.float32)
    d = frame_f32 + thr[..., None] if thr is not None else frame_f32.copy()
    for c in range(3):
        d[..., c] = frame_f32[..., c] + thr
    np.clip(d, 0, 255, out=d)
    d8 = d.astype(np.uint8)
    key = (d8[..., 0].astype(np.uint32) << 16 | d8[..., 1].astype(np.uint32) << 8
           | d8[..., 2].astype(np.uint32))
    flat_k = key.ravel()
    miss = np.unique(flat_k)
    if len(miss) > 0:
        cols = np.stack([(miss >> 16) & 255, (miss >> 8) & 255, miss & 255],
                        axis=1).astype(np.float32)
        dd = ((cols[:, None, :] - pal_f32[None, :, :]) ** 2).sum(axis=2)
        near = dd.argmin(axis=1)
        for k, p in zip(miss.tolist(), near.tolist()):
            cache[k] = p
    lut = np.full(1 << 24, 255, np.uint8)
    ks = np.fromiter(cache.keys(), np.uint32, len(cache))
    vs = np.fromiter(cache.values(), np.uint8, len(cache))
    lut[ks] = vs
    out = lut[flat_k].reshape(h, w)
    del lut
    return out


def rle_frame(fr):
    """(H,W) u8 indices -> RLE bytes (count-1 + payload, runs <= 256)."""
    out = bytearray()
    for row in fr:
        brk = np.flatnonzero(np.diff(row)) + 1
        starts = np.concatenate(([0], brk))
        ends = np.concatenate((brk, [len(row)]))
        for s, e in zip(starts, ends):
            L = e - s
            while L > 0:
                c = min(L, 256)
                out += bytes((c - 1, row[s]))
                L -= c
    return bytes(out)


def lzss_encode(data, win=65535, chain=24, store=48):
    n = len(data)
    out = bytearray()
    head = {}
    i = 0
    tokens = []

    def insert(pos):
        if pos + 3 <= n:
            key = data[pos] << 16 | data[pos + 1] << 8 | data[pos + 2]
            lst = head.get(key)
            if lst is None:
                head[key] = [pos]
            else:
                lst.append(pos)
                if len(lst) > store:
                    del lst[:store // 2]

    def best_match(pos):
        if pos + 3 > n:
            return 0, 0
        key = data[pos] << 16 | data[pos + 1] << 8 | data[pos + 2]
        lst = head.get(key)
        if not lst:
            return 0, 0
        best_len = 0
        best_off = 0
        limit = pos - win
        cnt = 0
        maxl = min(259, n - pos)
        for p in reversed(lst):
            if p < limit:
                break
            cnt += 1
            if cnt > chain:
                break
            if best_len and data[p + best_len] != data[pos + best_len]:
                continue
            l = 0
            while l < maxl and data[p + l] == data[pos + l]:
                l += 1
            if l > best_len:
                best_len, best_off = l, pos - p
                if l >= maxl:
                    break
        if best_len >= 5:
            return best_len, best_off
        return 0, 0

    def emit(tok):
        tokens.append(tok)
        if len(tokens) == 8:
            flags = 0
            blob = bytearray()
            for k, (lit, b) in enumerate(tokens):
                if lit:
                    flags |= 1 << (7 - k)
                blob += b
            out.append(flags)
            out.extend(blob)
            tokens.clear()

    while i < n:
        ml, mo = best_match(i)
        if ml and ml < 12 and i + 1 < n:
            ml2, _ = best_match(i + 1)
            if ml2 > ml:
                emit((True, bytes((data[i],))))
                insert(i)
                i += 1
                continue
        if ml:
            emit((False, bytes(((mo - 1) >> 8, (mo - 1) & 0xFF, ml - 4))))
            end = i + ml
            while i < end:
                insert(i)
                i += 1
        else:
            emit((True, bytes((data[i],))))
            insert(i)
            i += 1
    if tokens:
        flags = 0
        blob = bytearray()
        for k, (lit, b) in enumerate(tokens):
            if lit:
                flags |= 1 << (7 - k)
            blob += b
        out.append(flags)
        out += blob
    return bytes(out)


def main():
    vf = build_vf()
    print(f"vf: {vf}")
    frames = decode_all(vf)
    n_total = len(frames)

    t0 = time.time()
    samp = frames[::4][:, ::2, ::2, :].reshape(-1, 3)
    pal = median_cut(samp.copy(), 256)
    pal_f = pal.astype(np.float32)
    print(f"palette built from {len(samp)} samples ({time.time()-t0:.0f}s)")

    # median-filter each measured frame ONCE, reuse for all dither modes
    den = {}
    for name, sl in (("calm", CALM), ("burst", BURST)):
        t0 = time.time()
        out = np.empty((len(frames[sl]), H, W, 3), np.uint8)
        for i, fr in enumerate(frames[sl]):
            for c in range(3):
                out[i, ..., c] = median_filter(fr[..., c], size=3, mode="nearest")
        den[name] = out.astype(np.float32)
        print(f"median3 {name}: {len(out)} frames ({time.time()-t0:.0f}s)")

    per_dither = {}   # dither -> {window: [per-frame LZ sizes]}
    err = {}          # dither -> mean decode error
    for dither in ("bayer2", "line", "none"):
        cache = {}
        per_dither[dither] = {}
        err_acc = []
        for name in ("calm", "burst"):
            src = den[name]
            sizes = []
            for i in range(src.shape[0]):
                qi = quantize(src[i], pal_f, cache, dither)
                err_acc.append(np.abs(pal[qi].astype(np.float32)
                                      - (src[i] / 4).round() * 4).mean())
                rle = rle_frame(qi)
                t0 = time.time()
                lz = lzss_encode(rle)
                sizes.append(len(lz))
                if i % 20 == 0:
                    print(f"  {dither}/{name}: frame {i} rle={len(rle)//1024}K "
                          f"lz={len(lz)//1024}K ({time.time()-t0:.1f}s)")
                    sys.stdout.flush()
            per_dither[dither][name] = sizes
        err[dither] = float(np.mean(err_acc))
        c, b = per_dither[dither]["calm"], per_dither[dither]["burst"]
        print(f"== {dither}: calm avg {np.mean(c)/1024:.1f}K max {np.max(c)/1024:.1f}K | "
              f"burst avg {np.mean(b)/1024:.1f}K max {np.max(b)/1024:.1f}K | "
              f"err {err[dither]:.2f}/255")
        sys.stdout.flush()

    # sequential reference (cross-frame LZ window) on calm/bayer2
    t0 = time.time()
    cache = {}
    seq_rle = b"".join(
        rle_frame(quantize(den["calm"][i], pal_f, cache, "bayer2"))
        for i in range(0, len(den["calm"]), 4))
    per = [len(lzss_encode(rle_frame(quantize(den["calm"][i], pal_f, cache, "bayer2"))))
           for i in range(0, len(den["calm"]), 4)]
    seq = len(lzss_encode(seq_rle))
    print(f"sequential ref (calm/bayer2, every 4th frame): sum-per-frame "
          f"{sum(per)} vs sequential {seq} ({seq/max(sum(per),1)*100:.0f}%) "
          f"({time.time()-t0:.0f}s)")

    # 32K window on calm/bayer2
    per32 = [len(lzss_encode(rle_frame(quantize(den["calm"][i], pal_f, cache, "bayer2")), win=32768))
             for i in range(0, len(den["calm"]), 4)]
    print(f"32K window vs 64K (calm/bayer2): {sum(per32)} vs {sum(per)} "
          f"({sum(per32)/max(sum(per),1)*100:.1f}%)")

    print(f"\n=== projection to the full {n_total}-frame clip "
          f"(budget {BUDGET/1e6:.2f} MB) ===")
    n_calm_total = 240        # t < 8.0 s
    n_burst_total = n_total - 240
    for dither in ("bayer2", "line", "none"):
        c, b = per_dither[dither]["calm"], per_dither[dither]["burst"]
        full = n_calm_total * np.mean(c) + n_burst_total * np.mean(b)
        half = (n_calm_total // 2) * np.mean(c) + (n_burst_total // 2) * np.mean(b)
        print(f"  {dither:<6} err={err[dither]:.2f}  29.97fps: {full/1e6:6.2f} MB "
              f"[{'FITS' if full <= BUDGET else 'over '}]   "
              f"15fps: {half/1e6:6.2f} MB [{'FITS' if half <= BUDGET else 'over '}]")


if __name__ == "__main__":
    main()
