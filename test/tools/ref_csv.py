import struct, sys

# Independent SMF reference parser -> same CSV as probe_big's --csv.
# Deliberately a separate implementation from fMCG.cpp; the per-track tempo
# cursor is reset per track, per the SMF spec's per-track timelines.

def main():
    src = sys.argv[1]
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    csv_path = sys.argv[3] if len(sys.argv) > 3 else 'ref.csv'
    data = open(src, 'rb').read()
    assert data[:4] == b'MThd', 'not an SMF'
    hlen = struct.unpack('>I', data[4:8])[0]
    division = struct.unpack('>H', data[12:14])[0] & 0x7FFF or 480

    tracks = []
    pos = 8 + hlen
    size = len(data)
    while pos + 8 <= size:
        ln = struct.unpack('>I', data[pos+4:pos+8])[0]
        if data[pos:pos+4] == b'MTrk':
            tracks.append((pos + 8, min(ln, size - pos - 8)))
        pos += 8 + ln

    # Pass 1: tempo map + max tick
    tempo = [(0, 0.0, 500000, 120.0)]  # (tick, time_sec, us_per_quarter, bpm)
    max_tick = 0
    for off, ln in tracks:
        end = off + ln
        p = off
        tick = 0
        rs = 0
        while p < end:
            v = 0; n = 0
            while p < end and n < 4:
                b = data[p]; p += 1
                v = (v << 7) | (b & 0x7F); n += 1
                if not (b & 0x80): break
            tick += v
            if p >= end: break
            st = data[p]; p += 1
            if st < 0x80:
                p -= 1; st = rs
            elif st < 0xF0:
                rs = st
            else:
                rs = 0
            if st == 0xFF:
                typ = data[p]; p += 1
                l2 = 0
                while True:
                    b = data[p]; p += 1
                    l2 = (l2 << 7) | (b & 0x7F)
                    if not (b & 0x80): break
                if typ == 0x51 and l2 == 3 and p + 3 <= end:
                    us = (data[p] << 16) | (data[p+1] << 8) | data[p+2]
                    if us > 0:
                        tempo.append((tick, 0.0, us, 60000000.0 / us))
                p += l2
            elif st in (0xF0, 0xF7):
                l2 = 0
                while True:
                    b = data[p]; p += 1
                    l2 = (l2 << 7) | (b & 0x7F)
                    if not (b & 0x80): break
                p += l2
            elif 0xF1 <= st <= 0xF6:
                p += 1 if st in (0xF1, 0xF3) else (2 if st == 0xF2 else 0)
            elif 0x80 <= st < 0xF0:
                p += 1 if (st & 0xF0) in (0xC0, 0xD0) else 2
        if tick > max_tick: max_tick = tick

    tempo.sort(key=lambda t: t[0])
    for i in range(1, len(tempo)):
        dt = tempo[i][0] - tempo[i-1][0]
        tempo[i] = (tempo[i][0], tempo[i-1][1] + dt * tempo[i-1][2] / (division * 1e6),
                    tempo[i][2], tempo[i][3])
    ticks = [t[0] for t in tempo]

    def tick_to_sec(tick):
        lo, hi = 0, len(tempo)
        while lo < hi:
            mid = (lo + hi) // 2
            if tempo[mid][0] <= tick: lo = mid + 1
            else: hi = mid
        tc = tempo[lo - 1]
        return tc[1] + (tick - tc[0]) * tc[2] / (division * 1e6)

    max_time = tick_to_sec(max_tick)
    total_frames = int(max_time * fps + 0.9999999999) + 1
    # Pass 2: bucket note-ons / poly deltas per frame
    ons = [0] * total_frames
    deltas = [0] * total_frames
    total = 0
    for off, ln in tracks:
        end = off + ln
        p = off
        tick = 0
        rs = 0
        cursor = 0  # reset per track (spec: per-track timelines)
        rc = [[0] * 128 for _ in range(16)]
        while p < end:
            v = 0; n = 0
            while p < end and n < 4:
                b = data[p]; p += 1
                v = (v << 7) | (b & 0x7F); n += 1
                if not (b & 0x80): break
            tick += v
            if p >= end: break
            st = data[p]; p += 1
            if st < 0x80:
                p -= 1; st = rs
            elif st < 0xF0:
                rs = st
            else:
                rs = 0
            if st == 0xFF:
                p += 1
                l2 = 0
                while True:
                    b = data[p]; p += 1
                    l2 = (l2 << 7) | (b & 0x7F)
                    if not (b & 0x80): break
                p += l2
            elif st in (0xF0, 0xF7):
                l2 = 0
                while True:
                    b = data[p]; p += 1
                    l2 = (l2 << 7) | (b & 0x7F)
                    if not (b & 0x80): break
                p += l2
            elif 0xF1 <= st <= 0xF6:
                p += 1 if st in (0xF1, 0xF3) else (2 if st == 0xF2 else 0)
            elif 0x80 <= st < 0xF0:
                typ = st & 0xF0
                note = data[p]; p += 1
                vel = 0
                if typ not in (0xC0, 0xD0):
                    vel = data[p]; p += 1
                while cursor + 1 < len(tempo) and ticks[cursor + 1] <= tick:
                    cursor += 1
                tc = tempo[cursor]
                sec = tc[1] + (tick - tc[0]) * tc[2] / (division * 1e6)
                k = int(sec * fps)
                ch = st & 0x0F
                if typ == 0x90 and vel > 0:  # note-on (vel0_as_note_off = true)
                    total += 1
                    rc[ch][note] += 1
                    if k < total_frames:
                        ons[k] += 1
                        deltas[k] += 1
                elif typ == 0x90 or typ == 0x80:  # note-off (incl. vel-0 note-on)
                    if rc[ch][note] > 0:
                        rc[ch][note] -= 1
                        if k < total_frames:
                            deltas[k] -= 1
        # close remaining actives at the track's final tick
        while cursor + 1 < len(tempo) and ticks[cursor + 1] <= tick:
            cursor += 1
        tc = tempo[cursor]
        sec = tc[1] + (tick - tc[0]) * tc[2] / (division * 1e6)
        k = int(sec * fps)
        if k < total_frames:
            for ch in range(16):
                deltas[k] -= sum(rc[ch])

    # Sweep (same definitions as the video: NPS = notes in the last fps frames)
    out = ['frame,timestamp_sec,cumulative_notes,current_nps,peak_nps,polyphony,peak_polyphony,bpm']
    cum = 0
    poly = 0
    peak_poly = 0
    peak_nps = 0.0
    window = int(fps + 0.5)
    cum_hist = [0] * total_frames
    times = [t[1] for t in tempo]
    bpms = [t[3] for t in tempo]
    ti = 0
    for k in range(total_frames):
        cum += ons[k]
        cum_hist[k] = cum
        poly += deltas[k]
        peak_poly = max(peak_poly, poly)
        t = k / fps
        while ti + 1 < len(tempo) and times[ti + 1] <= t: ti += 1
        nw = cum - (cum_hist[k - window] if k >= window else 0)
        peak_nps = max(peak_nps, float(nw))
        out.append(f'{k},{t:.3f},{cum},{nw},{int(peak_nps)},{max(0, poly)},{peak_poly},{bpms[ti]:.2f}')
    open(csv_path, 'w').write('\n'.join(out) + '\n')
    print(f'reference: frames={total_frames} total_notes={total} -> {csv_path}')

main()
