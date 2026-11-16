#!/usr/bin/env python3
"""encode_lab.py — build the msp3520-video-lab flash assets from INTRO.mkv.

FULL-CLIP edition (v2). The lab used to bake a 20-frame cut as raw
indexed frames (base.idx, 153.6 KB/frame) plus four derived assets
(pre_i4/half/third/delta.spans). This edition stores the WHOLE 12.045 s
clip as ONE compressed base asset and derives everything else at runtime:

  1. desqueeze the anamorphic DVD frame (SAR 8:9 -> 640x480), detect and
     crop the 16:9 letterbox bars, center-crop to 3:2 and scale to the
     panel's 480x320 (pan-and-scan, no geometry distortion);
  2. sample at 10 fps (measured: the wire shows ~10.5 full frames/s at
     40 MHz, so 10 fps plays essentially real-time; full 29.97 fps would
     need ~22 MB — see WORKLOG 20);
  3. ONE global 256-colour palette in RGB666 space (median cut over the
     whole clip) — the palette bytes ARE the ILI9488 wire bytes, so the
     firmware expands one index to 3 wire bytes with a 768-byte LUT;
  4. encode frames as 8-bit indices with 2x2 Bayer ordered dithering
     (the established lab look), 3x3 median pre-filter (kills MPEG-2
     noise so static areas are bit-identical — the delta techniques
     depend on this);
  5. compress every frame INDEPENDENTLY (RLE runs, then the v6/v7 LZSS
     with a 32 KiB window — must stay <= LABVID_RING_SIZE in labvid.h)
     so any frame is reachable in any order;
  6. write, into ../main/:
       base.vlb2   "VLB2" u16 w u16 h u16 frames
                    u32 offset[frames+1]  (start of each frame's stream)
                    per frame: RLE rows (runs of [count-1][index], counts
                    summing to w) compressed with LZSS
       palette.pal 256 x 3 wire RGB666 bytes (the LUT)
       merge2.bin  64 KiB index-merge LUT: merge2[(a<<8)|b] = palette
                   index nearest to the average of colours a and b (the
                   pixel-doubling techniques decimate the staged 480-wide
                   frame through it; 3:1 is two LUT steps)
       mbmap.bin   "VLM1" u16 bw u16 bh u16 frames + per frame 75 B of
                   16x16 macroblock dirty bits (block differs from the
                   previous frame; frame 0 all-dirty)

Every frame is round-trip verified in-script (LZSS decode == RLE stream,
RLE expansion == the quantized frame), and the whole blob is re-read from
disk and re-decoded before the size report. The script FAILS if the total
asset size exceeds the budget (8 MB factory partition minus app code).
"""
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from scipy.ndimage import median_filter

SRC = "/home/z/my-project/upload/INTRO.mkv"
OUT = Path(__file__).resolve().parent.parent / "main"

# ---- geometry ----
PANEL_W, PANEL_H = 480, 320
DESQ_W, DESQ_H = 640, 480          # square-pixel 4:3 frame

# ---- bake config ----
FPS_BAKE = 10                      # 10 fps sampling of the 29.97 source
DITHER_AMP = 4.0                   # /255, one RGB666 step
LZ_WIN = 32768                     # <= LABVID_RING_SIZE (32 KiB) in labvid.h
BUDGET_ASSETS = 7_650_000          # bytes for all assets together

BAYER2 = np.array([[0, 2], [3, 1]], dtype=np.float32) / 4.0   # 2x2, mean 0.5


def ffmpeg_raw(args):
    return subprocess.run(["ffmpeg", "-v", "error"] + args,
                          capture_output=True, check=True).stdout


def grab_frame(t, vf, w, h):
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
    # center-crop the active area to 3:2, then scale to panel
    w3x2 = (h * 3) // 2
    if w3x2 > DESQ_W:
        w3x2 = DESQ_W
    cc = (DESQ_W - w3x2) // 2
    return (f"{desq},crop={w3x2}:{h}:{cc}:{top},"
            f"scale={PANEL_W}:{PANEL_H}")


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


# ---------------- RLE + LZSS (the v6/v7 codec, 1-byte payload) ---------

def rle_frame(fr):
    """(H,W) u8 indices -> RLE bytes: per row, runs of [count-1][index],
    counts sum to W (the firmware decoder's exact format)."""
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


def rle_expand_rows(stream, h, w):
    """Mirror of the firmware RLE layer (labvid.c lz_row): expand a whole
    frame, validating that every row's counts sum to exactly w."""
    out = bytearray()
    cur = 0
    for _ in range(h):
        px = 0
        while px < w:
            if cur + 2 > len(stream):
                raise AssertionError("RLE stream truncated")
            cb, idx = stream[cur], stream[cur + 1]
            cur += 2
            take = min(cb + 1, w - px)
            out += bytes((idx,)) * take
            px += take
        if px != w:
            raise AssertionError("RLE row overrun")
    if cur != len(stream):
        raise AssertionError("RLE stream has trailing bytes")
    return bytes(out)


def lzss_encode(data, win=LZ_WIN, chain=24, store=48):
    """The v6/v7 LZSS encoder (tools/convert_video.py), parameterised
    window: literal = 1 byte; match = 3 bytes (off-1 BE16, len-4), one
    flag byte per 8 tokens; offsets never exceed `win`."""
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
            # lazy match: if the next position matches longer, take a literal
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


def lzss_decode(src, total):
    """Mirror of the firmware LZSS layer (labvid.c lz_byte), for the
    in-script round-trip gate."""
    ring = bytearray(65536)
    out = bytearray()
    pos = 0
    sp = 0
    flags = 0
    nbits = 0
    while len(out) < total:
        if nbits == 0:
            if sp >= len(src):
                raise AssertionError("LZSS stream truncated")
            flags = src[sp]
            sp += 1
            nbits = 8
        bit = flags & 0x80
        flags = (flags << 1) & 0xFF
        nbits -= 1
        if bit:
            if sp >= len(src):
                raise AssertionError("LZSS literal truncated")
            b = src[sp]
            sp += 1
            ring[pos & 0xFFFF] = b
            out.append(b)
            pos += 1
        else:
            if sp + 3 > len(src):
                raise AssertionError("LZSS match truncated")
            off = ((src[sp] << 8) | src[sp + 1]) + 1
            ln = src[sp + 2] + 4
            sp += 3
            for k in range(ln):
                v = ring[(pos - off + k) & 0xFFFF]
                ring[(pos + k) & 0xFFFF] = v
                out.append(v)
            pos += ln
    if len(out) != total or sp != len(src):
        raise AssertionError("LZSS decode length mismatch")
    return bytes(out)


# ---------------- derived tables ---------------------------------------

def build_merge2(pal):
    """64 KiB LUT: merge2[(a<<8)|b] = palette index nearest (L2) to the
    average of palette colours a and b. Matches players.c expand_merged
    and is recomputed independently by verify_assets.py."""
    pal_f = pal.astype(np.float32)
    a = np.repeat(np.arange(256), 256)
    b = np.tile(np.arange(256), 256)
    m = np.empty(65536, np.uint8)
    for i0 in range(0, 65536, 4096):
        i1 = i0 + 4096
        avg = (pal_f[a[i0:i1]] + pal_f[b[i0:i1]]) / 2.0
        d = ((avg[:, None, :] - pal_f[None, :, :]) ** 2).sum(axis=2)
        m[i0:i1] = d.argmin(axis=1).astype(np.uint8)
    return m.tobytes()


def build_mbmap(idx_frames):
    """Per frame 75 B of macroblock dirty bits (block differs from the
    previous frame; frame 0 all-dirty) — bit (by*30+bx), LSB first,
    exactly what players.c mb_dirty() reads."""
    n = len(idx_frames)
    stride = (30 * 20 + 7) // 8        # 75
    bits = bytearray(stride * n)
    for i in range(600):               # frame 0: everything dirty
        bits[i >> 3] |= 1 << (i & 7)
    for f in range(1, n):
        d = idx_frames[f] != idx_frames[f - 1]
        dirty = d.reshape(20, 16, 30, 16).any(axis=(1, 3))   # (20,30)
        base = f * stride
        for by in range(20):
            row = dirty[by]
            for bx in range(30):
                if row[bx]:
                    i = by * 30 + bx
                    bits[base + (i >> 3)] |= 1 << (i & 7)
    hdr = (b"VLM1" + (30).to_bytes(2, "little") + (20).to_bytes(2, "little")
           + n.to_bytes(2, "little"))
    return hdr + bytes(bits)


# ---------------- main -------------------------------------------------

def main():
    t0 = time.time()
    OUT.mkdir(parents=True, exist_ok=True)

    vf = detect_crop()
    print(f"vf chain: {vf}")

    raw = ffmpeg_raw(["-i", SRC, "-vf", vf, "-r", str(FPS_BAKE),
                      "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
    n = len(raw) // (PANEL_W * PANEL_H * 3)
    dur = n / FPS_BAKE
    print(f"decoded {n} frames @ {FPS_BAKE} fps = {dur:.2f} s of content")
    if not (100 <= n <= 130):
        print(f"ERROR: expected 100-130 frames, got {n}")
        sys.exit(1)
    frames = np.frombuffer(raw, np.uint8).reshape(n, PANEL_H, PANEL_W, 3)

    # global palette over the WHOLE clip (every 2nd pixel)
    samp = frames[:, ::2, ::2, :].reshape(-1, 3)
    pal = median_cut(samp.copy(), 256)
    pal_f = pal.astype(np.float32)
    (OUT / "palette.pal").write_bytes(pal.tobytes())
    print(f"palette: 256 colours from {len(samp):,} samples "
          f"({time.time() - t0:.0f}s)")

    # median3 + dither + quantize every frame (the established pipeline)
    cache = {}
    idx_frames = []
    err_acc = []
    for i in range(n):
        fr = frames[i]
        den = np.stack([median_filter(fr[..., c], size=3, mode="nearest")
                        for c in range(3)], axis=-1)
        qi = quantize_with_dither(den.astype(np.float32), pal_f, cache)
        idx_frames.append(qi)
        err_acc.append(np.abs(pal[qi].astype(np.float32)
                              - (fr.astype(np.float32) / 4).round() * 4).mean())
        if (i + 1) % 20 == 0:
            print(f"  quantized {i + 1}/{n} frames ({time.time() - t0:.0f}s)")
            sys.stdout.flush()
    err = float(np.mean(err_acc))
    print(f"dither+quantize: mean decode error {err:.2f}/255")

    # per-frame RLE -> LZSS, each frame verified as it is built
    streams = []
    sizes = []
    for i, fr in enumerate(idx_frames):
        rle = rle_frame(fr)
        lz = lzss_encode(rle)
        if not lz:
            print(f"ERROR: frame {i} compressed to nothing")
            sys.exit(1)
        dec = lzss_decode(lz, len(rle))
        if dec != rle:
            print(f"ERROR: frame {i} LZSS round-trip failed")
            sys.exit(1)
        if rle_expand_rows(dec, PANEL_H, PANEL_W) != fr.tobytes():
            print(f"ERROR: frame {i} RLE expansion mismatch")
            sys.exit(1)
        streams.append(lz)
        sizes.append(len(lz))
        if (i + 1) % 20 == 0:
            print(f"  compressed {i + 1}/{n} frames ({time.time() - t0:.0f}s)")
            sys.stdout.flush()
    total_idx = n * PANEL_H * PANEL_W
    total_lz = sum(sizes)
    print(f"LZSS: {total_idx:,} -> {total_lz:,} B "
          f"({total_idx / total_lz:.2f}x); per frame "
          f"min {min(sizes)//1024}K avg {total_lz//n//1024}K "
          f"max {max(sizes)//1024}K")

    # base.vlb2
    hdr = (b"VLB2" + PANEL_W.to_bytes(2, "little")
           + PANEL_H.to_bytes(2, "little") + n.to_bytes(2, "little"))
    offs = bytearray()
    acc = 0
    for s in streams:
        offs += acc.to_bytes(4, "little")
        acc += len(s)
    offs += acc.to_bytes(4, "little")
    blob = hdr + bytes(offs) + b"".join(streams)
    (OUT / "base.vlb2").write_bytes(blob)

    # derived tables
    (OUT / "merge2.bin").write_bytes(build_merge2(pal))
    (OUT / "mbmap.bin").write_bytes(build_mbmap(idx_frames))

    # ---- re-read from disk and re-verify EVERY frame (firmware walk) ----
    d = (OUT / "base.vlb2").read_bytes()
    assert d[:4] == b"VLB2"
    w = int.from_bytes(d[4:6], "little")
    h = int.from_bytes(d[6:8], "little")
    nf = int.from_bytes(d[8:10], "little")
    assert (w, h, nf) == (PANEL_W, PANEL_H, n)
    table = 10 + 4 * (nf + 1)
    prev_off = 0
    for i in range(nf + 1):
        o = int.from_bytes(d[10 + 4 * i:14 + 4 * i], "little")
        assert o >= prev_off, f"offset[{i}] not monotonic"
        prev_off = o
    assert prev_off == len(d) - table, "last offset != payload length"
    for i in range(nf):
        s, e = (int.from_bytes(d[10 + 4 * i:14 + 4 * i], "little"),
                int.from_bytes(d[10 + 4 * (i + 1):14 + 4 * (i + 1)], "little"))
        dec = lzss_decode_strict(d[table + s:table + e])
        if rle_expand_rows(dec, h, w) != idx_frames[i].tobytes():
            print(f"ERROR: disk re-verify frame {i} mismatch")
            sys.exit(1)
    print(f"disk re-verify: all {nf} frames decode to the exact indices")

    # ---- size report + budget gate ----
    assets = {
        "base.vlb2": len(blob),
        "palette.pal": 768,
        "merge2.bin": 65536,
        "mbmap.bin": 10 + 75 * n,
    }
    total = sum(assets.values())
    print("\nasset sizes:")
    for k, v in assets.items():
        print(f"  {k:14s} {v:9,d} B  ({v / 1024:.0f} KiB)")
    print(f"  {'TOTAL':14s} {total:9,d} B  (budget {BUDGET_ASSETS:,d})")
    if total > BUDGET_ASSETS:
        print("OVER BUDGET — lower FPS_BAKE (9 -> ~6.6 MB) and re-run")
        sys.exit(1)
    print(f"\nall assets written to {OUT} ({time.time() - t0:.0f}s)")


def lzss_decode_strict(src):
    """lzss_decode with the exact output length derived by consuming the
    stream (used by the disk re-verify pass)."""
    # walk once to find the output length
    total = 0
    sp = 0
    flags = 0
    nbits = 0
    while sp < len(src):
        if nbits == 0:
            flags = src[sp]
            sp += 1
            nbits = 8
        bit = flags & 0x80
        flags = (flags << 1) & 0xFF
        nbits -= 1
        if bit:
            sp += 1
            total += 1
        else:
            sp += 3
            total += src[sp - 1] + 4
    return lzss_decode(src, total)


if __name__ == "__main__":
    main()
