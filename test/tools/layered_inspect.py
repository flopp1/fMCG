"""Sparse structural inspector for layered xz MIDI constructions.

Cuts a buffer of concatenated xz streams BACKWARD using each stream's footer +
index (no decompression needed to find member extents), dedups members by
(xxh/sha1, len), and recurses into unique members whose declared uncompressed
size is <= CAP. Leaves: prints size + chunk-structure summary of the prefix.
Never materialises anything bigger than CAP.
"""
import hashlib, lzma, struct, sys, zlib

CAP = 600 << 20
XZ_MAGIC = b"\xfd7zXZ\x00"


def vli(b, i):
    v = 0; s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80:
            return v, i


def stream_info(buf, end):
    """Given end offset of a stream (exclusive), return (start, unc_size, nblocks)."""
    foot = buf[end - 12:end]
    if foot[10:12] != b"YZ":
        return None
    bsize = (struct.unpack("<I", foot[4:8])[0] + 1) * 4
    idx_end = end - 12
    idx = idx_end - bsize
    if buf[idx] != 0:
        return None
    n, i = vli(buf, idx + 1)
    comp = unc = 0
    for _ in range(n):
        u, i = vli(buf, i); c, i = vli(buf, i)
        comp += (u + 3) & ~3; unc += c
    start = idx - comp - 12
    if start < 0 or buf[start:start + 6] != XZ_MAGIC:
        return None
    return start, unc, n


def cut_members(buf):
    """Backward cut. Returns list of (start, end, unc) in forward order, plus junk."""
    out = []
    end = len(buf)
    while end > 0:
        # stream padding: zero bytes in groups of 4
        while end >= 4 and buf[end - 4:end] == b"\0\0\0\0":
            end -= 4
        if end == 0:
            break
        si = stream_info(buf, end)
        if si is None:
            out.append((0, end, None))   # unparsable head
            break
        out.append((si[0], end, si[1]))
        end = si[0]
    out.reverse()
    return out


SEEN = {}


def summarize_midi(b, total):
    s = []
    i = 0
    n = len(b)
    k = 0
    while i + 8 <= n and k < 6:
        tag = b[i:i + 4]
        if tag in (b"MThd", b"MTrk"):
            L = struct.unpack(">I", b[i + 4:i + 8])[0]
            extra = ""
            if tag == b"MThd":
                extra = " fmt=%d ntrk=%d div=%d" % struct.unpack(">HHH", b[i + 8:i + 14]) if L >= 6 else ""
            s.append(f"{tag.decode()}@{i} len={L}{extra}")
            i += 8 + (L if tag == b"MThd" else 0)
            if tag == b"MTrk":
                s.append("body:" + b[i:i + 24].hex())
                break
            k += 1
        else:
            s.append(f"raw@{i}:" + b[i:i + 32].hex())
            break
    # tail
    s.append("tail:" + b[-24:].hex())
    return " | ".join(s)


def inspect(buf, depth, mult, label):
    pad = "  " * depth
    if buf[:6] != XZ_MAGIC:
        print(f"{pad}{label} LEAF len={len(buf)} x{mult}: {summarize_midi(buf, len(buf))}")
        return
    mem = cut_members(buf)
    # run-length summary of member keys
    keys = []
    for (a, e, unc) in mem:
        h = hashlib.sha1(buf[a:e]).hexdigest()[:10]
        keys.append((h, e - a, unc, a, e))
    runs = []
    for k in keys:
        if runs and runs[-1][0][:3] == k[:3]:
            runs[-1][1] += 1
        else:
            runs.append([k, 1])
    print(f"{pad}{label} {len(mem)} members, {len(set(k[0] for k in keys))} unique; runs:")
    for (h, ln, unc, a, e), c in runs[:40]:
        print(f"{pad}  [{h} clen={ln} unc={unc}] x{c}")
    if len(runs) > 40:
        print(f"{pad}  ... {len(runs)} runs total")
    uniq = {}
    for (h, ln, unc, a, e) in keys:
        uniq.setdefault(h, [ln, unc, a, e, 0])[4] += 1
    for h, (ln, unc, a, e, c) in uniq.items():
        tag = f"{label}/{h}"
        if h in SEEN:
            print(f"{pad}  {tag}: seen before ({SEEN[h]})")
            continue
        SEEN[h] = tag
        if unc is None:
            print(f"{pad}  {tag}: unparsable head len={ln} first={buf[a:a+16].hex()}")
            continue
        if unc > CAP:
            d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
            pre = d.decompress(buf[a:e], max_length=1 << 20)
            print(f"{pad}  {tag}: unc={unc} > CAP, prefix: " + (
                "xz-nested" if pre[:6] == XZ_MAGIC else summarize_midi(pre, unc)))
            continue
        try:
            payload = lzma.decompress(buf[a:e], format=lzma.FORMAT_XZ)
        except lzma.LZMAError as ex:
            d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
            got = b""
            try:
                got = d.decompress(buf[a:e])
            except lzma.LZMAError:
                pass
            print(f"{pad}  {tag}: DECODE ERROR {ex}; declared unc={unc}")
            continue
        inspect(payload, depth + 2, mult * c, tag)


if __name__ == "__main__":
    if len(sys.argv) > 2: CAP = int(float(sys.argv[2]))
    data = open(sys.argv[1], "rb").read()
    inspect(data, 0, 1, "root")
