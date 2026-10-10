"""Streaming forward cutter for a layer whose payload is too big to buffer.
Decodes the (single) member given, cuts its payload into xz members at validated
footers, run-length-prints member keys, saves unique members (<= KEEP bytes) to
outdir as <idx>_<hash>.xz for further inspection with xzinspect.py.
usage: xzstream.py <file> <path-of-member-indices e.g. 0,0> <outdir>"""
import hashlib, lzma, os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xzinspect import cut_members, XZ_MAGIC
KEEP = 512 << 20

def footer_ok(b, p):   # p = index of 'YZ' (footer end = p+2)
    if p < 10: return False
    f = b[p-10:p+2]
    if f[8] != 0 or f[9] not in (0, 1, 4, 0x0A): return False
    return zlib.crc32(f[4:10]) == struct.unpack("<I", f[0:4])[0]

def stream_payload(data):
    d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    pos = 0
    while not d.eof:
        chunk = d.decompress(data[pos:pos + (1 << 20)] if d.needs_input else b"", max_length=64 << 20)
        if d.needs_input: pos += 1 << 20
        if chunk: yield chunk
        if pos > len(data) and d.needs_input: break

def main():
    data = open(sys.argv[1], "rb").read()
    path = [int(x) for x in sys.argv[2].split(",") if x != ""]
    outdir = sys.argv[3]; os.makedirs(outdir, exist_ok=True)
    for idx in path[:-1] if path else []:
        m = cut_members(data); a, e, _ = m[idx]; data = lzma.decompress(data[a:e])
    m = cut_members(data); a, e, unc = m[path[-1]]
    print("streaming member", path, "clen", e - a, "unc", unc, flush=True)
    buf = bytearray(); total = 0; runs = []; uniq = {}; nmem = 0
    def emit(mb):
        nonlocal nmem
        h = hashlib.sha1(mb).hexdigest()[:10]; k = (h, len(mb))
        if runs and runs[-1][0] == k: runs[-1][1] += 1
        else: runs.append([k, 1])
        if k not in uniq:
            uniq[k] = nmem
            if len(mb) <= KEEP: open(os.path.join(outdir, f"{nmem}_{h}.xz"), "wb").write(mb)
        nmem += 1
    for chunk in stream_payload(data[a:e]):
        total += len(chunk); buf += chunk
        start = 0
        while True:
            p = buf.find(b"YZ", start + 12)
            if p < 0: break
            if footer_ok(buf, p) and buf[0:6] == XZ_MAGIC:
                # validate the whole stream backward from footer end
                mem = bytes(buf[:p + 2]); cm = cut_members(mem)
                if cm and cm[0][2] is not None and cm[0][0] == 0:
                    for (s, t, u) in cm: emit(mem[s:t])
                    end = p + 2
                    while buf[end:end+4] == b"\0\0\0\0": end += 4
                    del buf[:end]; start = 0; continue
            start = p - 10
        if buf[:6] != XZ_MAGIC and len(buf) > 0 and nmem == 0:
            print("payload is not xz members; prefix", bytes(buf[:32]).hex()); return
    if buf: print("trailing", len(buf), "bytes", bytes(buf[:32]).hex())
    print("payload total", total, "members", nmem, "unique", len(uniq))
    for (k, c) in runs[:200]: print(f"  [{k[0]} len={k[1]}] x{c}")
    if len(runs) > 200: print("  ...", len(runs), "runs")
main()
