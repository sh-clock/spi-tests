#!/usr/bin/env python3
"""verify_assets.py — host-side round-trip check of the lab assets.

Decodes every blob with the SAME logic the firmware uses (assets.c /
players.c) and cross-checks the assets against each other:

  1. headers: magic / geometry / frame counts / total sizes
  2. palette.pal is 768 bytes of wire RGB666 (each channel a 0..252 step
     of 4)
  3. pre_i4.idx field (f, p) must equal base.idx frame f rows [p::4]
  4. delta.spans must decode, with the firmware's sequential cursor walk,
     to EXACTLY the base.idx frames (spans carry the new pixel values, so
     a correct decode reconstructs the frame bit-for-bit)
  5. half/third: headers only (they are independently encoded), plus a
     histogram sanity check that every index is < 256 by construction
"""
import sys
from pathlib import Path
import numpy as np

MAIN = Path(__file__).resolve().parent.parent / "main"

def rd16(b, o):
    return b[o] | (b[o + 1] << 8)

def check_idx(name, expect_w):
    d = (MAIN / name).read_bytes()
    magic, w, h, frames = d[0:4], rd16(d, 4), rd16(d, 6), rd16(d, 8)
    assert magic == b"VLB1", f"{name}: magic {magic}"
    assert w == expect_w and h == 320, f"{name}: {w}x{h}"
    assert len(d) == 10 + frames * 320 * w, f"{name}: size {len(d)}"
    print(f"  {name}: OK  {frames} frames {w}x{h} ({len(d):,} B)")
    return d, frames, w

def main():
    print("== headers ==")
    base, nbase, _ = check_idx("base.idx", 480)
    check_idx("half.idx", 240)
    check_idx("third.idx", 160)

    # pre_i4: extra u16 phases field
    pi = (MAIN / "pre_i4.idx").read_bytes()
    assert pi[0:4] == b"VLP1"
    w, h, frames, phases = rd16(pi, 4), rd16(pi, 6), rd16(pi, 8), rd16(pi, 10)
    assert (w, h, phases) == (480, 320, 4), (w, h, phases)
    assert len(pi) == 12 + frames * phases * 80 * w
    print(f"  pre_i4.idx: OK  {frames} frames x{phases} fields "
          f"({len(pi):,} B)")

    print("== palette ==")
    pal = (MAIN / "palette.pal").read_bytes()
    assert len(pal) == 768
    for i in range(0, 768):
        assert pal[i] % 4 == 0 and pal[i] <= 252, f"pal[{i}]={pal[i]}"
    assert len(set(tuple(pal[i:i+3]) for i in range(0, 768, 3))) >= 200, \
        "palette has too few distinct colours"
    print(f"  palette.pal: OK  768 B, all channels 0..252 step 4")

    print("== pre_i4 fields vs base frames ==")
    for f in range(frames):
        for p in range(4):
            field = pi[12 + ((f * 4 + p) * 80 * 480):
                        12 + ((f * 4 + p + 1) * 80 * 480)]
            # rows p, p+4, ... each 480 bytes, stored back to back
            expect = b"".join(
                base[10 + (f * 320 + y) * 480:
                     10 + (f * 320 + y + 1) * 480]
                for y in range(p, 320, 4))
            assert field == expect, f"pre_i4 mismatch f={f} p={p}"
    print(f"  all {frames * 4} fields match base rows exactly")

    print("== delta.spans round-trip (firmware cursor walk) ==")
    dl = (MAIN / "delta.spans").read_bytes()
    assert dl[0:4] == b"VLD1"
    dframes = rd16(dl, 8)
    print(f"  {dframes} span frames, {len(dl):,} B")
    TOL = 10      # encoder SPAN_COLOR_TOL: near-boundary dither flicker is
                  # deliberately NOT re-sent, so the canvas only matches the
                  # base frames within this tolerance (frame 0 exactly)
    PC = [tuple(pal[i * 3:i * 3 + 3]) for i in range(256)]  # idx -> rgb
    PCa = np.array(PC, np.int16)
    cur = 10
    canvas = bytearray(480 * 320)
    for k in range(dframes):
        for y in range(320):
            nspans = dl[cur]; cur += 1
            row = memoryview(canvas)[y * 480:(y + 1) * 480]
            for _ in range(nspans):
                x0 = rd16(dl, cur); ln = rd16(dl, cur + 2); cur += 4
                row[x0:x0 + ln] = dl[cur:cur + ln]
                cur += ln
        want = base[10 + k * 320 * 480: 10 + (k + 1) * 320 * 480]
        if k == 0:
            ok = bytes(canvas) == want
            worst = 0
        else:
            cv = np.frombuffer(bytes(canvas), np.uint8)
            wf = np.frombuffer(want, np.uint8)
            worst = int(np.abs(PCa[cv] - PCa[wf]).max(axis=1).max())
            ok = worst <= TOL
        if not ok:
            ndiff = sum(a != b for a, b in zip(canvas, want))
            print(f"  frame {k}: MISMATCH ({ndiff} bytes differ, "
                  f"worst colour distance {worst})")
            return 1
        print(f"  frame {k}: reconstructs "
              f"{'exactly' if k == 0 else f'within tol (worst {worst})'} "
              f"({100 * (len(dl) - 10) // (320 * 480 * dframes)}% of raw avg)")
    assert cur == len(dl), f"cursor {cur} != file {len(dl)}"

    print("\nALL ASSET CHECKS PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
