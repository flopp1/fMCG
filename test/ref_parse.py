import struct, sys

data = open(sys.argv[1] if len(sys.argv) > 1 else 'test.mid', 'rb').read()
print('len', len(data))
pos = 0
assert data[0:4] == b'MThd'
hlen = struct.unpack('>I', data[4:8])[0]
fmt, ntrks, div = struct.unpack('>HHH', data[8:14])
print('fmt', fmt, 'ntrks', ntrks, 'div', div, 'hlen', hlen)
pos = 8 + hlen
while pos + 8 <= len(data):
    tag = data[pos:pos+4]
    ln = struct.unpack('>I', data[pos+4:pos+8])[0]
    print('chunk', tag, 'len', ln, 'at', pos+8)
    if tag == b'MTrk':
        p = pos + 8
        end = p + ln
        tick = 0
        rs = 0
        while p < end:
            v = 0; n = 0
            while True:
                b = data[p]; p += 1
                v = (v << 7) | (b & 0x7F); n += 1
                if not (b & 0x80) or n == 4: break
            tick += v
            st = data[p]; p += 1
            if st < 0x80:
                p -= 1; st = rs
            elif st < 0xF0:
                rs = st
            else:
                rs = 0
            if st == 0xFF:
                typ = data[p]; p += 1
                ln2 = 0; nn = 0
                while True:
                    b = data[p]; p += 1
                    ln2 = (ln2 << 7) | (b & 0x7F); nn += 1
                    if not (b & 0x80) or nn == 4: break
                payload = data[p:p+ln2]; p += ln2
                print(f'  tick {tick} meta {typ:02X} len {ln2} {payload.hex()}')
            elif st in (0xF0, 0xF7):
                ln2 = 0; nn = 0
                while True:
                    b = data[p]; p += 1
                    ln2 = (ln2 << 7) | (b & 0x7F); nn += 1
                    if not (b & 0x80) or nn == 4: break
                p += ln2
                print(f'  tick {tick} sysex {st:02X} len {ln2}')
            elif 0xF1 <= st <= 0xF6:
                skip = 1 if st in (0xF1, 0xF3) else (2 if st == 0xF2 else 0)
                p += skip
                print(f'  tick {tick} syscommon {st:02X} skip {skip}')
            else:
                t = st & 0xF0
                if t in (0xC0, 0xD0):
                    p += 1
                    print(f'  tick {tick} ch {st:02X} d1 {data[p-1]:02X}')
                else:
                    print(f'  tick {tick} ch {st:02X} d1 {data[p]:02X} d2 {data[p+1]:02X}')
                    p += 2
        print('  consumed', p - (pos+8), 'declared', ln, 'endtick', tick)
    pos += 8 + ln
