#!/usr/bin/env python3
"""Shrink a layered xz MIDI construction so the STANDARD scan can verify it.

Rebuilds the member grammar with every run of identical members cut to at most K
copies and at most MAX_RUNS runs per payload. Leaf members (raw MIDI tiles) are
reused byte-for-byte (never decompressed), so the shrunk file exercises exactly
the real tiles, cut phases and nesting -- only the repetition counts change. The
expanded size drops from TB to (header + K x tile) so `probe <f> 60 0` (standard
scan) can produce the reference for `probe <f> 60 1` (layered walk).

Big internal payloads are streamed (bounded memory); the stream is abandoned as
soon as MAX_RUNS runs are complete.

usage: shrink_layered.py <in> <out> [K=2] [MAX_RUNS=40]
"""
import hashlib
import lzma
import struct
import sys
import zlib

PAYLOAD_CAP = 1 << 30   # stop streaming a payload after this many decoded bytes

XZ_MAGIC = b"\xfd7zXZ\x00"


def vli(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, i


def stream_exact(b, fe):
    """True if b[0:fe] is exactly one xz stream (header at 0, footer ending at fe)."""
    if fe < 32 or b[:6] != XZ_MAGIC:
        return False
    f = b[fe - 12:fe]
    if f[10:12] != b"YZ" or f[8:10] != b[6:8]:
        return False
    if zlib.crc32(f[4:10]) != struct.unpack("<I", f[0:4])[0]:
        return False
    isz = (struct.unpack("<I", f[4:8])[0] + 1) * 4
    ix = fe - 12 - isz
    if ix < 12 or b[ix] != 0:
        return False
    if zlib.crc32(b[ix:ix + isz - 4]) != struct.unpack("<I", b[ix + isz - 4:ix + isz])[0]:
        return False
    try:
        n, i = vli(b, ix + 1)
        comp = 0
        for _ in range(n):
            u, i = vli(b, i)
            _, i = vli(b, i)
            comp += (u + 3) & ~3
    except IndexError:
        return False
    return 12 + comp == ix


def cut_backward(b):
    out, end = [], len(b)
    while end > 0:
        while end >= 4 and b[end - 4:end] == b"\0\0\0\0":
            end -= 4
        if end == 0:
            break
        f = b[end - 12:end]
        isz = (struct.unpack("<I", f[4:8])[0] + 1) * 4
        ix = end - 12 - isz
        n, i = vli(b, ix + 1)
        comp = 0
        for _ in range(n):
            u, i = vli(b, i)
            _, i = vli(b, i)
            comp += (u + 3) & ~3
        start = ix - comp - 12
        assert b[start:start + 6] == XZ_MAGIC, "not a clean xz stream chain"
        out.append(b[start:end])
        end = start
    out.reverse()
    return out


def runs_of(members):
    runs = []
    for m in members:
        if runs and runs[-1][0] == m:
            runs[-1][1] += 1
        else:
            runs.append([m, 1])
    return runs


def stream_runs(member, K, max_runs):
    """Decode `member` streaming; cut its payload into members; return runs (bytes, count)
    truncated to max_runs runs with counts capped at K. Returns None for a leaf."""
    d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    head = d.decompress(member, max_length=6)
    if head[:6] != XZ_MAGIC:
        return None
    d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    buf = bytearray()
    runs = []
    scan = 30
    pos = 0
    CH = 1 << 20

    decoded = [0]

    def done():
        if decoded[0] > PAYLOAD_CAP and runs and runs[-1][1] >= K:
            return True        # long tail of repeats: the prefix is a valid construction too
        return len(runs) > max_runs or (len(runs) == max_runs and runs[-1][1] >= K)

    while not d.eof and not done():
        if d.needs_input:
            if pos >= len(member):
                break
            chunk = d.decompress(member[pos:pos + CH], max_length=64 << 20)
            pos += CH
        else:
            chunk = d.decompress(b"", max_length=64 << 20)
        buf += chunk
        decoded[0] += len(chunk)
        while True:
            while buf[:4] == b"\0\0\0\0":
                del buf[:4]
            if len(buf) >= 6 and buf[:6] != XZ_MAGIC:
                if d.eof:
                    runs.append([bytes(buf), 1])     # raw tail
                    buf.clear()
                break
            # fast path: same length as the previous member
            if runs and len(buf) >= len(runs[-1][0]) and buf[:len(runs[-1][0])] == runs[-1][0]:
                del buf[:len(runs[-1][0])]
                runs[-1][1] += 1
                scan = 30
                if done():
                    break
                continue
            p = buf.find(b"YZ", scan)
            found = False
            while p >= 0:
                if stream_exact(buf, p + 2):
                    m = bytes(buf[:p + 2])
                    del buf[:p + 2]
                    if runs and runs[-1][0] == m:
                        runs[-1][1] += 1
                    else:
                        runs.append([m, 1])
                    found = True
                    scan = 30
                    break
                p = buf.find(b"YZ", p + 1)
            if not found:
                scan = max(30, len(buf) - 1)
                break
            if done():
                break
    if d.eof and buf:
        runs.append([bytes(buf), 1])
    return [(m, min(c, K)) for m, c in runs[:max_runs]]


CACHE = {}


def shrink_member(member, K, max_runs):
    key = hashlib.sha1(member).digest()
    if key in CACHE:
        return CACHE[key]
    if member[:6] != XZ_MAGIC:
        CACHE[key] = member
        return member
    runs = stream_runs(member, K, max_runs)
    if runs is None:
        out = member                       # leaf: reuse the real tile untouched
    else:
        payload = b"".join(shrink_member(m, K, max_runs) * c for m, c in runs)
        out = lzma.compress(payload, format=lzma.FORMAT_XZ, preset=1)
    CACHE[key] = out
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    K = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    max_runs = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    data = open(src, "rb").read()
    runs = [(m, min(c, K)) for m, c in runs_of(cut_backward(data))[:max_runs]]
    out = b"".join(shrink_member(m, K, max_runs) * c for m, c in runs)
    open(dst, "wb").write(out)
    print(f"{dst}: {len(out)} bytes")


if __name__ == "__main__":
    main()
