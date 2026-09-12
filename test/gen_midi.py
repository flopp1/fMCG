import struct, os

def vlq(n):
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, 0x80 | (n & 0x7F))
        n >>= 7
    return bytes(out)

def build_midi(path, events, ntrks=1, fmt=0, eot_delta=0):
    track = b''.join(vlq(d) + e for d, e in events) + vlq(eot_delta) + b'\xFF\x2F\x00'
    body = b'MThd' + struct.pack('>IHHH', 6, fmt, ntrks, 480)
    if ntrks == 1:
        body += b'MTrk' + struct.pack('>I', len(track)) + track
    else:
        body += b''.join(b'MTrk' + struct.pack('>I', len(t)) + t for t in track)
    with open(path, 'wb') as f:
        f.write(body)
    print(path, 'written:', len(body), 'bytes')

def build_midi_multi(path, per_track, fmt=1, division=480):
    """per_track: list of (events, eot_delta) tuples, one per track."""
    body = b'MThd' + struct.pack('>IHHH', 6, fmt, len(per_track), division)
    for events, eot_delta in per_track:
        t = b''.join(vlq(d) + e for d, e in events) + vlq(eot_delta) + b'\xFF\x2F\x00'
        body += b'MTrk' + struct.pack('>I', len(t)) + t
    with open(path, 'wb') as f:
        f.write(body)
    print(path, 'written:', len(body), 'bytes')

out = os.path.dirname(os.path.abspath(__file__))
ON = lambda n, v=100: bytes([0x90, n, v])
OFF = lambda n: bytes([0x80, n, 64])
TEMPO = lambda us: b'\xFF\x51\x03' + bytes([(us >> 16) & 0xFF, (us >> 8) & 0xFF, us & 0xFF])

# tempo2.mid: 3 tracks, tempo change mid-file, held notes across it, and
# notes in MULTIPLE tracks at tick 0 (catches a stale per-track tempo cursor).
# div=480; tempo: 120bpm@tick0 (0.5s/qn), 60bpm@tick960 (1s/qn).
# times: tick 0 -> 0.0s | 960 -> 1.0s | 1920 -> 2.0s | 2400 -> 3.0s | 2640 -> 3.5s
build_midi_multi(os.path.join(out, 'tempo2.mid'), [
    ([(0, TEMPO(500000)), (960, TEMPO(1000000))], 1680),          # EOT @2640
    ([(0, ON(60)), (960, OFF(60)), (0, ON(62)), (960, OFF(62))], 720),   # EOT @2640
    ([(0, ON(64)), (1920, OFF(64)), (480, ON(72)), (240, OFF(72))], 0),  # EOT @2640
])
# Expected: total_notes=4; cum: f0=2, f60=3, f180=4; poly: 2 (f0-119),
# 0 (f120-179), 1 (f180-209), 0 (f210+); peak_nps=2; frames=211 (3.5s)


# test.mid: tempo 120, note 60 on @tick0; F1/F2 system common; off @240;
# note 64 doubled (same pitch twice, refcount test) @480/600; offs @720;
# vel-0 note-on @720 (must NOT count); EOT tail delta 200160.
# Total 200880 ticks = 209.25s
build_midi(os.path.join(out, 'test.mid'), [
    (0, b'\xFF\x51\x03\x07\xA1\x20'),
    (0, b'\x90\x3C\x64'),
    (240, b'\xF1\x7F'),
    (0, b'\xF2\x00\x40'),
    (0, b'\x80\x3C\x40'),
    (240, b'\x90\x40\x64'),
    (120, b'\x90\x40\x64'),
    (120, b'\x80\x40\x40'),
    (0, b'\x80\x40\x40'),
    (0, b'\x90\x3E\x00'),
], eot_delta=200160)
# Expected: total_notes=3 (vel0=off) / 4 (vel0=on), poly peaks at 2, bpm=120

# short.mid: 1 second, 2 notes for the interactive run-through
build_midi(os.path.join(out, 'short.mid'), [
    (0, b'\xFF\x51\x03\x07\xA1\x20'),
    (0, b'\x90\x3C\x64'),
    (240, b'\x90\x41\x64'),
    (240, b'\x80\x3C\x40'),
    (240, b'\x80\x41\x40'),
], eot_delta=240)
# Expected: total_notes=2, duration 1.0s

track = vlq(0) + b'\x90\x3C\x64' + vlq(0) + b'\x80\x3C\x40'  # 10 bytes
body = b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', 20) + track
with open(os.path.join(out, 'desync.mid'), 'wb') as f:
    f.write(body)
print('desync.mid written:', len(body), 'bytes')

# notmidi.txt: valid file without MThd (reprompt test)
with open(os.path.join(out, 'notmidi.txt'), 'wb') as f:
    f.write(b'hello world, definitely not a midi file')
print('notmidi.txt written')

# Compressed fixtures for test_harness's archive-equivalence checks.
# Regenerated whenever the tools exist (test_harness expects both present).
import shutil, subprocess
src = os.path.join(out, 'test.mid')
txz = os.path.join(out, 'test.mid.tar.xz')
sz = os.path.join(out, 'test.mid.7z')
if shutil.which('tar'):
    subprocess.run(['tar', 'cJf', txz, '-C', out, 'test.mid'], check=False)
    print('test.mid.tar.xz written')
sevenz = shutil.which('7z') or shutil.which('7za')
if sevenz:
    subprocess.run([sevenz, 'a', '-y', sz, src], check=False,
                   stdout=subprocess.DEVNULL)
    print('test.mid.7z written')
if not (os.path.exists(txz) and os.path.exists(sz)):
    print('NOTE: some archive fixtures missing (need tar with xz and 7z); '
          'the archive-equivalence harness checks will fail until they exist.')
