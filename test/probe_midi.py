import struct, os, sys

p = r"D:\Night of Nights Qualito merge FINAL.mid"
size = os.path.getsize(p)
print(f'file size: {size} bytes ({size/2**30:.2f} GiB)')
f = open(p, 'rb')
head = f.read(64)
print('first 16 bytes:', head[:16].hex(), repr(head[:12]))

hlen = struct.unpack('>I', head[4:8])[0]
fmt, ntrks, div = struct.unpack('>HHH', head[8:14])
print(f'header_len={hlen} format={fmt} ntrks_declared={ntrks} division={div} (0x{div:04X})')

pos = 8 + hlen
n_mtrk = 0
n_other = 0
other_tags = {}
mtrk_bytes = 0
overlong = 0
biggest = 0
while pos + 8 <= size:
    f.seek(pos)
    tag = f.read(4)
    ln_b = f.read(4)
    if len(tag) < 4 or len(ln_b) < 4:
        print(f'truncated chunk header at {pos}')
        break
    ln = struct.unpack('>I', ln_b)[0]
    if tag == b'MTrk':
        n_mtrk += 1
        mtrk_bytes += ln
        biggest = max(biggest, ln)
        if pos + 8 + ln > size:
            overlong += 1
            print(f'  OVERLONG MTrk #{n_mtrk} at {pos}: declares {ln}')
            break
    else:
        n_other += 1
        other_tags[tag] = other_tags.get(tag, 0) + 1
    pos += 8 + ln

print(f'MTrk chunks walked: {n_mtrk}, bytes: {mtrk_bytes} ({mtrk_bytes/2**30:.2f} GiB), biggest track: {biggest/2**20:.1f} MiB')
print(f'non-MTrk chunks: {n_other} {other_tags}')
print(f'overlong: {overlong}, walk ended at {pos} (file {size}) -> diff {size - pos}')

size = os.path.getsize(p)
print(f'file size: {size} bytes ({size/2**30:.2f} GiB)')
f = open(p, 'rb')
head = f.read(64)
print('first 16 bytes:', head[:16].hex(), repr(head[:12]))

# RIFF/RMID wrapper?
if head[:4] == b'RIFF':
    print('RIFF wrapper detected, type:', head[8:12])
    # find 'MThd' in first 1KB
    idx = head.find(b'MThd')
    print('MThd at offset:', idx)

# Standard SMF header
hlen = struct.unpack('>I', head[4:8])[0]
fmt, ntrks, div = struct.unpack('>HHH', head[8:14])
print(f'header_len={hlen} format={fmt} ntrks_declared={ntrks} division={div} (0x{div:04X})')
if div & 0x8000:
    smpte = -((~div & 0xFFFF) + 1)
    print(f'  SMPTE timing: {smpte}')

pos = 8 + hlen
n_mtrk = 0
n_other = 0
other_tags = {}
mtrk_bytes = 0
overlong = 0
while pos + 8 <= size:
    f.seek(pos)
    tag = f.read(4)
    ln_b = f.read(4)
    if len(tag) < 4 or len(ln_b) < 4:
        print(f'truncated chunk header at {pos}')
        break
    ln = struct.unpack('>I', ln_b)[0]
    if tag == b'MTrk':
        n_mtrk += 1
        mtrk_bytes += ln
        if pos + 8 + ln > size:
            overlong += 1
            if overlong <= 5:
                print(f'  OVERLONG MTrk #{n_mtrk} at {pos}: declares {ln} ({ln/2**30:.2f} GiB), only {size-pos-8} remain')
            break
    else:
        n_other += 1
        other_tags[tag] = other_tags.get(tag, 0) + 1
    pos += 8 + ln

print(f'MTrk chunks walked: {n_mtrk}, bytes: {mtrk_bytes} ({mtrk_bytes/2**30:.2f} GiB)')
print(f'non-MTrk chunks: {n_other} {other_tags}')
print(f'overlong: {overlong}, walk ended at {pos} (file {size})')
print(f'sum of MTrk+headers vs size: {pos} vs {size} -> diff {size - pos}')
