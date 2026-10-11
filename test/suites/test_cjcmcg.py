"""CJCMCG preprocessed files (.cjcmcg) read by the engine (src/fmcg_cjcmcg.cpp).

The main property: a MIDI and the .cjcmcg CJCMCG itself would write for it
(test/tools/cjcmcg_write.py ports its ToolProcess) give identical fMCG stats --
notes, ticks, ppqn, peak polyphony, peak NPS (exact inclusive window) and the
frame count. The random MIDIs avoid the two things a .cjcmcg cannot represent:
velocity-0 note-ons (CJCMCG counts them as notes) and events after the last
note (the file has no End-of-Track ticks). Edge cases use hand-made files.
"""
import lzma
import os
import random
import sys
import tempfile
import unittest

import fmcgtest as T

sys.path.insert(0, os.path.join(T.TEST_DIR, "tools"))
import cjcmcg_write as CJ   # noqa: E402

KEYS = ["notes", "ticks", "ppqn", "peak_poly", "peak_nps", "frames"]


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def random_midi(rng):
    """Multi-track MIDI: tempo changes spread over tracks, running status, CC /
    pitch bend / program / sysex / text / lyric events, re-triggered keys.
    Every track ends right after its last note (or last tempo for the tempo
    track) and nothing happens after the song's last note."""
    ppqn = rng.choice([96, 384, 480, 960, 7680])
    ntrk = rng.randrange(2, 6)
    song_len = rng.randrange(2000, 40000)
    tracks = []
    # tempo track: tempos inside the song, ends at its last tempo
    ev = [(0, b"\xff\x51\x03" + rng.choice([400000, 500000, 600000]).to_bytes(3, "big"))]
    for _ in range(rng.randrange(0, 6)):
        ev.append((rng.randrange(0, song_len), b"\xff\x51\x03" + rng.randrange(150000, 1500000).to_bytes(3, "big")))
    if rng.random() < 0.5:
        ev.append((rng.randrange(0, song_len), b"\xff\x05\x03abc"))   # lyric (not an fMCG stat)
    tracks.append(sorted(ev, key=lambda e: e[0]))
    last_note = 0
    for t in range(1, ntrk):
        ev = []
        held = []
        tick = 0
        for _ in range(rng.randrange(20, 200)):
            tick += rng.choice([0, 0, 1, 7, 30, 120])
            if tick >= song_len:
                break
            r = rng.random()
            ch = rng.randrange(3)
            if r < 0.5 or not held:
                key = rng.choice([60, 61, 62, rng.randrange(128)])
                ev.append((tick, bytes([0x90 | ch, key, rng.randrange(1, 128)])))
                held.append((ch, key))
            elif r < 0.85:
                ch, key = held.pop(rng.randrange(len(held)))
                ev.append((tick, bytes([0x80 | ch, key, 64])))
            elif r < 0.9:
                ev.append((tick, bytes([0xB0 | ch, 7, rng.randrange(128)])))
            elif r < 0.93:
                ev.append((tick, bytes([0xE0 | ch, 0, 64])))
            elif r < 0.95:
                ev.append((tick, bytes([0xC0 | ch, 5])))
            elif r < 0.97:
                ev.append((tick, b"\xf0\x03\x7e\x01\xf7"))
            else:
                ev.append((tick, b"\xff\x01\x04text"))
        for ch, key in held:                                    # release the rest
            tick += rng.choice([1, 10, 100])
            ev.append((tick, bytes([0x80 | ch, key, 0])))
        while ev and ev[-1][1][0] & 0xF0 not in (0x80, 0x90):   # end on a note event
            ev.pop()
        if ev:
            last_note = max(last_note, ev[-1][0])
        tracks.append(ev)
    # nothing after the last note: clamp the tempo track
    tracks[0] = [e for e in tracks[0] if e[0] <= last_note] or [(0, tracks[0][0][1])]
    out = b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + len(tracks).to_bytes(2, "big") + ppqn.to_bytes(2, "big")
    for ev in tracks:
        body = bytearray()
        prev_tick, running = 0, None
        for tick, msg in ev:
            body += vlq(tick - prev_tick)
            prev_tick = tick
            st = msg[0]
            if st < 0xF0 and st == running and rng.random() < 0.7:
                body += msg[1:]                                  # running status
            else:
                body += msg
                running = st if st < 0xF0 else None
        body += b"\x00\xff\x2f\x00"
        out += b"MTrk" + len(body).to_bytes(4, "big") + bytes(body)
    return out


class Cjcmcg(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fmcg_cjcmcg_")

    def tearDown(self):
        for f in os.listdir(self.tmp):
            os.remove(os.path.join(self.tmp, f))
        os.rmdir(self.tmp)

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def probe(self, path):
        got, line = T.run_probe(path, 0, timeout=120)
        return got, line

    def test_matches_midi(self):
        """MIDI vs the .cjcmcg CJCMCG would write for it: identical stats."""
        for seed in range(T.seeds() * 2):
            rng = random.Random(f"cjcmcg-{seed}")
            mid = random_midi(rng)
            pm = self.write(f"s{seed}.mid", mid)
            pc = self.write(f"s{seed}.cjcmcg", CJ.from_midi(mid))
            want, wl = self.probe(pm)
            got, gl = self.probe(pc)
            with self.subTest(seed=seed):
                self.assertIsNotNone(want, wl)
                self.assertIsNotNone(got, gl)
                self.assertEqual({k: got[k] for k in KEYS}, {k: want[k] for k in KEYS})

    def test_spec_example(self):
        """The minimal example of the format spec (section 21)."""
        blob = bytes.fromhex("000001E0 00000001 00000000 07A120 00000001 00000000 00000000 01 00000002 0100 0001")
        got, line = self.probe(self.write("ex.cjcmcg", blob))
        self.assertIsNotNone(got, line)
        self.assertEqual((got["notes"], got["ticks"], got["ppqn"], got["peak_poly"], got["peak_nps"]),
                         (1, 1, 480, 1, 1))

    def test_synthetic_tempo_is_default(self):
        """CJCMCG's synthetic first tempo record means 120 BPM: a file with it and
        a tempo change later times exactly like the same file without it."""
        ticks = [(1, 0)] + [(0, 0)] * 958 + [(0, 1)]           # a note held for 959 ticks
        with_syn = CJ.encode(480, [(0, 5000000000.0 / 480), (480, 250000)], [(0, "")], 1, ticks)
        without = CJ.encode(480, [(480, 250000)], [], 1, ticks)
        a, la = self.probe(self.write("syn.cjcmcg", with_syn))
        b, lb = self.probe(self.write("nosyn.cjcmcg", without))
        self.assertIsNotNone(a, la)
        self.assertEqual(a, b)
        # 480 ticks at 120 BPM (0.5 s) + 479 ticks at 240 BPM (~0.25 s) at 60 fps
        self.assertTrue(40 <= a["frames"] <= 50, a)

    def test_compressed(self):
        """By name (song.cjcmcg.xz) and by content (song.xz holding a .cjcmcg)."""
        blob = CJ.from_midi(random_midi(random.Random("cjcmcg-compressed")))
        plain, lp = self.probe(self.write("p.cjcmcg", blob))
        named, ln = self.probe(self.write("p.cjcmcg.xz", lzma.compress(blob, format=lzma.FORMAT_XZ)))
        sniffed, ls = self.probe(self.write("p.xz", lzma.compress(blob, format=lzma.FORMAT_XZ)))
        self.assertIsNotNone(plain, lp)
        self.assertEqual(named, plain, ln)
        self.assertEqual(sniffed, plain, ls)

    def test_unmatched_note_offs(self):
        """Offs beyond the held count are ignored: polyphony never goes below 0."""
        ticks = [(0, 3), (2, 0), (0, 2), (1, 0)]
        got, line = self.probe(self.write("u.cjcmcg", CJ.encode(480, [], [], 3, ticks)))
        self.assertIsNotNone(got, line)
        self.assertEqual((got["notes"], got["peak_poly"]), (3, 2))

    def test_huge_counts(self):
        """64-bit varints: the largest stat in the corpus on a single tick."""
        n = 93823560428205
        got, line = self.probe(self.write("h.cjcmcg", CJ.encode(480, [], [], n, [(n, 0), (0, n)])))
        self.assertIsNotNone(got, line)
        self.assertEqual((got["notes"], got["peak_poly"], got["peak_nps"]), (n, n, n))

    def test_invalid(self):
        """Truncated .cjcmcg, and a non-MIDI non-.cjcmcg file: clean failure."""
        blob = CJ.from_midi(random_midi(random.Random("cjcmcg-trunc")))
        got, line = self.probe(self.write("t.cjcmcg", blob[:-3]))
        self.assertIsNone(got, line)
        got, line = self.probe(self.write("junk.bin", b"this is neither MIDI nor CJCMCG data" * 10))
        self.assertIsNone(got, line)


if __name__ == "__main__":
    unittest.main()
