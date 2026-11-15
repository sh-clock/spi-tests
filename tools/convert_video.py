#!/usr/bin/env python3
"""Convert Hill[729].mp4 into the compressed interlaced video blob
(video.bin) for the MSP3520 ESP32-S3 video player / SPI overclock tester.

Pipeline (all steps verified by an in-script roundtrip):
  1. ffmpeg: crop letterbox bars (320x208), scale to 480x320, 15 fps, rgb24
  2. 3x3 median filter (kills h264 dither noise -> much better compression)
  3. RGB888 -> wire bytes: RGB666 (3 bytes/pixel, COLMOD 0x66 — the only
     pixel format the ILI9488 supports on its native 4-wire SPI interface;
     verified against TFT_eSPI, LovyanGFX, Arduino_GFX and LCDWiki's own
     MSP3520 demo) or RGB565 (2 bytes/pixel, 0x55 — parallel panels only;
     on native SPI it produces a BLANK screen)
  4. interlace split: field 0 = even display lines, field 1 = odd lines, ...
  5. per-line RLE: runs of [count-1] + bpp payload bytes, counts sum to 480
  6. LZSS: 64 KiB window, literal = 1 byte, match = 3 bytes (off 1..65535,
     len 4..259), 1 flag byte per 8 tokens
  7. blob = 24-byte header + LZSS stream

Blob header (little-endian):
  0  "MIV2"
  4  u16 width            (480)
  6  u16 height           (320)
  8  u16 frames           (119)
  10 u16 flags            (bit0: 0 = even lines first)
  12 u32 rle_len          (decoded LZSS stream size in bytes)
  16 u32 fields           (frames * 2)
  20 u32 bpp              (3 = RGB666 default, 2 = RGB565)
"""

import argparse
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H = 480, 320
BPP = 3                     # wire bytes/pixel: 3 = RGB666 (ILI9488 SPI)
LZ_WIN = 65535
LZ_MIN_MATCH = 5
LZ_MAX_MATCH = 259
LZ_CHAIN = 24


# ---------------------------------------------------------------- step 1-3
def decode_frames(src, crop, fps):
    """ffmpeg -> list of HxWx3 uint8 arrays (median-filtered)"""
    vf = f"crop={crop},{'' if crop == 'in_w:in_h:0:0' else ''}scale={W}:{H}"
    raw = "/tmp/_conv_frames.rgb24"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", src, "-vf", vf, "-r", str(fps),
         "-f", "rawvideo", "-pix_fmt", "rgb24", raw],
        check=True)
    size = os.path.getsize(raw)
    n = size // (W * H * 3)
    data = np.fromfile(raw, dtype=np.uint8).reshape(n, H, W, 3)
    print(f"step1: decoded {n} frames @ {W}x{H}")

    out = np.empty((n, H, W, 3), dtype=np.uint8)
    for i in range(n):
        img = Image.fromarray(data[i]).filter(ImageFilter.MedianFilter(3))
        out[i] = np.asarray(img, dtype=np.uint8)
    print("step2: median3 applied")
    return out


def to_rgb565(frames):
    """RGB888 -> the 2 bytes/pixel RGB565 wire format, big-endian
    (hi = R4..R0 G5..G3, lo = G2..G0 B4..B0 — ILI9488 16bpp, MSB first)."""
    n = frames.shape[0]
    r = (frames[:, :, :, 0].astype(np.uint16) >> 3) & 0x1F   # 5 bits
    g = (frames[:, :, :, 1].astype(np.uint16) >> 2) & 0x3F   # 6 bits
    b = (frames[:, :, :, 2].astype(np.uint16) >> 3) & 0x1F   # 5 bits
    c565 = (r << 11) | (g << 5) | b
    out = np.empty((n, H, W, 2), dtype=np.uint8)
    out[:, :, :, 0] = (c565 >> 8) & 0xFF    # hi byte goes on the wire first
    out[:, :, :, 1] = c565 & 0xFF
    print("step3: RGB565 conversion (2 bytes/pixel wire format)")
    return out


def to_rgb666(frames):
    """RGB888 -> the 3 bytes/pixel RGB666 wire format (top 6 bits each).
    This is the ONLY pixel format the ILI9488 accepts on its native 4-wire
    SPI interface (RGB565/0x55 is parallel-interface only and gives a
    blank screen on native SPI)."""
    n = frames.shape[0]
    out = np.empty((n, H, W, 3), dtype=np.uint8)
    out[:, :, :, 0] = (frames[:, :, :, 0] >> 3) << 3
    out[:, :, :, 1] = (frames[:, :, :, 1] >> 2) << 2
    out[:, :, :, 2] = (frames[:, :, :, 2] >> 3) << 3
    print("step3: RGB666 conversion (3 bytes/pixel wire format)")
    return out


# ---------------------------------------------------------------- step 4-5
def build_rle_stream(frames_wire, progressive=True):
    """display lines -> one long RLE byte stream (frame- or field-sliced)"""
    n = frames_wire.shape[0]
    stream = bytearray()
    for i in range(n):
        f = frames_wire[i]
        units = (f,) if progressive else (f[0::2], f[1::2])
        for field in units:
            for line in field:
                same = np.all(line[1:] == line[:-1], axis=1)
                breaks = np.where(~same)[0] + 1
                starts = np.concatenate(([0], breaks))
                ends = np.concatenate((breaks, [len(line)]))
                for s, e in zip(starts, ends):
                    px = line[s].tobytes()          # bpp payload bytes
                    L = e - s
                    while L > 0:
                        c = min(L, 256)
                        stream += bytes((c - 1,)) + px
                        L -= c
    n_units = n if progressive else n * 2
    print(f"step4+5: {n} frames = {n_units} units, RLE stream "
          f"{len(stream)/1e6:.2f} MB")
    return bytes(stream)


# ---------------------------------------------------------------- step 6
def lzss_encode(data, chain=24, store=48):
    n = len(data)
    out = bytearray()
    head = {}
    i = 0
    tokens = []          # (is_literal, bytes)

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
        limit = pos - LZ_WIN
        cnt = 0
        maxl = min(LZ_MAX_MATCH, n - pos)
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
        if best_len >= LZ_MIN_MATCH:
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
    """mirror of the C decoder — used for verification"""
    ring = bytearray(65536)
    out = bytearray()
    pos = 0
    sp = 0
    flags = 0
    nbits = 0
    while len(out) < total:
        if nbits == 0:
            flags = src[sp]
            sp += 1
            nbits = 8
        bit = flags & 0x80
        flags = (flags << 1) & 0xFF
        nbits -= 1
        if bit:
            b = src[sp]
            sp += 1
            ring[pos & 0xFFFF] = b
            out.append(b)
            pos += 1
        else:
            off = ((src[sp] << 8) | src[sp + 1]) + 1
            sp += 2
            ln = src[sp] + 4
            sp += 1
            for k in range(ln):
                b = ring[(pos - off + k) & 0xFFFF]
                ring[(pos + k) & 0xFFFF] = b
                out.append(b)
            pos += ln
    if len(out) != total:
        raise RuntimeError("decode overran expected size")
    return bytes(out)


def rle_expand_stream(stream, n_fields, lines_per_field, bpp=None):
    """expand the RLE stream back into display lines (verification)"""
    if bpp is None:
        bpp = BPP
    out = bytearray()
    sp = 0
    for f in range(n_fields):
        for ln in range(lines_per_field):
            px = 0
            while px < W:
                cnt = stream[sp] + 1
                sp += 1
                rgb = stream[sp:sp + bpp]
                sp += bpp
                take = min(cnt, W - px)
                out += bytes(rgb) * take
                px += cnt
            if px != W:
                raise RuntimeError(f"field {f} line {ln}: px={px} != {W}")
    return bytes(out)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--crop", default="320:208:0:16",
                    help="ffmpeg crop before scaling (iw:ih:0:0 = no crop)")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--bpp", type=int, choices=(2, 3), default=3,
                    help="wire bytes/pixel: 3 = RGB666 (ILI9488 SPI, the "
                         "default) or 2 = RGB565 (parallel-interface panels "
                         "only — blank screen on native SPI)")
    ap.add_argument("--scan", choices=("progressive", "interlaced"),
                    default="progressive",
                    help="progressive = whole frames (v4 player, default); "
                         "interlaced = even/odd fields (legacy v1-v3 blobs)")
    ap.add_argument("--chain", type=int, default=24,
                    help="LZSS match chain length (deeper = smaller/slower)")
    ap.add_argument("--store", type=int, default=48,
                    help="hash table positions kept per 3-byte key")
    args = ap.parse_args()

    frames = decode_frames(args.input, args.crop, args.fps)
    global BPP
    BPP = args.bpp
    frames_wire = to_rgb565(frames) if BPP == 2 else to_rgb666(frames)
    progressive = args.scan == "progressive"
    stream = build_rle_stream(frames_wire, progressive)
    print("step6: LZSS encoding (this takes a minute) ...")
    sys.stdout.flush()
    lz = lzss_encode(stream, args.chain, args.store)
    n_frames = frames_wire.shape[0]

    flags = 2 if progressive else 0
    n_units = n_frames if progressive else n_frames * 2
    hdr = struct.pack("<4sHHHHII I".replace(" ", ""),
                      b"MIV2", W, H, n_frames, flags,
                      len(stream), n_units, BPP)
    blob = hdr + lz
    with open(args.output, "wb") as f:
        f.write(blob)

    print(f"\nRESULT: {args.output}")
    print(f"  frames {n_frames}, fields {n_frames*2}, "
          f"RLE {len(stream)/1e6:.2f} MB, LZ {len(lz)/1e6:.2f} MB")
    print(f"  blob total {len(blob)/1e6:.3f} MB "
          f"({len(blob)/n_frames/1024:.1f} KiB/frame)")
    raw = n_frames * W * H * BPP
    print(f"  compression vs raw wire data: {raw/len(blob):.1f}x")

    # ---- full roundtrip verification
    print("\nverify: LZSS roundtrip ...", flush=True)
    dec = lzss_decode(lz, len(stream))
    assert dec == stream, "LZSS ROUNDTRIP FAILED"
    print("  LZSS OK")
    print("verify: RLE expansion ...", flush=True)
    lines = rle_expand_stream(dec, n_units, H if progressive else H // 2)

    def interleave(field_even, field_odd):
        """two 160-line fields -> one 320-line frame (bytes)"""
        W3 = W * BPP
        ev = np.frombuffer(field_even, dtype=np.uint8).reshape(H // 2, W3)
        od = np.frombuffer(field_odd, dtype=np.uint8).reshape(H // 2, W3)
        fr = np.empty((H, W3), dtype=np.uint8)
        fr[0::2] = ev
        fr[1::2] = od
        return fr

    if progressive:
        f0 = np.frombuffer(lines[0:H * W * BPP],
                           dtype=np.uint8).reshape(H, W * BPP)
    else:
        f0 = interleave(lines[0:(H // 2) * W * BPP],
                        lines[(H // 2) * W * BPP:H * W * BPP])
    assert f0.tobytes() == frames_wire[0].reshape(H, W * BPP).tobytes(), \
        "RLE EXPANSION FAILED"
    print("  RLE reconstruction OK (frame 0 byte-identical)")

    W3 = W * BPP
    if progressive:
        # progressive: the stream is already whole frames in order
        allexp = np.frombuffer(lines, dtype=np.uint8).reshape(n_frames, H, W3)
    else:
        allexp = np.empty((n_frames, H, W3), dtype=np.uint8)
        for fi in range(n_frames):
            base = fi * H * W3
            allexp[fi] = interleave(lines[base:base + (H // 2) * W3],
                                    lines[base + (H // 2) * W3:base + H * W3])
    assert allexp.tobytes() == frames_wire.reshape(n_frames, H, W3).tobytes(), \
        "FULL EXPANSION FAILED"
    print(f"  all {n_frames} frames byte-identical — converter verified")


if __name__ == "__main__":
    main()
