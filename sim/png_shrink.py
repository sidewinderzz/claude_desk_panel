"""Shrink sim/shots/*.png into docs/screenshots/ for the README.

png_write.c emits stored (uncompressed) DEFLATE - it is a 200-line encoder with
no zlib to call, and 1.1 MB a frame is fine for a file you look at once. Twelve
of those in git is not. This re-deflates the same pixels at level 9: byte-for-byte
identical images, ~75x smaller. Lossless, no dependencies.

Run from the repo root:  python sim/png_shrink.py
"""
import os, struct, zlib

SRC = "sim/shots"
DST = "docs/screenshots"

def chunks(data):
    i = 8
    while i < len(data):
        ln = struct.unpack(">I", data[i:i+4])[0]
        typ = data[i+4:i+8]
        yield typ, data[i+8:i+8+ln]
        i += 8 + ln + 4

def emit(typ, body):
    return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ + body) & 0xffffffff)

os.makedirs(DST, exist_ok=True)

total_in = total_out = 0
for name in sorted(os.listdir(SRC)):
    if not name.endswith(".png"):
        continue
    raw = open(os.path.join(SRC, name), "rb").read()
    out = raw[:8]
    idat = b""
    for typ, body in chunks(raw):
        if typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            out += emit(b"IDAT", zlib.compress(zlib.decompress(idat), 9))
            out += emit(typ, body)
        else:
            out += emit(typ, body)
    open(os.path.join(DST, name), "wb").write(out)
    total_in += len(raw); total_out += len(out)
    print("%-20s %7.1f KB -> %6.1f KB" % (name, len(raw)/1024.0, len(out)/1024.0))
print("total %.2f MB -> %.2f MB" % (total_in/1048576.0, total_out/1048576.0))
