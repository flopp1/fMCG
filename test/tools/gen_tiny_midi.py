#!/usr/bin/env python3
# Minimal type-0 SMF: one note on/off, 96 PPQN.
import struct, sys

def vlq(n):
    out = bytes([n & 0x7F])
    n >>= 7
    while n:
        out = bytes([(n & 0x7F) | 0x80]) + out
        n >>= 7
    return out

def ev(dt, data):
    return vlq(dt) + bytes(data)

track = b"MTrk" + struct.pack(">I", 0)
# header: fmt 0, 1 track, 96 ppqn
body = (
    ev(0, [0xFF, 0x51, 3, 0x07, 0xA1, 0x20]) +   # tempo 500000
    ev(0, [0x90, 60, 100]) +
    ev(96, [0x80, 60, 0]) +
    ev(0, [0xFF, 0x2F, 0])
)
track = b"MTrk" + struct.pack(">I", len(body)) + body
mid = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96) + track
open(sys.argv[1], "wb").write(mid)