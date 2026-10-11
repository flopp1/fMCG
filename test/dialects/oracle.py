"""Brute-force reference for layered / tiled / mid-event-cut MIDI constructions.

Ground truth = fully materialise the construction, then parse it as ONE flat byte
stream with the STANDARD scanner's semantics (src/fmcg_engine.cpp: scan_stream +
parse_track_body + sweep_to_frames). Every xz member's payload is recursively
expanded (a payload that begins with the xz magic is itself a sequence of members;
4-byte zero groups between members are xz stream padding), the expansions are
concatenated, and the result is walked as a chunk stream with running status that
carries across any former member boundary.

Only usable on KB-scale instances -- that is the point: the fast engine must agree
with it on thousands of small constructions, which is where every bug in the
layered walk reproduces.

Standard-scan rules mirrored here (keep in sync with the engine):
  * the first chunk must be MThd; 6 header field bytes are always read, extra header
    bytes (len > 6) skipped; later MThd chunks are ordinary skipped chunks;
  * division: 0x4D54 ('MT') => 12-byte MThd, 480 ppqn and the next tag is 'MT'+2
    bytes; high byte < 0xE7 => masked to 15 bits; 0 => 480; >= 0xE700 kept (0xFFFF
    = 65535 ppqn);
  * a partial chunk tag at EOF ends the scan; a partial chunk LENGTH is an error;
  * MTrk bodies are limited to the declared length (clamped at EOF);
  * VLQs read at most 4 bytes; a VLQ cut by the body end still adds its partial value;
  * the delta is added before the event is read, so truncated events still advance;
  * status < 0x80 with running status 0 consumes nothing (the byte is re-read as a
    delta); meta/sysex lengths are skipped (clamped); F1/F3 skip 1, F2 skips 2;
  * F0..F7 and FF cancel running status; real-time bytes F8..FE keep it;
  * channel events read their data bytes whatever their value; note number is
    clamped & 0x7F; note-on vel 0 is an off when vel0_off; offs are refcounted per
    (channel, note) and only fire when the count is > 0;
  * tempo meta (FF 51 03) is applied when its 3 bytes are inside the body;
  * peak NPS: closed 1-second window on exact integer time (tick x us/quarter).
"""
import lzma
from collections import defaultdict, deque

XZ_MAGIC = b"\xfd7zXZ\x00"


def expand(data: bytes) -> bytes:
    """Expand a sequence of xz members recursively; non-xz data passes through."""
    if data[:6] != XZ_MAGIC:
        return data
    out = []
    while data:
        if data[:4] == b"\0\0\0\0":    # stream padding between members
            data = data[4:]
            continue
        if data[:6] != XZ_MAGIC:       # trailing non-xz bytes after the last member
            out.append(data)
            break
        d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
        payload = d.decompress(data)
        out.append(expand(payload))
        data = d.unused_data
    return b"".join(out)


CLOSE_HELD = False   # canonical: poly = ons - offs; held notes are NOT closed unless the GUI option says so


class ScanError(Exception):
    """The standard scan returns failure (probe prints FAIL)."""


def _vlq(buf, i, end):
    """ByteStream::vlq: up to 4 bytes; stops early at `end` with the partial value."""
    v = 0
    for _ in range(4):
        if i >= end:
            break
        b = buf[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return v, i


def parse_track(body, vel0_off=True):
    """Returns (cells{tick:[ons,delta,cc]}, tempos[(tick,us)], end_tick)."""
    cells = defaultdict(lambda: [0, 0, 0])
    tempos = []
    rc = defaultdict(int)
    tick = 0
    running = 0
    i, n = 0, len(body)
    while i < n:
        d, i = _vlq(body, i, n)
        tick += d
        if i >= n:
            break
        b = body[i]
        if b >= 0x80:
            status = b
            i += 1
            # sysex / system common / meta cancel running status; real-time
            # bytes (F8..FE) leave it alone (MIDI spec)
            if b < 0xF0:
                running = b
            elif not (0xF8 <= b <= 0xFE):
                running = 0
        else:
            status = running
        if status == 0xFF:
            if i >= n:
                break
            typ = body[i]
            i += 1
            ln, i = _vlq(body, i, n)
            if typ == 0x51 and ln == 3:
                if i + 3 <= n:
                    us = int.from_bytes(body[i:i + 3], "big")
                    if us > 0:
                        tempos.append((tick, us))
                i = min(n, i + 3)
            else:
                i = min(n, i + ln)
            continue
        if status in (0xF0, 0xF7):
            ln, i = _vlq(body, i, n)
            i = min(n, i + ln)
            continue
        if status >= 0xF0:
            if status in (0xF1, 0xF3):
                i = min(n, i + 1)
            elif status == 0xF2:
                i = min(n, i + 2)
            continue
        if status < 0x80:
            continue               # running status 0: consume nothing
        et = status & 0xF0
        if et in (0xC0, 0xD0):
            if i >= n:
                break
            i += 1
            continue
        if i + 2 > n:
            break                  # truncated final event is not counted
        n1 = body[i] & 0x7F
        n2 = body[i + 1]
        i += 2
        ch = status & 0x0F
        if et == 0x90 and (n2 > 0 or not vel0_off):
            rc[(ch, n1)] += 1
            cells[tick][0] += 1
            cells[tick][1] += 1
        elif et in (0x80, 0x90):
            if rc[(ch, n1)] > 0:
                rc[(ch, n1)] -= 1
                cells[tick][1] -= 1
        elif et == 0xB0:
            cells[tick][2] += 1
    held = sum(rc.values())
    if held and CLOSE_HELD:
        cells[tick][1] -= held
    return dict(cells), tempos, tick


def parse_flat(flat: bytes, vel0_off=True):
    n = len(flat)
    if n < 4 or flat[:4] != b"MThd":
        raise ScanError("not a MIDI file")
    if n < 14:
        raise ScanError("truncated MIDI header")
    hlen = int.from_bytes(flat[4:8], "big")
    d2, d1 = flat[12], flat[13]
    division = (d2 << 8) | d1
    pos = 14 + max(0, hlen - 6)
    mt_split = False
    if division == 0x4D54:
        division = 480
        mt_split = True
    elif d2 < 0xE7:
        division &= 0x7FFF
    if division == 0:
        division = 480
    all_cells = defaultdict(lambda: [0, 0, 0])
    tempos = []
    max_tick = 0
    ntracks = 0
    while True:
        if mt_split:
            mt_split = False
            if pos + 2 > n:
                break
            tag = b"MT" + flat[pos:pos + 2]
            pos += 2
        else:
            if pos + 4 > n:
                break
            tag = flat[pos:pos + 4]
            pos += 4
        if pos + 4 > n:
            raise ScanError("truncated chunk header")
        L = int.from_bytes(flat[pos:pos + 4], "big")
        pos += 4
        body = flat[pos:pos + L]
        pos += L
        if tag != b"MTrk":
            continue
        cells, tp, end = parse_track(body, vel0_off)
        ntracks += 1
        for t, (o, d, c) in cells.items():
            all_cells[t][0] += o
            all_cells[t][1] += d
            all_cells[t][2] += c
        tempos += tp
        max_tick = max(max_tick, end)
    return dict(all_cells), tempos, max_tick, division


def stats(flat: bytes):
    cells, tempos, max_tick, div = parse_flat(flat)
    byt = {}
    for t, us in tempos:       # stable: later wins on equal tick
        byt[t] = us
    tm = [(0, 500000)]
    for t in sorted(byt):
        if t == 0:
            tm[0] = (0, byt[t])
        else:
            tm.append((t, byt[t]))
    anchors = [0]              # exact integer time in units of 1/div microseconds
    for i in range(1, len(tm)):
        anchors.append(anchors[-1] + (tm[i][0] - tm[i - 1][0]) * tm[i - 1][1])

    def itime(t):
        lo = 0
        for idx, (tt, _) in enumerate(tm):
            if tt <= t:
                lo = idx
        return anchors[lo] + (t - tm[lo][0]) * tm[lo][1]

    one_sec = 1000000 * div
    notes = sum(v[0] for v in cells.values())
    poly = peak_poly = 0
    win = deque()
    wsum = peak_nps = 0
    for t in sorted(cells):
        o, d, _ = cells[t]
        if o == 0 and d == 0:
            continue
        if o:
            T = itime(t)
            win.append((T, o))
            wsum += o
            while win and win[0][0] + one_sec < T:
                wsum -= win.popleft()[1]
            peak_nps = max(peak_nps, wsum)
        poly += d
        peak_poly = max(peak_poly, poly)
    return dict(notes=notes, ticks=max_tick, ppqn=div, peak_poly=peak_poly, peak_nps=peak_nps)


def stats_of_file(blob: bytes):
    return stats(expand(blob))
