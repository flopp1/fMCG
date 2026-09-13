// Regression for the burst window-boundary bug: a dense zero-delta-chained
// stream crosses the ByteStream's 512KB window many times; when a window ended
// exactly one byte after a chained zero delta, the next event's status byte
// was misread as a VLQ delta, inflating ticks and corrupting stats.
//
// The generator emits alternating note-on/note-off pairs chained by zero
// deltas (4 bytes per event) at tick 0, then a final delta ending the track.
// Expected results are exact: every note-on counted, zero max tick inflation
// (end tick == the final delta), and the byte-wise (FMCG_NO_BURST) build must
// produce identical output.
#include "../fMCG_core.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(const char* name, long long got, long long want) {
    printf("%-34s got=%lld want=%lld %s\n", name, got, want,
           got == want ? "[OK]" : "[FAIL]");
    if (got != want) fails++;
}

int main() {
    // ~2.5MB of track data => ~5 window crossings at CAP=512KB.
    // Event: 90 nn vv (3 bytes) chained by zero delta (1 byte) = 4 bytes.
    const int pairs = 300000;   // 300k on + 300k off = 600k events, 2.4MB
    std::vector<uint8_t> trk;
    trk.reserve((size_t)pairs * 8 + 8);
    uint64_t last_tick = 0;
    for (int i = 0; i < pairs; ++i) {
        uint8_t note = (uint8_t)(i & 0x7F);
        trk.push_back(0x00);                    // zero delta (chained)
        trk.push_back(0x90); trk.push_back(note); trk.push_back(100);
        trk.push_back(0x00);                    // zero delta
        trk.push_back(0x80); trk.push_back(note); trk.push_back(0x00);
    }
    // End the track at a known tick: delta 0x40 = 64.
    trk.push_back(0x40);
    last_tick = 64;
    trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);

    std::vector<uint8_t> f;
    f.insert(f.end(), {'M','T','h','d'}); 
    auto put32 = [&f](uint32_t x) { f.push_back(x>>24); f.push_back(x>>16); f.push_back(x>>8); f.push_back(x); };
    auto put16 = [&f](uint16_t x) { f.push_back(x>>8); f.push_back(x); };
    put32(6); put16(0); put16(1); put16(480);
    f.insert(f.end(), {'M','T','r','k'}); put32((uint32_t)trk.size());
    f.insert(f.end(), trk.begin(), trk.end());

    const char* path = "_burst_window_regression.mid";
    FILE* fp = fopen(path, "wb");
    fwrite(f.data(), 1, f.size(), fp);
    fclose(fp);

    ProgressCallbacks cb;   // no spec callback: tick 64 is far under the limit
    uint16_t div = 0; uint64_t notes = 0, ticks = 0;
    auto frames = ScaleMidiProcessor::process_midi(path, 60.0, div, notes, true, ticks, cb);

    check("notes counted", (long long)notes, (long long)pairs);
    check("total ticks", (long long)ticks, (long long)last_tick);
    check("frame count sane", (long long)(frames.size() >= 2), 1);   // at least tick-0 and tick-64 frames
    remove(path);

    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: ALL OK\n", fails);
    return fails ? 1 : 0;
}
