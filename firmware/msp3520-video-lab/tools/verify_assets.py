#!/usr/bin/env python3
"""verify_assets.py — host-side gate for the lab assets (full-clip v2).

Decodes every blob with the SAME logic the firmware uses (labvid.c /
assets.c / players.c) and cross-checks the assets against each other:

  1. headers: magic / geometry / frame counts / offset table / sizes
  2. palette.pal: 768 bytes of wire RGB666 (channels 0..252 step 4)
  3. base.vlb2: every frame decodes (RLE+LZSS, the firmware walk) to a
     full 480x320 index frame — the Python mirror of labvid.c
  4. mbmap.bin: every frame's dirty bitmap is recomputed from the
     DECODED frames and must match bit-for-bit (independent check of
     both the decoder and the encoder's bitmap)
  5. merge2.bin: all 65536 entries recomputed from palette.pal — the
     exact table players.c expand_merged() reads
  6. budget: total asset size fits the 8 MB factory partition next to
     ~0.6 MB of app code

Exit code 0 = all gates passed.
"""
import sys
from pathlib import Path
import numpy as np

MAIN = Path(__file__).resolve().parent.parent / "main"
BUDGET_ASSETS = 7_650_000       # keep in sync with tools/encode_lab.py


def rd16(b, o):
    return b[o] | (b[o + 1] << 8)


def rd32(b, o):
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24))


# ---------------- firmware-mirror decoders (labvid.c) ------------------

def lzss_decode(src, total):
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
            if off > 32768:
                raise AssertionError(f"LZSS offset {off} > ring 32768")
            for k in range(ln):
                v = ring[(pos - off + k) & 0xFFFF]
                ring[(pos + k) & 0xFFFF] = v
                out.append(v)
            pos += ln
    if len(out) != total or sp != len(src):
        raise AssertionError("LZSS decode length mismatch")
    return bytes(out)


def lzss_stream_len(src):
    """Output length of a stream, by walking it (strict consumption)."""
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
    if sp != len(src):
        raise AssertionError("stream walk did not consume exactly")
    return total


def decode_frame(src, w, h):
    """Mirror of labvid.c: LZSS layer -> per-row RLE expansion."""
    rle = lzss_decode(src, lzss_stream_len(src))
    out = bytearray()
    cur = 0
    for _ in range(h):
        px = 0
        while px < w:
            if cur + 2 > len(rle):
                raise AssertionError("RLE truncated")
            cb, idx = rle[cur], rle[cur + 1]
            cur += 2
            take = min(cb + 1, w - px)
            out += bytes((idx,)) * take
            px += take
        if px != w:
            raise AssertionError("RLE row overrun")
    if cur != len(rle):
        raise AssertionError("RLE trailing bytes")
    return bytes(out)


# ---------------- gates ------------------------------------------------

def main():
    ok = True

    print("== base.vlb2 header ==")
    d = (MAIN / "base.vlb2").read_bytes()
    if d[:4] != b"VLB2":
        print(f"FAIL: magic {d[:4]!r}")
        return 1
    w, h, n = rd16(d, 4), rd16(d, 6), rd16(d, 8)
    if (w, h) != (480, 320):
        print(f"FAIL: geometry {w}x{h}")
        return 1
    table = 10 + 4 * (n + 1)
    if len(d) < table or n == 0:
        print(f"FAIL: size {len(d)} for {n} frames")
        return 1
    offs = [rd32(d, 10 + 4 * i) for i in range(n + 1)]
    if any(offs[i] > offs[i + 1] for i in range(n)):
        print("FAIL: offsets not monotonic")
        return 1
    if offs[-1] != len(d) - table:
        print(f"FAIL: last offset {offs[-1]} != payload {len(d) - table}")
        return 1
    if offs[0] != 0:
        print("FAIL: first offset != 0")
        return 1
    print(f"  OK  {n} frames {w}x{h}, payload {len(d) - table:,} B, "
          f"table {table} B, total {len(d):,} B")

    print("== palette.pal ==")
    pal = (MAIN / "palette.pal").read_bytes()
    if len(pal) != 768:
        print(f"FAIL: {len(pal)} B, want 768")
        return 1
    for i in range(768):
        if pal[i] % 4 or pal[i] > 252:
            print(f"FAIL: pal[{i}]={pal[i]} not a RGB666 wire value")
            return 1
    distinct = len(set(tuple(pal[i:i + 3]) for i in range(0, 768, 3)))
    print(f"  OK  768 B, {distinct} distinct colours (wire RGB666)")

    print("== decode every frame (firmware walk) ==")
    frames = []
    for i in range(n):
        s, e = table + offs[i], table + offs[i + 1]
        fr = decode_frame(d[s:e], w, h)
        if len(fr) != w * h:
            print(f"FAIL: frame {i} decoded {len(fr)} B")
            return 1
        frames.append(fr)
        if (i + 1) % 20 == 0:
            print(f"  decoded {i + 1}/{n}")
    print(f"  OK  all {n} frames decode to full {w}x{h} index frames")

    print("== mbmap.bin vs decoded frames ==")
    mb = (MAIN / "mbmap.bin").read_bytes()
    if mb[:4] != b"VLM1":
        print(f"FAIL: magic {mb[:4]!r}")
        return 1
    bw, bh, mframes = rd16(mb, 4), rd16(mb, 6), rd16(mb, 8)
    stride = (bw * bh + 7) // 8
    if (bw, bh) != (30, 20) or mframes != n or len(mb) != 10 + stride * n:
        print(f"FAIL: {bw}x{bh} x{mframes} frames, {len(mb)} B")
        return 1
    arr = np.frombuffer(b"".join(frames), np.uint8).reshape(n, h, w)
    mism = 0
    for f in range(n):
        if f == 0:
            expect = np.ones((bh, bw), bool)
        else:
            expect = (arr[f] != arr[f - 1]).reshape(bh, 16, bw, 16).any(axis=(1, 3))
        got = np.zeros((bh, bw), bool)
        bm = mb[10 + f * stride: 10 + (f + 1) * stride]
        for by in range(bh):
            for bx in range(bw):
                i = by * bw + bx
                got[by, bx] = bool((bm[i >> 3] >> (i & 7)) & 1)
        if not np.array_equal(expect, got):
            mism += 1
            if mism == 1:
                print(f"FAIL: bitmap mismatch at frame {f}")
    if mism:
        print(f"  FAIL  {mism} frames with wrong dirty bitmaps")
        return 1
    print(f"  OK  all {n} dirty bitmaps recomputed bit-for-bit")

    print("== merge2.bin vs palette ==")
    m2 = (MAIN / "merge2.bin").read_bytes()
    if len(m2) != 65536:
        print(f"FAIL: {len(m2)} B, want 65536")
        return 1
    pal_f = np.frombuffer(pal, np.uint8).astype(np.float32).reshape(256, 3)
    a = np.repeat(np.arange(256), 256)
    b = np.tile(np.arange(256), 256)
    m2a = np.frombuffer(m2, np.uint8)
    bad = 0
    for i0 in range(0, 65536, 4096):
        i1 = i0 + 4096
        avg = (pal_f[a[i0:i1]] + pal_f[b[i0:i1]]) / 2.0
        near = ((avg[:, None, :] - pal_f[None, :, :]) ** 2).sum(axis=2).argmin(axis=1)
        bad += int((near != m2a[i0:i1]).sum())
    if bad:
        print(f"FAIL: {bad} merge2 entries differ from recomputation")
        return 1
    print("  OK  all 65536 merge entries match palette averages")

    print("== budget ==")
    sizes = {
        "base.vlb2": len(d), "palette.pal": len(pal),
        "merge2.bin": len(m2), "mbmap.bin": len(mb),
    }
    total = sum(sizes.values())
    for k, v in sizes.items():
        print(f"  {k:14s} {v:9,d} B")
    print(f"  {'TOTAL':14s} {total:9,d} B / {BUDGET_ASSETS:,d}")
    if total > BUDGET_ASSETS:
        print("FAIL: over budget")
        return 1

    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
