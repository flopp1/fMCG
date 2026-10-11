"""Write CJCMCG preprocessed files (.cjcmcg), for tests.

  python -I test/tools/cjcmcg_write.py <in.mid> <out.cjcmcg>

`from_midi` is a line-by-line port of CJCMCG's `ToolProcess` (the C# code the
format was reverse-engineered from): the same MIDI walk -- every 0x9n event
counts as a note-on (velocity ignored), 0x8n as a note-off, running status, the
same meta/sysex handling -- the same synthetic first tempo record (5e9 /
resolution, CJCMCG's internal unit for 120 BPM, written with the C# double ->
byte arithmetic), the same sort order and the same serialisation. `encode`
writes arbitrary content (for hand-made / edge-case files).
"""
import math
import sys


def varint(v):
    if v == 0:
        return b"\x00"
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def _u24_csharp(y):
    """(byte)(y/256/256 % 256), (byte)(y/256 % 256), (byte)(y % 256) with C# double semantics."""
    return bytes([int(math.fmod(y / 256 / 256, 256)), int(math.fmod(y / 256, 256)), int(math.fmod(y, 256))])


def encode(resol, tempos, lyrics, total_notes, ticks):
    """tempos: [(tick, value)] (value: int, or float for CJCMCG's synthetic record);
    lyrics: [(tick, str)]; ticks: [(ons, offs)] indexed by tick."""
    b = bytearray()
    b += resol.to_bytes(4, "big") + len(tempos).to_bytes(4, "big")
    for tick, val in tempos:
        b += tick.to_bytes(4, "big") + _u24_csharp(val)
    b += len(lyrics).to_bytes(4, "big")
    for tick, text in lyrics:
        t = text.encode("utf-8")
        b += tick.to_bytes(4, "big") + len(t).to_bytes(4, "big") + t
    b += varint(total_notes) + len(ticks).to_bytes(4, "big")
    for on, off in ticks:
        b += varint(on) + varint(off)
    return bytes(b)


def from_midi(data):
    """Port of CJCMCG ToolProcess: MIDI bytes -> .cjcmcg bytes."""
    pos = 0

    def rb():
        nonlocal pos
        if pos >= len(data):
            raise ValueError("Unexpected file end")
        v = data[pos]
        pos += 1
        return v

    for _ in range(8):
        rb()
    rb(); rb()
    trkcnt = rb() * 256 + rb()
    resol = rb() * 256 + rb()
    bpm = [(0, -1, 0, 5000000000.0 / resol)]          # (tick, trk, cnt, value)
    lrcs = [(0, -1, -1, "")]
    nts, nto = [], []
    noteall = 0
    for trk in range(trkcnt):
        bpmcnt = lrccnt = 0
        notes = 0
        for _ in range(4):
            rb()
        leng = 0
        for _ in range(4):
            leng = leng * 256 + rb()
        state = {"lstcmd": 256, "leng": leng}

        def getnum():
            ans, ch = 0, 256
            while ch >= 128:
                ch = rb()
                state["leng"] -= 1
                ans = ans * 128 + (ch & 0x7F)
            return ans

        def get():
            if state["lstcmd"] != 256:
                v = state["lstcmd"]
                state["lstcmd"] = 256
                return v
            state["leng"] -= 1
            return rb()

        def extend(tm):
            while len(nts) <= tm:
                nts.append(0)
            while len(nto) <= tm:
                nto.append(0)

        tm = 0
        prvcmd = 256
        while True:
            added = getnum()
            tm += added
            cmd = rb()
            state["leng"] -= 1
            if cmd < 128:
                state["lstcmd"] = cmd
                cmd = prvcmd
            prvcmd = cmd
            cm = cmd & 0xF0
            if cm == 0x90:
                get(); rb(); state["leng"] -= 1
                extend(tm)
                nts[tm] += 1
                notes += 1
            elif cm == 0x80:
                get(); rb(); state["leng"] -= 1
                extend(tm)
                nto[tm] += 1
            elif cm in (0xC0, 0xD0) or cmd == 0xF3:
                get()
            elif cm in (0xE0, 0xB0, 0xA0) or cmd == 0xF2:
                get(); rb(); state["leng"] -= 1
            elif cmd == 0xF0:
                if get() == 0xF7:
                    continue
                while True:
                    state["leng"] -= 1
                    if rb() == 0xF7:
                        break
            elif cmd in (0xF4, 0xF1, 0xF5, 0xF9, 0xFD, 0xF6, 0xF7, 0xF8, 0xFA, 0xFC, 0xFE):
                pass
            elif cmd == 0xFF:
                cmd = get()
                if cmd == 0:
                    rb(); rb(); rb(); state["leng"] -= 3
                elif (1 <= cmd <= 10 and cmd != 5) or cmd == 0x7F:
                    ff = getnum()
                    for _ in range(ff):
                        rb()
                        state["leng"] -= 1
                elif cmd in (0x20, 0x21):
                    rb(); rb(); state["leng"] -= 2
                elif cmd == 0x2F:
                    rb(); state["leng"] -= 1
                    break
                elif cmd == 0x51:
                    bpmcnt += 1
                    rb(); state["leng"] -= 1
                    v = get()
                    v = v * 256 + get()
                    v = v * 256 + get()
                    bpm.append((tm, trk, bpmcnt, v))
                elif cmd == 5:
                    lrccnt += 1
                    ff = getnum()
                    s = bytes(rb() for _ in range(ff))
                    state["leng"] -= ff
                    lrcs.append((tm, trk, lrccnt, s.decode("gbk", errors="replace")))
                elif cmd in (0x54, 0x58):
                    for _ in range(5):
                        rb()
                    state["leng"] -= 5
                elif cmd == 0x59:
                    rb(); rb(); rb(); state["leng"] -= 3
        while state["leng"] > 0:
            rb()
            state["leng"] -= 1
        noteall += notes
    bpm.sort(key=lambda r: (r[0], r[1], r[2]))
    lrcs.sort(key=lambda r: (r[0], r[1], r[2]))
    return encode(resol, [(r[0], r[3]) for r in bpm], [(r[0], r[3]) for r in lrcs], noteall,
                  list(zip(nts, nto)))


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as f:
        out = from_midi(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(out)
