"""Miniature generators for every construction dialect seen in the corpus.

Each generator returns (file_bytes, description). They are tiny (KB) but keep the
structural property that matters: members are cut at arbitrary byte offsets
(mid-event, mid-VLQ), tiled through several nested layers, and carry unusual
headers. See DIALECTS.md for the mapping to real corpus files.
"""
import lzma
import random

CHECKS = [lzma.CHECK_NONE, lzma.CHECK_CRC32, lzma.CHECK_CRC64, lzma.CHECK_SHA256]


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def xz(data, check=lzma.CHECK_CRC64):
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=check, preset=0)


def mthd(div=480, ntrks=1, fmt=1, hlen=6):
    return b"MThd" + hlen.to_bytes(4, "big") + fmt.to_bytes(2, "big") + ntrks.to_bytes(2, "big") + div.to_bytes(2, "big")


def mtrk(body, decl=None):
    return b"MTrk" + (len(body) if decl is None else decl).to_bytes(4, "big") + body


EOT = b"\x00\xff\x2f\x00"


def melody_body(rng, nev=40, with_tempo=True, eot=True):
    b = bytearray()
    if with_tempo:
        b += b"\x00\xff\x51\x03" + rng.choice([400000, 500000, 600000]).to_bytes(3, "big")
    held = []
    for _ in range(nev):
        b += vlq(rng.choice([0, 0, 1, 3, 10, 130]))
        ch = rng.randrange(2)
        r = rng.random()
        if r < 0.55 or not held:
            note = rng.randrange(128)
            b += bytes([0x90 | ch, note, rng.randrange(1, 128)])
            held.append((ch, note))
        elif r < 0.8:
            ch, note = held.pop(rng.randrange(len(held)))
            b += bytes([0x80 | ch, note, 64])
        elif r < 0.9:
            ch, note = held.pop(rng.randrange(len(held)))
            b += bytes([0x90 | ch, note, 0])        # vel-0 note-off
        else:
            b += bytes([0xB0, 7, rng.randrange(128)])
    if eot:
        b += EOT
    return bytes(b)


def spam_body(rng, nev=30, status_each=False, eot=True):
    """Zero-delta note-on spam (black-MIDI style); optionally running status."""
    b = bytearray()
    for i in range(nev):
        b += b"\x00"
        if status_each or i == 0:
            b += b"\x90"
        b += bytes([rng.randrange(128), rng.randrange(1, 128)])
    if eot:
        b += EOT
    return bytes(b)


def tile(unit, n):
    return unit * n


# ---------------------------------------------------------------- dialects

def d_plain(rng):
    return mthd(480, 2) + mtrk(melody_body(rng)) + mtrk(melody_body(rng, 25)), "plain 2-track"


def d_xz1(rng):
    f, _ = d_plain(rng)
    return xz(f, rng.choice(CHECKS)), "single xz layer"


def d_tiled_members(rng):
    """Header member + one spam-track member tiled N times, 1-3 nested layers (ievan/paprika)."""
    div = rng.choice([480, 960, 7680])
    header = mthd(div, 3) + mtrk(melody_body(rng, 20))
    chunk = mtrk(spam_body(rng, rng.randrange(5, 40), status_each=rng.random() < 0.5))
    unit = xz(chunk, rng.choice(CHECKS))
    cur = tile(unit, rng.randrange(2, 7))
    for _ in range(rng.randrange(0, 3)):
        cur = tile(xz(cur, rng.choice(CHECKS)), rng.randrange(2, 5))
    return xz(header, rng.choice(CHECKS)) + cur, "header member + tiled track members, nested"


def d_tiled_whole(rng):
    """Whole-file repeat through layers: header repeated too (multi-MThd flat stream)."""
    f = mthd(480, 1) + mtrk(spam_body(rng, 20))
    cur = tile(xz(f), rng.randrange(2, 5))
    for _ in range(rng.randrange(0, 2)):
        cur = tile(xz(cur), rng.randrange(2, 4))
    return cur, "whole-file tiled (repeated MThd)"


def _dump_pieces(rng, min_piece):
    note, vel = rng.randrange(1, 128), rng.randrange(1, 128)   # nonzero: phase is content-derivable
    tilebytes = b"\x00" + bytes([note, vel])
    m = rng.randrange(40, 200)
    stream = b"\x00\x90" + bytes([note, vel]) + tilebytes * m + EOT
    stub = mthd(480, 1) + b"MTrk" + (1 << 28).to_bytes(4, "big")
    cuts, pos = [], 0
    while True:
        pos += rng.randrange(min_piece, 3 * min_piece + 40)
        if pos > len(stream) - len(EOT) - max(min_piece, 8):
            break    # keep the End-of-Track meta whole inside the last piece
        cuts.append(pos)
    pieces, last = [], 0
    for c in cuts + [len(stream)]:
        pieces.append(stream[last:c])
        last = c
    return stub, pieces


def d_dump_midevent(rng):
    """Headerless running-status dump under a fiction-length MTrk stub, cut mid-event (Just-1-note)."""
    stub, pieces = _dump_pieces(rng, 8)
    members = [xz(stub, rng.choice(CHECKS))] + [xz(p, rng.choice(CHECKS)) for p in pieces]
    return b"".join(members), "headerless dump, mid-event cuts (pieces >= 8 bytes)"


def d_dump_tinycuts(rng):
    """Same dump but pieces may be 1-7 bytes: phase is NOT derivable from content.
    Known limitation (needs the ordered framing-state transducer); must at least be LOUD."""
    stub, pieces = _dump_pieces(rng, 1)
    members = [xz(stub, rng.choice(CHECKS))] + [xz(p, rng.choice(CHECKS)) for p in pieces]
    return b"".join(members), "headerless dump, arbitrary tiny cuts"


def d_dump_tiled_slices(rng):
    """Fixed-size slices of an endless 3-byte tile -> 3 phase-shifted uniques, each repeated."""
    note, vel = rng.randrange(1, 128), rng.randrange(1, 128)
    sl = rng.choice([61, 62, 64, 100])               # not a multiple of 3 -> phase drifts
    nsl = rng.randrange(6, 30)
    stream = b"\x00\x90" + bytes([note, vel]) + (b"\x00" + bytes([note, vel])) * ((sl * nsl) // 3 + 4)
    stub = mthd(480, 1) + b"MTrk" + (1 << 28).to_bytes(4, "big")
    members = [xz(stub)]
    pos = 0
    while len(stream) - pos >= 2 * sl:      # last piece is >= sl bytes: phase stays derivable
        members.append(xz(stream[pos:pos + sl]))
        pos += sl
    members.append(xz(stream[pos:] + EOT))
    return b"".join(members), "endless-tile slices (phase-shifted uniques)"


def d_mthd12(rng):
    hdr = b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + (0xFFFF).to_bytes(2, "big")  # division missing
    return xz(hdr + mtrk(spam_body(rng, 25))), "12-byte MThd (no division)"


def d_mixed_checks(rng):
    parts = [mthd(480, 3), mtrk(melody_body(rng, 15)), mtrk(spam_body(rng, 12)), mtrk(spam_body(rng, 12))]
    return b"".join(xz(p, rng.choice(CHECKS)) for p in parts), "mixed xz check types across streams"


def d_empty_fragments(rng):
    parts = [mthd(480, 2), mtrk(melody_body(rng, 15)), mtrk(spam_body(rng, 12))]
    out = []
    for p in parts:
        for _ in range(rng.randrange(0, 3)):
            out.append(xz(b"", rng.choice(CHECKS)))      # header-only fragment
        out.append(xz(p, rng.choice(CHECKS)))
    return b"".join(out), "empty xz fragments between real members"


def d_grid(rng):
    step = rng.choice([1, 1, 2])
    n = rng.randrange(50, 300)
    seed = b"\x00\x90\x60\x7f"
    body = seed + (bytes([step]) + b"\x90\x60\xff") * n + EOT
    return xz(mthd(rng.choice([480, 0xFFFF]), 1) + mtrk(body)), "uniform grid (NoiseMidi style)"


def d_tempo_in_tiles(rng):
    chunk = mtrk(b"\x00\xff\x51\x03\x07\xa1\x20" + spam_body(rng, 10, eot=False)
                 + b"\x40\x90\x10\x20" + EOT)
    cur = tile(xz(chunk), rng.randrange(2, 5))
    return xz(mthd(480, 1)) + cur, "tempo meta inside tiled tracks"


def d_held_notes(rng):
    """Tracks end with notes still held (no note-off): refcount closure at track end."""
    chunk = mtrk(b"\x00\x90\x30\x40\x05\x90\x32\x40" + b"\x05\xff\x2f\x00")
    return xz(mthd(480, 1)) + tile(xz(chunk), rng.randrange(2, 8)), "tiled tracks with held notes"


def _tempo_track(rng):
    """Track with tempo changes (some at ticks shared with other tracks, different values) + notes."""
    ev = []
    for _ in range(rng.randrange(1, 4)):
        ev.append((rng.choice([0, 0, 4, 8, 20]), rng.choice([250000, 400000, 500000, 800000, 1200000])))
    ev.sort(key=lambda e: e[0])
    b, t = bytearray(), 0
    for tick, us in ev:
        b += vlq(tick - t) + b"\xff\x51\x03" + us.to_bytes(3, "big")
        t = tick
        for _ in range(rng.randrange(0, 3)):
            b += vlq(rng.choice([0, 1, 5])) + bytes([0x90, rng.randrange(128), rng.randrange(1, 128)])
            t += 0
    # a few notes at distinct later ticks so NPS depends on the final tempo map
    for _ in range(rng.randrange(2, 6)):
        b += vlq(rng.choice([3, 30, 200])) + bytes([0x90, rng.randrange(128), rng.randrange(1, 128)])
    return mtrk(bytes(b) + EOT)


def d_tempo_diff(rng):
    """Tiles with DIFFERING tempo patterns at shared ticks, interleaved and nested:
    the result must equal the flat scan (last tempo per tick in flat order wins)."""
    kinds = [xz(_tempo_track(rng), rng.choice(CHECKS)) for _ in range(rng.randrange(2, 4))]
    seq = b""
    for _ in range(rng.randrange(3, 7)):
        seq += tile(rng.choice(kinds), rng.randrange(1, 4))
    cur = tile(xz(seq), rng.randrange(1, 4)) if rng.random() < 0.5 else seq
    header = xz(mthd(rng.choice([480, 960]), 1))
    return header + cur, "tiled tracks with differing, interleaved tempo patterns"


GENS = {k[2:]: v for k, v in globals().items() if k.startswith("d_")}
