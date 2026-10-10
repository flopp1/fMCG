import struct, os, sys

# Extract the first K MTrk chunks of a big SMF into a standalone valid MIDI.
src, dst, k = sys.argv[1], sys.argv[2], int(sys.argv[3])
size = os.path.getsize(src)
with open(src, 'rb') as f:
    head = f.read(8)
    assert head[:4] == b'MThd'
    hlen = struct.unpack('>I', head[4:8])[0]
    hdr_rest = f.read(hlen)  # the MThd chunk data itself
    fmt, declared, div = struct.unpack('>HHH', hdr_rest[:6])
    body = bytearray()
    out_tracks = 0
    pos = 8 + hlen
    while pos + 8 <= size and out_tracks < k:
        f.seek(pos)
        tag = f.read(4)
        ln = struct.unpack('>I', f.read(4))[0]
        if tag == b'MTrk':
            body += tag + struct.pack('>I', ln) + f.read(ln)
            out_tracks += 1
        pos += 8 + ln
with open(dst, 'wb') as o:
    o.write(b'MThd' + struct.pack('>IHHH', 6, fmt, out_tracks, div))
    o.write(body)
print(f'extracted {out_tracks} tracks, {len(body)/2**20:.1f} MiB -> {dst} (division {div})')
