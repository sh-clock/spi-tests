#!/usr/bin/env python3
"""Host-verify main/video.c against the blob:
compile the device decoder with gcc, decode everything, and compare
byte-for-byte with the Python reference expansion."""

import hashlib
import struct
import subprocess
import sys

sys.path.insert(0, "/home/z/my-project/scripts")
from convert_video import lzss_decode, rle_expand_stream  # noqa: E402

PROJ = "/home/z/my-project/download/msp3520-video-oc-test"
BLOB = f"{PROJ}/main/video.bin"
OUT = "/tmp/c_decoded_lines.bin"

blob = open(BLOB, "rb").read()
hdr = struct.unpack("<4sHHHHIII", blob[:24])
magic, W, H, frames, flags, rle_len, fields, _ = hdr
print(f"blob: {len(blob)} bytes  hdr={magic} {W}x{H} frames={frames} "
      f"fields={fields} rle_len={rle_len}")

# reference: LZSS decode -> RLE expand
lz = blob[24:]
dec = lzss_decode(lz, rle_len)
expected = rle_expand_stream(dec, fields, H // 2)
print(f"reference expansion: {len(expected)} bytes, "
      f"sha256={hashlib.sha256(expected).hexdigest()[:16]}")

# compile + run the device code
subprocess.run(
    ["gcc", "-O2", "-Wall", "-Wextra", "-I", f"{PROJ}/main",
     "-o", "/tmp/video_host_test",
     "/home/z/my-project/scripts/test_video_host.c",
     f"{PROJ}/main/video.c"],
    check=True)
subprocess.run(["/tmp/video_host_test", BLOB, OUT], check=True)

actual = open(OUT, "rb").read()
if actual == expected:
    print(f"C decoder output matches: {len(actual)} bytes byte-identical")
    print("sha256:", hashlib.sha256(actual).hexdigest()[:16])
    print("HOST VERIFY: PASS")
else:
    n = min(len(actual), len(expected))
    first = next((i for i in range(n) if actual[i] != expected[i]), n)
    print(f"HOST VERIFY: FAIL  (first difference at byte {first}, "
          f"len actual={len(actual)} expected={len(expected)})")
    sys.exit(1)
